"""
Lab web UI server (http://127.0.0.1:8080). Lab tool only.

One port serves both:
  GET /, /app.js, /style.css   static files from this folder
  /ws                          WebSocket to the browser

To the browser:  hello (tests + recent log), ocpp (every OCPP message), status (firmware
                 status every 0.5 s, or offline), test (running / result), reply (command answer)
From the browser: {"t":"cmd","cmd":"plug 1"}  {"t":"run","id":"TC_003_CS"}  {"t":"run_all"}

The test buttons call lab.tests.framework.run_one, i.e. exactly the code the terminal
command (scripts/run_lab.sh) runs.
"""
import asyncio
import http
import json
import mimetypes
import os

import websockets

from lab.tests.framework import TESTS, run_one
from lab.web.telemetry import Telemetry

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/style.css": "style.css"}
# Only these control commands may be sent from the browser.
ALLOWED = ("plug", "unplug", "ev_disconnect", "card", "power", "lock_fail", "fault",
           "net_down", "net_up", "status", "reboot", "noise", "hang")


def ev_json(ev):
    return {"wall": ev.wall, "dir": ev.direction, "type": ev.type, "id": ev.id,
            "action": ev.action, "raw": ev.raw}


class WebUI:
    def __init__(self, csms, ctrl, host="127.0.0.1", port=8080):
        self.csms, self.ctrl = csms, ctrl
        self.host, self.port = host, port
        self.clients = set()
        self.test_lock = asyncio.Lock()
        self.results = {}
        self.running = None
        self.telemetry = Telemetry()
        self.tasks = set()
        self.queue = asyncio.Queue(maxsize=1024)
        csms.listeners.append(self._event)

    def _event(self, ev):
        self.telemetry.event(ev)
        if self.queue.full():
            self.queue.get_nowait()
        self.queue.put_nowait({"t": "ocpp", "ev": ev_json(ev)})

    def _task(self, coro):
        task = asyncio.create_task(coro)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    async def stop(self):
        self.csms.listeners.remove(self._event)
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        self.server.close()
        await self.server.wait_closed()
        self.telemetry.close()

    async def start(self):
        self.server = await websockets.serve(self._ws, self.host, self.port, process_request=self._http,
                                              max_size=16384, origins=[f"http://{self.host}:{self.port}", f"http://localhost:{self.port}"])
        self._task(self._pump())
        self._task(self._poll_status())

    # ---------------- HTTP (static files) ----------------
    async def _http(self, path, headers):
        path = path.split("?")[0]
        if path == "/ws":
            return None                              # continue with the WebSocket handshake
        name = STATIC.get(path)
        if not name:
            return http.HTTPStatus.NOT_FOUND, [("Content-Type", "text/plain")], b"not found\n"
        with open(os.path.join(HERE, name), "rb") as f:
            body = f.read()
        ctype = mimetypes.guess_type(name)[0] or "application/octet-stream"
        return http.HTTPStatus.OK, [("Content-Type", ctype + "; charset=utf-8"),
                                    ("Cache-Control", "no-store")], body

    # ---------------- broadcasting ----------------
    async def _pump(self):
        while True:
            msg = await self.queue.get()
            await self._broadcast(msg)

    async def _broadcast(self, msg):
        data = json.dumps(msg)
        for ws in list(self.clients):
            try:
                await ws.send(data)
            except websockets.ConnectionClosed:
                self.clients.discard(ws)

    async def _poll_status(self):
        while True:
            try:
                st = await asyncio.wait_for(self.ctrl.status(), 3)
                msg = {"t": "status", "ok": True, "st": st,
                       "csms_connected": bool(self.csms.conns), "stats": self.telemetry.stats()}
                self.telemetry.plugged = st.get("c1.cable") == "1"
            except Exception:
                msg = {"t": "status", "ok": False, "csms_connected": bool(self.csms.conns)}
            await self._broadcast(msg)
            await asyncio.sleep(0.5)

    # ---------------- browser connection ----------------
    async def _ws(self, ws, path):
        self.clients.add(ws)
        await ws.send(json.dumps({
            "t": "hello",
            "tests": [{"id": k, "name": v[0]} for k, v in TESTS.items()],
            "results": self.results,
            "running": self.running,
            "events": [ev_json(e) for e in self.csms.events[-300:]],
        }))
        try:
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except ValueError:
                    continue
                if not isinstance(msg, dict):
                    continue
                if msg.get("t") == "ocpp":
                    self._task(self._ocpp(msg))
                elif msg.get("t") == "cmd":
                    self._task(self._cmd(ws, str(msg.get("cmd", ""))))
                elif msg.get("t") == "run" and isinstance(msg.get("id"), str) and msg["id"] in TESTS:
                    self._task(self._run([msg["id"]]))
                elif msg.get("t") == "run_all":
                    self._task(self._run(list(TESTS)))
        except websockets.ConnectionClosed:
            pass
        finally:
            self.clients.discard(ws)

    async def _cmd(self, ws, cmd):
        if not cmd or "\n" in cmd or "\r" in cmd or cmd.split(" ")[0] not in ALLOWED:
            reply = "ERR not allowed from the web UI"
        elif self.running is not None:
            reply = "ERR a test is running"
        else:
            try:
                async with self.test_lock:
                    reply = await self.ctrl.cmd(cmd)
                    if reply.startswith("OK"):
                        self.telemetry.command(cmd)
            except ConnectionError as e:
                reply = f"ERR {e}"
        await self._broadcast({"t": "reply", "cmd": cmd, "reply": reply})

    async def _run(self, ids):
        if self.test_lock.locked():
            await self._broadcast({"t": "reply", "cmd": "test", "reply": "ERR a test is already running"})
            return
        async with self.test_lock:
            for tc in ids:
                self.running = tc
                await self._broadcast({"t": "test", "id": tc, "state": "running"})
                res, why, secs = await run_one(tc, self.csms, self.ctrl)
                self.results[tc] = {"state": res, "reason": why, "secs": round(secs)}
                await self._broadcast({"t": "test", "id": tc, **self.results[tc]})
            self.running = None

    async def _ocpp(self, msg):
        action, payload = msg.get("action"), msg.get("payload")
        allowed = {"RemoteStartTransaction", "RemoteStopTransaction", "ReserveNow", "CancelReservation",
                   "TriggerMessage", "GetLocalListVersion", "SendLocalList", "ClearCache", "GetConfiguration", "ChangeConfiguration"}
        if not isinstance(action, str) or action not in allowed or not isinstance(payload, dict):
            return
        if self.running is not None:
            reply = "ERR a test is running"
        else:
            async with self.test_lock:
                try:
                    if not self.csms.conns:
                        raise ConnectionError("충전기가 CSMS에 연결되지 않았습니다")
                    conn = next(reversed(self.csms.conns.values()))
                    result = await conn.call(action, payload, timeout=15)
                    reply = json.dumps(result, ensure_ascii=False)
                except Exception as exc:
                    reply = f"ERR {exc}"
        await self._broadcast({"t": "reply", "cmd": action, "reply": reply})
