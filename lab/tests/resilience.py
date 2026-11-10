"""
Offline / power loss / fault / authorization cache scenarios (docs/DESIGN_resilience.md targets).
OCTT document section 2.4 (cache), 2.14 (power failure), 2.15 (offline), 2.17 (fault).
Step numbers follow the "Scenario Detail(s)" table of each test case.
"""
import asyncio
import contextlib
import datetime as dt

from lab.tests.framework import testcase
from lab.tests.core import state_authorized, state_charging, status_is, field_is, _keys


# ---------------------------------------------------------------- helpers

@contextlib.asynccontextmanager
async def settings(ctx, values):
    """Set configuration keys for one test and put the old values back afterwards,
    also when the test fails, so later tests start from the same configuration."""
    res = await ctx.call("GetConfiguration", {"key": list(values)}, "before")
    old = {k: v.get("value") for k, v in _keys(res).items()}
    missing = [k for k in values if k not in old]
    ctx.require(not missing, "before", f"configuration keys not supported: {missing}")
    for key, val in values.items():
        res = await ctx.call("ChangeConfiguration", {"key": key, "value": str(val).lower()}, "before")
        ctx.require(res.get("status") == "Accepted", "before", f"ChangeConfiguration {key}: {res}")
    try:
        yield
    finally:
        conn = ctx.csms.conns and ctx.conn()
        for key, val in old.items():
            if conn and val is not None:
                try:
                    await conn.call("ChangeConfiguration", {"key": key, "value": val}, 10)
                except Exception:
                    pass


async def put_in_cache(ctx, tag):
    """Memory state IdTagCached: one online Authorize Accepted puts the tag in the cache.
    The same card again cancels the authorization, so the connector is idle afterwards."""
    await ctx.hw(f"card 1 {tag}")
    ev = await ctx.expect("Authorize", "cache", check=field_is("idTag", tag))
    ctx.require(ctx.response_to(ev)["idTagInfo"]["status"] == "Accepted", "cache", "Authorize not Accepted")
    await ctx.expect("StatusNotification", "cache", check=status_is("Preparing", 1))
    await ctx.hw(f"card 1 {tag}")
    await ctx.expect("StatusNotification", "cache", check=status_is("Available", 1))


async def power_cycle(ctx, off_seconds=1.0):
    await ctx.hw("power off")
    await asyncio.sleep(off_seconds)
    await ctx.hw("power on")


async def go_offline(ctx):
    await ctx.hw("net_down")
    await asyncio.sleep(1)
    st = await ctx.ctrl.status()
    ctx.require(st.get("ws") == "down", "offline", f"after net_down ws={st.get('ws')}")


def _ts(p):
    try:
        mv = p["meterValue"][0]
        return dt.datetime.fromisoformat(mv["timestamp"].replace("Z", "+00:00")), mv
    except (KeyError, IndexError, ValueError):
        return None, None


# ---------------------------------------------------------------- 2.4 authorization cache

@testcase("TC_007_1_CS", "Regular Start Charging Session - Cached Id")
async def tc_007_1(ctx):
    tag = ctx.cfg["valid_idtag"]
    async with settings(ctx, {"AuthorizationCacheEnabled": True, "LocalPreAuthorize": True}):
        await put_in_cache(ctx, tag)
        start = ctx.mark                                   # everything after the cache was filled
        await ctx.hw(f"card 1 {tag}")
        await ctx.expect("StatusNotification", 1, check=status_is("Preparing", 1))
        await ctx.hw("plug 1")
        await ctx.expect_unordered("3/5", [
            ("StartTransaction", None, field_is("idTag", tag)),
            ("StatusNotification", None, status_is("Charging", 1)),
        ])
        auth = [e for e in ctx.csms.events[start:] if e.direction == "cp->cs" and e.action == "Authorize"]
        ctx.require(not auth, 1, "sent Authorize.req for a cached idTag although LocalPreAuthorize is true")


