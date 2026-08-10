#!/usr/bin/env python3
"""Rank entity-aware bridges between spawn and exit nav components."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
import json
import math
from pathlib import Path


def flood(seeds: list[int], graph: dict[int, set[int]]) -> set[int]:
    seen = set(seeds)
    queue = deque(seeds)
    while queue:
        source = queue.popleft()
        for destination in graph[source]:
            if destination not in seen:
                seen.add(destination)
                queue.append(destination)
    return seen


def intersects(a: dict, b: dict) -> bool:
    return all(a["maxs"][axis] >= b["mins"][axis] and
               a["mins"][axis] <= b["maxs"][axis] for axis in range(3))


def distance(a: dict, b: dict) -> float:
    return math.sqrt(sum(max(0.0, a["mins"][axis] - b["maxs"][axis],
                             b["mins"][axis] - a["maxs"][axis]) ** 2
                         for axis in range(3)))


def expanded_access(bounds: dict, player: dict, margin: float = 8.0) -> dict:
    return {
        "mins": [
            bounds["mins"][0] - player["maxs"][0] - margin,
            bounds["mins"][1] - player["maxs"][1] - margin,
            bounds["maxs"][2] - player["mins"][2] - 4.0,
        ],
        "maxs": [
            bounds["maxs"][0] - player["mins"][0] + margin,
            bounds["maxs"][1] - player["mins"][1] + margin,
            bounds["maxs"][2] - player["mins"][2] + 4.0,
        ],
    }


def exit_areas(nav: dict, areas: dict[int, dict]) -> list[int]:
    player = nav["player_bounds"]
    goals = []
    for interactor in nav["interactors"]:
        if interactor["type"] != "level_exit":
            continue
        bounds = {
            "mins": [interactor["bounds"]["mins"][axis] - player["maxs"][axis]
                     for axis in range(3)],
            "maxs": [interactor["bounds"]["maxs"][axis] - player["mins"][axis]
                     for axis in range(3)],
        }
        for area_id, area in areas.items():
            if (area["supported"] or area["water_level"] > 0) and intersects(area["bounds"], bounds):
                goals.append(area_id)
    return sorted(set(goals))


def nearest_component_areas(region: dict, component: set[int], areas: dict[int, dict], limit: int = 3) -> list[dict]:
    ranked = sorted((distance(region, areas[area_id]["bounds"]), area_id)
                    for area_id in component if area_id in areas)
    return [{"area": area_id, "distance": round(value, 3)}
            for value, area_id in ranked[:limit]]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nav", type=Path, required=True)
    parser.add_argument("--source", type=int, required=True)
    parser.add_argument("--limit", type=int, default=30)
    args = parser.parse_args()
    nav = json.loads(args.nav.read_text(encoding="utf-8"))
    areas = {int(area["id"]): area for area in nav["areas"]}
    graph: dict[int, set[int]] = defaultdict(set)
    reverse_graph: dict[int, set[int]] = defaultdict(set)
    for reachability in nav["reachabilities"]:
        source = int(reachability["source_area"])
        destination = int(reachability["destination_area"])
        graph[source].add(destination)
        reverse_graph[destination].add(source)
    goals = exit_areas(nav, areas)
    forward = flood([args.source], graph)
    reverse = flood(goals, reverse_graph)
    player = nav["player_bounds"]
    activators: dict[str, list[int]] = defaultdict(list)
    for interactor in nav["interactors"]:
        if interactor.get("target"):
            activators[interactor["target"]].append(int(interactor["id"]))

    candidates = []
    for interactor in nav["interactors"]:
        kind = interactor["type"]
        stops = interactor.get("movement_stop_bounds", [])
        if kind in {"platform", "train"} and stops:
            stop_rows = []
            best_forward = math.inf
            best_reverse = math.inf
            for index, stop in enumerate(stops):
                region = expanded_access(stop, player)
                near_forward = nearest_component_areas(region, forward, areas)
                near_reverse = nearest_component_areas(region, reverse, areas)
                best_forward = min(best_forward, near_forward[0]["distance"] if near_forward else math.inf)
                best_reverse = min(best_reverse, near_reverse[0]["distance"] if near_reverse else math.inf)
                stop_rows.append({
                    "stop": index,
                    "bounds": stop,
                    "forward": near_forward,
                    "reverse": near_reverse,
                })
            score = best_forward + best_reverse
            candidates.append({
                "score": score,
                "id": int(interactor["id"]),
                "kind": kind,
                "class": interactor["classname"],
                "model": interactor["model"],
                "activation": interactor["activation"],
                "target": interactor["target"],
                "targetname": interactor["targetname"],
                "speed": interactor["speed"],
                "wait": interactor["wait"],
                "activators": activators.get(interactor.get("targetname", ""), []),
                "stops": stop_rows,
            })
        elif kind in {"door", "teleporter", "trigger", "button"}:
            near_forward = nearest_component_areas(interactor["swept_bounds"], forward, areas, 1)
            near_reverse = nearest_component_areas(interactor["swept_bounds"], reverse, areas, 1)
            score = ((near_forward[0]["distance"] if near_forward else math.inf) +
                     (near_reverse[0]["distance"] if near_reverse else math.inf))
            if score <= 256.0 or (near_forward and near_forward[0]["distance"] == 0) or (near_reverse and near_reverse[0]["distance"] == 0):
                candidates.append({
                    "score": score,
                    "id": int(interactor["id"]),
                    "kind": kind,
                    "class": interactor["classname"],
                    "model": interactor["model"],
                    "activation": interactor["activation"],
                    "target": interactor["target"],
                    "targetname": interactor["targetname"],
                    "activators": activators.get(interactor.get("targetname", ""), []),
                    "forward": near_forward,
                    "reverse": near_reverse,
                })
    candidates.sort(key=lambda item: (item["score"], item["kind"], item["id"]))
    output = {
        "map": nav.get("map"),
        "source": args.source,
        "exit_areas": goals,
        "source_component": len(forward),
        "exit_reverse_component": len(reverse),
        "overlap": len(forward & reverse),
        "candidate_bridges": candidates[:args.limit],
    }
    print(json.dumps(output, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
