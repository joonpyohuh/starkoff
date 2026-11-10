"""
Lab command line.

  python -m lab.run --list
  python -m lab.run --qemu --test TC_001_CS          start firmware in QEMU (fresh), run a test
  python -m lab.run --qemu --all --markdown          every test, print a README table
  python -m lab.run --csms                           only run the mock CSMS (Ctrl-C to stop)

Without --qemu the firmware must already be running (scripts/run_qemu.sh) and
pointing at this CSMS (default ws://10.0.2.2:9000/ocpp).
"""
import argparse
import asyncio
import os
import signal
import subprocess
import sys
import time

from lab.csms.csms import CSMS
from lab.ctrl import Ctrl
from lab.tests import core, extra  # noqa: F401  (registers test cases)
from lab.tests.framework import TESTS, run_one

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def start_qemu(log_path):
    log = open(log_path, "w")
    print(f"[lab] starting firmware in QEMU (log: {os.path.relpath(log_path, REPO)})", flush=True)
    return subprocess.Popen([os.path.join(REPO, "scripts", "run_qemu.sh"), "--fresh"],
                            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)


def stop_qemu(proc):
    if proc and proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)


def markdown(results):
    lines = ["| Test case | Name | Result | Details |", "|---|---|---|---|"]
    for tc, name, res, why, secs in results:
        why = why.replace("|", "\\|")
        lines.append(f"| {tc} | {name} | {res} | {why} ({secs:.0f} s) |")
    return "\n".join(lines)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list test cases")
    ap.add_argument("--test", action="append", default=[], help="test case id (repeatable)")
    ap.add_argument("--all", action="store_true", help="run every test case")
    ap.add_argument("--qemu", action="store_true", help="start the firmware in QEMU first")
    ap.add_argument("--csms", action="store_true", help="only run the mock CSMS")
    ap.add_argument("--markdown", action="store_true", help="print results as a Markdown table")
    ap.add_argument("--port", type=int, default=9000, help="CSMS WebSocket port")
    ap.add_argument("--ctrl-port", type=int, default=7000, help="firmware control port")
    ap.add_argument("--quiet", action="store_true", help="do not print every OCPP message")
    a = ap.parse_args()

    if a.list:
        for tc, (name, _) in TESTS.items():
            print(f"{tc:<12} {name}")
        return 0

    csms = CSMS(port=a.port, verbose=not a.quiet)
    await csms.start()
    print(f"[lab] mock CSMS listening on ws://0.0.0.0:{a.port}/ocpp/<id>", flush=True)
    os.makedirs(os.path.join(REPO, "lab", "results"), exist_ok=True)
    qemu = start_qemu(os.path.join(REPO, "lab", "results", "qemu.log")) if a.qemu else None
    ctrl = Ctrl(port=a.ctrl_port)
    try:
        if a.csms:
            await asyncio.Event().wait()
        ids = list(TESTS) if a.all else a.test
        unknown = [t for t in ids if t not in TESTS]
        if unknown or not ids:
            print(f"unknown or no test ids {unknown}; use --list", file=sys.stderr)
            return 2
        results = []
        for tc in ids:
            print(f"\n[lab] ===== {tc} {TESTS[tc][0]} =====", flush=True)
            res, why, secs = await run_one(tc, csms, ctrl)
            mark = {"PASS": "통과!", "FAIL": "실패!"}.get(res, res)
            print(f"[lab] {tc}: {mark} {('이유: ' + why) if res != 'PASS' else why}", flush=True)
            results.append((tc, TESTS[tc][0], res, why, secs))
        print("\n[lab] ===== summary =====")
        for tc, name, res, why, secs in results:
            print(f"{tc:<12} {res:<5} {secs:5.0f}s  {why}")
        if a.markdown:
            print("\n" + markdown(results))
        return 0 if all(r[2] == "PASS" for r in results) else 1
    finally:
        ctrl.close()
        stop_qemu(qemu)
        await csms.stop()


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        pass
