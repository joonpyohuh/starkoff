"""Additional Core and profile scenarios, based on OCTT section 2. Lab is not official OCTT."""

import asyncio
import datetime as dt
from lab.tests.framework import testcase
from lab.tests.core import state_charging, state_authorized, status_is, field_is


async def config(ctx, key, value):
    res = await ctx.call("ChangeConfiguration", {"key": key, "value": str(value).lower()}, "config")
    ctx.require(res.get("status") == "Accepted", "config", f"{key}: {res}")


async def local_list(ctx, version=2, entries=None, kind="Full"):
    return await ctx.call(
        "SendLocalList",
        {
            "listVersion": version,
            "updateType": kind,
            "localAuthorizationList": (
                entries if entries is not None else [{"idTag": "TAG001", "idTagInfo": {"status": "Accepted"}}]
            ),
        },
        "list",
    )


async def reserve(ctx, connector=1, ident=10, seconds=60, parent=None):
    p = {
        "connectorId": connector,
        "reservationId": ident,
        "idTag": "TAG001",
        "expiryDate": (dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=seconds)).isoformat(),
    }
    if parent:
        p["parentIdTag"] = parent
    return await ctx.call("ReserveNow", p, "reserve")


@testcase("TC_004_2_CS", "Identification first - ConnectionTimeOut")
async def timeout_local(ctx):
    await config(ctx, "ConnectionTimeOut", 3)
    await state_authorized(ctx)
    await ctx.expect("StatusNotification", 2, timeout=8, check=status_is("Available", 1))


@testcase("TC_011_2_CS", "Remote start - ConnectionTimeOut")
async def timeout_remote(ctx):
    await config(ctx, "ConnectionTimeOut", 3)
    await ctx.call("RemoteStartTransaction", {"connectorId": 1, "idTag": "TAG001"}, 1)
    await ctx.expect("StatusNotification", 7, check=status_is("Preparing", 1))
    await ctx.expect("StatusNotification", 9, timeout=8, check=status_is("Available", 1))


for code, stop, unlock in [("1", True, True), ("2", True, False), ("3", False, False)]:

    def register(code=code, stop=stop, unlock=unlock):
        @testcase(f"TC_005_{code}_CS", f"EV disconnect - stop={stop}, unlock={unlock}")
        async def scenario(ctx):
            await config(ctx, "StopTransactionOnEVSideDisconnect", stop)
            await config(ctx, "UnlockConnectorOnEVSideDisconnect", unlock)
            tx = await state_charging(ctx)
            await ctx.hw("ev_disconnect 1")
            if stop:
                await ctx.expect("StopTransaction", 1, check=field_is("reason", "EVDisconnected"))
                await ctx.expect("StatusNotification", 3, check=status_is("Finishing", 1))
            else:
                await ctx.expect("StatusNotification", 1, check=status_is("SuspendedEV", 1))
            st = await ctx.ctrl.status()
            ctx.require(st.get("c1.locked") == ("0" if unlock else "1"), "lock", f"unexpected lock: {st}")
            if not stop:
                await ctx.call("RemoteStopTransaction", {"transactionId": tx}, 3)
                await ctx.expect("StopTransaction", 7, check=field_is("reason", "Remote"))
            elif not unlock:
                res = await ctx.call("UnlockConnector", {"connectorId": 1}, 5)
                ctx.require(res.get("status") == "Unlocked", 6, str(res))

    register()


for code, typ in [("015", "Hard"), ("016", "Soft")]:

    def register(code=code, typ=typ):
        @testcase(f"TC_{code}_CS", f"{typ} reset during transaction")
        async def scenario(ctx):
            tx = await state_charging(ctx)
            res = await ctx.call("Reset", {"type": typ}, 1)
            ctx.require(res.get("status") == "Accepted", 2, str(res))
            await ctx.expect(
                "StopTransaction",
                3,
                check=lambda p: (
                    None if p.get("reason") == typ + "Reset" and p.get("transactionId") == tx else str(p)
                ),
            )
            await ctx.expect("BootNotification", 7, timeout=30)
            await ctx.expect(
                "StatusNotification",
                9,
                where=lambda p: p.get("connectorId") == 1,
                check=lambda p: None if p.get("status") in ("Preparing", "Finishing") else str(p),
            )

    register()


