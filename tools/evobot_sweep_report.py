#!/usr/bin/env python3
"""Summarize staged EvoBot route-sweep JSON artifacts."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TRAVEL_TYPES = (
    "walk", "drop", "jump", "water jump", "teleport", "platform", "swim", "water entry",
    "water exit", "unresolved water jump",
)


def field(text: str, name: str) -> int:
    match = re.search(rf"^{re.escape(name)}:\s*(\d+)", text, re.MULTILINE)
    return int(match.group(1)) if match else 0


def problem(text: str) -> str:
    match = re.search(r"^problem type:\s*(.+)$", text, re.MULTILINE)
    if match:
        return match.group(1).strip()
    if "no implemented reachability closes" in text:
        return "frontier gap"
    return "none"


def summarize(path: Path) -> dict[str, object]:
    rows = json.loads(path.read_text(encoding="utf-8"))
    totals = {name: 0 for name in TRAVEL_TYPES}
    maps: dict[str, object] = {}
    validation_ok = 0
    route_validation_ok = 0
    persistence_ok = 0
    generation_rows: list[tuple[str, dict[str, object]]] = []
    for row in rows:
        outputs = row["outputs"]
        status = outputs.get("reach_status", "")
        counts = {name: field(status, name) for name in TRAVEL_TYPES}
        if row.get("metrics", {}).get("reachability_counts"):
            persisted = row["metrics"]["reachability_counts"]
            counts = {
                name: int(persisted.get(name.replace(" ", "_"), 0))
                for name in TRAVEL_TYPES
            }
        if row.get("metrics", {}).get("generation"):
            generation_rows.append((row["map"], row["metrics"]["generation"]))
        for name, count in counts.items():
            totals[name] += count
        reach_ok = "reachability validation: ok" in outputs.get(
            "reach_validate", ""
        ).lower()
        route_ok = "routing validation: ok" in outputs.get(
            "route_validate", ""
        ).lower()
        validation_ok += reach_ok
        route_validation_ok += route_ok
        loaded = bool(outputs.get("loaded_reach_status"))
        persisted_ok = (
            loaded
            and outputs.get("loaded_reach_status") == status
            and "reachability validation: ok" in outputs.get(
                "loaded_reach_validate", ""
            ).lower()
            and "routing validation: ok" in outputs.get(
                "loaded_route_validate", ""
            ).lower()
            and re.search(
                r"^result:\s*(\S+)", outputs.get("loaded_route", ""), re.MULTILINE
            ).group(1).lower() == row["result"]
        ) if loaded and re.search(
            r"^result:\s*(\S+)", outputs.get("loaded_route", ""), re.MULTILINE
        ) else False
        persistence_ok += persisted_ok
        maps[row["map"]] = {
            "result": row["result"],
            "seconds": row["seconds"],
            "counts": counts,
            "reachability_validation": reach_ok,
            "routing_validation": route_ok,
            "persistence": persisted_ok if loaded else None,
            "problem": problem(outputs.get("route", "")),
            "error": row.get("error", ""),
        }
    seconds = [float(row["seconds"]) for row in rows]
    generation = None
    if generation_rows:
        numeric_keys = sorted({
            key for _, values in generation_rows for key, value in values.items()
            if isinstance(value, (int, float))
        })
        generation = {
            "totals": {
                key: sum(float(values.get(key, 0)) for _, values in generation_rows)
                for key in numeric_keys
            },
            "maximums": {
                key: {
                    "value": max(float(values.get(key, 0)) for _, values in generation_rows),
                    "map": max(
                        generation_rows,
                        key=lambda item: float(item[1].get(key, 0)),
                    )[0],
                }
                for key in numeric_keys
            },
        }
    return {
        "artifact": str(path.resolve()),
        "map_count": len(rows),
        "reachable": sum(row["result"] == "reachable" for row in rows),
        "unreachable": sum(row["result"] == "unreachable" for row in rows),
        "errors": sum(bool(row.get("error")) for row in rows),
        "reachability_validation_ok": validation_ok,
        "routing_validation_ok": route_validation_ok,
        "persistence_ok": persistence_ok if any(
            row["outputs"].get("loaded_reach_status") for row in rows
        ) else None,
        "travel_totals": totals,
        "generation_metrics": generation,
        "timing": {
            "total_seconds": round(sum(seconds), 3),
            "average_seconds": round(sum(seconds) / len(seconds), 3),
            "maximum_seconds": max(seconds),
            "maximum_map": rows[seconds.index(max(seconds))]["map"],
        },
        "maps": maps,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("stages", nargs="+", help="NAME=path")
    args = parser.parse_args()
    stages = {}
    for item in args.stages:
        name, raw_path = item.split("=", 1)
        stages[name] = summarize(Path(raw_path))
    order = list(stages)
    contributions = {}
    regressions = []
    for index, name in enumerate(order):
        if index == 0:
            continue
        previous = stages[order[index - 1]]
        current = stages[name]
        contributions[name] = {
            key: current["travel_totals"][key] - previous["travel_totals"][key]
            for key in TRAVEL_TYPES
        }
        for map_name, prior in previous["maps"].items():
            after = current["maps"][map_name]
            if prior["result"] == "reachable" and after["result"] != "reachable":
                regressions.append({
                    "stage": name, "map": map_name,
                    "before": prior["result"], "after": after["result"],
                })
    final = stages[order[-1]]
    report = {
        "excluded_maps": ["e1m8"],
        "stages": stages,
        "stage_contributions": contributions,
        "routing_regressions": regressions,
        "final_reachable_maps": [
            name for name, row in final["maps"].items()
            if row["result"] == "reachable"
        ],
        "final_unreachable_maps": [
            name for name, row in final["maps"].items()
            if row["result"] != "reachable"
        ],
    }
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({
        "output": str(args.output.resolve()),
        "final_reachable": len(report["final_reachable_maps"]),
        "regressions": len(regressions),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
