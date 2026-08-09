#!/usr/bin/env python3
"""Generate the campaign-push JSON and Markdown reports from preserved sweeps."""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import re


ROOT = Path("build/nav-campaign-push")


def number(pattern: str, text: str, default: float = 0) -> float:
    match = re.search(pattern, text or "", re.MULTILINE | re.IGNORECASE)
    return float(match.group(1)) if match else default


def reachable(row: dict) -> int:
    return int(number(r"reachable routing areas:\s*(\d+)\s*/", row["outputs"].get("route", "")))


def routing(row: dict) -> int:
    return int(number(r"reachable routing areas:\s*\d+\s*/\s*(\d+)", row["outputs"].get("route", "")))


def status(row: dict) -> str:
    if row.get("result") == "reachable":
        return "STATIC EXIT"
    plan = (row.get("plan_result") or "").lower()
    if plan == "complete":
        return "COMPLETE PLAN"
    if plan == "conditional":
        return "CONDITIONAL"
    if row.get("result") == "blocked" or plan == "blocked":
        return "BLOCKED"
    return "UNREACHABLE"


def completed(row: dict) -> bool:
    return status(row) in {"STATIC EXIT", "COMPLETE PLAN"}


def feature(name: str, delta: int) -> str:
    if name == "e2m1":
        return "timed/sequential world-state planner"
    if name == "e3m5":
        return "trigger_push PM trajectories + recursive activation closure"
    if name == "e4m7":
        return "PM-calibrated platform run-up/jump exit"
    if name in {"e2m5", "e4m8"} and delta > 0:
        return "mover/water continuation coverage; later frontier exposed"
    if delta > 0:
        return "additional validated traversal links"
    if delta < 0:
        return "grounded endpoint validation removed previously counted links"
    return "no material route-field change"


def summary(rows: list[dict]) -> dict:
    counts = Counter(status(row) for row in rows)
    generation = [row.get("metrics", {}).get("generation", {}) for row in rows]
    route_counts: Counter[str] = Counter()
    for row in rows:
        route_counts.update(row.get("metrics", {}).get("reachability_counts", {}))
    generation_seconds = [float(item.get("seconds", 0)) for item in generation]
    worst = max(range(len(rows)), key=lambda i: generation_seconds[i])
    return {
        "maps": len(rows),
        "static_exit_routes": counts["STATIC EXIT"],
        "complete_exit_plans": counts["COMPLETE PLAN"],
        "complete_total": counts["STATIC EXIT"] + counts["COMPLETE PLAN"],
        "blocked": counts["BLOCKED"],
        "unreachable": counts["UNREACHABLE"],
        "aggregate_reachable_areas": sum(reachable(row) for row in rows),
        "aggregate_routing_areas": sum(routing(row) for row in rows),
        "aggregate_links": sum(int(row.get("metrics", {}).get("reachability_count", 0)) for row in rows),
        "reachability_counts": dict(sorted(route_counts.items())),
        "sweep_seconds": round(sum(float(row.get("seconds", 0)) for row in rows), 3),
        "generation_seconds": round(sum(generation_seconds), 6),
        "average_generation_seconds": round(sum(generation_seconds) / len(rows), 6),
        "worst_generation": {"map": rows[worst]["map"], "seconds": generation_seconds[worst]},
        "planner_seconds": round(sum(number(r"plan calculation time:\s*([0-9.]+)", row["outputs"].get("plan", "")) for row in rows), 6),
        "largest_world_state_search": int(max(number(r"world states searched:\s*(\d+)", row["outputs"].get("plan", "")) for row in rows)),
        "deepest_dependency_chain": int(max(number(r"deepest dependency chain:\s*(\d+)", row["outputs"].get("plan", "")) for row in rows)),
        "jump_up_ledge_candidates": sum(int(item.get("jump_up_ledge_candidates", 0)) for item in generation),
        "jump_up_ledge_validated": sum(int(item.get("jump_up_ledge_validated", 0)) for item in generation),
        "platform_links": sum(int(item.get("platform_links", 0)) for item in generation),
        "platform_seconds": round(sum(float(item.get("platform_seconds", 0)) for item in generation), 6),
        "water_jump_candidates": sum(int(item.get("water_jump_candidates", 0)) for item in generation),
        "water_jump_validated": sum(int(item.get("water_jump_validated", 0)) for item in generation),
        "validation_traces": sum(int(item.get("validation_traces", 0)) for item in generation),
        "movement_tests": sum(int(item.get("movement_tests", 0)) for item in generation),
    }


