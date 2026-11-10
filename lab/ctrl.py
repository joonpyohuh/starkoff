"""
Client for the firmware's TCP control port (simulated hardware).
Keeps one connection open (Zephyr TCP does not support half-close, see DECISIONS D14).
Reconnects automatically, e.g. after the firmware rebooted.
"""
import asyncio


class Ctrl:
    def __init__(self, host="127.0.0.1", port=7000):
        self.host, self.port = host, port
        self.reader = self.writer = None
        self.lock = asyncio.Lock()

    async def _connect(self, timeout):
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            try:
                self.reader, self.writer = await asyncio.wait_for(
                    asyncio.open_connection(self.host, self.port), 2)
                return
            except (OSError, asyncio.TimeoutError):
                if asyncio.get_running_loop().time() > deadline:
                    raise ConnectionError(f"control port {self.host}:{self.port} not reachable")
                await asyncio.sleep(0.3)

    def _drop(self):
        if self.writer:
            self.writer.close()
        self.reader = self.writer = None

    async def cmd(self, line, timeout=5, connect_timeout=20):
        """Send one command, return the reply line (e.g. 'OK', 'ERR ...')."""
        async with self.lock:
            for attempt in (1, 2):
                if not self.writer:
                    await self._connect(connect_timeout)
                try:
                    self.writer.write((line + "\n").encode())
                    await self.writer.drain()
                    reply = await asyncio.wait_for(self.reader.readline(), timeout)
                    if reply:
                        return reply.decode().strip()
                except (OSError, asyncio.TimeoutError):
                    pass
                self._drop()   # stale connection (firmware rebooted): retry once
            raise ConnectionError(f"no reply to '{line}'")

    async def status(self):
        """Parse the 'status' line into a dict."""
        reply = await self.cmd("status")
        return dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)

    def close(self):
        self._drop()
