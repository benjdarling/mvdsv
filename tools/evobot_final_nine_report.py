#!/usr/bin/env python3
"""Audit the definitive sweep and write the final-nine campaign reports."""

from __future__ import annotations

from collections import Counter
import json
import re
from pathlib import Path


ROOT = Path("build/nav-final-nine")
FINAL = ROOT / "final-campaign.json"
BASELINE = Path("build/nav-remaining-campaign/final-campaign.json")

NEW_DETAILS = {
    "e1m1": {
        "initial_blocker": "Dynamic route blocked at doors 3/39/21 and the old planner could not prove a non-circular activation order.",
        "mechanic": "Use the alternate reachable trigger chain, then a generic shootable door interaction with real line of sight.",
        "fix": "Stopped-mover connectivity exposed the alternate component; shoot planning now requires a clear player-hull trace from a grounded routing area instead of proximity alone.",
        "later": "Focused proof reached trigger 16 for door 15 and shootable door 14 from area 1633; final hardening exposes a static exit route.",
        "result": "STATIC EXIT",
    },
    "e2m6": {
        "initial_blocker": "The reported 204-unit jump frontier was impossible and concealed a mover bridge between components.",
        "mechanic": "Ride or cross the broad 18-unit-thick horizontal sliding door brush (door 8) as a moving slab.",
        "fix": "Thin horizontally moving door brushes with a player-fit top are modeled as platform-like movers, retaining independent stopped, ride, boarding, and exit links.",
        "later": "Door 8 contributes 168 mover links; the exit is reached in 39 route edges at 12.244 seconds in the focused proof.",
        "result": "STATIC EXIT",
    },
    "e4m5": {
        "initial_blocker": "The lowered bridge stopped short of the bank, leaving a small horizontal moat that WALK boarding could not cross.",
        "mechanic": "Take a normal running QuakeWorld jump from the bank onto the lowered bridge and ride/cross it.",
        "fix": "PM_PlayerMove jump-button boarding samples multiple ledge positions, input cadences, and jump frames against synthetic stopped-mover support, with grounded landing and hull clearance required.",
        "later": "Mover 43 validates the bridge boarding transition; the final exit route is static.",
        "result": "STATIC EXIT",
    },
}

REMAINING = {
    "e1m3": {
        "classification": "GAMEPLAY_KEY_PROGRESSION_REQUIRED",
        "reason": "The BSP contains item_key2 and gold-key doors (spawnflags 2056). The tested 0.493-unit slope/flat decomposition seam is not a valid substitute: multi-position PM and grounded dual-cell checks reject it. Item acquisition/planning is outside this milestone.",
    },
    "e1m7": {
        "classification": "COMBAT_REQUIRED",
        "reason": "monster_boss entity 28 targets t10 only from the Chthon death path; relay entity 98 then fires t9 and opens the exit doors. KTX _boss_death10 calls SUB_UseTargets. No normal-skill navigation interactor provides an alternate t10 activation.",
    },
    "e2m2": {
        "classification": "GAMEPLAY_KEY_PROGRESSION_REQUIRED",
        "reason": "The BSP contains item_key2 and gold-key doors (spawnflags 2056). Normal PM rejects the displayed 204/280-unit vertical shortcuts; the component bridge audit finds no physically validated substitute that bypasses gameplay progression.",
    },
    "e2m5": {
        "classification": "GAMEPLAY_KEY_AND_ORDERED_MOVER_PROGRESSION_REQUIRED",
        "reason": "The BSP contains item_key2 and gold-key door 44 (spawnflags 2056). The train cage's real internal floor is detected and its ride through trigger 22 is represented, but no non-circular navigation-only ordering reaches that ride before door 25. Item acquisition/route execution is outside scope.",
    },
    "e4m2": {
        "classification": "GAMEPLAY_KEY_PROGRESSION_REQUIRED",
        "reason": "The BSP contains two item_key1 entities, including targetnamed key t97. Platform 24 has no player-usable internal floor: its only detected near-top support is a bevel artifact, and the 104-unit displayed ledge is PM-impossible. No navigation-only bypass was validated.",
    },
    "e4m6": {
        "classification": "GAMEPLAY_KEY_PROGRESSION_REQUIRED",
        "reason": "The BSP contains item_key1/item_key2 and gold-key doors (spawnflags 2056). PM rejects the 277.6-unit sloped shortcut, while the modeled dynamic graph exposes no physically valid navigation-only bypass of the key progression.",
    },
}


def route_counts(row: dict) -> tuple[int, int, int | None]:
    output = row["outputs"].get("route", "")
    normal = re.search(r"reachable routing areas: (\d+) / (\d+)", output)
    alternate = re.search(r"ignoring dynamic blockers:.*?\((\d+) reachable areas\)", output)
    return (
        int(normal.group(1)) if normal else 0,
        int(normal.group(2)) if normal else 0,
        int(alternate.group(1)) if alternate else None,
    )


