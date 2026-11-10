"""
OCTT Core profile test cases (docs/reference/testcases.txt, section 2).
Each step number matches the "Scenario Detail(s)" table of the document.
"""
import asyncio

from lab.tests.framework import testcase

# Required Core keys and allowed accessibility (TC_019_CS, step 4 table). R = readonly true.
CORE_KEYS = {
    "AuthorizeRemoteTxRequests": "R|RW",
    "ClockAlignedDataInterval": "RW",
    "ConnectionTimeOut": "RW",
    "ConnectorPhaseRotation": "RW",
    "GetConfigurationMaxKeys": "R",
    "HeartbeatInterval": "RW",
    "LocalAuthorizeOffline": "RW",
    "LocalPreAuthorize": "RW",
    "MeterValuesAlignedData": "RW",
    "MeterValuesSampledData": "RW",
    "MeterValueSampleInterval": "RW",
    "NumberOfConnectors": "R",
    "ResetRetries": "RW",
    "StopTransactionOnInvalidId": "RW",
    "StopTxnAlignedData": "RW",
    "StopTxnSampledData": "RW",
    "SupportedFeatureProfiles": "R",
    "TransactionMessageAttempts": "RW",
    "TransactionMessageRetryInterval": "RW",
    "UnlockConnectorOnEVSideDisconnect": "R|RW",
}


def status_is(expected, connector=None):
    def check(p):
        if p.get("status") != expected:
            return f"status is {p.get('status')}, expected {expected}"
        if connector is not None and p.get("connectorId") != connector:
            return f"connectorId is {p.get('connectorId')}, expected {connector}"
        return None
    return check


async def expect_status_all(ctx, step, expected, timeout=15):
    """StatusNotification for connectorId=0 and every connector, any order."""
    for c in (0, ctx.cfg["connector_id"]):
        await_from = ctx.mark
        await ctx.expect("StatusNotification", step, timeout,
                         where=lambda p, c=c: p.get("connectorId") == c,
                         check=status_is(expected))
        ctx.mark = await_from      # the two notifications may come in any order
    ctx.mark = len(ctx.csms.events)


async def expect_heartbeats(ctx, step, interval):
    hb1 = await ctx.expect("Heartbeat", step, timeout=interval + 5)
    hb2 = await ctx.expect("Heartbeat", step, timeout=interval + 5)
    gap = hb2.t - hb1.t
    ctx.require(abs(gap - interval) <= 2, step,
                f"Heartbeat.req every {gap:.1f} s, expected every {interval} s")
    ctx.note(f"heartbeat gap {gap:.1f} s (interval {interval} s)")


@testcase("TC_001_CS", "Cold Boot Charge Point")
async def tc_001(ctx):
    rej, hb = 10, ctx.cfg["heartbeat_interval"]
    ctx.csms.responder.boot_script = [("Rejected", rej)]
    ctx.csms.responder.heartbeat_interval = hb

    # [Power cycle the Charge Point.]
    await ctx.hw("power off")
    await asyncio.sleep(1)
    await ctx.hw("power on")

    b1 = await ctx.expect("BootNotification", step=1, timeout=40)
    # Step 2: CSMS answered Rejected (interval rej). Step 3: next Boot only after rej seconds,
    # and nothing else in between (spec 4.2: SHALL NOT send any OCPP message while Rejected).
    b2 = await ctx.expect("BootNotification", step=3, timeout=rej + 20)
    between = [e for e in ctx.csms.events
               if e.direction == "cp->cs" and e.type == 2 and b1.t < e.t < b2.t]
    ctx.require(not between, 3, f"sent {between[0].action if between else ''} while Rejected")
    waited = b2.t - b1.t
    ctx.require(waited >= rej - 0.5, 3,
                f"BootNotification.req resent after {waited:.1f} s, interval was {rej} s")
    ctx.note(f"re-boot after {waited:.1f} s (Rejected interval {rej} s)")

    await expect_status_all(ctx, step=5, expected="Available")
    await expect_heartbeats(ctx, step=7, interval=hb)


