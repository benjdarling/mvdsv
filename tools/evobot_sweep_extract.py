#!/usr/bin/env python3
"""Stream selected records from a large mvdsv_route_sweep JSON file."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def records(path: Path):
    buffered: list[str] = []
    in_record = False
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            if not in_record and line == "  {\n":
                in_record = True
                buffered = [line]
                continue
            if not in_record:
                continue
            buffered.append(line)
            if line in ("  },\n", "  }\n"):
                text = "".join(buffered).rstrip()
                if text.endswith(","):
                    text = text[:-1]
                yield json.loads(text)
                in_record = False


def plan_metric(text: str, label: str) -> int | None:
    match = re.search(rf"^{re.escape(label)}: (\d+)$", text, re.MULTILINE)
    return int(match.group(1)) if match else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=Path)
    parser.add_argument("maps", nargs="*")
    parser.add_argument("--outputs", action="store_true")
    args = parser.parse_args()
    wanted = set(args.maps)
    for record in records(args.path):
        if wanted and record.get("map") not in wanted:
            continue
        plan = record.get("outputs", {}).get("plan", "")
        summary = {
            "map": record.get("map"),
            "result": record.get("result"),
            "plan_result": record.get("plan_result"),
            "seconds": record.get("seconds"),
            "error": record.get("error"),
            "states": plan_metric(plan, "world states searched"),
            "duplicate_prunes": plan_metric(plan, "duplicate states pruned"),
            "budget_depth_prunes": plan_metric(plan, "budget/depth states pruned"),
            "maximum_dfs_depth": plan_metric(plan, "maximum active DFS depth"),
        }
        print(json.dumps(summary, sort_keys=True))
        if args.outputs:
            output_keys = ["route", "frontier_report", "plan", "route_validate"]
            output_keys.extend(sorted(
                key for key in record.get("outputs", {}) if key.startswith("debug_")
            ))
            for key in output_keys:
                print(f"--- {record.get('map')} {key} ---")
                print(record.get("outputs", {}).get(key, ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
