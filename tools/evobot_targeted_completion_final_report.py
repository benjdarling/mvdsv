#!/usr/bin/env python3
"""Create the final targeted campaign-completion report from sweep artifacts."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path("build/nav-targeted-completion")
PRIMARY = ("e4m8", "e3m5", "e2m5")

FINAL_BLOCKERS = {
    "e1m1": "Activation planner cannot prove the interactor 3/39/21 sequence; the exit is reachable only when dynamic blockers are ignored.",
    "e1m3": "Unvalidated 80-unit jump-up ledge at portal 4130 (area 903 -> 904).",
    "e1m6": "Unvalidated 64-unit jump-up ledge at portal 2954 (area 766 -> 731).",
    "e1m7": "No PM-validated transition across liquid portal 1304 (area 249 -> 253, 24-unit rise; interactor 4).",
    "e2m2": "280-unit vertical frontier at portal 24039; requires an unproven mover/vertical mechanic.",
    "e2m5": "Underwater trigger 22 targets door 25 but is not proven reachable before that door; later blockers are interactors 47, 37, and 26.",
    "e2m6": "204-unit vertical frontier at portal 10435; no normal-jump traversal validates.",
    "e4m1": "Only conditional interactor-2 connectivity is found; the planner cannot prove the complete activation sequence.",
    "e4m2": "104-unit jump-up frontier at portal 5937 (interactor 54).",
    "e4m4": "68-unit jump-up frontier at portal 7531.",
    "e4m5": "64-unit jump-up frontier at portal 1689.",
    "e4m6": "277.6-unit vertical frontier at portal 6163.",
}

FEATURES = {
    "e1m4": "Solid-lip JUMP, non-portal water transition, and PM teleporter landing",
    "e2m1": "General solid-lip/water/teleporter connectivity (plan became static)",
    "e2m4": "Solid-lip JUMP, non-portal water transition, and PM teleporter landing",
    "e2m5": "Non-portal water connectivity made the exit reachable when dynamic blockers are ignored",
    "e3m5": "PM teleporter landing, non-portal water transition, and planner world-state fixes",
    "e4m8": "Solid-lip JUMP over a shallow ramp/lip, validated with PM_PlayerMove",
    "end": "Solid-lip JUMP, teleporter landing, and non-portal water connectivity",
}


def load(name: str):
    return json.loads((ROOT / name).read_text(encoding="utf-8"))


def route_counts(row: dict) -> tuple[int, int]:
    match = re.search(r"reachable routing areas: (\d+) / (\d+)", row["outputs"]["route"])
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


def status(row: dict) -> str:
    if row.get("result") == "reachable":
        return "STATIC EXIT"
    if row.get("plan_result") == "complete":
        return "COMPLETE PLAN"
    if row.get("result") == "conditional":
        return "CONDITIONAL"
    if row.get("result") == "blocked" or row.get("plan_result") == "blocked":
        return "BLOCKED"
    return "UNREACHABLE"


def complete(row: dict) -> bool:
    return status(row) in {"STATIC EXIT", "COMPLETE PLAN"}


def signed(value: float | int, decimals: int = 0) -> str:
    if decimals:
        return f"{value:+.{decimals}f}"
    return f"{int(value):+d}"


def main() -> int:
    baseline = load("baseline.json")
    final = load("final-campaign.json")
    drop = load("drop-ab.json")
    before = {row["map"]: row for row in baseline}
    after = {row["map"]: row for row in final}

    # Retain exact raw final records for the three primary targets.
    for map_name in PRIMARY:
        (ROOT / f"{map_name}-final.json").write_text(
            json.dumps([after[map_name]], indent=2) + "\n", encoding="utf-8"
        )

    newly_complete = [name for name in after if complete(after[name]) and not complete(before[name])]
    map_rows = []
    for name in after:
        before_reachable, _ = route_counts(before[name])
        after_reachable, _ = route_counts(after[name])
        before_status = status(before[name])
        after_status = status(after[name])
        changed = before_reachable != after_reachable or before_status != after_status
        feature = FEATURES.get(name, "Campaign-wide connectivity changes" if changed else "None measured")
        map_rows.append({
            "map": name,
            "new_completion": name in newly_complete,
            "before_status": before_status,
            "after_status": after_status,
            "before_reachable_areas": before_reachable,
            "after_reachable_areas": after_reachable,
            "feature": feature,
            "final_blocker": "EXIT" if complete(after[name]) else FINAL_BLOCKERS.get(name, "See raw route diagnostic"),
        })

    generation_seconds = sum(row["metrics"]["generation"]["seconds"] for row in final)
    total_map_seconds = sum(row["seconds"] for row in final)
    worst_generation = max(final, key=lambda row: row["metrics"]["generation"]["seconds"])
    worst_total = max(final, key=lambda row: row["seconds"])

    report = {
        "campaign_result": {
            "before_complete": sum(complete(row) for row in baseline),
            "after_complete": sum(complete(row) for row in final),
            "total_maps": len(final),
            "new_completions": newly_complete,
        },
        "completion_progression": [
            {"stage": "Baseline", "complete": 14},
            {"stage": "DROP 20/50/80", "complete": 14},
            {"stage": "Solid-lip e4m8 fix", "complete": 15},
            {"stage": "General PM teleport/water/planner fixes", "complete": 19},
            {"stage": "Final campaign", "complete": 19},
        ],
        "drop_ab": drop,
        "primary_blocker_chains": {
            "e4m8": [
                "Missing connectivity across a shallow solid ramp/lip from area 416.",
                "Generic solid-lip candidate pass validated the jump with actual jump-button PM movement.",
                "EXIT (STATIC EXIT).",
            ],
            "e3m5": [
                "Door/interactor 18 blocked the static route.",
                "The trigger 29 -> door 22 -> button 23 chain was present, but greedy closure could destroy a valid exit state.",
                "Target-controlled teleporters lacked actual PM-derived destination landings.",
                "A 17-unit water/ground decomposition gap lacked a shared portal.",
                "EXIT (COMPLETE PLAN).",
            ],
            "e2m5": [
                "A non-portal water/ground decomposition boundary stopped physical coverage.",
                "Generic PM-validated non-portal water transitions made the exit reachable when dynamic blockers are ignored.",
                "Door 25 remains behind an unproven trigger-22 activation cycle; interactors 47, 37, and 26 follow.",
                "BLOCKED: no safe assumption that the trigger fires.",
            ],
        },
        "maps": map_rows,
        "performance": {
            "target_generation_seconds": {
                name: after[name]["metrics"]["generation"]["seconds"] for name in PRIMARY
            },
            "campaign_generation_seconds": generation_seconds,
            "campaign_map_total_seconds": total_map_seconds,
            "worst_generation_map": worst_generation["map"],
            "worst_generation_seconds": worst_generation["metrics"]["generation"]["seconds"],
            "worst_total_map": worst_total["map"],
            "worst_total_seconds": worst_total["seconds"],
        },
        "validation": {
            "generation": "31/31",
            "reachability_pre_load": "31/31",
            "dijkstra_pre_load": "31/31",
            "spawn_resolution": "31/31",
            "reachability_post_load": "31/31",
            "dijkstra_post_load": "31/31",
            "persistence": "31/31",
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
        "source_control": {
            "canonical": {"head": "21d2ee0", "status": ["M src/evobot_nav_convex.c", "M src/evobot_nav_route.c"], "diff_stat": "2 files changed, 658 insertions(+), 41 deletions(-)"},
            "mvdsv": {"head": "1468195", "status": ["m external/evobot", "M tools/mvdsv_route_sweep.py", "?? tools/evobot_nav_component_audit.py", "?? tools/evobot_targeted_completion_report.py", "?? tools/quake_bsp_entities.py", "?? tools/evobot_targeted_completion_final_report.py"], "external_evobot": "c01dce5"},
            "ezquake": {"head": "64735a59", "status": ["m external/evobot"], "external_evobot": "c01dce5"},
            "diff_check": "passed in all three repositories (line-ending warnings only)",
            "commits_created": 0,
        },
        "visual_verification": {
            "result": "not completed",
            "reason": "The automated ezQuake capture launched once before rendering and once without -basedir; the latter caused the palette.lmp dialog. This is not reported as visual proof.",
        },
        "regressions": {
            "complete_maps_lost": 0,
            "final_reachable_area_reductions_vs_baseline": 0,
            "drop_ab_note": "The isolated DROP A/B had -9 aggregate reachable areas (e1m2 -6, e4m5 -3) because new representatives failed landing-headroom checks; subsequent general fixes recovered beyond baseline.",
        },
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    campaign = report["campaign_result"]
    lines = [
        "# CAMPAIGN RESULT", "",
        f"Before: **{campaign['before_complete']} / {campaign['total_maps']} complete**  ",
        f"After: **{campaign['after_complete']} / {campaign['total_maps']} complete**  ",
        f"New completions: **+{len(newly_complete)}** ({', '.join(newly_complete)})", "",
        "## Primary targets", "",
        "| Map | Before reachable | After reachable | Before status | After status | Blockers solved | Final blocker |",
        "|---|---:|---:|---|---|---|---|",
    ]
    solved = {
        "e4m8": "Shallow solid ramp/lip lacked a candidate; generic PM-validated solid-lip JUMP added",
        "e3m5": "Controlled teleporter landing, water/decomposition transition, and planner state handling",
        "e2m5": "Water/decomposition boundary; dynamic-ignored route now reaches exit",
    }
    for name in PRIMARY:
        b, _ = route_counts(before[name]); a, _ = route_counts(after[name])
        final_blocker = "EXIT" if complete(after[name]) else FINAL_BLOCKERS[name]
        lines.append(f"| {name} | {b} | {a} | {status(before[name])} | **{status(after[name])}** | {solved[name]} | {final_blocker} |")

    db = drop["midpoint_current"]; da = drop["sample_20_50_80"]; dc = drop["change"]
    drop_metrics = [
        ("Candidate ledges", "candidate_ledges", 0), ("Tested launch points", "tested_launch_points", 0),
        ("PM simulations", "pm_simulations", 0), ("Validated candidate drops", "validated_candidate_drops", 0),
        ("Deduplicated DROP links", "deduplicated_drop_links", 0), ("Total DROP links", "total_drop_links", 0),
        ("Aggregate reachable areas", "aggregate_reachable_areas", 0), ("Complete campaign maps", "complete_campaign_maps", 0),
        ("DROP generation time (s)", "drop_generation_seconds", 3),
    ]
    lines += ["", "## DROP 20/50/80 result", "", "| Metric | Midpoint/current | 20/50/80 | Change |", "|---|---:|---:|---:|"]
    for label, key, decimals in drop_metrics:
        fmt = f"{{:.{decimals}f}}" if decimals else "{}"
        lines.append(f"| {label} | {fmt.format(db[key])} | {fmt.format(da[key])} | {signed(dc[key], decimals)} |")
    for name in PRIMARY:
        key = drop["change"]["target_reachable_areas"][name]
        lines.append(f"| {name} reachable areas | {db['target_reachable_areas'][name]} | {da['target_reachable_areas'][name]} | {signed(key)} |")
    lines += ["", "20/50/80 found more valid and distinct DROP links, but produced no target-map or completion gain by itself. Its isolated cost rose by 15.507 s (+59.7%). Short ledges retain one midpoint sample to avoid redundant PM work.", ""]

    lines += ["## Completion progression", ""]
    for stage in report["completion_progression"]:
        lines.append(f"- {stage['stage']}: {stage['complete']}/31")

    lines += ["", "## Blocker chains", ""]
    for name in PRIMARY:
        lines.append(f"### {name}")
        lines.append("")
        for item in report["primary_blocker_chains"][name]:
            lines.append(f"- {item}")
        lines.append("")

    lines += ["## Campaign map-by-map", "", "| Map | Before status | After status | Before reachable | After reachable | Feature(s) responsible | Final blocker |", "|---|---|---|---:|---:|---|---|"]
    for row in map_rows:
        name = f"**{row['map']} (NEW)**" if row["new_completion"] else row["map"]
        lines.append(f"| {name} | {row['before_status']} | {row['after_status']} | {row['before_reachable_areas']} | {row['after_reachable_areas']} | {row['feature']} | {row['final_blocker']} |")

    p = report["performance"]
    lines += [
        "", "## Performance and regression", "",
        f"Target generation: e4m8 {p['target_generation_seconds']['e4m8']:.3f} s; e3m5 {p['target_generation_seconds']['e3m5']:.3f} s; e2m5 {p['target_generation_seconds']['e2m5']:.3f} s. Full campaign generation totaled {p['campaign_generation_seconds']:.3f} s; recorded per-map pipeline time totaled {p['campaign_map_total_seconds']:.3f} s. Worst generation map was {p['worst_generation_map']} ({p['worst_generation_seconds']:.3f} s); worst total map was {p['worst_total_map']} ({p['worst_total_seconds']:.3f} s).", "",
        "No previously complete map lost completion, and no map finished with fewer reachable areas than the fresh baseline. The isolated DROP A/B temporarily reduced the aggregate by 9 areas (e1m2 -6, e4m5 -3): the old representatives failed the new landing-headroom validation. Later generic connectivity recovered both maps beyond baseline.", "",
        "## Validation, builds, and synchronization", "",
        "- Generation, pre/post-load reachability validation, pre/post-load Dijkstra validation, spawn resolution, and persistence: **31/31** each; tool errors: **0**.",
        "- Builds passed: canonical EvoBot Release, MVDSV Release, ezQuake Release, ezQuake Debug.",
        "- Portable synchronization: canonical -> MVDSV **12/12 identical**; canonical -> ezQuake **12/12 identical** by SHA-256.",
        "- `git diff --check` passed in all three repositories (only CRLF conversion warnings). No commits were created.", "",
        "## Visual verification caveat", "",
        "Representative live ezQuake visual verification was attempted but not successfully captured. The first automated capture occurred before rendering; the retry accidentally omitted `-basedir` and produced the `gfx/palette.lmp` dialog. That launch error is unrelated to the builds/navigation results, and this report does not claim visual proof.", "",
        "## Source control", "",
        "- Canonical EvoBot `21d2ee0`: modified `src/evobot_nav_convex.c`, `src/evobot_nav_route.c`; 2 files, +658/-41.",
        "- MVDSV `1468195`: modified external EvoBot submodule and sweep tooling; new audit/report/BSP tools. External EvoBot revision `c01dce5` with synchronized working-tree changes.",
        "- ezQuake `64735a59`: modified external EvoBot submodule. External EvoBot revision `c01dce5` with synchronized working-tree changes.",
    ]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {ROOT / 'final-report.md'} and {ROOT / 'final-report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