@testcase("TC_007_2_CS", "Remote Start Charging Session - Cached Id")
async def tc_007_2(ctx):
    tag = ctx.cfg["valid_idtag"]
    async with settings(ctx, {"AuthorizationCacheEnabled": True, "LocalPreAuthorize": True,
                              "AuthorizeRemoteTxRequests": True}):
        await put_in_cache(ctx, tag)
        res = await ctx.call("RemoteStartTransaction", {"connectorId": 1, "idTag": tag}, 1)
        ctx.require(res.get("status") == "Accepted", 2, f"RemoteStartTransaction.conf {res}")
        start = ctx.mark
        await ctx.expect("StatusNotification", 3, check=status_is("Preparing", 1))
        await ctx.hw("plug 1")
        await ctx.expect_unordered("5/7", [
            ("StartTransaction", None, field_is("idTag", tag)),
            ("StatusNotification", None, status_is("Charging", 1)),
        ])
        auth = [e for e in ctx.csms.events[start:] if e.direction == "cp->cs" and e.action == "Authorize"]
        ctx.require(not auth, 2, "sent Authorize.req although the idTag is cached (note of step 2)")


@testcase("TC_061_1_CS", "Clear Authorization Data in Authorization Cache")
async def tc_061_1(ctx):
    tag = ctx.cfg["valid_idtag"]
    async with settings(ctx, {"AuthorizationCacheEnabled": True, "LocalPreAuthorize": True,
                              "ConnectionTimeOut": ctx.cfg["connection_timeout"]}):
        await state_authorized(ctx, step="A")              # step 1: Preparing is part of it
        await ctx.hw(f"card 1 {tag}")                      # swipe again: deauthorize
        await ctx.expect("StatusNotification", 3, check=status_is("Available", 1),
                         timeout=ctx.cfg["connection_timeout"] + 10)
        res = await ctx.call("ClearCache", {}, 5)
        ctx.require(res.get("status") == "Accepted", 6, f"ClearCache.conf {res}")
        await ctx.hw(f"card 1 {tag}")
        ev = await ctx.expect("Authorize", 7, check=field_is("idTag", tag))
        ctx.require(ctx.response_to(ev)["idTagInfo"]["status"] == "Accepted", 8, "Authorize.conf not Accepted")


@testcase("TC_061_2_CS", "Clear Authorization Data in Authorization Cache - Remote")
async def tc_061_2(ctx):
    tag = ctx.cfg["valid_idtag"]
    async with settings(ctx, {"AuthorizationCacheEnabled": True, "LocalPreAuthorize": True,
                              "AuthorizeRemoteTxRequests": True}):
        await put_in_cache(ctx, tag)
        res = await ctx.call("ClearCache", {}, 1)
        ctx.require(res.get("status") == "Accepted", 2, f"ClearCache.conf {res}")
        await state_charging(ctx, step="3")   # with an empty cache, Authorize.req must be sent again


# ---------------------------------------------------------------- 2.14 power failure

@testcase("TC_032_1_CS", "Power failure with backup power - stop transaction before going down")
async def tc_032_1(ctx):
    async with settings(ctx, {"BackupPower": True}):
        tx = await state_charging(ctx)
        await ctx.hw("power off")                           # [Disconnect the power]
        await ctx.expect_unordered("1/3", [
            ("StopTransaction", None, lambda p: None if p.get("reason") == "PowerLoss" and p.get("transactionId") == tx
             else f"reason/transactionId {p.get('reason')}/{p.get('transactionId')}, expected PowerLoss/{tx}"),
            ("StatusNotification", None, status_is("Finishing", 1)),
        ], timeout=15)
        await asyncio.sleep(1)
        await ctx.hw("power on")                            # [Reconnect the power]
        await ctx.expect("BootNotification", 5, timeout=40)
        await ctx.expect_unordered(7, [
            ("StatusNotification", lambda p: p.get("connectorId") == 1,
             lambda p: None if p.get("status") in ("Finishing", "Preparing") else f"status {p.get('status')}"),
            ("StatusNotification", lambda p: p.get("connectorId") == 0, status_is("Available")),
        ])
        later = [e for e in ctx.csms.events[ctx.mark:] if e.action == "StopTransaction"]
        ctx.require(not later, 7, "StopTransaction sent again after reboot (already sent before going down)")


