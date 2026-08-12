#!/usr/bin/env python3
"""Launch E1M1 and capture an exact human client movement reference."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from mvdsv_route_sweep import rcon, wait_until_ready
from run_evobot_e1m1_execution_test import command_line, current_map


def copy_capture(basedir: Path, output: Path, label: str) -> Path | None:
    source = basedir / "evosp" / "evobot" / "captures" / f"{label}.jsonl"
    if not source.is_file():
        return None
    destination = output / f"{label}.jsonl"
    shutil.copy2(source, destination)
    return destination


def copy_demo(basedir: Path, output: Path, label: str) -> Path | None:
    candidates = sorted(
        basedir.glob(f"evosp/**/{label}*.mvd"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        return None
    destination = output / candidates[0].name
    shutil.copy2(candidates[0], destination)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--ezquake", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--player-name", default="evobot-human")
    parser.add_argument("--label")
    parser.add_argument("--port", type=int, default=27720)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--launch-client", action="store_true")
    args = parser.parse_args()

    exe = args.exe.resolve()
    basedir = args.basedir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    label = args.label or datetime.now().strftime("e1m1-human-%Y%m%d-%H%M%S")
    password = f"evobothuman{args.port}"
    server_log = output / f"{label}-server.log"
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    with server_log.open("wb") as log:
        server = subprocess.Popen(
            command_line(exe, basedir, args.port, password),
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            creationflags=creation_flags,
        )

    client: subprocess.Popen[bytes] | None = None
    recording = False
    completed = False
    error: str | None = None
    started = time.monotonic()
    try:
        wait_until_ready(server, args.port, password)
        nav_output = rcon(args.port, password, "evobot_nav_load", timeout=30.0)
        if "failed" in nav_output.lower() or "no navigation" in nav_output.lower():
            raise RuntimeError(f"navigation load failed: {nav_output.strip()}")
        validation = rcon(
            args.port, password, "evobot_nav_reach_validate", timeout=30.0
        )
        if "validation: ok" not in validation.lower():
            raise RuntimeError(f"navigation validation failed: {validation.strip()}")

        if args.launch_client:
            if not args.ezquake or not args.ezquake.is_file():
                raise RuntimeError("--launch-client requires a valid --ezquake path")
            client = subprocess.Popen([
                str(args.ezquake.resolve()),
                "-basedir", str(basedir),
                "-game", "evosp",
                "+set", "vid_fullscreen", "0",
                "+name", args.player_name,
                "+connect", f"127.0.0.1:{args.port}",
            ])
            print(f"ezQuake launched as {args.player_name!r}.", flush=True)
        else:
            print(
                f"Connect to 127.0.0.1:{args.port} with player name "
                f"{args.player_name!r}.",
                flush=True,
            )

        print("Waiting for the player to spawn...", flush=True)
        while time.monotonic() - started < args.timeout:
            response = rcon(
                args.port,
                password,
                f'evobot_human_record_start "{args.player_name}" {label}',
                timeout=2.0,
            )
            if "capture started" in response.lower():
                recording = True
                break
            if "unknown command" in response.lower():
                raise RuntimeError("mvdsv does not contain the human recorder commands")
            if server.poll() is not None:
                raise RuntimeError("mvdsv exited while waiting for the player")
            time.sleep(0.5)
        if not recording:
            raise RuntimeError("timed out waiting for the player to spawn")

        rcon(args.port, password, f"easyrecord {label}", timeout=2.0)
        print(
            "Recording. Complete E1M1 normally, then press attack at the "
            "intermission so the server reaches E1M2.",
            flush=True,
        )
        while time.monotonic() - started < args.timeout:
            if server.poll() is not None:
                raise RuntimeError("mvdsv exited during capture")
            map_name = current_map(args.port, password)
            if map_name == "e1m2":
                completed = True
                print("E1M2 reached; capture complete.", flush=True)
                break
            time.sleep(0.25)
        if not completed:
            raise RuntimeError("capture timed out before E1M2")
    except KeyboardInterrupt:
        print("Capture cancelled.", flush=True)
        error = "capture cancelled"
    except Exception as exc:
        error = str(exc)
        print(f"Capture failed: {error}", flush=True)
    finally:
        if server.poll() is None:
            if recording:
                try:
                    rcon(args.port, password, "evobot_human_record_stop", timeout=1.0)
                except Exception:
                    pass
            try:
                rcon(args.port, password, "sv_demostop", timeout=1.0)
            except Exception:
                pass
            try:
                rcon(args.port, password, "quit", timeout=1.0)
            except Exception:
                pass
            try:
                server.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                server.terminate()
                server.wait(timeout=5.0)

    capture = copy_capture(basedir, output, label)
    demo = copy_demo(basedir, output, label)
    if capture:
        print(f"Movement capture: {capture}", flush=True)
    else:
        print("Movement capture file was not found.", flush=True)
    if demo:
        print(f"Visual MVD: {demo}", flush=True)
    if error or not completed or not capture:
        return 1
    print("Send the JSONL capture back for human-vs-bot rule analysis.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