@testcase("TC_069_CS", "Stop using same parentIdTag")
async def parent_stop(ctx):
    ctx.csms.responder.parents = {"TAG001": "STARKOFF-CORP", "TAG002": "STARKOFF-CORP"}
    await state_charging(ctx)
    await ctx.hw("card 1 TAG002")
    await ctx.expect("Authorize", 1, check=field_is("idTag", "TAG002"))
    await ctx.expect("StopTransaction", 3, check=field_is("idTag", "TAG002"))


for code, action, payload, expected in [
    ("027", "RemoteStartTransaction", {"connectorId": 0, "idTag": "TAG001"}, "Rejected"),
    ("031", "UnlockConnector", {"connectorId": 99}, "NotSupported"),
    ("040_1", "ChangeConfiguration", {"key": "Testing", "value": "true"}, "NotSupported"),
    ("040_2", "ChangeConfiguration", {"key": "MeterValueSampleInterval", "value": "-1"}, "Rejected"),
    ("055", "TriggerMessage", {"requestedMessage": "MeterValues", "connectorId": 2}, "Rejected"),
    ("062", "DataTransfer", {"vendorId": "unknown.vendor"}, "UnknownVendorId"),
]:

    def register(code=code, action=action, payload=payload, expected=expected):
        @testcase(f"TC_{code}_CS", f"{action} - {expected}")
        async def scenario(ctx):
            res = await ctx.call(action, payload, 1)
            ctx.require(res.get("status") == expected, 2, f"expected {expected}, got {res}")

    register()


@testcase("TC_042_2_CS", "Empty local list reports version zero")
async def empty_list(ctx):
    res = await local_list(ctx, 1, [])
    ctx.require(res.get("status") == "Accepted", 2, str(res))
    res = await ctx.call("GetLocalListVersion", {}, 3)
    ctx.require(res.get("listVersion") == 0, 4, str(res))


@testcase("TC_043_CS", "Full and differential local list updates")
async def update_list(ctx):
    ctx.require((await local_list(ctx)).get("status") == "Accepted", 2, "full failed")
    ctx.require(
        (
            await local_list(
                ctx, 5, [{"idTag": "TAG002", "idTagInfo": {"status": "Accepted"}}], "Differential"
            )
        ).get("status")
        == "Accepted",
        4,
        "differential failed",
    )


@testcase("TC_043_2_CS", "Local list version 2 -> 5 -> reject 4")
async def version_mismatch(ctx):
    for version, kind, expected in [
        (2, "Full", "Accepted"),
        (5, "Differential", "Accepted"),
        (4, "Differential", "VersionMismatch"),
    ]:
        res = await local_list(ctx, version, kind=kind)
        ctx.require(res.get("status") == expected, version, str(res))
        res = await ctx.call("GetLocalListVersion", {}, version)
        ctx.require(res.get("listVersion") == (2 if version == 2 else 5), version, str(res))


@testcase("TC_043_3_CS", "Local list storage failure preserves previous version")
async def list_failure(ctx):
    await local_list(ctx, 2)
    await ctx.hw("cfg set _StoreFail true")
    try:
        res = await local_list(ctx, 3)
        ctx.require(res.get("status") == "Failed", 2, str(res))
        ctx.require(
            (await ctx.call("GetLocalListVersion", {}, 3)).get("listVersion") == 2, 4, "version changed"
        )
    finally:
        await ctx.hw("cfg set _StoreFail false")