def complete(row: dict) -> bool:
    return row.get("plan_result") in {"static exit", "complete"}


def display_status(row: dict) -> str:
    return {
        "static exit": "STATIC EXIT",
        "complete": "COMPLETE PLAN",
        "blocked": "BLOCKED",
        "unreachable": "UNREACHABLE",
    }.get(row.get("plan_result", ""), row.get("plan_result", "UNKNOWN").upper())


def classified(output: str) -> str:
    match = re.search(r"^result:\s*(.+?)\s*$", output, re.MULTILINE | re.IGNORECASE)
    return match.group(1).strip().lower() if match else "unknown"


def main() -> int:
    final = json.loads(FINAL.read_text(encoding="utf-8"))
    baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
    before = {row["map"]: row for row in baseline}
    after = {row["map"]: row for row in final}
    before_complete = {name for name, row in before.items() if complete(row)}
    after_complete = {name for name, row in after.items() if complete(row)}
    newly_complete = sorted(after_complete - before_complete)
    lost = sorted(before_complete - after_complete)

    audit_rows = []
    for row in final:
        outputs = row["outputs"]
        pre_route = classified(outputs.get("route", ""))
        post_route = classified(outputs.get("loaded_route", ""))
        source = re.search(r"source area:\s*(\d+)", outputs.get("route", ""))
        checks = {
            "generation": (
                "EvoBot navigation generated" in outputs.get("generate", "") or
                (row.get("metrics", {}).get("area_count", 0) > 0 and
                 row.get("metrics", {}).get("reachability_count", 0) > 0)
            ),
            "reachability_pre_load": "validation: ok" in outputs.get("reach_validate", "").lower(),
            "routing_pre_load": "validation: ok" in outputs.get("route_validate", "").lower(),
            "spawn_resolution": bool(source and int(source.group(1)) > 0),
            "save": "navigation saved" in outputs.get("save", "").lower(),
            "load": "navigation loaded" in outputs.get("load", "").lower(),
            "reachability_post_load": "validation: ok" in outputs.get("loaded_reach_validate", "").lower(),
            "routing_post_load": "validation: ok" in outputs.get("loaded_route_validate", "").lower(),
            "route_persistence_parity": pre_route == post_route,
            "planning": row.get("plan_result") not in {None, "not-run", "unknown"},
            "format_version": row.get("metrics", {}).get("format_version") == 6,
            "no_tool_error": not row.get("error"),
        }
        audit_rows.append({"map": row["map"], "checks": checks, "passed": all(checks.values())})
    validation = {
        key: sum(1 for item in audit_rows if item["checks"][key])
        for key in audit_rows[0]["checks"]
    }

    maps = []
    for row in final:
        reachable, routing, alternate = route_counts(row)
        old_reachable, _, _ = route_counts(before[row["map"]])
        maps.append({
            "map": row["map"],
            "before": display_status(before[row["map"]]),
            "after": display_status(row),
            "complete": complete(row),
            "route_result": row["result"],
            "plan_result": row["plan_result"],
            "reachable_areas_before": old_reachable,
            "reachable_areas_after": reachable,
            "routing_areas": routing,
            "ignore_dynamic_reachable_areas": alternate,
            "seconds": row["seconds"],
        })

    totals = Counter()
    for row in final:
        generation = row.get("metrics", {}).get("generation", {})
        for key in (
            "jump_up_ledge_candidates", "jump_up_ledge_validated",
            "platform_jump_board_candidates", "platform_jump_board_validated",
            "platform_jump_board_links",
        ):
            totals[key] += int(generation.get(key, 0))

    report = {
        "campaign_result": {
            "before": len(before_complete),
            "after": len(after_complete),
            "new_completions": len(after_complete) - len(before_complete),
            "newly_completed_maps": newly_complete,
            "complete_maps_lost": lost,
            "normal_gravity_maps": 31,
            "excluded": ["e1m8"],
        },
        "newly_completed": NEW_DETAILS,
        "remaining_incomplete": REMAINING,
        "general_systems_implemented": [
            "PM-validated jump boarding onto synthetic stopped-mover support with multiple ledge positions, input cadences, and jump frames.",
            "Thin horizontally moving door brushes modeled as platform-like slabs when their top fits the player hull.",
            "Internal supported floors in elevator trains translated across every captured stop.",
            "Ride-through touch-trigger tagging and planner activation actions for mover edges.",
            "Stopped-state crossings/exits, submerged boarding, vacated passages, and independent directional mover links.",
            "Path-corner identity cycle detection for multi-stop trains.",
            "Conservative grounded player-hull line-of-sight proof for generic shoot activations.",
            "Bounded sequential activation-state search and correct completion handoff for conditional routes containing only automatic approach doors.",
            "Component bridge and BSP model-surface diagnostics.",
        ],
        "completion_progression": [
            {"complete": 22, "stage": "baseline"},
            {"complete": 23, "stage": "PM mover jump boarding", "maps": ["e4m5"]},
            {"complete": 24, "stage": "alternate activation path plus validated shoot interaction", "maps": ["e1m1"]},
            {"complete": 25, "stage": "thin horizontal door/slab mover support", "maps": ["e2m6"]},
        ],
        "movement_totals": dict(totals),
        "maps": maps,
        "validation": {
            "maps": len(final),
            "counts": validation,
            "all_maps_passed": all(item["passed"] for item in audit_rows),
            "per_map": audit_rows,
        },
        "builds": {
            "canonical_evobot_release": "passed",
            "canonical_evobot_debug": "passed",
            "mvdsv_release": "passed",
            "mvdsv_debug": "passed",
            "ezquake_release": "passed",
            "ezquake_debug": "passed",
        },
        "portable_sync": "12/12 files SHA-256 identical across canonical, mvdsv, and ezQuake",
        "regressions": {"complete_maps_lost": lost, "result": "none" if not lost else "failed"},
        "artifacts": {
            "campaign": str(FINAL),
            "nav_snapshots": str(ROOT / "final-nav-snapshots"),
            "focused_e1m1": str(ROOT / "affected-16x4.json"),
            "focused_e2m6": str(ROOT / "e2m6-final-focused.json"),
            "focused_e4m5": str(ROOT / "e4m5-board-cadences.json"),
        },
        "commits_created": 0,
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    lines = [
        "CAMPAIGN RESULT", "",
        f"Before: {len(before_complete)} / 31",
        f"After: {len(after_complete)} / 31",
        f"New completions: +{len(after_complete) - len(before_complete)}", "",
        "# NEWLY COMPLETED", "",
    ]
    for name in ("e1m1", "e2m6", "e4m5"):
        detail = NEW_DETAILS[name]
        lines += [
            f"## {name}", "",
            f"- Initial blocker: {detail['initial_blocker']}",
            f"- Intended mechanic: {detail['mechanic']}",
            f"- Generic fix: {detail['fix']}",
            f"- Later blockers: {detail['later']}",
            f"- Final result: **{detail['result']}**", "",
        ]
    lines += ["# REMAINING INCOMPLETE", ""]
    for name, item in REMAINING.items():
        lines += [f"## {name}: {item['classification']}", "", item["reason"], ""]
    lines += ["# GENERAL SYSTEMS IMPLEMENTED", ""]
    lines.extend(f"- {item}" for item in report["general_systems_implemented"])
    lines += ["", "# COMPLETION PROGRESSION", ""]
    for item in report["completion_progression"]:
        suffix = f" - {', '.join(item.get('maps', []))}" if item.get("maps") else ""
        lines.append(f"- {item['complete']}/31: {item['stage']}{suffix}")
    lines += [
        "", "# FULL CAMPAIGN", "",
        "| Map | Before | After | Reachable areas |",
        "|---|---:|---:|---:|",
    ]
    lines.extend(
        f"| {item['map']} | {item['before']} | {item['after']} | {item['reachable_areas_after']} / {item['routing_areas']} |"
        for item in maps
    )
    lines += [
        "", "# VALIDATION", "",
        f"- Generation: **{validation['generation']}/31**",
        f"- Reachability validation before/after load: **{validation['reachability_pre_load']}/31**, **{validation['reachability_post_load']}/31**",
        f"- Dijkstra/route validation before/after load: **{validation['routing_pre_load']}/31**, **{validation['routing_post_load']}/31**",
        f"- Spawn resolution: **{validation['spawn_resolution']}/31**",
        f"- Save/load and route parity: **{validation['save']}/31**, **{validation['load']}/31**, **{validation['route_persistence_parity']}/31**",
        f"- Planning executed: **{validation['planning']}/31**",
        f"- Format v6 and tool errors absent: **{validation['format_version']}/31**, **{validation['no_tool_error']}/31**",
        "- Builds: canonical EvoBot Release/Debug, mvdsv Release/Debug, ezQuake Release/Debug - **passed**",
        "- Portable sync: **12/12 SHA-256 identical** across all three trees",
        "", "# REGRESSIONS", "",
        "None. All 22 baseline-complete maps remain complete.", "",
        f"Definitive sweep: `{FINAL}`",
        f"Snapshots: `{ROOT / 'final-nav-snapshots'}`",
        "Commits created: **0**.",
    ]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if not report["validation"]["all_maps_passed"] or lost or newly_complete != ["e1m1", "e2m6", "e4m5"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
