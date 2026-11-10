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
