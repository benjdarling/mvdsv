#!/usr/bin/env python3
"""Cold-start physical E1M1 EvoBot execution harness."""

from __future__ import annotations

import argparse
import json
import math
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

from mvdsv_route_sweep import rcon, wait_until_ready


STATUS_RE = re.compile(r"EVOBOT_EXEC_JSON (\{.*\})")
HISTORY_RE = re.compile(r"EVOBOT_EXEC_HISTORY_JSON (\{.*\})")
MAP_RE = re.compile(r"EVOBOT_MAP\s+(\S+)")


def command_line(exe: Path, basedir: Path, port: int, password: str) -> list[str]:
    return [
        str(exe), "-d", "-noerrormsgbox", "-basedir", str(basedir),
        "-game", "evosp", "-progtype", "0", "-port", str(port),
        "+set", "sv_progsname", "spprogs", "+set", "sv_cheats", "1",
        "+set", "deathmatch", "0", "+set", "coop", "1",
        "+set", "rcon_password", password, "+set", "sv_crypt_rcon", "0",
        "+map", "e1m1",
    ]


def current_map(port: int, password: str) -> str:
    match = MAP_RE.search(rcon(port, password, "evobot_exec_map", timeout=1.0))
    return match.group(1).lower() if match else ""


def parse_status(text: str) -> dict[str, object] | None:
    match = STATUS_RE.search(text)
    return json.loads(match.group(1)) if match else None


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def run_once(exe: Path, basedir: Path, root: Path, run_number: int,
             port: int, timeout: float, generate: bool,
             capture_history: bool) -> dict[str, object]:
    password = f"evobotexec{port}"
    run_stem = f"run-{run_number:03d}"
    jsonl_path = root / f"{run_stem}.jsonl"
    summary_path = root / f"{run_stem}-summary.json"
    history_path = root / f"{run_stem}-failure-history.jsonl"
    frame_history_path = root / f"{run_stem}-frame-history.jsonl"
    failure_nav_path = root / f"{run_stem}-failure-navigation.txt"
    server_log = root / f"{run_stem}-server.log"
    for stale_path in (
        jsonl_path,
        summary_path,
        history_path,
        frame_history_path,
        failure_nav_path,
        server_log,
        root / f"{run_stem}-initial-route.txt",
        root / f"{run_stem}-initial-plan.txt",
    ):
        stale_path.unlink(missing_ok=True)
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with server_log.open("wb") as log:
        process = subprocess.Popen(
            command_line(exe, basedir, port, password), stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT, creationflags=creation_flags,
        )
    started = time.monotonic()
    records: list[dict[str, object]] = []
    route_text = ""
    plan_text = ""
    result = "FAIL"
    reason = "unknown"
    final_map = ""
    event_sequence = -1
    distance = 0.0
    previous_origin: list[float] | None = None
    actual_areas: list[int] = []
    monster_isolation_verified = False
    try:
        wait_until_ready(process, port, password)
        final_map = current_map(port, password)
        if final_map != "e1m1":
            raise RuntimeError(f"expected e1m1, got {final_map or '<unknown>'}")
        nav_output = rcon(port, password,
                          "evobot_nav_generate" if generate else "evobot_nav_load",
                          timeout=180.0)
        if "failed" in nav_output.lower() or "no navigation" in nav_output.lower():
            raise RuntimeError(f"navigation setup failed: {nav_output.strip()}")
        validation = rcon(port, password, "evobot_nav_reach_validate", timeout=30.0)
        if "failed" in validation.lower() or "validation: ok" not in validation.lower():
            raise RuntimeError(f"reachability validation failed: {validation.strip()}")
        # Generation writes the authoritative graph.  Loading an older graph can
        # perform a deterministic nav-layer migration (for example, PM-proven
        # no-button JUMPs becoming WALKs); persist that before any bot is added so
        # execution never owns or repeats reachability classification.
        if generate or "normalized" in nav_output.lower():
            save_output = rcon(port, password, "evobot_nav_save", timeout=30.0)
            if "saved:" not in save_output.lower():
                raise RuntimeError(f"navigation save failed: {save_output.strip()}")
        add_output = rcon(port, password, "evobot_add e1m1bot", timeout=2.0)
        if "EvoBot added" not in add_output:
            raise RuntimeError(f"bot creation failed: {add_output.strip()}")
        time.sleep(0.25)
        route_output = rcon(port, password, "evobot_nav_route_exit", timeout=5.0)
        if ("result: reachable" not in route_output and
                "result: conditional" not in route_output and
                "result: blocked" not in route_output):
            raise RuntimeError(f"exit route unavailable: {route_output.strip()}")
        route_text = rcon(port, password, "evobot_nav_route_dump", timeout=5.0)
        interactor_text = "\n".join(
            rcon(port, password, f"evobot_nav_interactor {interactor_id}", timeout=2.0)
            for interactor_id in
            (3, 4, 8, 9, 13, 14, 15, 16, 21, 22, 23, 24,
             26, 27, 28, 39, 40, 44, 49))
        (root / f"{run_stem}-initial-route.txt").write_text(
            route_output + "\n" + route_text + "\n" + interactor_text,
            encoding="utf-8")
        plan_path = root / f"{run_stem}-initial-plan.txt"
        plan_status = rcon(port, password, "evobot_nav_plan_exit", timeout=180.0)
        plan_text = rcon(port, password, "evobot_nav_plan_dump", timeout=5.0)
        plan_path.write_text(plan_status + "\n" + plan_text, encoding="utf-8")
        start_output = rcon(port, password, "evobot_exec_start e1m1bot", timeout=2.0)
        if "execution started" not in start_output:
            raise RuntimeError(f"executor failed to start: {start_output.strip()}")
        started = time.monotonic()
        jsonl = jsonl_path.open("w", encoding="utf-8")
        try:
            jsonl.write(json.dumps({"event": "BOT_SPAWNED", "map": "e1m1"}) + "\n")
            next_map_poll = 0.0
            while time.monotonic() - started < timeout:
                if process.poll() is not None:
                    reason = f"server exited with code {process.returncode}"
                    break
                now = time.monotonic()
                if now >= next_map_poll:
                    try:
                        observed_map = current_map(port, password)
                        if observed_map:
                            final_map = observed_map
                    except (TimeoutError, socket.timeout, OSError):
                        next_map_poll = now + 0.4
                        time.sleep(0.1)
                        continue
                    next_map_poll = now + 0.4
                    if final_map != "e1m1":
                        if final_map == "e1m2":
                            result, reason = "PASS", "physical changelevel to e1m2"
                            record = {"event": "MAP_CHANGED", "from": "e1m1",
                                      "to": "e1m2", "elapsed": now - started}
                            records.append(record)
                            jsonl.write(json.dumps(record) + "\n")
                            jsonl.write(json.dumps({"event": "TEST_COMPLETE",
                                                    "result": "PASS"}) + "\n")
                        else:
                            reason = f"unexpected map change to {final_map or '<unknown>'}"
                        break
                try:
                    snapshot = parse_status(rcon(port, password,
                                                 "evobot_exec_status e1m1bot",
                                                 timeout=1.0))
                except (TimeoutError, socket.timeout, OSError):
                    time.sleep(0.1)
                    continue
                if snapshot:
                    origin = snapshot.get("origin")
                    if isinstance(origin, list) and len(origin) == 3:
                        if previous_origin:
                            distance += math.dist(previous_origin, origin)
                        previous_origin = [float(value) for value in origin]
                    area = int(snapshot.get("area", 0))
                    if area and (not actual_areas or actual_areas[-1] != area):
                        actual_areas.append(area)
                    monster_isolation_verified |= bool(snapshot.get("test_notarget"))
                    sequence = int(snapshot.get("event_sequence", 0))
                    if sequence != event_sequence:
                        event_sequence = sequence
                    records.append(snapshot)
                    jsonl.write(json.dumps(snapshot, separators=(",", ":")) + "\n")
                    jsonl.flush()
                    # Quake's end-of-level trigger enters intermission on the same map;
                    # it does not necessarily issue a physical changelevel during this
                    # test window. E1M1's fixed intermission camera is far outside the
                    # playable exit volume and is therefore unambiguous completion proof.
                    if (isinstance(origin, list) and len(origin) == 3 and
                            float(origin[0]) < -150.0 and float(origin[1]) > 2600.0 and
                            float(origin[2]) > 128.0):
                        result, reason = "PASS", "physical E1M1 exit to intermission"
                        record = {"event": "INTERMISSION_REACHED",
                                  "elapsed": now - started, "origin": origin}
                        records.append(record)
                        jsonl.write(json.dumps(record, separators=(",", ":")) + "\n")
                        jsonl.write(json.dumps({"event": "TEST_COMPLETE",
                                                "result": "PASS"}) + "\n")
                        break
                    if (snapshot.get("movement_disabled") and
                            snapshot.get("state") == "VERIFY_TRANSITION" and
                            int(snapshot.get("route_index", 0)) >=
                            int(snapshot.get("route_length", 0))):
                        result, reason = "PASS", "physical E1M1 exit entered intermission"
                        record = {"event": "INTERMISSION_REACHED",
                                  "elapsed": now - started, "origin": origin}
                        records.append(record)
                        jsonl.write(json.dumps(record, separators=(",", ":")) + "\n")
                        jsonl.write(json.dumps({"event": "TEST_COMPLETE",
                                                "result": "PASS"}) + "\n")
                        break
                    if snapshot.get("state") == "FAILED":
                        reason = f"executor failed: {snapshot.get('event')} at step " \
                                 f"{int(snapshot.get('route_index', 0)) + 1}"
                        break
                time.sleep(0.1)
            else:
                reason = f"execution timeout after {timeout:.1f}s"
        finally:
            jsonl.close()
        if capture_history:
            try:
                history = rcon(port, password, "evobot_exec_history e1m1bot",
                               timeout=10.0)
                with frame_history_path.open("w", encoding="utf-8") as output:
                    for match in HISTORY_RE.finditer(history):
                        output.write(match.group(1) + "\n")
            except Exception:
                pass
        if result != "PASS":
            try:
                history = rcon(port, password, "evobot_exec_history e1m1bot",
                               timeout=5.0)
                with history_path.open("w", encoding="utf-8") as output:
                    for match in HISTORY_RE.finditer(history):
                        output.write(match.group(1) + "\n")
            except Exception:
                pass
            try:
                failure_route = rcon(port, password, "evobot_nav_route_exit",
                                     timeout=5.0)
                failure_route_dump = rcon(port, password, "evobot_nav_route_dump",
                                          timeout=5.0)
                failure_plan = rcon(port, password, "evobot_nav_plan_exit",
                                    timeout=180.0)
                failure_plan_dump = rcon(port, password, "evobot_nav_plan_dump",
                                         timeout=5.0)
                failure_interactors = "\n".join(
                    rcon(port, password,
                         f"evobot_nav_interactor {interactor_id}", timeout=2.0)
                    for interactor_id in
                    (3, 4, 8, 9, 13, 14, 15, 16, 21, 22, 23, 24,
                     26, 27, 28, 39, 40, 44, 49))
                failure_nav_path.write_text(
                    failure_route + "\n" + failure_route_dump + "\n" +
                    failure_plan + "\n" + failure_plan_dump + "\n" +
                    failure_interactors,
                    encoding="utf-8")
            except Exception:
                pass
    except Exception as error:
        reason = f"{type(error).__name__}: {error}"
    finally:
        if process.poll() is None:
            try:
                rcon(port, password, "quit", timeout=0.5)
            except Exception:
                pass
        try:
            process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
    last = next((record for record in reversed(records)
                 if "reachabilities_completed" in record), {})
    route_steps = [line for line in route_text.splitlines() if line.startswith("step ")]
    planned_ids = [int(match.group(1)) for line in route_steps
                   if (match := re.match(r"step \d+ reach (\d+)", line))]
    travel_types: dict[str, int] = {}
    for line in route_steps:
        match = re.search(r" type (\S+)", line)
        if match:
            travel_types[match.group(1)] = travel_types.get(match.group(1), 0) + 1
    physical_elapsed = float(last.get("total_elapsed", time.monotonic() - started))
    summary = {
        "run": run_number, "result": result, "reason": reason,
        "completion_time": round(physical_elapsed, 3),
        "final_map": final_map, "route_steps": len(route_steps),
        "planned_reachabilities": planned_ids, "travel_types": travel_types,
        "reachabilities_completed": int(last.get("reachabilities_completed", 0)),
        "replans": int(last.get("replans", 0)),
        "stuck_detections": int(last.get("stuck_detections", 0)),
        "traversal_retries": int(last.get("traversal_retries", 0)),
        "jumps_attempted": int(last.get("jumps_attempted", 0)),
        "jumps_succeeded": int(last.get("jumps_succeeded", 0)),
        "distance_travelled": round(distance, 3), "actual_areas": actual_areas,
        "monster_isolation": "bot-only FL_NOTARGET",
        "monster_isolation_verified": monster_isolation_verified,
        "telemetry": str(jsonl_path),
        "initial_route": str(root / f"{run_stem}-initial-route.txt"),
        "initial_plan": str(root / f"{run_stem}-initial-plan.txt"),
        "failure_history": str(history_path) if history_path.exists() else None,
        "frame_history": str(frame_history_path)
        if frame_history_path.exists() else None,
        "failure_navigation": str(failure_nav_path)
        if failure_nav_path.exists() else None,
    }
    write_json(summary_path, summary)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--base-port", type=int, default=27720)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--load-nav", action="store_true")
    parser.add_argument("--capture-history", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    summaries = []
    for index in range(args.runs):
        print(f"[{index + 1}/{args.runs}] cold-start e1m1", flush=True)
        summary = run_once(args.exe.resolve(), args.basedir.resolve(),
                           args.output.resolve(), index + 1,
                           args.base_port + index, args.timeout,
                           not args.load_nav, args.capture_history)
        summaries.append(summary)
        print(f"  {summary['result']}: {summary['reason']}", flush=True)
        write_json(args.output / "runs-summary.json", summaries)
        if summary["result"] != "PASS":
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