@testcase("TC_002_CS", "Cold Boot Charge Point - Pending")
async def tc_002(ctx):
    pend, hb, mvi = 15, ctx.cfg["heartbeat_interval"], ctx.cfg["meter_value_interval"]
    ctx.csms.responder.boot_script = [("Pending", pend)]
    ctx.csms.responder.heartbeat_interval = hb

    await ctx.hw("power off")
    await asyncio.sleep(1)
    await ctx.hw("power on")
    b1 = await ctx.expect("BootNotification", step=1, timeout=40)
    await asyncio.sleep(1)

    res = await ctx.call("GetConfiguration", {}, step=3)
    ctx.require(res.get("configurationKey"), 4, f"GetConfiguration.conf has no keys: {res}")
    res = await ctx.call("ChangeConfiguration",
                         {"key": "MeterValueSampleInterval", "value": str(mvi)}, step=5)
    ctx.require(res.get("status") == "Accepted", 6,
                f"ChangeConfiguration.conf status is {res.get('status')}, expected Accepted")

    b2 = await ctx.expect("BootNotification", step=7, timeout=pend + 20)
    between = [e.action for e in ctx.csms.events
               if e.direction == "cp->cs" and e.type == 2 and b1.t < e.t < b2.t]
    ctx.require(not between, 7, f"sent {between} while Pending (only answers are allowed)")
    waited = b2.t - b1.t
    ctx.require(waited >= pend - 0.5, 7,
                f"BootNotification.req resent after {waited:.1f} s, interval was {pend} s")

    await expect_status_all(ctx, step=9, expected="Available")
    await expect_heartbeats(ctx, step=11, interval=hb)


def _keys(res):
    return {k["key"]: k for k in res.get("configurationKey", [])}


@testcase("TC_019_CS", "Retrieve configuration")
async def tc_019(ctx):
    res = await ctx.call("GetConfiguration", {"key": ["SupportedFeatureProfiles"]}, step=1)
    ctx.require(not res.get("unknownKey"), 2, f"unknownKey is not empty: {res}")
    keys = _keys(res)
    ctx.require("SupportedFeatureProfiles" in keys, 2, f"key missing: {res}")
    profiles = [p.strip() for p in keys["SupportedFeatureProfiles"].get("value", "").split(",")]
    ctx.require("Core" in profiles, 2, f"SupportedFeatureProfiles = {profiles}, must contain Core")

    res = await ctx.call("GetConfiguration", {}, step=3)
    allk = _keys(res)
    for key, acc in CORE_KEYS.items():
        ctx.require(key in allk, 4, f"required key {key} missing in GetConfiguration.conf")
        ro = allk[key].get("readonly")
        ok = {"R": ro is True, "RW": ro is False, "R|RW": ro in (True, False)}[acc]
        ctx.require(ok, 4, f"{key} readonly={ro}, expected accessibility {acc}")

    res = await ctx.call("GetConfiguration", {"key": ["GetConfigurationMaxKeys"]}, step=5)
    maxkeys = int(_keys(res)["GetConfigurationMaxKeys"]["value"])
    wanted = list(allk)[:min(maxkeys, len(allk))]
    res = await ctx.call("GetConfiguration", {"key": wanted}, step=7)
    ctx.require(not res.get("unknownKey"), 8, f"unknownKey is not empty: {res.get('unknownKey')}")
    got = _keys(res)
    missing = [k for k in wanted if k not in got]
    ctx.require(not missing, 8, f"keys requested but not returned: {missing}")
    ctx.note(f"{len(allk)} keys, GetConfigurationMaxKeys={maxkeys}")


@testcase("TC_021_CS", "Change/set Configuration")
async def tc_021(ctx):
    mvi = str(ctx.cfg["meter_value_interval"])
    res = await ctx.call("GetConfiguration", {"key": ["MeterValueSampleInterval"]}, step=0)
    if _keys(res)["MeterValueSampleInterval"]["value"] == mvi:
        # Before-state of the document: value must NOT already be the configured one.
        await ctx.call("ChangeConfiguration",
                       {"key": "MeterValueSampleInterval", "value": str(int(mvi) + 1)}, step=0)

    res = await ctx.call("ChangeConfiguration",
                         {"key": "MeterValueSampleInterval", "value": mvi}, step=1)
    ctx.require(res.get("status") == "Accepted", 2,
                f"ChangeConfiguration.conf status is {res.get('status')}, expected Accepted")
    res = await ctx.call("GetConfiguration", {"key": ["MeterValueSampleInterval"]}, step=3)
    val = _keys(res).get("MeterValueSampleInterval", {}).get("value")
    ctx.require(val == mvi, 4, f"MeterValueSampleInterval is {val}, expected {mvi}")