for code, remote in [("008_1", False), ("008_2", True)]:

    def register(code=code, remote=remote):
        @testcase(f"TC_{code}_CS", "Local list authorization" + (" - remote" if remote else " - card"))
        async def scenario(ctx):
            await config(ctx, "LocalPreAuthorize", True)
            await config(ctx, "AuthorizationCacheEnabled", False)
            await local_list(ctx)
            mark = len(ctx.csms.events)
            if remote:
                await ctx.call("RemoteStartTransaction", {"connectorId": 1, "idTag": "TAG001"}, 5)
            else:
                await ctx.hw("card 1 TAG001")
            await ctx.expect("StatusNotification", 7, check=status_is("Preparing", 1))
            await ctx.hw("plug 1")
            await ctx.expect("StartTransaction", 9, check=field_is("idTag", "TAG001"))
            ctx.require(
                not any(e.type == 2 and e.action == "Authorize" for e in ctx.csms.events[mark:]),
                "authorize",
                "unexpected server authorization",
            )

    register()


for code, connector, remote, parent in [
    ("046_1", 1, False, False),
    ("046_2", 1, True, False),
    ("049", 0, True, False),
    ("053_1", 1, False, True),
    ("053_2", 1, True, True),
]:

    def register(code=code, connector=connector, remote=remote, parent=parent):
        @testcase(
            f"TC_{code}_CS",
            f"Reservation transaction - connector {connector}, remote={remote}, parent={parent}",
        )
        async def scenario(ctx):
            if parent:
                ctx.csms.responder.parents = {"TAG001": "STARKOFF-CORP", "TAG002": "STARKOFF-CORP"}
            res = await reserve(ctx, connector, parent="STARKOFF-CORP" if parent else None)
            ctx.require(res.get("status") == "Accepted", 2, str(res))
            await ctx.expect("StatusNotification", 3, check=status_is("Reserved", 1))
            tag = "TAG002" if parent else "TAG001"
            if not parent:
                await ctx.hw("card 1 TAG002")
                await ctx.expect_none(["Authorize", "StartTransaction"], 0.5, "wrong card")
            if remote:
                res = await ctx.call("RemoteStartTransaction", {"connectorId": 1, "idTag": tag}, 5)
                ctx.require(res.get("status") == "Accepted", 6, str(res))
            else:
                await ctx.hw(f"card 1 {tag}")
            await ctx.expect("StatusNotification", 8, check=status_is("Preparing", 1))
            await ctx.hw("plug 1")
            await ctx.expect(
                "StartTransaction",
                10,
                check=lambda p: None if p.get("reservationId") == 10 and p.get("idTag") == tag else str(p),
            )

    register()


@testcase("TC_047_CS", "Reservation expiry releases connector")
async def expire(ctx):
    ctx.require((await reserve(ctx, seconds=3)).get("status") == "Accepted", 2, "reserve failed")
    await ctx.expect("StatusNotification", 3, check=status_is("Reserved", 1))
    await ctx.expect("StatusNotification", 5, timeout=8, check=status_is("Available", 1))


for code, connector, condition, expected in [
    ("048_1", 1, "fault", "Faulted"),
    ("048_2", 1, "plug", "Occupied"),
    ("048_3", 1, "unavailable", "Unavailable"),
    ("050_1", 0, "fault", "Faulted"),
    ("050_2", 0, "plug", "Occupied"),
    ("050_3", 0, "unavailable", "Unavailable"),
]:

    def register(code=code, connector=connector, condition=condition, expected=expected):
        @testcase(f"TC_{code}_CS", f"Reserve connector {connector} - {expected}")
        async def scenario(ctx):
            if condition == "fault":
                await ctx.hw("fault 1 overtemp")
            elif condition == "plug":
                await ctx.hw("plug 1")
            else:
                await ctx.call("ChangeAvailability", {"connectorId": 0, "type": "Inoperative"}, 1)
            await ctx.expect("StatusNotification", 1, where=lambda p: p.get("connectorId") == 1)
            res = await reserve(ctx, connector)
            ctx.require(res.get("status") == expected, 2, str(res))

    register()


