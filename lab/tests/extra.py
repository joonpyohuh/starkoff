"""
Lab checks that are NOT OCTT test cases (ids start with X_).
They check behaviour from the spec / our design documents that the OCTT list
does not cover directly.
"""

import asyncio

from lab.tests.framework import testcase
from lab.csms.csms import CallError


@testcase("X_NOTIMPL", "Unknown action -> CALLERROR NotImplemented")
async def x_notimpl(ctx):
    try:
        await ctx.conn().call("FooBarAction", {}, timeout=10)
    except CallError as e:
        ctx.require(e.code == "NotImplemented", 1, f"CALLERROR code {e.code}, expected NotImplemented")
        return
    except asyncio.TimeoutError:
        ctx.fail(1, "no answer to an unknown action (every CALL needs a CALLRESULT or CALLERROR)")
    ctx.fail(1, "unknown action was answered with CALLRESULT")


@testcase("X_RECONNECT", "Connection loss and reconnect without reboot")
async def x_reconnect(ctx):
    await ctx.hw("net_down")
    conn = ctx.conn()
    try:
        await asyncio.wait_for(conn.closed.wait(), 30)
    except asyncio.TimeoutError:
        pass  # the CP closed its side; the CSMS may only notice on its own ping timeout
    st = await ctx.ctrl.status()
    ctx.require(st.get("ws") == "down", 1, f"after net_down ws={st.get('ws')}, expected down")
    await asyncio.sleep(3)
    ctx.mark = len(ctx.csms.events)
    await ctx.hw("net_up")
    # Reconnect without reboot: no BootNotification, but the current status again (connectorId 0 and 1)
    ev = await ctx.expect("StatusNotification", step=2, timeout=30)
    boots = [e for e in ctx.csms.events[ctx.mark - 1 :] if e.action == "BootNotification"]
    ctx.require(not boots, 2, "sent BootNotification after a reconnect without reboot")
    ctx.note(f"reconnected, first message StatusNotification(connectorId={ev.payload.get('connectorId')})")


@testcase("X_REJECTED_SILENT", "While Rejected the CP answers nothing")
async def x_rejected_silent(ctx):
    ctx.csms.responder.boot_script = [("Rejected", 20)]
    await ctx.hw("power off")
    await asyncio.sleep(1)
    await ctx.hw("power on")
    await ctx.expect("BootNotification", step=1, timeout=40)
    await asyncio.sleep(1)
    try:
        await ctx.conn().call("GetConfiguration", {}, timeout=5)
        ctx.fail(2, "answered GetConfiguration while Rejected (spec 4.2: SHALL NOT respond)")
    except asyncio.TimeoutError:
        pass
    except CallError as e:
        ctx.fail(2, f"answered with CALLERROR {e.code} while Rejected (must stay silent)")
    await ctx.expect("BootNotification", step=3, timeout=30)  # then Accepted (default)
    ctx.note("no answer while Rejected, BootNotification retried after the interval")


# ---------------------------------------------------------------- A1 checks (new features)
import datetime as _dt
import time as _time


async def _settings(ctx, values):
    """Set keys, return the old values (caller restores them)."""
    res = await ctx.call("GetConfiguration", {"key": list(values)}, "before")
    old = {k["key"]: k.get("value") for k in res.get("configurationKey", [])}
    for k, v in values.items():
        r = await ctx.call("ChangeConfiguration", {"key": k, "value": str(v)}, "before")
        ctx.require(r.get("status") == "Accepted", "before", f"{k}: {r}")
    return old


async def _restore(ctx, old):
    for k, v in old.items():
        try:
            await ctx.conn().call("ChangeConfiguration", {"key": k, "value": v}, 10)
        except Exception:
            pass


@testcase("X_WATCHDOG", "CPU hang: LED goes dark, software watchdog reboots (~30 s)")
async def x_watchdog(ctx):
    st = await ctx.ctrl.status()
    ctx.require(st.get("led") == "green", 0, f"LED not green before the test: {st.get('led')}")
    t0 = _time.monotonic()
    await ctx.hw("hang")
    dark_after = None
    while _time.monotonic() - t0 < 10:
        st = await ctx.ctrl.status()  # the control thread still answers: only the main loop hangs
        if st.get("led") != "green":
            dark_after = _time.monotonic() - t0
            break
        await asyncio.sleep(0.2)
    ctx.require(
        dark_after is not None and dark_after <= 2.5,
        1,
        f"status LED still green {(_time.monotonic() - t0):.1f} s after the hang (expected dark within 2 s)",
    )
    boot = await ctx.expect("BootNotification", 2, timeout=60)
    reboot_after = boot.t - t0
    ctx.require(
        25 <= reboot_after <= 45,
        2,
        f"BootNotification {reboot_after:.1f} s after the hang, expected ~30 s (watchdog) + boot",
    )
    ctx.note(f"LED dark after {dark_after:.1f} s, reboot+BootNotification after {reboot_after:.1f} s")


async def _cable_after(ctx, cmd, want, limit=2.0):
    """Send plug/unplug, return seconds until status shows c1.cable == want."""
    t0 = _time.monotonic()
    await ctx.hw(cmd)
    while _time.monotonic() - t0 < limit:
        if (await ctx.ctrl.status()).get("c1.cable") == want:
            return _time.monotonic() - t0
        await asyncio.sleep(0.02)
    return None