# ======================================================================
# Transactions (OCTT 2.2 - 2.7). Reusable states from section 2.26.
# ======================================================================

def field_is(name, expected):
    def check(p):
        return None if p.get(name) == expected else f"{name} is {p.get(name)!r}, expected {expected!r}"
    return check


def conn_is(c):
    return lambda p: p.get("connectorId") == c


async def authorize_remote_flag(ctx, step):
    res = await ctx.call("GetConfiguration", {"key": ["AuthorizeRemoteTxRequests"]}, step)
    return _keys(res).get("AuthorizeRemoteTxRequests", {}).get("value") == "true"


async def state_authorized(ctx, step="A"):
    """Reusable state Authorized: present valid idTag -> Authorize.req + StatusNotification(Preparing)."""
    tag = ctx.cfg["valid_idtag"]
    await ctx.hw(f"card {ctx.cfg['connector_id']} {tag}")
    await ctx.expect_unordered(f"{step}.1/3", [
        ("Authorize", None, field_is("idTag", tag)),
        ("StatusNotification", None, status_is("Preparing")),
    ])


async def state_charging(ctx, step="C"):
    """Reusable state Charging: remote start, then plug in. Returns the transactionId."""
    c, tag = ctx.cfg["connector_id"], ctx.cfg["valid_idtag"]
    art = await authorize_remote_flag(ctx, f"{step}.1")
    res = await ctx.call("RemoteStartTransaction", {"connectorId": c, "idTag": tag}, f"{step}.3")
    ctx.require(res.get("status") == "Accepted", f"{step}.4",
                f"RemoteStartTransaction.conf status is {res.get('status')}, expected Accepted")
    if art:
        await ctx.expect("Authorize", f"{step}.5", check=field_is("idTag", tag))
    await ctx.expect("StatusNotification", f"{step}.7", check=status_is("Preparing", c))
    await ctx.hw(f"plug {c}")
    start, _ = await ctx.expect_unordered(f"{step}.9/11", [
        ("StartTransaction", None, lambda p: None if p.get("connectorId") == c and p.get("idTag") == tag
         else f"connectorId/idTag are {p.get('connectorId')}/{p.get('idTag')}"),
        ("StatusNotification", None, status_is("Charging", c)),
    ])
    return ctx.response_to(start)["transactionId"]


@testcase("TC_003_CS", "Regular Charging Session - Plugin First")
async def tc_003(ctx):
    c, tag = ctx.cfg["connector_id"], ctx.cfg["valid_idtag"]
    await ctx.hw(f"plug {c}")
    await ctx.expect("StatusNotification", 1, check=status_is("Preparing", c))
    await ctx.hw(f"card {c} {tag}")
    auth = await ctx.expect("Authorize", 3, check=field_is("idTag", tag))
    ctx.require(ctx.response_to(auth)["idTagInfo"]["status"] == "Accepted", 4, "Authorize.conf not Accepted")
    start, _ = await ctx.expect_unordered("5/7", [
        ("StartTransaction", None, field_is("idTag", tag)),
        ("StatusNotification", None, status_is("Charging", c)),
    ])
    ctx.note(f"transactionId {ctx.response_to(start)['transactionId']}")


@testcase("TC_004_1_CS", "Regular Charging Session - Identification First")
async def tc_004_1(ctx):
    c = ctx.cfg["connector_id"]
    await ctx.call("ChangeConfiguration", {"key": "MeterValueSampleInterval",
                                           "value": str(ctx.cfg["meter_value_interval"])}, "before")
    await state_authorized(ctx, step="1")
    await ctx.hw(f"plug {c}")
    await ctx.expect_unordered("3/5", [
        ("StartTransaction", None, field_is("idTag", ctx.cfg["valid_idtag"])),
        ("StatusNotification", None, status_is("Charging", c)),
    ])