for code, ident, expected in [("051", 10, "Accepted"), ("052", 11, "Rejected")]:

    def register(code=code, ident=ident, expected=expected):
        @testcase(f"TC_{code}_CS", "Cancel reservation - " + expected)
        async def scenario(ctx):
            await reserve(ctx)
            await ctx.expect("StatusNotification", 3, check=status_is("Reserved", 1))
            res = await ctx.call("CancelReservation", {"reservationId": ident}, 1)
            ctx.require(res.get("status") == expected, 2, str(res))
            if expected == "Accepted":
                await ctx.expect("StatusNotification", 3, check=status_is("Available", 1))

    register()


@testcase("TC_054_CS", "Trigger MeterValues, Heartbeat, StatusNotification")
async def trigger(ctx):
    for action in ("MeterValues", "Heartbeat", "StatusNotification"):
        p = {"requestedMessage": action}
        if action != "Heartbeat":
            p["connectorId"] = 1
        res = await ctx.call("TriggerMessage", p, action)
        ctx.require(res.get("status") == "Accepted", action, str(res))
        ev = await ctx.expect(action, action)
        if action == "MeterValues":
            ctx.require("transactionId" not in ev.payload, action, str(ev.payload))
            ctx.require(
                all(
                    v.get("context") == "Trigger" for m in ev.payload["meterValue"] for v in m["sampledValue"]
                ),
                action,
                str(ev.payload),
            )
    for action in ("DiagnosticsStatusNotification", "FirmwareStatusNotification"):
        res = await ctx.call("TriggerMessage", {"requestedMessage": action}, action)
        ctx.require(res.get("status") == "NotImplemented", action, str(res))


for code, context, key in [
    ("070", "Sample.Periodic", "MeterValueSampleInterval"),
    ("071", "Sample.Clock", "ClockAlignedDataInterval"),
]:

    def register(code=code, context=context, key=key):
        @testcase(f"TC_{code}_CS", "Meter values - " + context)
        async def scenario(ctx):
            await config(ctx, "MeterValueSampleInterval", 0)
            await config(ctx, "ClockAlignedDataInterval", 0)
            await config(ctx, key, 3)
            await state_charging(ctx)
            values = []
            for i in range(3):
                ev = await ctx.expect(
                    "MeterValues",
                    i,
                    timeout=8,
                    where=lambda p: p["meterValue"][0]["sampledValue"][0]["context"] == context,
                )
                timestamp = dt.datetime.fromisoformat(
                    ev.payload["meterValue"][0]["timestamp"].replace("Z", "+00:00")
                )
                values.append(timestamp.timestamp())
                if context == "Sample.Clock":
                    ctx.require(timestamp.second % 3 == 0, i, str(ev.payload))
            ctx.require(all(b - a == 3 for a, b in zip(values, values[1:])), 3, str(values))

    register()

for code, action, payload in [
    ("026", "RemoteStartTransaction", {"connectorId": 1, "idTag": "TAG002"}),
    ("028", "RemoteStopTransaction", {"transactionId": 999999}),
]:

    def register(code=code, action=action, payload=payload):
        @testcase(f"TC_{code}_CS", action + " - rejected while charging")
        async def scenario(ctx):
            await state_charging(ctx)
            res = await ctx.call(action, payload, 1)
            ctx.require(res.get("status") == "Rejected", 2, str(res))

    register()


@testcase("TC_018_1_CS", "Unlock during transaction")
async def unlock_transaction(ctx):
    await state_charging(ctx)
    res = await ctx.call("UnlockConnector", {"connectorId": 1}, 1)
    ctx.require(res.get("status") == "Unlocked", 2, str(res))
    await ctx.expect("StopTransaction", 3, check=field_is("reason", "UnlockCommand"))


@testcase("TC_030_CS", "Unlock motor failure")
async def unlock_failure(ctx):
    await ctx.hw("lock_fail 1 on")
    res = await ctx.call("UnlockConnector", {"connectorId": 1}, 1)
    ctx.require(res.get("status") == "UnlockFailed", 2, str(res))