@testcase("X_DEBOUNCE", "Cable detect debouncing: noise never changes the connector state")
async def x_debounce(ctx):
    d_plug = await _cable_after(ctx, "plug 1", "1")
    d_unplug = await _cable_after(ctx, "unplug 1", "0")
    ctx.require(
        d_plug is not None and d_unplug is not None, 1, f"plug/unplug not detected: {d_plug}/{d_unplug}"
    )
    try:
        await ctx.hw("noise 1 on")
        for cable, label in (("0", "unplugged"), ("1", "plugged")):
            if cable == "1":
                d = await _cable_after(ctx, "plug 1", "1", limit=3.0)
                ctx.require(d is not None, 2, "plug not detected while noise is on")
            await asyncio.sleep(1)  # let the StatusNotification of the last real change arrive first
            g0 = int((await ctx.ctrl.status()).get("c1.glitches", 0))
            start = len(ctx.csms.events)
            changes = 0
            t0 = _time.monotonic()
            while _time.monotonic() - t0 < 15:
                if (await ctx.ctrl.status()).get("c1.cable") != cable:
                    changes += 1
                await asyncio.sleep(0.05)
            g1 = int((await ctx.ctrl.status()).get("c1.glitches", 0))
            notes = [
                e.payload.get("status")
                for e in ctx.csms.events[start:]
                if e.action == "StatusNotification" and e.payload.get("connectorId") == 1
            ]
            ctx.require(
                g1 - g0 > 100, 3, f"{label}: only {g1 - g0} noisy samples in 15 s (noise not active?)"
            )
            ctx.require(
                changes == 0 and not notes,
                3,
                f"{label}: cable state flipped {changes}x, StatusNotification {notes} during noise",
            )
            ctx.note(f"{label}: {g1 - g0} glitches filtered in 15 s, 0 state changes")
        d = await _cable_after(ctx, "unplug 1", "0", limit=3.0)
        ctx.require(d is not None, 4, "unplug not detected while noise is on")
    finally:
        await ctx.ctrl.cmd("noise 1 off")
    ctx.note(
        f"delay without noise: plug {d_plug * 1000:.0f} ms, unplug {d_unplug * 1000:.0f} ms (incl. status polling)"
    )


def _mv_ts(mv):
    return _dt.datetime.fromisoformat(mv["timestamp"].replace("Z", "+00:00"))


@testcase("X_TXDATA", "StopTransaction.transactionData: Begin, samples, End; never larger than one message")
async def x_txdata(ctx):
    from lab.tests.core import state_charging, status_is

    old = await _settings(
        ctx, {"StopTxnSampledData": "Energy.Active.Import.Register", "MeterValueSampleInterval": "2"}
    )
    try:
        tx = await state_charging(ctx)
        await asyncio.sleep(7)  # ~3 periodic samples
        await ctx.call("RemoteStopTransaction", {"transactionId": tx}, 1)
        stop = await ctx.expect("StopTransaction", 2, timeout=15)
        data = stop.payload.get("transactionData")
        ctx.require(
            isinstance(data, list) and len(data) >= 3, 2, f"transactionData missing/short: {stop.raw[:300]}"
        )
        ctxs = [mv["sampledValue"][0].get("context") for mv in data]
        ctx.require(
            ctxs[0] == "Transaction.Begin"
            and ctxs[-1] == "Transaction.End"
            and all(c == "Sample.Periodic" for c in ctxs[1:-1]),
            2,
            f"contexts {ctxs}",
        )
        times = [_mv_ts(mv) for mv in data]
        ctx.require(times == sorted(times), 2, f"timestamps not in order: {[t.time() for t in times]}")
        wh = [int(mv["sampledValue"][0]["value"]) for mv in data]
        ctx.require(wh == sorted(wh) and wh[0] <= wh[-1], 2, f"energy not increasing: {wh}")
        ctx.require(
            stop.payload.get("meterStop") == wh[-1],
            2,
            f"meterStop {stop.payload.get('meterStop')} != Transaction.End value {wh[-1]}",
        )
        ctx.note(f"case 1: {ctxs.count('Sample.Periodic')} periodic samples, Wh {wh}")

        # case 2: all four measurands, long transaction -> must stay a valid message <= 1 KB payload
        await ctx.hw("unplug 1")
        # the unplug is debounced (~120 ms); until then the connector is still Finishing and a
        # RemoteStart is (correctly) Rejected, so wait for Available first
        await ctx.expect(
            "StatusNotification",
            "C2.0",
            where=lambda p: p.get("connectorId") == 1 and p.get("status") == "Available",
        )
        await _settings(
            ctx,
            {
                "StopTxnSampledData": "Energy.Active.Import.Register,Power.Active.Import,Current.Import,Voltage",
                "MeterValueSampleInterval": "1",
            },
        )
        tx = await state_charging(ctx, step="C2")
        await asyncio.sleep(10)  # more samples than the ring (6) holds
        await ctx.call("RemoteStopTransaction", {"transactionId": tx}, 3)
        stop = await ctx.expect("StopTransaction", 4, timeout=15)
        size = len(json_dumps(stop.payload))
        data = stop.payload.get("transactionData", [])
        ctx.require(
            stop.payload.get("transactionId") == tx and stop.payload.get("reason") == "Remote",
            4,
            f"StopTransaction core fields wrong: {stop.raw[:200]}",
        )
        ctx.require(size <= 1024, 4, f"payload {size} bytes > 1024")
        ctx.require(
            data and data[-1]["sampledValue"][0].get("context") == "Transaction.End",
            4,
            f"Transaction.End missing when the message is full: {stop.raw[:300]}",
        )
        ctx.require(all(len(mv["sampledValue"]) == 4 for mv in data), 4, "sample with missing measurands")
        ctx.note(f"case 2: payload {size} bytes, {len(data)} MeterValue elements kept")
    finally:
        await _restore(ctx, old)


def json_dumps(p):
    import json

    return json.dumps(p, separators=(",", ":"))