@testcase("TC_034_CS", "Power Failure with Unavailable Status")
async def tc_034(ctx):
    res = await ctx.call("ChangeAvailability", {"connectorId": 0, "type": "Inoperative"}, 1)
    ctx.require(res.get("status") == "Accepted", 2, f"ChangeAvailability.conf {res}")
    await ctx.expect("StatusNotification", 3, check=status_is("Unavailable"))
    await power_cycle(ctx)
    await ctx.expect("BootNotification", 5, timeout=40)
    await ctx.expect_unordered(7, [
        ("StatusNotification", lambda p: p.get("connectorId") == 0, status_is("Unavailable")),
        ("StatusNotification", lambda p: p.get("connectorId") == 1, status_is("Unavailable")),
    ])
    await ctx.call("ChangeAvailability", {"connectorId": 0, "type": "Operative"}, "after")


# ---------------------------------------------------------------- 2.15 offline behaviour

@testcase("TC_036_CS", "Connection Loss During Transaction")
async def tc_036(ctx):
    interval = 5
    async with settings(ctx, {"MeterValueSampleInterval": interval}):
        await state_charging(ctx)
        await go_offline(ctx)                              # [Remove the connectivity]
        await asyncio.sleep(interval * 3 + 2)              # [Wait for at least 3 meter values]
        offline_until = dt.datetime.now(dt.timezone.utc)
        ctx.mark = len(ctx.csms.events)
        await ctx.hw("net_up")                             # [Restore the connectivity]
        evs = []
        while True:
            try:
                evs.append(await ctx.expect("MeterValues", 1, timeout=interval + 10))
            except Exception:
                break
            t, _ = _ts(evs[-1].payload)
            if t and t > offline_until:                    # a new (online) sample: queue is drained
                break
        ctx.require(evs, 1, "no MeterValues.req after the connection was restored")
        times, problems = [], []
        for e in evs:
            t, mv = _ts(e.payload)
            if t is None:
                problems.append(f"no timestamp in {e.raw[:120]}")
                continue
            times.append(t)
            for sv in mv.get("sampledValue", []):
                if sv.get("format", "Raw") != "Raw" or sv.get("context", "Sample.Periodic") != "Sample.Periodic":
                    problems.append(f"format/context {sv.get('format')}/{sv.get('context')}")
        ctx.require(not problems, 1, "; ".join(problems[:3]))
        queued = [t for t in times if t <= offline_until]
        ctx.require(len(queued) >= 3, 1, f"only {len(queued)} queued meter values sent after reconnect, expected >= 3")
        ctx.require(times == sorted(times), 1, f"not in chronological order: {[t.strftime('%H:%M:%S') for t in times]}")
        gaps = [(b - a).total_seconds() for a, b in zip(queued, queued[1:])]
        ctx.require(all(abs(g - interval) <= 1 for g in gaps), 1,
                    f"gaps between queued timestamps {gaps}, expected {interval} s")
        ctx.note(f"{len(queued)} queued MeterValues, gaps {gaps}")


async def _offline_start(ctx, tag):
    await go_offline(ctx)
    await ctx.hw(f"card 1 {tag}")                          # [EV Driver starts offline a transaction]
    await ctx.hw("plug 1")
    await asyncio.sleep(1.5)
    st = await ctx.ctrl.status()
    ctx.require(st.get("c1.status") == "Charging", "offline",
                f"offline transaction not started, status {st.get('c1.status')}")
    ctx.mark = len(ctx.csms.events)
    await ctx.hw("net_up")                                 # [Restore connectivity]


@testcase("TC_037_1_CS", "Offline Start Transaction - Valid IdTag")
async def tc_037_1(ctx):
    tag = ctx.cfg["valid_idtag"]
    async with settings(ctx, {"LocalAuthorizeOffline": True, "AllowOfflineTxForUnknownId": True}):
        await _offline_start(ctx, tag)
        start, _ = await ctx.expect_unordered("1/3", [
            ("StartTransaction", None, field_is("idTag", tag)),
            ("StatusNotification", lambda p: p.get("connectorId") == 1, status_is("Charging")),
        ], timeout=30)
        ctx.require(ctx.response_to(start)["idTagInfo"]["status"] == "Accepted", 2, "StartTransaction.conf not Accepted")