@testcase("TC_068_CS", "Stop transaction - IdTag in StopTransaction matches IdTag in StartTransaction")
async def tc_068(ctx):
    c = ctx.cfg["connector_id"]
    await state_charging(ctx)
    await ctx.hw(f"card {c} {ctx.cfg['valid_idtag_2']}")
    await ctx.expect_none(["Authorize", "StopTransaction"], 5, step=1)
    await ctx.hw(f"card {c} {ctx.cfg['valid_idtag']}")
    await ctx.expect_unordered("3/5", [
        ("StopTransaction", None, field_is("idTag", ctx.cfg["valid_idtag"])),
        ("StatusNotification", None, status_is("Finishing", c)),
    ])


@testcase("TC_010_CS", "Remote Start Charging Session - Cable Plugged in First")
async def tc_010(ctx):
    c, tag = ctx.cfg["connector_id"], ctx.cfg["valid_idtag"]
    art = await authorize_remote_flag(ctx, 1)
    await ctx.hw(f"plug {c}")
    await ctx.expect("StatusNotification", 3, check=status_is("Preparing", c))
    res = await ctx.call("RemoteStartTransaction", {"connectorId": c, "idTag": tag}, 5)
    ctx.require(res.get("status") == "Accepted", 6, f"RemoteStartTransaction.conf status is {res.get('status')}")
    if art:
        await ctx.expect("Authorize", 7, check=field_is("idTag", tag))
    await ctx.expect_unordered("9/11", [
        ("StartTransaction", None, lambda p: None if p.get("connectorId") == c and p.get("idTag") == tag
         else f"connectorId/idTag are {p.get('connectorId')}/{p.get('idTag')}"),
        ("StatusNotification", None, status_is("Charging", c)),
    ])


@testcase("TC_011_1_CS", "Remote Start Charging Session - Remote Start First")
async def tc_011_1(ctx):
    await state_charging(ctx, step="")


@testcase("TC_012_CS", "Remote Stop Charging Session")
async def tc_012(ctx):
    c = ctx.cfg["connector_id"]
    tx = await state_charging(ctx)
    res = await ctx.call("RemoteStopTransaction", {"transactionId": tx}, 1)
    ctx.require(res.get("status") == "Accepted", 2, f"RemoteStopTransaction.conf status is {res.get('status')}")
    await ctx.expect_unordered("3/5", [
        ("StopTransaction", None, lambda p: None if p.get("reason") == "Remote" and p.get("transactionId") == tx
         else f"reason/transactionId are {p.get('reason')}/{p.get('transactionId')}, expected Remote/{tx}"),
        ("StatusNotification", None, status_is("Finishing", c)),
    ])


async def _reset_without_tx(ctx, kind):
    c = ctx.cfg["connector_id"]
    res = await ctx.call("ChangeAvailability", {"connectorId": c, "type": "Inoperative"}, 1)
    ctx.require(res.get("status") == "Accepted", 2, f"ChangeAvailability.conf status is {res.get('status')}")
    await ctx.expect("StatusNotification", 3, check=status_is("Unavailable", c))
    res = await ctx.call("Reset", {"type": kind}, 5)
    ctx.require(res.get("status") == "Accepted", 6, f"Reset.conf status is {res.get('status')}")
    await ctx.expect("BootNotification", 7, timeout=40)
    await ctx.expect_unordered(9, [
        ("StatusNotification", conn_is(c), status_is("Unavailable")),
        ("StatusNotification", conn_is(0), status_is("Available")),
    ])
    await asyncio.sleep(1)
    res = await ctx.call("ChangeAvailability", {"connectorId": c, "type": "Operative"}, 11)
    ctx.require(res.get("status") == "Accepted", 12, f"ChangeAvailability.conf status is {res.get('status')}")
    await ctx.expect("StatusNotification", 13, check=status_is("Available", c))


@testcase("TC_013_CS", "Hard Reset Without transaction")
async def tc_013(ctx):
    await _reset_without_tx(ctx, "Hard")


@testcase("TC_014_CS", "Soft Reset Without Transaction")
async def tc_014(ctx):
    await _reset_without_tx(ctx, "Soft")


@testcase("TC_017_1_CS", "Unlock connector - no charging session running (Not fixed cable)")
async def tc_017_1(ctx):
    res = await ctx.call("UnlockConnector", {"connectorId": ctx.cfg["connector_id"]}, 1)
    ctx.require(res.get("status") == "Unlocked", 2, f"UnlockConnector.conf status is {res.get('status')}, expected Unlocked")