@testcase("TC_024_CS", "Lock failure prevents transaction")
async def lock_failure(ctx):
    await state_authorized(ctx)
    await ctx.hw("lock_fail 1 on")
    await ctx.hw("plug 1")
    await ctx.expect(
        "StatusNotification",
        9,
        check=lambda p: (
            None if p.get("status") == "Faulted" and p.get("errorCode") == "ConnectorLockFailure" else str(p)
        ),
    )
    await ctx.expect_none(["StartTransaction"], 1, "no transaction")


for code, remote in [("023_4", False), ("023_5", True)]:

    def register(code=code, remote=remote):
        @testcase(f"TC_{code}_CS", "Invalid card refuses charging" + (" - remote" if remote else ""))
        async def scenario(ctx):
            if remote:
                await ctx.call("RemoteStartTransaction", {"connectorId": 1, "idTag": "BADTAG"}, 1)
            else:
                await ctx.hw("card 1 BADTAG")
            await ctx.expect("Authorize", 3, check=field_is("idTag", "BADTAG"))
            await ctx.hw("plug 1")
            await ctx.expect("StatusNotification", 5, check=status_is("Preparing", 1))
            await ctx.expect_none(["StartTransaction"], 1, "no transaction")

    register()


@testcase("TC_032_2_CS", "Power loss queues transaction stop across reboot")
async def power_loss(ctx):
    tx = await state_charging(ctx)
    await ctx.hw("power off")
    await asyncio.sleep(0.3)
    await ctx.hw("power on")
    await ctx.expect("BootNotification", 1, timeout=30)
    await ctx.expect(
        "StopTransaction",
        5,
        timeout=20,
        check=lambda p: None if p.get("transactionId") == tx and p.get("reason") == "PowerLoss" else str(p),
    )


@testcase("TC_039_CS", "Offline start and stop replay in order")
async def offline_transaction(ctx):
    await config(ctx, "AllowOfflineTxForUnknownId", True)
    await ctx.hw("net_down")
    await asyncio.sleep(0.5)
    await ctx.hw("card 1 TAG001")
    await ctx.hw("plug 1")
    await asyncio.sleep(0.5)
    st = await ctx.ctrl.status()
    ctx.require(st.get("c1.status") == "Charging", "offline", str(st))
    await ctx.hw("card 1 TAG001")
    await asyncio.sleep(0.2)
    await ctx.hw("unplug 1")
    await ctx.hw("net_up")
    start = await ctx.expect("StartTransaction", 1, timeout=20)
    stop = await ctx.expect("StopTransaction", 3, timeout=20)
    ctx.require(
        ctx.response_to(start)["transactionId"] == stop.payload.get("transactionId"), 3, str(stop.payload)
    )


@testcase("X_LOCAL_PERSIST", "Local list and reservation survive reboot")
async def local_persist(ctx):
    await local_list(ctx, 7)
    await reserve(ctx, ident=100)
    await ctx.expect("StatusNotification", 1, check=status_is("Reserved", 1))
    await ctx.hw("reboot")
    await ctx.expect("BootNotification", 2, timeout=30)
    await ctx.expect(
        "StatusNotification", 3, where=lambda p: p.get("connectorId") == 1, check=status_is("Reserved", 1)
    )
    res = await ctx.call("GetLocalListVersion", {}, 4)
    ctx.require(res.get("listVersion") == 7, 4, str(res))


@testcase("X_LOCAL_DELETE", "Differential delete and duplicate rejection")
async def local_delete(ctx):
    await local_list(ctx, 2)
    res = await local_list(ctx, 3, [{"idTag": "TAG001"}], "Differential")
    ctx.require(res.get("status") == "Accepted", 1, str(res))
    ctx.require((await ctx.call("GetLocalListVersion", {}, 2)).get("listVersion") == 0, 2, "not empty")
    entry = {"idTag": "TAG001", "idTagInfo": {"status": "Accepted"}}
    res = await local_list(ctx, 4, [entry, entry])
    ctx.require(res.get("status") == "Failed", 3, str(res))


