#!/usr/bin/env python3
"""Build the final report for the remaining-campaign navigation milestone."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path("build/nav-remaining-campaign")

BLOCKERS = {
    "e1m1": "Dynamic route exists, but no non-circular path to trigger 40/door 39 is present; the planner cannot prove the 3/39/21 sequence.",
    "e1m3": "The closest frontier is an 80-unit rise from train-supported area 903; PM rejects it and train 23 has no boarding/ride links.",
    "e1m7": "The Chthon finale depends on combat/QC state: relay t10 targets gate t9, but no navigation interactor activates t10.",
    "e2m2": "The reported 280-unit vertical frontier is not a normal jump; the alternate mover/mechanic route remains disconnected.",
    "e2m5": "Trigger 22 is reached during the multi-stop elevator/train sequence, but the relevant trains have no physically proven boarding/ride links.",
    "e2m6": "The 204-unit vertical frontier is PM-impossible; the reverse exit component remains separated by mover-controlled geometry.",
    "e4m2": "The exit component remains separated around platform 24 and invalid one-unit support slivers; the displayed 104-unit ledge is not jumpable.",
    "e4m5": "Neither main nor secret exit component joins the source component; the displayed 64-unit ledge lands in the wrong lower area.",
    "e4m6": "The exit reverse component remains geographically disconnected; the displayed 277.6-unit sloped frontier is PM-impossible.",
}


def load(name: str) -> list[dict]:
    return json.loads((ROOT / name).read_text(encoding="utf-8"))


def complete(row: dict) -> bool:
    return row.get("plan_result") in {"static exit", "complete"}


def route_counts(row: dict) -> tuple[int, int]:
    match = re.search(r"reachable routing areas: (\d+) / (\d+)", row["outputs"]["route"])
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


def status(row: dict) -> str:
    if row.get("plan_result") == "static exit":
        return "STATIC EXIT"
    if row.get("plan_result") == "complete":
        return "COMPLETE PLAN"
    return str(row.get("plan_result", row.get("result", "unknown"))).upper()


def main() -> int:
    baseline = load("baseline.json")
    final = load("final-campaign.json")
    before = {row["map"]: row for row in baseline}
    after = {row["map"]: row for row in final}
    before_complete = sorted(name for name, row in before.items() if complete(row))
    after_complete = sorted(name for name, row in after.items() if complete(row))
    newly_complete = sorted(set(after_complete) - set(before_complete))
    lost = sorted(set(before_complete) - set(after_complete))
    reachable_reductions = {}
    for name, row in after.items():
        old_reachable, _ = route_counts(before[name])
        new_reachable, _ = route_counts(row)
        if new_reachable < old_reachable:
            reachable_reductions[name] = new_reachable - old_reachable

    old_candidates = sum(int(row["metrics"]["generation"]["jump_up_ledge_candidates"]) for row in baseline)
    new_candidates = sum(int(row["metrics"]["generation"]["jump_up_ledge_candidates"]) for row in final)
    old_validated = sum(int(row["metrics"]["generation"]["jump_up_ledge_validated"]) for row in baseline)
    new_validated = sum(int(row["metrics"]["generation"]["jump_up_ledge_validated"]) for row in final)

    maps = []
    for row in final:
        name = row["map"]
        old = before[name]
        old_reachable, old_total = route_counts(old)
        new_reachable, new_total = route_counts(row)
        maps.append({
            "map": name,
            "before": status(old),
            "after": status(row),
            "before_reachable_areas": old_reachable,
            "after_reachable_areas": new_reachable,
            "routing_areas": new_total or old_total,
            "newly_complete": name in newly_complete,
            "remaining_blocker": BLOCKERS.get(name, ""),
        })

    report = {
        "campaign": {
            "maps": 31,
            "excluded": ["e1m8"],
            "before_complete": len(before_complete),
            "after_complete": len(after_complete),
            "newly_complete": newly_complete,
            "complete_maps_lost": lost,
        },
        "implemented": [
            "Segment-precise dynamic attribution for multi-stop train paths instead of one aggregate swept AABB.",
            "PM-validated jump/drop trial directions for horizontal or degenerate decomposition portals.",
            "Planner self-dependency guard: an activator path may not cross the state change it is intended to cause.",
            "Component-audit support for multiple sources/destinations and current portal JSON type fields.",
        ],
        "jump_up_ledge": {
            "candidates_before": old_candidates,
            "candidates_after": new_candidates,
            "validated_before": old_validated,
            "validated_after": new_validated,
            "validated_delta": new_validated - old_validated,
            "maps_newly_exit_routeable": newly_complete,
        },
        "remaining_maps": BLOCKERS,
        "maps": maps,
        "validation": {
            "generation": "31/31",
            "reachability_pre_load": "31/31",
            "routing_pre_load": "31/31",
            "reachability_post_load": "31/31",
            "routing_post_load": "31/31",
            "persistence_result_parity": "31/31",
            "tool_errors": 0,
        },
        "builds": {
            "canonical_evobot_release": "passed",
            "mvdsv_release": "passed",
            "ezquake_release": "passed",
            "ezquake_debug": "passed",
        },
        "portable_sync": {
            "canonical_to_mvdsv": "12/12 SHA-256 identical",
            "canonical_to_ezquake": "12/12 SHA-256 identical",
        },
        "regressions": {
            "complete_maps_lost": lost,
            "reachable_area_reductions": reachable_reductions,
            "note": "e2m4 remains a complete plan; four areas now receive precise train/dynamic attribution instead of the previous aggregate classification.",
        },
        "artifacts": {
            "baseline": str(ROOT / "baseline.json"),
            "final_campaign": str(ROOT / "final-campaign.json"),
            "final_nav_snapshots": str(ROOT / "final-nav-snapshots"),
        },
        "commits_created": 0,
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# EvoBot remaining-campaign completion report", "",
        f"Result: **{len(before_complete)}/31 -> {len(after_complete)}/31 complete**.", "",
        f"New completions: **{', '.join(newly_complete)}**. Previously complete maps lost: **{len(lost)}**.", "",
        "## Implemented", "",
    ]
    lines.extend(f"- {item}" for item in report["implemented"])
    lines += [
        "", "## New completions", "",
        "| Map | Before | After | Route evidence |", "|---|---|---|---|",
        "| e4m1 | BLOCKED plan / conditional route | STATIC EXIT | Segment-precise train sweep removes false path-wide blocker attribution. |",
        "| e1m6 | UNREACHABLE | STATIC EXIT | PM validates the missing horizontal/decomposition-boundary transition. |",
        "| e4m4 | UNREACHABLE | COMPLETE PLAN | PM validates area 1491 -> 1461 through horizontal portal 7532; route-level result is conditional only for automatic door handling. |",
        "", "## Jump-up ledge coverage", "",
        f"Campaign candidates: **{old_candidates} -> {new_candidates}**. Validated links: **{old_validated} -> {new_validated} (+{new_validated - old_validated})**.", "",
        "The candidate population is unchanged; the new generic direction fallback allows existing horizontal/degenerate candidates to reach PM validation. Every added link is accepted only after grounded landing, destination entry, and hull-clear checks.", "",
        "## Remaining maps", "",
        "| Map | Final blocker |", "|---|---|",
    ]
    lines.extend(f"| {name} | {reason} |" for name, reason in BLOCKERS.items())
    lines += [
        "", "## Full campaign", "",
        "| Map | Before | After | Reachable areas after |", "|---|---|---|---:|",
    ]
    lines.extend(
        f"| {row['map']} | {row['before']} | {row['after']} | {row['after_reachable_areas']} / {row['routing_areas']} |"
        for row in maps
    )
    lines += [
        "", "## Verification", "",
        "- Generation, reachability validation, route validation, save/reload, and post-load validation: **31/31**.",
        "- Builds: canonical EvoBot Release, mvdsv Release, ezQuake Release, ezQuake Debug: **passed**.",
        "- Portable files: canonical -> mvdsv **12/12**, canonical -> ezQuake **12/12**, SHA-256 identical.",
        "- Coverage regression check: no complete map was lost. `e2m4` has 4 fewer static reachable areas (1469 -> 1465) from corrected dynamic attribution, but retains the same one-button complete plan and the all-open graph remains 1573 areas.",
        "- Commits created: **0**.", "",
        "Artifacts: `final-campaign.json`, `final-report.json`, and `final-nav-snapshots/` in this directory.",
    ]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