@testcase("TC_037_2_CS", "Offline Start Transaction - Invalid IdTag - StopTransactionOnInvalidId = false")
async def tc_037_2(ctx):
    bad = ctx.cfg["invalid_idtag"]
    async with settings(ctx, {"LocalAuthorizeOffline": True, "AllowOfflineTxForUnknownId": True,
                              "StopTransactionOnInvalidId": False}):
        await _offline_start(ctx, bad)
        start = await ctx.expect("StartTransaction", 1, timeout=30, check=field_is("idTag", bad))
        ctx.require(ctx.response_to(start)["idTagInfo"]["status"] == "Invalid", 2, "CSMS did not answer Invalid")
        await ctx.expect("StatusNotification", 3, timeout=15, check=status_is("Charging", 1))
        await ctx.expect("StatusNotification", 5, timeout=15, check=status_is("SuspendedEVSE", 1))
        stops = [e for e in ctx.csms.events[ctx.mark:] if e.action == "StopTransaction"]
        ctx.require(not stops, 5, "sent StopTransaction although StopTransactionOnInvalidId is false")


@testcase("TC_037_3_CS", "Offline Start Transaction - Invalid IdTag - StopTransactionOnInvalidId = true")
async def tc_037_3(ctx):
    bad = ctx.cfg["invalid_idtag"]
    async with settings(ctx, {"LocalAuthorizeOffline": True, "AllowOfflineTxForUnknownId": True,
                              "StopTransactionOnInvalidId": True}):
        await _offline_start(ctx, bad)
        start = await ctx.expect("StartTransaction", 1, timeout=30, check=field_is("idTag", bad))
        ctx.require(ctx.response_to(start)["idTagInfo"]["status"] == "Invalid", 2, "CSMS did not answer Invalid")
        await ctx.expect("StatusNotification", 3, timeout=15, check=status_is("Charging", 1))
        await ctx.expect("StopTransaction", 5, timeout=15, check=field_is("reason", "DeAuthorized"))
        await ctx.expect("StatusNotification", 7, timeout=15, check=status_is("Finishing", 1))


@testcase("TC_038_CS", "Offline Stop Transaction")
async def tc_038(ctx):
    tag = ctx.cfg["valid_idtag"]
    tx = await state_charging(ctx)
    await go_offline(ctx)                                  # [Remove the connectivity]
    await ctx.hw(f"card 1 {tag}")                          # [EV Driver stops the transaction offline]
    await asyncio.sleep(1)
    ctx.mark = len(ctx.csms.events)
    await ctx.hw("net_up")                                 # [Restore the connectivity]
    await ctx.expect_unordered("1/3", [
        ("StopTransaction", None, lambda p: None if p.get("transactionId") == tx and p.get("reason", "Local") == "Local"
         else f"transactionId/reason {p.get('transactionId')}/{p.get('reason')}, expected {tx}/Local"),
        ("StatusNotification", lambda p: p.get("connectorId") == 1, status_is("Finishing")),
    ], timeout=30)


# ---------------------------------------------------------------- 2.17 fault

@testcase("TC_041_CS", "Fault behavior")
async def tc_041(ctx):
    await ctx.hw("fault 1 overtemp")                       # [Set the Charge Point in fault state]
    await ctx.expect("StatusNotification", 1, check=status_is("Faulted", 1))
    await ctx.hw(f"card 1 {ctx.cfg['valid_idtag']}")       # [The EV Driver tries to start a transaction]
    await ctx.hw("plug 1")
    await ctx.expect_none(["StartTransaction"], 10, step=3)   # <Configured Transaction Duration> = 10 s
    st = await ctx.ctrl.status()
    ctx.require(st.get("c1.relay") == "0", 3, "relay closed (energy flowing) while Faulted")
