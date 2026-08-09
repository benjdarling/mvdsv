#!/usr/bin/env python3
"""Build reproducible artifacts for the targeted campaign-completion push."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TARGETS = ("e4m8", "e3m5", "e2m5")


def load(path: Path) -> list[dict[str, object]]:
    return json.loads(path.read_text(encoding="utf-8"))


def reachable_areas(row: dict[str, object]) -> int:
    outputs = row["outputs"]
    assert isinstance(outputs, dict)
    match = re.search(r"reachable routing areas: (\d+)", str(outputs.get("route", "")))
    return int(match.group(1)) if match else 0


def complete(row: dict[str, object]) -> bool:
    return row.get("result") == "reachable" or row.get("plan_result") == "complete"


def aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    generations = [row["metrics"]["generation"] for row in rows]
    by_map = {str(row["map"]): row for row in rows}
    return {
        "candidate_ledges": sum(int(item.get("drop_candidate_ledges", 0)) for item in generations),
        "tested_launch_points": sum(int(item.get("drop_tested_launch_points", 0)) for item in generations),
        "pm_simulations": sum(int(item.get("drop_pm_simulations", 0)) for item in generations),
        "validated_candidate_drops": sum(int(item.get("drop_validated_candidates", 0)) for item in generations),
        "deduplicated_drop_links": sum(int(item.get("drop_deduplicated_links", 0)) for item in generations),
        "total_drop_links": sum(int(row["metrics"]["reachability_counts"].get("drop", 0)) for row in rows),
        "aggregate_reachable_areas": sum(reachable_areas(row) for row in rows),
        "target_reachable_areas": {
            name: reachable_areas(by_map[name]) for name in TARGETS
        },
        "complete_campaign_maps": sum(complete(row) for row in rows),
        "drop_generation_seconds": sum(float(item.get("edge_drop_seconds", 0)) for item in generations),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    before = aggregate(load(args.before))
    after = aggregate(load(args.after))
    numeric_change = {
        key: after[key] - before[key]
        for key in (
            "candidate_ledges",
            "tested_launch_points",
            "pm_simulations",
            "validated_candidate_drops",
            "deduplicated_drop_links",
            "total_drop_links",
            "aggregate_reachable_areas",
            "complete_campaign_maps",
            "drop_generation_seconds",
        )
    }
    target_change = {
        name: after["target_reachable_areas"][name] -
        before["target_reachable_areas"][name]
        for name in TARGETS
    }
    result = {
        "midpoint_current": before,
        "sample_20_50_80": after,
        "change": {
            **numeric_change,
            "target_reachable_areas": target_change,
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["change"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