@testcase("X_TX_RETRY", "Transaction CALLERROR retries before advancing queue")
async def retry_transaction(ctx):
    from lab.csms.csms import CallError

    await config(ctx, "TransactionMessageRetryInterval", 1)
    await config(ctx, "TransactionMessageAttempts", 2)
    attempts = 0

    def start(payload):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise CallError("InternalError", "injected processing failure")
        return {"transactionId": 5000, "idTagInfo": {"status": "Accepted"}}

    ctx.csms.responder.overrides["StartTransaction"] = start
    await state_authorized(ctx)
    await ctx.hw("plug 1")
    first = await ctx.expect("StartTransaction", 1)
    retry = await ctx.expect("StartTransaction", 2, timeout=6)
    ctx.require(
        first.payload == retry.payload and retry.t - first.t >= 0.9,
        2,
        "retry changed payload or ignored backoff",
    )
    await ctx.hw("card 1 TAG001")
    await ctx.expect("StopTransaction", 3, check=field_is("transactionId", 5000))


@testcase("X_PENDING_TRIGGER", "Pending trigger bypasses persisted transaction backlog")
async def pending_trigger(ctx):
    await config(ctx, "AllowOfflineTxForUnknownId", True)
    await ctx.hw("net_down")
    await asyncio.sleep(0.3)
    await ctx.hw("card 1 TAG001")
    await ctx.hw("plug 1")
    await asyncio.sleep(0.3)
    ctx.csms.responder.boot_script = [("Pending", 5)]
    await ctx.hw("reboot")
    await ctx.expect("BootNotification", 1, timeout=30)
    res = await ctx.call("TriggerMessage", {"requestedMessage": "Heartbeat"}, 2)
    ctx.require(res.get("status") == "Accepted", 2, str(res))
    await ctx.expect("Heartbeat", 3, timeout=3)
    await ctx.expect("BootNotification", 4, timeout=10)
    await ctx.expect("StartTransaction", 5)
    await ctx.expect("StopTransaction", 6, check=field_is("reason", "PowerLoss"))


@testcase("X_TX_RETRY_EXHAUSTED", "StartTransaction processing failure stops energy after final attempt")
async def exhausted_transaction(ctx):
    from lab.csms.csms import CallError

    await config(ctx, "TransactionMessageAttempts", 1)

    def reject(payload):
        raise CallError("InternalError", "persistent processing failure")

    ctx.csms.responder.overrides["StartTransaction"] = reject
    await state_authorized(ctx)
    await ctx.hw("plug 1")
    await ctx.expect("StartTransaction", 1)
    await ctx.expect("StatusNotification", 2, where=lambda p: p.get("status") == "Finishing")
    st = await ctx.ctrl.status()
    ctx.require(st.get("c1.relay") == "0", 3, str(st))


@testcase("X_METER_RECONFIG", "Meter interval changes apply immediately; alignment restarts at UTC midnight")
async def meter_reconfigure(ctx):
    await state_charging(ctx)
    await config(ctx, "MeterValueSampleInterval", 1)
    await ctx.expect("MeterValues", 1, timeout=4)
    await config(ctx, "MeterValueSampleInterval", 0)
    await ctx.expect_none(["MeterValues"], 1.5, 2)
    ctx.csms.responder.overrides["Heartbeat"] = lambda p: {"currentTime": "2026-11-10T23:59:58Z"}
    await ctx.call("TriggerMessage", {"requestedMessage": "Heartbeat"}, 3)
    await ctx.expect("Heartbeat", 4)
    await config(ctx, "ClockAlignedDataInterval", 50000)
    ev = await ctx.expect("MeterValues", 5, timeout=5)
    ctx.require(ev.payload["meterValue"][0]["timestamp"] == "2026-11-11T00:00:00Z", 5, str(ev.payload))
