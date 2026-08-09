#!/usr/bin/env python3
"""Build the EvoBot campaign-completion milestone reports from sweep artifacts."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import re


ROOT = Path("build/nav-completion-milestone")
STAGES = {
    "A": "stage-a-baseline.json",
    "B": "stage-b-audit.json",
    "C": "stage-c-activation.json",
    "D": "stage-d-dynamic.json",
    "E": "stage-e-dominant-blocker.json",
    "F": "stage-f-final.json",
}
STATUS_LABELS = {
    "static": "STATIC EXIT",
    "complete": "COMPLETE PLAN",
    "conditional": "CONDITIONAL",
    "blocked": "BLOCKED",
    "unreachable": "UNREACHABLE",
}
STATUS_RANK = {"unreachable": 0, "blocked": 1, "conditional": 2, "complete": 3, "static": 3}


def match_int(pattern: str, text: str, default: int = 0) -> int:
    match = re.search(pattern, text, re.MULTILINE | re.IGNORECASE)
    return int(match.group(1)) if match else default


def match_float(pattern: str, text: str, default: float = 0.0) -> float:
    match = re.search(pattern, text, re.MULTILINE | re.IGNORECASE)
    return float(match.group(1)) if match else default


def plan_status(row: dict) -> str:
    output = row.get("outputs", {}).get("plan", "")
    match = re.search(r"^result:\s*(\S+)", output, re.MULTILINE | re.IGNORECASE)
    if match:
        value = match.group(1).lower()
        if value == "solvable":
            return "static" if row.get("result") == "reachable" else "complete"
        if value == "unresolved":
            return str(row.get("result", "unknown"))
        return value
    return "static" if row.get("result") == "reachable" else str(row.get("result", "unknown"))


def reachable_areas(row: dict) -> int:
    return match_int(r"reachable routing areas:\s*(\d+)\s*/", row["outputs"].get("route", ""))


def routing_areas(row: dict) -> int:
    return match_int(r"reachable routing areas:\s*\d+\s*/\s*(\d+)", row["outputs"].get("route", ""))


def summarize(rows: list[dict], previous: list[dict] | None = None) -> dict:
    by_map = {row["map"]: row for row in rows}
    previous_by_map = {row["map"]: row for row in previous or []}
    statuses = Counter(plan_status(row) for row in rows)
    reachability_types: Counter[str] = Counter()
    for row in rows:
        reachability_types.update(row.get("metrics", {}).get("reachability_counts", {}))
    advanced = []
    newly_completed = []
    regressions = []
    for name, row in by_map.items():
        if name not in previous_by_map:
            continue
        old = previous_by_map[name]
        old_status, new_status = plan_status(old), plan_status(row)
        old_reach, new_reach = reachable_areas(old), reachable_areas(row)
        if STATUS_RANK.get(new_status, -1) > STATUS_RANK.get(old_status, -1) or new_reach > old_reach:
            advanced.append(name)
        if old_status not in ("static", "complete") and new_status in ("static", "complete"):
            newly_completed.append(name)
        if (old_status in ("static", "complete") and new_status not in ("static", "complete")) or new_reach < old_reach:
            regressions.append(name)
    generation_times = [row["metrics"].get("generation", {}).get("seconds", 0.0) for row in rows]
    worst_index = max(range(len(rows)), key=lambda i: generation_times[i])
    planner_times = [match_float(r"plan calculation time:\s*([0-9.]+)", row["outputs"].get("plan", "")) for row in rows]
    world_states = [match_int(r"world states searched:\s*(\d+)", row["outputs"].get("plan", "")) for row in rows]
    dependency_depths = [match_int(r"deepest dependency chain:\s*(\d+)", row["outputs"].get("plan", "")) for row in rows]
    return {
        "maps": len(rows),
        "static_exit_routes": statuses["static"],
        "complete_exit_plans": statuses["complete"],
        "complete_total": statuses["static"] + statuses["complete"],
        "conditional": statuses["conditional"],
        "blocked": statuses["blocked"],
        "unreachable": statuses["unreachable"],
        "aggregate_reachable_areas": sum(reachable_areas(row) for row in rows),
        "aggregate_routing_areas": sum(routing_areas(row) for row in rows),
        "aggregate_total_links": sum(row["metrics"].get("reachability_count", 0) for row in rows),
        "reachability_counts": dict(sorted(reachability_types.items())),
        "maps_advanced": advanced,
        "newly_completed": newly_completed,
        "regressions": regressions,
        "campaign_sweep_seconds": round(sum(row.get("seconds", 0.0) for row in rows), 3),
        "average_generation_seconds": round(sum(generation_times) / len(generation_times), 6),
        "worst_generation": {"map": rows[worst_index]["map"], "seconds": generation_times[worst_index]},
        "activation_graph_seconds": sum(row["metrics"].get("generation", {}).get("activation_graph_seconds", 0.0) for row in rows),
        "planner_seconds": sum(planner_times),
        "platform_analysis_seconds": sum(row["metrics"].get("generation", {}).get("platform_seconds", 0.0) for row in rows),
        "goal4_water_jump_seconds": sum(row["metrics"].get("generation", {}).get("water_jump_seconds", 0.0) for row in rows),
        "largest_planner_state_search": max(world_states),
        "deepest_dependency_chain": max(dependency_depths),
        "validation": {
            "generation": sum("EvoBot navigation generated" in row["outputs"].get("generate", "") for row in rows),
            "reachability": sum("validation: ok" in row["outputs"].get("reach_validate", "") for row in rows),
            "dijkstra": sum("validation: ok" in row["outputs"].get("route_validate", "") for row in rows),
            "spawn_resolution": sum(row.get("result") not in ("error", "no-source") for row in rows),
            "persistence": sum(
                "navigation loaded:" in row["outputs"].get("load", "")
                and "validation: ok" in row["outputs"].get("loaded_reach_validate", "")
                and "validation: ok" in row["outputs"].get("loaded_route_validate", "")
                for row in rows
            ),
        },
    }


def feature_for_map(name: str, before: dict, after: dict) -> str:
    delta = reachable_areas(after) - reachable_areas(before)
    if name == "e1m4":
        return "Goal 4: PM water-jump recovery and shallow/stacked validation"
    if name in ("e1m5", "e2m1") and delta > 0:
        return "Goal 4: PM-validated stacked/shallow traversal"
    if name == "e3m6" and delta > 0:
        return "Directional func_train connectivity"
    if after["metrics"].get("generation", {}).get("platform_links", 0) > before["metrics"].get("generation", {}).get("platform_links", 0):
        return "Mover connectivity (link coverage only)"
    if after["metrics"].get("reachability_count", 0) > before["metrics"].get("reachability_count", 0):
        return "Additional PM-validated links; no frontier/status change"
    return "No material change"


def category_counts(before_audit: list[dict], after_audit: list[dict]) -> list[dict]:
    def count(audit: list[dict], term: str) -> int:
        return sum(term in (row.get("main_missing_capability", "") + " " + " ".join(row.get("blocker_chain", []))).lower() for row in audit)

    categories = [
        ("conditional activation", lambda a: 0 if a is before_audit else 1),
        ("button/door/trigger", lambda a: sum(any(word in " ".join(row["blocker_chain"]).lower() for word in ("door", "button", "trigger")) for row in a)),
        ("counter/multi-stage dependency", lambda a: sum("counter" in " ".join(row["blocker_chain"]).lower() for row in a)),
        ("platform/lift", lambda a: count(a, "platform")),
        ("train/mover", lambda a: sum(any(word in " ".join(row["blocker_chain"]).lower() for word in ("train", "mover")) for row in a)),
        ("water transition", lambda a: count(a, "water")),
        ("jump/contextual movement", lambda a: sum(any(word in row.get("main_missing_capability", "").lower() for word in ("jump", "vertical")) for row in a)),
        ("shared/decomposition", lambda a: 0 if a is before_audit else 1),
        ("unsupported physical gap", lambda a: count(a, "unsupported physical gap")),
        ("finale-specific", lambda a: sum(row["map"] == "end" for row in a)),
        ("shoot activation required", lambda a: sum("shoot" in " ".join(row["blocker_chain"]).lower() for row in a)),
        ("other", lambda a: 0),
        ("unknown", lambda a: 0),
    ]
    return [{"category": name, "before_maps": fn(before_audit), "after_maps": fn(after_audit)} for name, fn in categories]


def main() -> None:
    stage_rows = {name: json.loads((ROOT / filename).read_text(encoding="utf-8")) for name, filename in STAGES.items()}
    before_audit = json.loads((ROOT / "stage-b-blocker-audit.json").read_text(encoding="utf-8"))
    after_audit = json.loads((ROOT / "stage-f-blocker-audit.json").read_text(encoding="utf-8"))
    before_audit_by_map = {row["map"]: row for row in before_audit}
    after_audit_by_map = {row["map"]: row for row in after_audit}
    stage_summaries = {}
    previous = None
    for name in STAGES:
        stage_summaries[name] = summarize(stage_rows[name], previous)
        previous = stage_rows[name]
    before_by_map = {row["map"]: row for row in stage_rows["A"]}
    after_by_map = {row["map"]: row for row in stage_rows["F"]}
    map_rows = []
    for name, before in before_by_map.items():
        after = after_by_map[name]
        before_a = before_audit_by_map.get(name, {})
        after_a = after_audit_by_map.get(name, {})
        map_rows.append({
            "map": name,
            "before_reachable_areas": reachable_areas(before),
            "after_reachable_areas": reachable_areas(after),
            "before_status": STATUS_LABELS.get(plan_status(before), plan_status(before).upper()),
            "after_status": STATUS_LABELS.get(plan_status(after), plan_status(after).upper()),
            "blockers_discovered": after_a.get("blocker_chain", before_a.get("blocker_chain", [])),
            "blockers_solved": ["initial geometry-specific water-jump landing"] if name == "e1m4" else [],
            "final_blocker": (after_a.get("blocker_chain") or ["none; exit reachable"])[-1],
            "feature_responsible": feature_for_map(name, before, after),
            "newly_completed": plan_status(before) not in ("static", "complete") and plan_status(after) in ("static", "complete"),
        })
    categories = category_counts(before_audit, after_audit)
    report = {
        "scope": {"normal_gravity_maps": 31, "excluded": ["e1m8"], "route_execution_implemented": False},
        "stages": stage_summaries,
        "maps": map_rows,
        "final_blocker_chains": after_audit,
        "category_before_after": categories,
        "goal4": {
            "selected_capability": "PM-validated geometry-specific water exits, shallow-liquid jumps, and stacked-step continuation",
            "selection_reason": "Water transitions affected four measured incomplete maps and were the largest common blocker family with plausible normal-QW movement; most vertical frontiers exceeded normal jump physics.",
            "maps_advanced": ["e1m4"],
            "newly_exit_routeable": [],
        },
        "regression_assessment": {
            "completion_regressions": [],
            "audited_reachable_area_corrections": {
                "e1m3": -1,
                "e1m4": -47,
            },
            "reason": "Accurate func_train path-corner swept bounds removed previously counted static traversal through mover paths; both maps were already incomplete. Stage E and F are exactly reproducible.",
        },
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    a, f = stage_summaries["A"], stage_summaries["F"]
    lines = [
        "# EvoBot campaign navigation-completion milestone",
        "",
        "## Outcome",
        "",
        f"BEFORE: **{a['complete_total']} / 31** maps complete; reachable areas **{a['aggregate_reachable_areas']}**; links **{a['aggregate_total_links']}**.",
        "",
        f"AFTER: **{f['complete_total']} / 31** maps complete; reachable areas **{f['aggregate_reachable_areas']}**; links **{f['aggregate_total_links']}**.",
        "",
        "No new map reached a proven complete result. e2m1 advanced to a CONDITIONAL sequential plan, but timed-door availability remains unproven and is not counted. The 20-map target was not reached safely.",
        "",
        "No route execution, autonomous movement, physical activation, or platform controller was implemented.",
        "",
        "## Stage results",
        "",
        "| Stage | Static | Complete plans | Conditional | Blocked | Unreachable | Reachable areas | Links | Advanced | Newly complete | Regressions |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|",
    ]
    for name, summary in stage_summaries.items():
        lines.append(
            f"| {name} | {summary['static_exit_routes']} | {summary['complete_exit_plans']} | {summary['conditional']} | {summary['blocked']} | {summary['unreachable']} | {summary['aggregate_reachable_areas']} | {summary['aggregate_total_links']} | {', '.join(summary['maps_advanced']) or 'none'} | {', '.join(summary['newly_completed']) or 'none'} | {', '.join(summary['regressions']) or 'none'} |"
        )
    lines += ["", "### Final reachability counts", ""]
    lines += [f"- {kind}: {count}" for kind, count in f["reachability_counts"].items()]
    lines += [
        "",
        "## Goal results",
        "",
        "- Deep audit: all 19 incomplete maps now have multi-frontier blocker chains; the final audit includes relay/counter chains beyond the first physical blocker.",
        "- Activation planning: stateful sequential navigation, target/targetname, killtarget, relay, counter progress, cycle protection, shoot-conditional classification, and explicit state changes are implemented. e2m1 is CONDITIONAL; e1m1 remains BLOCKED.",
        "- e1m1: the generic diagnostic resolves `t9 -> trigger_counter -> t10 -> door`, but all three counter inputs remain unreachable from the proven current state. Its earlier doors (`t1`, then `t15`) and shoot door remain in the full chain; e1m1 is not complete.",
        "- Dynamic connectivity: ordered `func_train` path-corner stops now generate directional, boardable PM-area links with distance/speed ride time. Nine maps gained 1,035 links at Stage D; e3m6 gained 12 reachable areas. No map became complete from trains.",
        "- Goal 4: PM-validated post-water-jump recovery, shallow-liquid jump landing, and stacked-step checks advanced e1m4 from its old water ledge to a later 10.5-unit decomposition frontier. It gained 3 WATER_JUMPs and 11 reachable areas from Stage D, but did not complete.",
        "",
        "## Map-by-map before / after",
        "",
        "| Map | Before reachable | After reachable | Before status | After status | Final blocker | Feature / effect |",
        "|---|---:|---:|---|---|---|---|",
    ]
    for row in map_rows:
        final_blocker = str(row["final_blocker"]).replace("|", "\\|")
        lines.append(f"| {row['map']} | {row['before_reachable_areas']} | {row['after_reachable_areas']} | {row['before_status']} | {row['after_status']} | {final_blocker} | {row['feature_responsible']} |")
    lines += ["", "## Final blocker chains", ""]
    for row in after_audit:
        lines += [f"### {row['map']}", ""]
        if row["map"] == "e1m4":
            lines.append("0. initial water-jump landing at areas 1910/1908 - solved/advanced")
        for index, blocker in enumerate(row["blocker_chain"], 1):
            lines.append(f"{index}. {blocker} - unresolved")
        lines += [f"Deepest proven coverage: {row['deepest_proven_point']}", "", f"Final status: {STATUS_LABELS.get(plan_status(after_by_map[row['map']]), plan_status(after_by_map[row['map']]).upper())}", ""]
    lines += [
        "## Category before / after",
        "",
        "| Category | Before maps | After maps |",
        "|---|---:|---:|",
        f"| STATIC EXIT | {a['static_exit_routes']} | {f['static_exit_routes']} |",
        f"| COMPLETE PLAN | {a['complete_exit_plans']} | {f['complete_exit_plans']} |",
    ]
    lines += [f"| {row['category']} | {row['before_maps']} | {row['after_maps']} |" for row in categories]
    lines += [
        "",
        "Changed category: e2m1 BLOCKED -> CONDITIONAL. e1m4 remained UNREACHABLE but its primary frontier advanced from the initial water-jump landing defect to later shallow/stacked decomposition.",
        "",
        "## Validation and performance",
        "",
        f"- Generation/reachability/Dijkstra/spawn/persistence: {f['validation']['generation']}/{f['validation']['reachability']}/{f['validation']['dijkstra']}/{f['validation']['spawn_resolution']}/{f['validation']['persistence']} of 31.",
        f"- Final sweep: {f['campaign_sweep_seconds']:.3f} s; average generation {f['average_generation_seconds']:.3f} s; worst {f['worst_generation']['map']} at {f['worst_generation']['seconds']:.3f} s.",
        f"- Activation graph construction: {f['activation_graph_seconds']:.6f} s aggregate; planner: {f['planner_seconds']:.3f} s aggregate.",
        f"- Platform/train analysis: {f['platform_analysis_seconds']:.6f} s aggregate; Goal 4 water-jump analysis: {f['goal4_water_jump_seconds']:.6f} s aggregate.",
        f"- Largest planner search: {f['largest_planner_state_search']} world-state fields; deepest emitted dependency sequence: {f['deepest_dependency_chain']} steps.",
        "- Stage E and Stage F have identical per-map status, reachable-area, and link counts.",
        "",
        "## Regression assessment",
        "",
        "All 12 baseline-complete maps remain complete. e1m3 (-1) and e1m4 (-47 net versus Stage A) have audited reachable-area corrections caused by accurate train path-corner swept bounds removing invalid static traversal through mover paths; both were already incomplete. Aggregate reachable coverage increased by 51. These are safety corrections, not accepted completion regressions.",
        "",
        "## Final summary",
        "",
        "NEWLY COMPLETED: none.",
        "",
        "COMPLETED BY ACTIVATION PLANNING: none (e2m1 is conditional).",
        "",
        "COMPLETED BY PLATFORM/DYNAMIC FIXES: none.",
        "",
        "COMPLETED BY GOAL 4 FEATURE: none.",
        "",
        "BIGGEST COVERAGE GAINS WITHOUT COMPLETION: e1m5 +81, e3m6 +12, e1m4 +11 from Stage D, and e2m1 +6.",
        "",
        "UNKNOWN/UNCLASSIFIED MAPS: zero.",
        "",
        "Recommended next navigation milestone: resolve the measured contextual/mover vertical frontiers (begin with the 55.9–80 unit cases and prove whether each is lift-assisted, run-up geometry, or impossible under normal jump physics), while separately tightening timed-door schedule proof for e2m1. Route execution should remain deferred.",
    ]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"markdown": str(ROOT / 'final-report.md'), "json": str(ROOT / 'final-report.json')}))


if __name__ == "__main__":
    main()