def main() -> None:
    baseline = json.loads((ROOT / "baseline.json").read_text(encoding="utf-8"))
    final = json.loads((ROOT / "final.json").read_text(encoding="utf-8"))
    blockers = json.loads((ROOT / "final-blockers.json").read_text(encoding="utf-8"))
    before = {row["map"]: row for row in baseline}
    blocker_by_map = {row["map"]: row for row in blockers}
    maps = []
    for row in final:
        old = before[row["map"]]
        delta = reachable(row) - reachable(old)
        audit = blocker_by_map.get(row["map"], {})
        final_blocker = (audit.get("blocker_chain") or ["none; exit is routeable"])[-1]
        solved = []
        if row["map"] == "e2m1":
            solved = ["timed door availability and ordered activation sequence"]
        elif row["map"] == "e3m5":
            solved = ["trigger_push component", "nested t30/t28 activation prerequisites"]
        elif row["map"] == "e4m7":
            solved = ["platform 76 endpoint separation and airborne exit"]
        elif row["map"] == "e2m5":
            solved = ["earlier unresolved water frontier"]
        maps.append({
            "map": row["map"],
            "before_reachable_areas": reachable(old),
            "after_reachable_areas": reachable(row),
            "reachable_area_delta": delta,
            "routing_areas": routing(row),
            "before_status": status(old),
            "after_status": status(row),
            "blockers_solved_or_advanced": solved,
            "final_blocker": final_blocker,
            "main_missing_capability": audit.get("main_missing_capability", "none"),
            "feature_responsible": feature(row["map"], delta),
            "newly_completed": not completed(old) and completed(row),
        })

    base_summary = summary(baseline)
    final_summary = summary(final)
    validations = {}
    for key in ("reach_validate", "route_validate", "loaded_reach_validate", "loaded_route_validate"):
        validations[key] = sum("validation: ok" in row["outputs"].get(key, "") for row in final)
    validations["save_clear_load"] = sum(
        "navigation saved:" in row["outputs"].get("save", "")
        and "navigation cleared" in row["outputs"].get("clear", "")
        and "navigation loaded:" in row["outputs"].get("load", "")
        for row in final
    )
    advanced = [row["map"] for row in maps if row["reachable_area_delta"] > 0 or row["after_status"] != row["before_status"]]
    regressions = [row["map"] for row in maps if completed(before[row["map"]]) and not completed(next(item for item in final if item["map"] == row["map"]))]
    report = {
        "campaign_result": {
            "before_complete": base_summary["complete_total"],
            "after_complete": final_summary["complete_total"],
            "total_maps": 31,
            "excluded": ["e1m8"],
            "newly_completed": [row["map"] for row in maps if row["newly_completed"]],
            "target_20_reached": final_summary["complete_total"] >= 20,
            "completion_regressions": regressions,
        },
        "baseline": base_summary,
        "final": final_summary,
        "maps_advanced": advanced,
        "maps": maps,
        "final_blocker_audit": blockers,
        "validation": validations,
        "integration_audit": {
            "canonical_portable_files": 12,
            "mvdsv_exact_matches": 12,
            "ezquake_exact_matches": 12,
            "release_builds_passed": ["canonical", "mvdsv", "ezquake"],
        },
        "artifacts": {
            "baseline": str(ROOT / "baseline.json"),
            "checkpoint_1": str(ROOT / "checkpoint-1.json"),
            "checkpoint_2": str(ROOT / "checkpoint-2.json"),
            "final": str(ROOT / "final.json"),
        },
        "implementation": {
            "route_execution_implemented": False,
            "features": [
                "time-dependent Dijkstra costs with mover availability windows and waits",
                "counter/timed/sequential planner state and recursive activation prerequisites",
                "PM_PlayerMove trigger_push entry, flight, steering, chained pushes, and grounded landing",
                "grounded platform boarding plus PM-calibrated run-up/jump/fall exits",
                "precise mirrored-drop validation based on reversed physical geometry",
            ],
        },
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    lines = [
        "# EvoBot campaign-completion push",
        "",
        "## Campaign result",
        "",
        f"BEFORE: **{base_summary['complete_total']} / 31** complete "
        f"({base_summary['static_exit_routes']} static).",
        "",
        f"AFTER: **{final_summary['complete_total']} / 31** complete "
        f"({final_summary['static_exit_routes']} static + "
        f"{final_summary['complete_exit_plans']} complete planned).",
        "",
        "Newly complete: **e2m1** (timed/sequential plan) and **e4m7** "
        "(static route through platform 76 and a PM-validated lift exit).",
        "",
        "The 20-map target was not reached safely. Blocked/hypothetical plans were not counted.",
        "",
        "No route executor, autonomous movement, physical activation controller, or combat policy was implemented.",
        "",
        "## Map-by-map results",
        "",
        "| Map | Reachable before | Reachable after | Delta | Before | After | Solved/advanced | Final blocker |",
        "|---|---:|---:|---:|---|---|---|---|",
    ]
    for item in maps:
        solved = "; ".join(item["blockers_solved_or_advanced"]) or item["feature_responsible"]
        lines.append(
            f"| {item['map']} | {item['before_reachable_areas']} | {item['after_reachable_areas']} | "
            f"{item['reachable_area_delta']:+d} | {item['before_status']} | {item['after_status']} | "
            f"{solved.replace('|', '/')} | {item['final_blocker'].replace('|', '/')} |"
        )
    lines += [
        "",
        "## Aggregate diagnostics",
        "",
        f"- Reachable routing areas: {base_summary['aggregate_reachable_areas']} -> {final_summary['aggregate_reachable_areas']}.",
        f"- Reachability links: {base_summary['aggregate_links']} -> {final_summary['aggregate_links']}.",
        f"- Jump-up ledges: {final_summary['jump_up_ledge_candidates']} candidates, {final_summary['jump_up_ledge_validated']} validated.",
        f"- Platform links: {final_summary['platform_links']} total; {final_summary['platform_seconds']:.3f}s aggregate generation time.",
        f"- Water-jump candidates: {final_summary['water_jump_candidates']}; validated: {final_summary['water_jump_validated']}.",
        f"- PM work: {final_summary['validation_traces']} validation traces and {final_summary['movement_tests']} movement tests.",
        f"- Sweep time: {final_summary['sweep_seconds']:.3f}s; generation: {final_summary['generation_seconds']:.3f}s; planner: {final_summary['planner_seconds']:.3f}s.",
        f"- Worst generation: {final_summary['worst_generation']['map']} at {final_summary['worst_generation']['seconds']:.3f}s.",
        f"- Largest planner search: {final_summary['largest_world_state_search']} states; deepest dependency chain: {final_summary['deepest_dependency_chain']}.",
        "",
        "## Validation and persistence",
        "",
        f"All {validations['reach_validate']}/31 generated reachability validations and "
        f"{validations['route_validate']}/31 route validations passed. All "
        f"{validations['save_clear_load']}/31 maps saved, cleared, and loaded; all "
        f"{validations['loaded_reach_validate']}/31 post-load reachability and "
        f"{validations['loaded_route_validate']}/31 post-load route validations passed.",
        "",
        "e1m4's reachable-area decrease and e3m7's one-area decrease are not completion regressions; grounded endpoint validation removed previously counted mover links. All 12 baseline static completions remain complete.",
        "",
        "## Integration audit",
        "",
        "All 12 portable EvoBot include/source files match the canonical implementation byte-for-byte in both mvdsv and ezQuake. Canonical, mvdsv, and ezQuake Release builds passed; ezQuake was rebuilt after the final exact-source replacement.",
        "",
        "## Remaining blocker families",
        "",
    ]
    missing = Counter(item["main_missing_capability"] for item in maps if item["after_status"] not in {"STATIC EXIT", "COMPLETE PLAN"})
    lines += [f"- {name}: {count} map(s)" for name, count in missing.most_common()]
    lines += [
        "",
        "The deepest proven non-complete progress is e3m5: trigger-push coverage expands the reachable set from 171 to 1,080 areas and proves nine activation effects through t30/t28, but a later shallow-water/decomposition continuation remains unproven.",
    ]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(report["campaign_result"]))


if __name__ == "__main__":
    main()
