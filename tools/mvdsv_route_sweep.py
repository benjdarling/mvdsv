#!/usr/bin/env python3
"""Run isolated EvoBot navigation-to-exit checks through local mvdsv RCON."""

from __future__ import annotations

import argparse
from collections import Counter
import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_MAPS = [
    "start",
    *(f"e1m{i}" for i in range(1, 9)),
    *(f"e2m{i}" for i in range(1, 8)),
    *(f"e3m{i}" for i in range(1, 8)),
    *(f"e4m{i}" for i in range(1, 9)),
    "end",
]


def rcon(port: int, password: str, command: str, timeout: float = 120.0) -> str:
    request = b"\xff\xff\xff\xff" + f"rcon {password} {command}".encode() + b"\x00"
    chunks: list[str] = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(request, ("127.0.0.1", port))
        while True:
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                if chunks:
                    break
                raise
            if data.startswith(b"\xff\xff\xff\xff"):
                data = data[4:]
            if data[:1] in (b"n", b"l"):
                data = data[1:]
            chunks.append(data.rstrip(b"\x00").decode("latin-1", errors="replace"))
            sock.settimeout(0.2)
    return "".join(chunks).replace("\r", "")


def wait_until_ready(process: subprocess.Popen[bytes], port: int, password: str) -> str:
    deadline = time.monotonic() + 15.0
    last_error = "no response"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"mvdsv exited during startup with code {process.returncode}")
        try:
            return rcon(port, password, "status", timeout=0.5)
        except (TimeoutError, socket.timeout, OSError) as error:
            last_error = str(error)
            time.sleep(0.1)
    raise RuntimeError(f"mvdsv did not answer RCON: {last_error}")


def classify(route_output: str) -> str:
    match = re.search(r"^result:\s*(\S+)", route_output, re.MULTILINE | re.IGNORECASE)
    if match:
        return match.group(1).lower()
    if "no accessible level-exit areas found" in route_output.lower():
        return "no-exit"
    if "add an EvoBot or provide an explicit source area" in route_output:
        return "no-source"
    return "unknown"


def classify_plan(plan_output: str) -> str:
    match = re.search(r"^result:\s*(.+?)\s*$", plan_output, re.MULTILINE | re.IGNORECASE)
    return match.group(1).strip().lower() if match else "unknown"


