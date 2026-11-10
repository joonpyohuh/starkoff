"""
Mock Central System (CSMS) for OCPP 1.6J. Lab tool only, not part of the firmware.

- WebSocket server, path /ocpp/<ChargePointId>, subprotocol "ocpp1.6".
- Answers every CALL from the charge point automatically (see Responder).
  Tests change the answers, e.g. TC_001 makes the first BootNotification Rejected.
- Every message in and out is recorded (Event) so tests can wait for them and
  the web UI can show them.
- Tests can send CALLs to the charge point with CPConn.call().
"""
import asyncio
import datetime as dt
import itertools
import json
import time

import websockets

SUBPROTOCOL = "ocpp1.6"


def utc_now_iso():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


class Event:
    """One OCPP message seen by the CSMS."""

    def __init__(self, direction, raw, msg):
        self.t = time.monotonic()
        self.wall = utc_now_iso()
        self.direction = direction          # "cp->cs" or "cs->cp"
        self.raw = raw
        self.msg = msg                      # parsed JSON list
        self.type = msg[0] if isinstance(msg, list) and msg else None
        self.id = msg[1] if isinstance(msg, list) and len(msg) > 1 else None
        self.action = msg[2] if self.type == 2 and len(msg) > 2 else None
        self.payload = msg[3] if self.type == 2 and len(msg) > 3 else (
            msg[2] if self.type == 3 and len(msg) > 2 else None)

    def short(self):
        arrow = ">>" if self.direction == "cs->cp" else "<<"
        return f"{arrow} {self.raw}"


class Responder:
    """How the CSMS answers CALLs from the charge point. Tests may change any field."""

    def __init__(self):
        self.heartbeat_interval = 300
        # List of (status, interval) for the next BootNotifications; when empty -> Accepted.
        self.boot_script = []
        self.valid_tags = {"TAG001": "Accepted", "TAG002": "Accepted"}
        self.next_tx_id = 1
        self.overrides = {}   # action -> function(payload) -> response payload

    def answer(self, action, payload):
        if action in self.overrides:
            return self.overrides[action](payload)
        if action == "BootNotification":
            status, interval = (self.boot_script.pop(0) if self.boot_script
                                else ("Accepted", self.heartbeat_interval))
            return {"status": status, "currentTime": utc_now_iso(), "interval": interval}
        if action == "Heartbeat":
            return {"currentTime": utc_now_iso()}
        if action == "Authorize":
            return {"idTagInfo": {"status": self.valid_tags.get(payload.get("idTag"), "Invalid")}}
        if action == "StartTransaction":
            tx = self.next_tx_id
            self.next_tx_id += 1
            return {"transactionId": tx,
                    "idTagInfo": {"status": self.valid_tags.get(payload.get("idTag"), "Invalid")}}
        if action == "StopTransaction":
            return {"idTagInfo": {"status": "Accepted"}} if payload.get("idTag") else {}
        # StatusNotification, MeterValues, ... have empty confirmations
        return {}


class CPConn:
    """One connected charge point."""

    def __init__(self, csms, cp_id, ws):
        self.csms = csms
        self.cp_id = cp_id
        self.ws = ws
        self.pending = {}            # our CALL id -> Future
        self.ids = itertools.count(1)
        self.closed = asyncio.Event()

    async def call(self, action, payload, timeout=15):
        """Send a CALL to the charge point, return its CALLRESULT payload.
        Raises CallError on CALLERROR, asyncio.TimeoutError if no answer."""
        mid = f"cs-{next(self.ids)}"
        fut = asyncio.get_running_loop().create_future()
        self.pending[mid] = fut
        await self.csms._send(self, [2, mid, action, payload])
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self.pending.pop(mid, None)


class CallError(Exception):
    def __init__(self, code, desc):
        super().__init__(f"{code}: {desc}")
        self.code = code


class CSMS:
    def __init__(self, host="0.0.0.0", port=9000, verbose=True):
        self.host, self.port = host, port
        self.verbose = verbose
        self.responder = Responder()
        self.conns = {}              # cp_id -> CPConn (latest connection)
        self.events = []             # every Event, in order
        self.listeners = []          # callables(event) e.g. web UI
        self._new_event = asyncio.Condition()
        self._server = None

    async def start(self):
        self._server = await websockets.serve(
            self._handler, self.host, self.port, subprotocols=[SUBPROTOCOL],
            ping_interval=20, ping_timeout=20)

    async def stop(self):
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    # ---------------- recording ----------------
    async def _record(self, direction, raw, msg):
        ev = Event(direction, raw, msg)
        self.events.append(ev)
        if self.verbose:
            print(f"[{ev.wall[11:23]}] {ev.short()[:300]}", flush=True)
        for fn in self.listeners:
            try:
                fn(ev)
            except Exception:  # a broken listener must not stop the CSMS
                pass
        async with self._new_event:
            self._new_event.notify_all()
        return ev

    async def _send(self, conn, msg):
        raw = json.dumps(msg, separators=(",", ":"))
        await conn.ws.send(raw)
        await self._record("cs->cp", raw, msg)

    async def wait_event(self, pred, start_index, timeout):
        """Wait for the first Event at index >= start_index with pred(ev) true.
        Returns (event, index) or (None, None) on timeout."""
        deadline = time.monotonic() + timeout
        i = start_index
        async with self._new_event:
            while True:
                while i < len(self.events):
                    if pred(self.events[i]):
                        return self.events[i], i
                    i += 1
                left = deadline - time.monotonic()
                if left <= 0:
                    return None, None
                try:
                    await asyncio.wait_for(self._new_event.wait(), left)
                except asyncio.TimeoutError:
                    pass

    # ---------------- connection handler ----------------
    async def _handler(self, ws, path):
        cp_id = path.rstrip("/").split("/")[-1]
        if ws.subprotocol != SUBPROTOCOL:
            print(f"[csms] {cp_id}: client did not offer subprotocol {SUBPROTOCOL}", flush=True)
        conn = CPConn(self, cp_id, ws)
        self.conns[cp_id] = conn
        print(f"[csms] {cp_id} connected from {ws.remote_address} path={path}", flush=True)
        try:
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except ValueError:
                    await self._record("cp->cs", raw, ["?"])
                    continue
                ev = await self._record("cp->cs", raw, msg)
                if ev.type == 2:
                    payload = self.responder.answer(ev.action, ev.payload or {})
                    if payload is not None:   # None = do not answer (tests of timeouts)
                        await self._send(conn, [3, ev.id, payload])
                elif ev.type in (3, 4) and ev.id in conn.pending:
                    fut = conn.pending[ev.id]
                    if not fut.done():
                        if ev.type == 3:
                            fut.set_result(msg[2])
                        else:
                            fut.set_exception(CallError(msg[2], msg[3] if len(msg) > 3 else ""))
        except websockets.ConnectionClosed:
            pass
        finally:
            conn.closed.set()
            if self.conns.get(cp_id) is conn:
                del self.conns[cp_id]
            print(f"[csms] {cp_id} disconnected", flush=True)
