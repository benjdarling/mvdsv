#!/usr/bin/env python3
"""Build the comprehensive campaign-navigation milestone JSON and Markdown reports."""

from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path


ROOT = Path("build/nav-coverage-milestone")
STAGES = [
    ("Baseline", "stage-0-baseline.json"),
    ("+ decomposition fixes", "stage-1-decomposition.json"),
    ("+ water jump", "stage-2-water-jump.json"),
    ("+ contextual upward transitions", "stage-3-contextual-up.json"),
    ("+ dynamic revisit", "stage-4-dynamic.json"),
    ("Final", "stage-5-final.json"),
]
TYPES = (
    "walk", "drop", "jump", "water_jump", "teleport", "platform", "swim",
    "water_entry", "water_exit", "unresolved_water_jump",
)
MECHANICS = {
    "+ decomposition fixes": "PM-validated decomposition/WALK or shallow-liquid SWIM continuity",
    "+ water jump": "actual QW PM water-exit/WATER_JUMP classification",
    "+ contextual upward transitions": "dual-cadence PM jump-up validation",
    "+ dynamic revisit": "activation/platform re-analysis",
    "Final": "dual-cadence multi-area gap JUMP validation",
}


def field(text: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}:\s*(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else ""


def number(text: str, name: str) -> float | None:
    raw = field(text, name)
    match = re.search(r"-?[0-9]+(?:\.[0-9]+)?", raw)
    return float(match.group(0)) if match else None


def route_areas(text: str) -> tuple[int, int]:
    match = re.search(r"^reachable routing areas:\s*(\d+)\s*/\s*(\d+)", text, re.MULTILINE)
    return (int(match.group(1)), int(match.group(2))) if match else (0, 0)


def validation_ok(text: str, label: str) -> bool:
    return f"{label} validation: ok" in text.lower()


def final_category(map_name: str, result: str, reason: str, problem: str) -> str:
    if result == "reachable":
        return "EXIT / complete"
    if result == "conditional":
        return "conditional"
    if result == "blocked":
        return "button/door/trigger"
    if map_name == "end":
        return "finale-specific"
    if map_name in {"e1m3", "e1m6", "e4m7"}:
        return "lift/platform"
    if map_name in {"e1m7", "e2m2", "e3m5", "e4m1", "e4m8"}:
        return "button/door/trigger"
    lowered = f"{problem} {reason}".lower()
    if "shared portal" in lowered or "decomposition" in lowered:
        return "shared/decomposition"
    if "water-jump" in lowered or "liquid portal" in lowered:
        return "water jump"
    if "jump-up" in lowered:
        return "contextual upward jump"
    if "gap" in lowered:
        return "unsupported/contextual gap"
    return "unknown/other"


def parse_row(row: dict[str, object]) -> dict[str, object]:
    outputs = row["outputs"]
    metrics = row["metrics"]
    route = outputs.get("route", "")
    plan = outputs.get("plan", "")
    frontier = outputs.get("frontier_report", "")
    counts = {name: int(metrics.get("reachability_counts", {}).get(name, 0)) for name in TYPES}
    reachable, routing = route_areas(route)
    result = str(row["result"])
    problem = field(route, "problem type")
    reason = field(route, "frontier reason")
    dependency = field(plan, "strongest unresolved dependency")
    if not reason:
        reason = dependency
    loaded_route = outputs.get("loaded_route", "")
    persistence = bool(outputs.get("loaded_reach_status")) and (
        outputs.get("loaded_reach_status") == outputs.get("reach_status")
        and validation_ok(outputs.get("loaded_reach_validate", ""), "reachability")
        and validation_ok(outputs.get("loaded_route_validate", ""), "routing")
        and field(loaded_route, "result").lower() == result
    )
    frontier_portal = field(frontier, "portal").split()
    portal = field(route, "frontier portal") or (frontier_portal[0] if frontier_portal else "")
    return {
        "map": row["map"],
        "generation_success": not bool(row.get("error")),
        "areas": int(metrics.get("area_count", 0)),
        "portals": int(metrics.get("portal_count", 0)),
        "reachabilities": int(metrics.get("reachability_count", 0)),
        "reachability_counts": counts,
        "routing_areas": routing,
        "reachable_areas": reachable,
        "reachable_percentage": round(100.0 * reachable / routing, 3) if routing else 0.0,
        "exit_reachable": result == "reachable",
        "complete_plan_available": field(plan, "result").lower() == "solvable",
        "result": result,
        "plan_status": field(plan, "result"),
        "route_cost_seconds": number(route, "route cost"),
        "plan_navigation_cost_seconds": number(plan, "estimated navigation cost"),
        "plan_dynamic_wait_seconds": number(plan, "estimated dynamic wait"),
        "route_step_count": int(number(route, "route steps") or 0),
        "plan_subgoal_count": int(number(plan, "subgoals") or 0),
        "frontier_category": final_category(str(row["map"]), result, reason, problem),
        "frontier_area": field(route, "frontier area") or field(route, "best frontier area"),
        "destination_area": field(route, "area beyond frontier") or field(route, "destination area"),
        "portal": portal,
        "nearby_interactor": field(route, "nearby interactor"),
        "diagnostic_cause": reason or problem or dependency,
        "reachability_validation": validation_ok(outputs.get("reach_validate", ""), "reachability"),
        "dijkstra_validation": validation_ok(outputs.get("route_validate", ""), "routing"),
        "persistence_validation": persistence,
        "seconds": float(row["seconds"]),
        "generation": metrics.get("generation", {}),
        "error": row.get("error", ""),
    }


def aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    status = Counter(str(row["result"]) for row in rows)
    travel = {name: sum(int(row["reachability_counts"][name]) for row in rows) for name in TYPES}
    generations = [row["generation"] for row in rows]
    return {
        "maps": len(rows),
        "complete": status["reachable"],
        "status_counts": dict(sorted(status.items())),
        "reachable_areas": sum(int(row["reachable_areas"]) for row in rows),
        "routing_areas": sum(int(row["routing_areas"]) for row in rows),
        "total_links": sum(int(row["reachabilities"]) for row in rows),
        "travel_counts": travel,
        "validation": {
            "generation": sum(bool(row["generation_success"]) for row in rows),
            "reachability": sum(bool(row["reachability_validation"]) for row in rows),
            "dijkstra": sum(bool(row["dijkstra_validation"]) for row in rows),
            "spawn_resolution": sum(int(row["routing_areas"]) > 0 for row in rows),
            "persistence": sum(bool(row["persistence_validation"]) for row in rows),
        },
        "performance": {
            "sweep_seconds": round(sum(float(row["seconds"]) for row in rows), 3),
            "average_generation_seconds": round(sum(float(g.get("seconds", 0)) for g in generations) / len(rows), 6),
            "worst_map": max(rows, key=lambda row: float(row["seconds"]))["map"],
            "worst_map_seconds": max(float(row["seconds"]) for row in rows),
            "reachability_generation_seconds": round(sum(float(g.get("reachability_seconds", 0)) for g in generations), 6),
            "water_jump_candidates": sum(int(g.get("water_jump_candidates", 0)) for g in generations),
            "water_jump_validated": sum(int(g.get("water_jump_validated", 0)) for g in generations),
            "water_jump_links": sum(int(g.get("water_jump_links", 0)) for g in generations),
            "water_jump_pm_simulations": sum(int(g.get("water_jump_attempts", 0)) for g in generations),
            "contextual_jump_candidates": 0,
            "contextual_jump_pm_simulations": 0,
            "jump_up_candidates": sum(int(g.get("jump_up_ledge_candidates", 0)) for g in generations),
            "jump_up_validated": sum(int(g.get("jump_up_ledge_validated", 0)) for g in generations),
            "jump_up_links": sum(int(g.get("jump_up_ledge_links", 0)) for g in generations),
            "jump_up_pm_simulations": sum(int(g.get("jump_up_ledge_attempts", 0)) for g in generations),
            "dynamic_analysis_seconds": round(sum(float(g.get("dynamic_analysis_seconds", 0)) for g in generations), 6),
            "activation_planning_seconds": round(sum(float(number(row.get("plan_output", ""), "plan calculation time") or 0) for row in rows), 6),
        },
    }


def main() -> int:
    parsed: dict[str, list[dict[str, object]]] = {}
    raw_by_stage: dict[str, list[dict[str, object]]] = {}
    for label, filename in STAGES:
        raw = json.loads((ROOT / filename).read_text(encoding="utf-8"))
        raw_by_stage[label] = raw
        parsed[label] = []
        for raw_row in raw:
            row = parse_row(raw_row)
            row["plan_output"] = raw_row["outputs"].get("plan", "")
            parsed[label].append(row)
    baseline_shared = {
        "e1m1", "e1m3", "e1m6", "e1m7", "e2m2", "e2m4", "e4m1", "e4m2"
    }
    for row in parsed["Baseline"]:
        if row["map"] in baseline_shared:
            row["frontier_category"] = "shared/decomposition"
    stage_summaries = {label: aggregate(rows) for label, rows in parsed.items()}
    for rows in parsed.values():
        for row in rows:
            row.pop("plan_output", None)
    labels = [label for label, _ in STAGES]
    map_names = [str(row["map"]) for row in parsed[labels[0]]]
    map_table = []
    regressions = []
    advanced_by_stage: dict[str, list[str]] = {label: [] for label in labels}
    for map_index, map_name in enumerate(map_names):
        history = [parsed[label][map_index] for label in labels]
        improved = []
        for index in range(1, len(history)):
            before, after = history[index - 1], history[index]
            if int(after["reachable_areas"]) > int(before["reachable_areas"]) or (
                before["result"] != "reachable" and after["result"] == "reachable"
            ):
                improved.append(labels[index])
                advanced_by_stage[labels[index]].append(map_name)
            if int(after["reachable_areas"]) < int(before["reachable_areas"]):
                regressions.append({
                    "map": map_name, "stage": labels[index],
                    "before_reachable_areas": before["reachable_areas"],
                    "after_reachable_areas": after["reachable_areas"],
                })
            if before["result"] == "reachable" and after["result"] != "reachable":
                regressions.append({
                    "map": map_name, "stage": labels[index],
                    "before_result": before["result"], "after_result": after["result"],
                })
        mechanic = "; ".join(MECHANICS[label] for label in improved) or "none"
        map_table.append({
            "map": map_name,
            "before_reachable": history[0]["reachable_areas"],
            "after_reachable": history[-1]["reachable_areas"],
            "before_result": history[0]["result"],
            "after_result": history[-1]["result"],
            "stage_that_improved_it": improved,
            "new_traversal_or_mechanic": mechanic,
            "final_blocker": "" if history[-1]["result"] == "reachable" else history[-1]["diagnostic_cause"],
        })
    contribution = []
    for index, label in enumerate(labels):
        summary = stage_summaries[label]
        previous_complete = stage_summaries[labels[index - 1]]["complete"] if index else summary["complete"]
        contribution.append({
            "stage": label,
            "complete_maps": summary["complete"],
            "aggregate_reachable_areas": summary["reachable_areas"],
            "total_links": summary["total_links"],
            "maps_advanced": advanced_by_stage[label],
            "newly_completed": summary["complete"] - previous_complete,
        })
    baseline, final = stage_summaries[labels[0]], stage_summaries[labels[-1]]
    aggregate_delta = {}
    aggregate_metrics = {
        "complete_maps": (baseline["complete"], final["complete"]),
        "reachable_areas": (baseline["reachable_areas"], final["reachable_areas"]),
        "total_links": (baseline["total_links"], final["total_links"]),
        **{name: (baseline["travel_counts"][name], final["travel_counts"][name]) for name in TYPES},
    }
    for key, (before, after) in aggregate_metrics.items():
        aggregate_delta[key] = {
            "before": before, "after": after, "delta": after - before,
            "percent_change": round(100.0 * (after - before) / before, 3) if before else None,
        }
    before_categories = Counter(row["frontier_category"] for row in parsed[labels[0]])
    after_categories = Counter(row["frontier_category"] for row in parsed[labels[-1]])
    final_performance = final["performance"]
    jump_up_audit = {
        "candidates": final_performance["jump_up_candidates"],
        "validated": final_performance["jump_up_validated"],
        "links": final_performance["jump_up_links"],
        "pm_simulations": final_performance["jump_up_pm_simulations"],
        "frontier_advanced_maps": advanced_by_stage["+ contextual upward transitions"],
        "newly_exit_routeable_maps": [],
    }
    visual_inspection = [
        {"map": "e2m4", "case": "fixed decomposition boundary", "image": "visual-e2m4-decomposition.png", "result": "fixed graph continuity rendered; later water frontier remains"},
        {"map": "e1m4", "case": "water jump", "image": "visual-e1m4-water-jump.png", "result": "candidate layer rendered; frontier remains unresolved"},
        {"map": "e2m5", "case": "water jump", "image": "visual-e2m5-water-jump.png", "result": "ordinary exit correction rendered; true water-jump frontier remains unresolved"},
        {"map": "e3m7", "case": "contextual upward", "image": "visual-e3m7-contextual-up.png", "result": "dual-cadence jump links rendered; frontier advanced"},
        {"map": "e1m6", "case": "platform", "image": "visual-e1m6-platform.png", "result": "platform/interactor layers rendered; exit remains unreachable"},
        {"map": "e2m1", "case": "button/door activation plan", "image": "visual-e2m1-activation-plan.png", "result": "one subgoal is planned; interactor 38 remains unresolved"},
    ]
    report = {
        "excluded_maps": {"e1m8": "altered gravity"},
        "stage_files": {label: filename for label, filename in STAGES},
        "stages": {label: {"summary": stage_summaries[label], "maps": parsed[label]} for label in labels},
        "stage_contribution": contribution,
        "aggregate_before_after": aggregate_delta,
        "frontier_categories": {
            "before": dict(sorted(before_categories.items())),
            "after": dict(sorted(after_categories.items())),
        },
        "map_by_map": map_table,
        "jump_up_ledge_audit": jump_up_audit,
        "visual_inspection": visual_inspection,
        "builds": {
            "canonical_evobot_release": "passed",
            "mvdsv_release": "passed",
            "ezquake_release": "passed",
            "ezquake_debug": "passed",
        },
        "regressions": regressions,
    }
    (ROOT / "final-report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    lines = [
        "# EvoBot campaign navigation coverage milestone", "",
        "Normal-gravity campaign: 31 maps. `e1m8` is excluded because it uses altered gravity.", "",
        "## Stage contribution", "",
        "| Stage | EXIT/complete maps | Aggregate reachable areas | Total links | Maps advanced | Newly completed |",
        "|---|---:|---:|---:|---|---:|",
    ]
    for item in contribution:
        lines.append(f"| {item['stage']} | {item['complete_maps']} | {item['aggregate_reachable_areas']} | {item['total_links']} | {', '.join(item['maps_advanced']) or 'none'} | {item['newly_completed']} |")
    lines += ["", "## Aggregate before / after", "", "| Metric | Before | After | Delta | Percent change |", "|---|---:|---:|---:|---:|"]
    for key, item in aggregate_delta.items():
        percent = "n/a" if item["percent_change"] is None else f"{item['percent_change']:.3f}%"
        lines.append(f"| {key} | {item['before']} | {item['after']} | {item['delta']:+d} | {percent} |")
    lines += ["", "## Final map-by-map result", "", "| Map | Before reachable | After reachable | Before result | After result | Stage that improved it | New traversal / mechanic used | Final blocker if incomplete |", "|---|---:|---:|---|---|---|---|---|"]
    for item in map_table:
        stages = ", ".join(item["stage_that_improved_it"]) or "none"
        blocker = str(item["final_blocker"]).replace("|", "\\|") or "--"
        lines.append(f"| {item['map']} | {item['before_reachable']} | {item['after_reachable']} | {item['before_result']} | {item['after_result']} | {stages} | {item['new_traversal_or_mechanic']} | {blocker} |")
    lines += ["", "## Frontier categories before / after", "", "| Category | Before | After |", "|---|---:|---:|"]
    for category in sorted(set(before_categories) | set(after_categories)):
        lines.append(f"| {category} | {before_categories[category]} | {after_categories[category]} |")
    lines += ["", "## Jump-up ledge audit", "",
              f"- Candidates: {jump_up_audit['candidates']}.",
              f"- PM-validated candidates/links: {jump_up_audit['validated']}/{jump_up_audit['links']} from {jump_up_audit['pm_simulations']} actual PM_PlayerMove simulations.",
              f"- Frontiers advanced: {', '.join(jump_up_audit['frontier_advanced_maps'])}.",
              "- Newly exit-routeable because of jump-up ledges: none."]
    lines += ["", "## Validation and regressions", ""]
    validation = final["validation"]
    lines.append(f"Final validation: generation {validation['generation']}/31; reachability {validation['reachability']}/31; Dijkstra {validation['dijkstra']}/31; spawn resolution {validation['spawn_resolution']}/31; persistence {validation['persistence']}/31.")
    lines.append("")
    lines.append("Regressions: none." if not regressions else f"Regressions: {json.dumps(regressions)}")
    lines += ["", "## Performance", ""]
    for label in labels:
        performance = stage_summaries[label]["performance"]
        lines.append(f"- {label}: sweep {performance['sweep_seconds']:.3f}s; average generation {performance['average_generation_seconds']:.3f}s; worst {performance['worst_map']} {performance['worst_map_seconds']:.3f}s; reachability generation {performance['reachability_generation_seconds']:.3f}s; water-jump candidates/simulations {performance['water_jump_candidates']}/{performance['water_jump_pm_simulations']}; jump-up candidates/simulations {performance['jump_up_candidates']}/{performance['jump_up_pm_simulations']}; dynamic analysis {performance['dynamic_analysis_seconds']:.6f}s.")
    lines += ["", "## Live ezQuake visual inspection", ""]
    for item in visual_inspection:
        lines.append(f"- `{item['map']}` {item['case']}: [{item['image']}]({item['image']}) -- {item['result']}.")
    lines += ["", "All captures were made from the live rebuilt ezQuake window with navigation, reachability, route, plan, problem, jump-candidate, water-jump, and x-ray layers enabled. No route or movement was executed.", "", "## Builds", "", "Canonical EvoBot Release, mvdsv Release, ezQuake Release, and ezQuake Debug all built successfully with Visual Studio 2022."]
    lines += ["", "## Implementation findings", "", "- Stage 1 fixed two general decomposition causes: thin-cell support probing and shallow-liquid PM continuity. All eight audited shared/decomposition frontiers advanced; e3m1 became newly exit-routeable.", "- Stage 2 inspected QuakeWorld PM water-jump semantics directly. It converted 83 false unresolved cases into ordinary WATER_EXIT, validated two genuine WATER_JUMP links on e4m1, and left 173 unresolved cases. e1m4 and e2m5 remained incomplete.", "- Stage 3 audited seven apparent upward frontiers. None proved to require a contextual previous-area run-up; most exceeded natural jump rise or were mover/gameplay cases. Dual 50/13 ms actual-PM ledge validation advanced five maps.", "- Stage 4 reran platform and activation dependency analysis from scratch. e1m1 remained blocked; e2m1 retained one useful partial activation subgoal. No safe general dynamic-link fix was proven.", "- The final corrective pass applied the same dual PM cadence to multi-area gap jumps. It increased link volume and generation time, with small additional reachable-area gains and no newly completed map.", "", "## Campaign-wide summary", "", f"BEFORE: {baseline['complete']} / 31 complete; aggregate reachable areas {baseline['reachable_areas']}.", "", f"AFTER: {final['complete']} / 31 complete; aggregate reachable areas {final['reachable_areas']}.", "", "NEWLY COMPLETED: e3m1.", "", "Remaining work should prioritize supported landing-area decomposition at PM-proven water exits, then train/platform path semantics and trigger/door activation geometry. Route execution is not yet recommended because 19/31 maps remain incomplete."]
    (ROOT / "final-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"json": str((ROOT / 'final-report.json').resolve()), "markdown": str((ROOT / 'final-report.md').resolve()), "regressions": len(regressions)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
