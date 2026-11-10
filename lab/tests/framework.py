"""
Tiny test framework for OCTT-style scenarios.

A test is an async function decorated with @testcase("TC_xxx_CS", "name").
It gets a Ctx with helpers that follow the wording of the OCTT document:
  ev = await ctx.expect("StatusNotification", step=1, check=lambda p: ...)
  res = await ctx.call("GetConfiguration", {...}, step=1)
  await ctx.hw("plug 1")
A failure raises TestFail with "Step N: expected ..., got ...".
"""
import asyncio
import time

from lab.csms.csms import CallError, Responder

TESTS = {}


class TestFail(Exception):
    pass


class NotApplicable(Exception):
    pass


def testcase(tc_id, name):
    def deco(fn):
        TESTS[tc_id] = (name, fn)
        return fn
    return deco


# Values the OCTT document calls <Configured ...>
CONFIG = {
    "connector_id": 1,
    "valid_idtag": "TAG001",
    "valid_idtag_2": "TAG002",
    "invalid_idtag": "BADTAG",
    "heartbeat_interval": 10,
    "meter_value_interval": 15,
    "connection_timeout": 15,
}


class Ctx:
    def __init__(self, csms, ctrl, cfg=None):
        self.csms = csms
        self.ctrl = ctrl
        self.cfg = dict(CONFIG, **(cfg or {}))
        self.mark = len(csms.events)   # events before this index are "already seen"
        self.notes = []

    # ---------------- helpers ----------------
    def note(self, text):
        self.notes.append(text)

    def fail(self, step, text):
        raise TestFail(f"Step {step}: {text}")

    def require(self, cond, step, text):
        if not cond:
            self.fail(step, text)

    def _seen_since(self, start):
        acts = [e.action for e in self.csms.events[start:] if e.direction == "cp->cs" and e.type == 2]
        return ", ".join(acts) if acts else "nothing"

    async def expect(self, action, step, timeout=15, check=None, where=None):
        """Wait for the next CALL `action` from the charge point (other messages are skipped).
        where(payload) -> bool selects among messages of that action (e.g. connectorId).
        check(payload) -> None or error text, validates the selected message."""
        start = self.mark

        def pred(e):
            return (e.direction == "cp->cs" and e.type == 2 and e.action == action
                    and (where is None or where(e.payload or {})))

        ev, idx = await self.csms.wait_event(pred, start, timeout)
        if ev is None:
            self.fail(step, f"expected {action}.req within {timeout} s, "
                            f"but the charge point sent: {self._seen_since(start)}")
        self.mark = idx + 1
        if check:
            err = check(ev.payload or {})
            if err:
                self.fail(step, f"{action}.req {err}. Received: {ev.raw}")
        return ev

    async def expect_none(self, actions, duration, step):
        """The charge point must NOT send any of `actions` for `duration` seconds."""
        start = self.mark
        ev, idx = await self.csms.wait_event(
            lambda e: e.direction == "cp->cs" and e.type == 2 and e.action in actions,
            start, duration)
        if ev is not None:
            self.fail(step, f"expected no {'/'.join(actions)} for {duration} s, "
                            f"but got {ev.raw}")

    async def call(self, action, payload, step, timeout=15):
        conn = self.conn()
        try:
            res = await conn.call(action, payload, timeout)
        except asyncio.TimeoutError:
            self.fail(step, f"no {action}.conf within {timeout} s")
        except CallError as e:
            self.fail(step, f"{action} answered with CALLERROR {e}")
        self.mark = len(self.csms.events)
        return res

    def conn(self):
        if not self.csms.conns:
            raise TestFail("charge point is not connected to the CSMS")
        return next(reversed(self.csms.conns.values()))

    async def hw(self, cmd):
        reply = await self.ctrl.cmd(cmd)
        if not reply.startswith("OK"):
            raise TestFail(f"control command '{cmd}' failed: {reply}")
        return reply

    async def wait_ready(self, timeout=90):
        """General preconditions (OCTT 1.3): connected, Accepted, connectors Available, idle."""
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            try:
                last = await self.ctrl.status()
            except ConnectionError:
                last = {}
            if (last.get("boot") == "Accepted" and last.get("ws") == "up"
                    and last.get("queue") == "0" and self.csms.conns
                    and all(v == "Available" for k, v in last.items() if k.endswith(".status"))):
                await asyncio.sleep(0.5)
                self.mark = len(self.csms.events)
                return
            await asyncio.sleep(0.5)
        raise TestFail(f"precondition: charge point not ready (status: {last})")

    async def reset_cp_state(self):
        """Bring the charge point back to the general preconditions."""
        for c in range(1, 2):
            await self.ctrl.cmd(f"fault {c} off")
            await self.ctrl.cmd(f"lock_fail {c} off")
            st = await self.ctrl.status()
            if st.get(f"c{c}.cable") == "1":
                await self.ctrl.cmd(f"unplug {c}")
        await self.ctrl.cmd("net_up")


async def run_one(tc_id, csms, ctrl, cfg=None):
    """Returns (result, reason, seconds). result: PASS / FAIL / N/A / ERROR."""
    name, fn = TESTS[tc_id]
    csms.responder = Responder()
    ctx = Ctx(csms, ctrl, cfg)
    t0 = time.monotonic()
    try:
        await ctx.reset_cp_state()
        await ctx.wait_ready()
        await fn(ctx)
        return "PASS", "; ".join(ctx.notes), time.monotonic() - t0
    except TestFail as e:
        return "FAIL", str(e), time.monotonic() - t0
    except NotApplicable as e:
        return "N/A", str(e), time.monotonic() - t0
    except Exception as e:  # bug in the test or the lab
        return "ERROR", f"{type(e).__name__}: {e}", time.monotonic() - t0
    finally:
        csms.responder = Responder()
