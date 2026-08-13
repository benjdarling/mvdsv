#!/usr/bin/env python3
"""Poll live EvoBot telemetry, save one spectated run, then render its grid map."""

from __future__ import annotations

import argparse
import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

from mvdsv_route_sweep import rcon


STATUS_RE = re.compile(r"EVOBOT_EXEC_JSON (\{.*\})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--bot", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--nav-obj", type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    active = False
    samples = 0
    with args.output.open("w", encoding="utf-8") as destination:
        while time.monotonic() < deadline:
            try:
                response = rcon(args.port, args.password,
                                f"evobot_exec_status {args.bot}", timeout=0.5)
            except (OSError, TimeoutError, socket.timeout):
                if active:
                    break
                time.sleep(0.05)
                continue
            match = STATUS_RE.search(response)
            if not match:
                time.sleep(0.05)
                continue
            snapshot = json.loads(match.group(1))
            total = float(snapshot.get("total_elapsed", 0.0))
            state = str(snapshot.get("state", ""))
            if total > 0.0 and state not in ("IDLE", ""):
                active = True
                destination.write(json.dumps(snapshot, separators=(",", ":")) + "\n")
                destination.flush()
                samples += 1
            if active and (state == "FAILED" or
                           (snapshot.get("movement_disabled") and
                            state == "VERIFY_TRANSITION")):
                break
            time.sleep(0.10)

    if not samples:
        return 1
    renderer = Path(__file__).with_name("render_evobot_run.py")
    command = [sys.executable, str(renderer), str(args.output),
               "--label", args.bot]
    if args.nav_obj and args.nav_obj.exists():
        command += ["--nav-obj", str(args.nav_obj)]
    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
