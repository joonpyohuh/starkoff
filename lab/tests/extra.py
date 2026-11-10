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
    boots = [e for e in ctx.csms.events[ctx.mark - 1:] if e.action == "BootNotification"]
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
    await ctx.expect("BootNotification", step=3, timeout=30)   # then Accepted (default)
    ctx.note("no answer while Rejected, BootNotification retried after the interval")
