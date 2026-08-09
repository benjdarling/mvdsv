#!/usr/bin/env python3
"""Extract a concise, non-mutating blocker-chain audit from a campaign sweep."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def field(text: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}:\s*(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else ""


def classify(reason: str, dependencies: list[str]) -> str:
    lowered = f"{reason} {' '.join(dependencies)}".lower()
    if "water-jump" in lowered or "liquid portal" in lowered:
        return "water transition geometry"
    if "jump-up" in lowered:
        height = re.search(r"\(([0-9.]+) units\)", lowered)
        if height and float(height.group(1)) > 64.0:
            return "gameplay/mover or physically impossible vertical frontier"
        return "jump-up/contextual landing geometry"
    if "platform/" in lowered:
        return "platform boarding/state connectivity"
    if "train/" in lowered:
        return "train/mover state connectivity"
    if "--shoot-->" in lowered or "activation shoot" in lowered:
        return "shoot activation (conditional planning)"
    if "door/" in lowered or "button/" in lowered or "trigger/" in lowered:
        return "activation dependency/world-state planning"
    if "frontier gap" in lowered:
        return "unsupported physical gap or finale-specific transition"
    return "unknown/other"


def audit_row(row: dict[str, object]) -> dict[str, object]:
    outputs = row["outputs"]
    dump = outputs.get("debug_0", "")
    route = outputs.get("route", "")
    blocker_lines = re.findall(r"^blocker \d+: (.*)$", dump, re.MULTILINE)
    dependency_lines = re.findall(
        r"^dependency depth 0: interactor \d+ (.*)$", dump, re.MULTILINE
    )
    frontier = field(dump, "physical frontier") or field(route, "frontier reason")
    chain = dependency_lines[:]
    if frontier:
        chain.append(frontier)
    if not chain:
        chain.append(field(route, "frontier reason") or "no specific blocker isolated")
    deepest = field(dump, "deepest proven point")
    if ";" in deepest:
        deepest = deepest.split(";", 1)[0]
    return {
        "map": row["map"],
        "status": row["result"],
        "blocker_chain": chain,
        "route_blocker_steps": blocker_lines,
        "deepest_proven_point": deepest or "current reachable component",
        "main_missing_capability": classify(frontier, dependency_lines),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()
    raw = json.loads(args.input.read_text(encoding="utf-8"))
    rows = [audit_row(row) for row in raw if row.get("result") != "reachable"]
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    lines = [
        "# Deep blocker audit",
        "",
        "This is diagnostic hypothetical progression only; no assumed blocker is marked solved.",
        "",
        "| Map | Blocker 1 | Blocker 2 | Blocker 3+ | Deepest proven point | Main missing capability |",
        "|---|---|---|---|---|---|",
    ]
    for row in rows:
        chain = list(row["blocker_chain"])
        cells = chain[:2] + ["; ".join(chain[2:])]
        cells += [""] * (3 - len(cells))
        escaped = [str(value).replace("|", "\\|") for value in cells]
        lines.append(
            f"| {row['map']} | {escaped[0]} | {escaped[1]} | {escaped[2]} | "
            f"{row['deepest_proven_point']} | {row['main_missing_capability']} |"
        )
    args.markdown.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"maps": len(rows), "json": str(args.json), "markdown": str(args.markdown)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
