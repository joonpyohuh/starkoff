"""SQLite demo observations. UI timings are operator-generated, not field statistics."""
import datetime as dt
import json
import sqlite3
import time
from pathlib import Path


class Telemetry:
    def __init__(self, path=None):
        path = Path(path) if path else Path(__file__).resolve().parents[1] / "results" / "telemetry.sqlite3"
        path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS events (at TEXT, direction TEXT, action TEXT, raw TEXT);
            CREATE TABLE IF NOT EXISTS connections (at TEXT, seconds REAL);
        """)
        self.card_at = None
        self.plugged = False

    def event(self, ev):
        self.db.execute("INSERT INTO events VALUES (?, ?, ?, ?)", (ev.wall, ev.direction, ev.action, ev.raw))
        self.db.commit()

    def command(self, cmd):
        parts = cmd.split()
        if parts[:2] == ["card", "1"] and not self.plugged:
            self.card_at = time.monotonic()
        elif parts == ["plug", "1"]:
            if not self.plugged and self.card_at is not None:
                self.db.execute("INSERT INTO connections VALUES (?, ?)",
                                (dt.datetime.now(dt.timezone.utc).isoformat(), time.monotonic() - self.card_at))
                self.db.commit()
            self.plugged = True
            self.card_at = None
        elif parts == ["unplug", "1"]:
            self.plugged = False
            self.card_at = None
        elif parts[:1] in (["power"], ["reboot"]):
            self.card_at = None

    def stats(self):
        count, mean, maximum = self.db.execute("SELECT count(*), avg(seconds), max(seconds) FROM connections").fetchone()
        return {"count": count, "mean": mean, "max": maximum}

    def close(self):
        self.db.close()