def run_map(
    executable: Path,
    basedir: Path,
    map_name: str,
    port: int,
    password: str,
    verify_persistence: bool = False,
    plan: bool = False,
    debug_commands: list[str] | None = None,
    nav_snapshot_dir: Path | None = None,
) -> dict[str, object]:
    command = [
        str(executable),
        "-d",
        "-noerrormsgbox",
        "-basedir",
        str(basedir),
        "-game",
        "evosp",
        "-progtype",
        "0",
        "-port",
        str(port),
        "+set",
        "sv_progsname",
        "spprogs",
        "+set",
        "sv_cheats",
        "1",
        "+set",
        "deathmatch",
        "0",
        "+set",
        "coop",
        "1",
        "+exec",
        "evobot_route_sweep.cfg",
        "+map",
        map_name,
    ]
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creation_flags,
    )
    started = time.monotonic()
    outputs: dict[str, str] = {}
    metrics: dict[str, object] = {}
    plan_result = "not-run"
    try:
        outputs["startup"] = wait_until_ready(process, port, password)
        outputs["generate"] = rcon(port, password, "evobot_nav_generate")
        outputs["reach_status"] = rcon(port, password, "evobot_nav_reach_status")
        outputs["reach_validate"] = rcon(
            port, password, "evobot_nav_reach_validate"
        )
        outputs["add_bot"] = rcon(port, password, "evobot_add routebot")
        time.sleep(0.1)
        outputs["route"] = rcon(port, password, "evobot_nav_route_exit")
        result = classify(outputs["route"])
        if result == "no-source":
            time.sleep(1.0)
            outputs["route_retry"] = rcon(port, password, "evobot_nav_route_exit")
            result = classify(outputs["route_retry"])
        outputs["problem_report"] = rcon(
            port, password, "evobot_nav_problem_report"
        )
        outputs["frontier_report"] = rcon(
            port, password, "evobot_nav_frontier_report"
        )
        if plan:
            outputs["plan"] = rcon(port, password, "evobot_nav_plan_exit")
            plan_result = classify_plan(outputs["plan"])
        for index, debug_command in enumerate(debug_commands or []):
            outputs[f"debug_{index}"] = rcon(port, password, debug_command)
        outputs["route_validate"] = rcon(
            port, password, "evobot_nav_route_validate"
        )
        if verify_persistence:
            outputs["save"] = rcon(port, password, "evobot_nav_save")
            nav_path = basedir / "evosp" / "evobot" / "nav" / f"{map_name}.botnav"
            if nav_snapshot_dir is not None:
                nav_snapshot_dir.mkdir(parents=True, exist_ok=True)
                (nav_snapshot_dir / nav_path.name).write_bytes(nav_path.read_bytes())
            persisted = json.loads(nav_path.read_text(encoding="utf-8"))
            metrics["format_version"] = persisted.get("version")
            metrics["generation"] = persisted.get("generation", {})
            metrics["reachability_counts"] = dict(
                sorted(
                    Counter(
                        reachability.get("travel_type", "unknown")
                        for reachability in persisted.get("reachabilities", [])
                    ).items()
                )
            )
            metrics["area_count"] = len(persisted.get("areas", []))
            metrics["portal_count"] = len(persisted.get("portals", []))
            metrics["interactor_count"] = len(persisted.get("interactors", []))
            metrics["reachability_count"] = len(
                persisted.get("reachabilities", [])
            )
            push_ids = {
                interactor.get("id")
                for interactor in persisted.get("interactors", [])
                if interactor.get("classname") == "trigger_push"
            }
            metrics["push_interactors"] = [
                interactor
                for interactor in persisted.get("interactors", [])
                if interactor.get("id") in push_ids
            ]
            metrics["push_links"] = [
                reachability
                for reachability in persisted.get("reachabilities", [])
                if reachability.get("source_interactor") in push_ids
            ]
            metrics["unresolved_water_jumps"] = [
                reachability
                for reachability in persisted.get("reachabilities", [])
                if reachability.get("travel_type") == "unresolved_water_jump"
            ]
            mover_ids = {
                interactor.get("id")
                for interactor in persisted.get("interactors", [])
                if interactor.get("type") in {"platform", "train"}
            }
            metrics["mover_interactors"] = [
                interactor
                for interactor in persisted.get("interactors", [])
                if interactor.get("id") in mover_ids
            ]
            metrics["platform_links"] = [
                reachability
                for reachability in persisted.get("reachabilities", [])
                if reachability.get("travel_type") == "platform"
            ]
            outputs["clear"] = rcon(port, password, "evobot_nav_clear")
            outputs["load"] = rcon(port, password, "evobot_nav_load")
            outputs["loaded_reach_status"] = rcon(
                port, password, "evobot_nav_reach_status"
            )
            outputs["loaded_reach_validate"] = rcon(
                port, password, "evobot_nav_reach_validate"
            )
            outputs["loaded_route"] = rcon(
                port, password, "evobot_nav_route_exit"
            )
            outputs["loaded_route_validate"] = rcon(
                port, password, "evobot_nav_route_validate"
            )
        error = ""
    except Exception as exception:  # Preserve diagnostics and continue the sweep.
        result = "error"
        error = f"{type(exception).__name__}: {exception}"
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
    return {
        "map": map_name,
        "result": result,
        "plan_result": plan_result,
        "seconds": round(time.monotonic() - started, 3),
        "error": error,
        "outputs": outputs,
        "metrics": metrics,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--maps", nargs="*", default=DEFAULT_MAPS)
    parser.add_argument("--exclude", nargs="*", default=[])
    parser.add_argument("--base-port", type=int, default=27630)
    parser.add_argument("--verify-persistence", action="store_true")
    parser.add_argument("--plan", action="store_true")
    parser.add_argument("--debug-command", action="append", default=[])
    parser.add_argument("--nav-snapshot-dir", type=Path)
    args = parser.parse_args()

    maps = [name.lower() for name in args.maps if name.lower() not in args.exclude]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, object]] = []
    sweep_config = args.basedir.resolve() / "evosp" / "evobot_route_sweep.cfg"
    persistence_files = [
        args.basedir.resolve() / "evosp" / "evobot" / "nav" / f"{name}.botnav"
        for name in maps
    ] if args.verify_persistence else []
    if sweep_config.exists():
        parser.error(f"refusing to overwrite existing test config: {sweep_config}")
    persistence_backups = {
        path: path.read_bytes() for path in persistence_files if path.exists()
    }
    sweep_config.write_text(
        "rcon_password evobotsweep\nsv_crypt_rcon 0\n", encoding="ascii"
    )
    try:
        for index, map_name in enumerate(maps):
            print(f"[{index + 1}/{len(maps)}] {map_name}", flush=True)
            result = run_map(
                args.exe.resolve(),
                args.basedir.resolve(),
                map_name,
                args.base_port + index,
                "evobotsweep",
                args.verify_persistence,
                args.plan,
                args.debug_command,
                args.nav_snapshot_dir.resolve() if args.nav_snapshot_dir else None,
            )
            results.append(result)
            print(
                f"  {result['result']} ({result['seconds']:.3f}s)"
                + (f" - {result['error']}" if result["error"] else ""),
                flush=True,
            )
            args.output.write_text(json.dumps(results, indent=2), encoding="utf-8")
    finally:
        sweep_config.unlink(missing_ok=True)
        for path in persistence_files:
            if path in persistence_backups:
                path.write_bytes(persistence_backups[path])
            else:
                path.unlink(missing_ok=True)

    counts: dict[str, int] = {}
    for result in results:
        counts[str(result["result"])] = counts.get(str(result["result"]), 0) + 1
    print(json.dumps(counts, sort_keys=True))
    return 0 if all(
        result["result"] == "reachable" or result["plan_result"] == "complete"
        for result in results
    ) else 1


if __name__ == "__main__":
    sys.exit(main())
