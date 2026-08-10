#!/usr/bin/env python3
"""Describe entity-state connections between directed nav components.

This is an offline diagnostic: it does not invent reachabilities.  It reports
which validated area components touch each captured mover stop, how existing
platform links use that mover, and the target graph that can change its state.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import json
from pathlib import Path
import sys
from typing import Iterable


def flood(seeds: Iterable[int], graph: dict[int, set[int]]) -> set[int]:
    seen = set(seeds)
    queue = deque(seen)
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


def expanded_touch(bounds: dict, player: dict, margin: float = 1.0) -> dict:
    return {
        "mins": [bounds["mins"][axis] - player["maxs"][axis] - margin
                 for axis in range(3)],
        "maxs": [bounds["maxs"][axis] - player["mins"][axis] + margin
                 for axis in range(3)],
    }


def standing_region(bounds: dict, player: dict, fringe: float = 16.0) -> dict:
    top = bounds["maxs"][2] - player["mins"][2]
    return {
        "mins": [bounds["mins"][0] - player["maxs"][0] - fringe,
                 bounds["mins"][1] - player["maxs"][1] - fringe,
                 top - 6.0],
        "maxs": [bounds["maxs"][0] - player["mins"][0] + fringe,
                 bounds["maxs"][1] - player["mins"][1] + fringe,
                 top + 6.0],
    }


def exit_areas(nav: dict, areas: dict[int, dict]) -> list[int]:
    goals = []
    for interactor in nav["interactors"]:
        if interactor["type"] != "level_exit":
            continue
        region = expanded_touch(interactor["bounds"], nav["player_bounds"])
        goals.extend(area_id for area_id, area in areas.items()
                     if (area["supported"] or area["water_level"] > 0) and
                     intersects(area["bounds"], region))
    return sorted(set(goals))


def label(area_id: int, forward: set[int], reverse: set[int]) -> str:
    if area_id in forward and area_id in reverse:
        return "both"
    if area_id in forward:
        return "spawn"
    if area_id in reverse:
        return "exit"
    return "intermediate"


def strongly_connected(nodes: Iterable[int], graph: dict[int, set[int]]) -> tuple[dict[int, int], dict[int, int]]:
    sys.setrecursionlimit(100000)
    index = 0
    stack: list[int] = []
    on_stack: set[int] = set()
    indices: dict[int, int] = {}
    low: dict[int, int] = {}
    raw_components: list[list[int]] = []

    def visit(node: int) -> None:
        nonlocal index
        indices[node] = index
        low[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for destination in graph[node]:
            if destination not in indices:
                visit(destination)
                low[node] = min(low[node], low[destination])
            elif destination in on_stack:
                low[node] = min(low[node], indices[destination])
        if low[node] == indices[node]:
            component = []
            while True:
                member = stack.pop()
                on_stack.remove(member)
                component.append(member)
                if member == node:
                    break
            raw_components.append(component)

    for node in nodes:
        if node not in indices:
            visit(node)
    raw_components.sort(key=lambda members: (-len(members), min(members)))
    mapping: dict[int, int] = {}
    sizes: dict[int, int] = {}
    for component_id, members in enumerate(raw_components, 1):
        sizes[component_id] = len(members)
        for member in members:
            mapping[member] = component_id
    return mapping, sizes


def region_areas(region: dict, areas: dict[int, dict], forward: set[int],
                 reverse: set[int], components: dict[int, int],
                 sccs: dict[int, int]) -> dict:
    ids = [area_id for area_id, area in areas.items()
           if (area["supported"] or area["water_level"] > 0) and
           intersects(area["bounds"], region)]
    counts = Counter(label(area_id, forward, reverse) for area_id in ids)
    return {
        "counts": dict(sorted(counts.items())),
        "physical_components": dict(sorted(Counter(
            str(components[area_id]) for area_id in ids).items(),
            key=lambda item: (-item[1], int(item[0])))),
        "strong_components": dict(sorted(Counter(
            str(sccs[area_id]) for area_id in ids).items(),
            key=lambda item: (-item[1], int(item[0])))),
        "areas": {name: sorted(area_id for area_id in ids
                               if label(area_id, forward, reverse) == name)[:24]
                  for name in ("spawn", "intermediate", "exit", "both")
                  if counts[name]},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nav", type=Path, required=True)
    parser.add_argument("--source", type=int, required=True)
    parser.add_argument("--destination", type=int, nargs="+",
                        help="override level-exit areas for a focused component audit")
    parser.add_argument("--relevant-only", action="store_true")
    args = parser.parse_args()

    nav = json.loads(args.nav.read_text(encoding="utf-8"))
    areas = {int(area["id"]): area for area in nav["areas"]}
    interactors = {int(item["id"]): item for item in nav["interactors"]}
    graph: dict[int, set[int]] = defaultdict(set)
    reverse_graph: dict[int, set[int]] = defaultdict(set)
    undirected: dict[int, set[int]] = defaultdict(set)
    mover_links: dict[int, list[dict]] = defaultdict(list)
    for reachability in nav["reachabilities"]:
        source = int(reachability["source_area"])
        destination = int(reachability["destination_area"])
        graph[source].add(destination)
        reverse_graph[destination].add(source)
        undirected[source].add(destination)
        undirected[destination].add(source)
        mover = int(reachability.get("mover_interactor", 0))
        if mover:
            mover_links[mover].append(reachability)

    goals = sorted(set(args.destination)) if args.destination else exit_areas(nav, areas)
    forward = flood([args.source], graph)
    reverse = flood(goals, reverse_graph)
    directed_pairs = {(int(item["source_area"]), int(item["destination_area"]))
                      for item in nav["reachabilities"]}
    frontier_portals = []
    for portal in nav.get("portals", []):
        area_a = int(portal["area_a"])
        area_b = int(portal["area_b"])
        if area_a in forward and area_b in reverse:
            source_area, destination_area = area_a, area_b
        elif area_b in forward and area_a in reverse:
            source_area, destination_area = area_b, area_a
        else:
            continue
        vertices = portal.get("vertices", [])
        center = ([sum(vertex[axis] for vertex in vertices) / len(vertices)
                   for axis in range(3)] if vertices else [0, 0, 0])
        source_floor = float(areas[source_area].get("floor_height", 0))
        destination_floor = float(areas[destination_area].get("floor_height", 0))
        frontier_portals.append({
            "portal": portal["id"],
            "source_area": source_area,
            "destination_area": destination_area,
            "source_floor": source_floor,
            "destination_floor": destination_floor,
            "floor_delta": destination_floor - source_floor,
            "center": center,
            "kind": portal.get("type", ""),
            "forward_link": (source_area, destination_area) in directed_pairs,
            "reverse_link": (destination_area, source_area) in directed_pairs,
        })
    frontier_portals.sort(key=lambda item: (abs(item["floor_delta"]),
                                            item["portal"]))
    sccs, scc_sizes = strongly_connected(areas, graph)
    scc_edges: dict[tuple[int, int], list[dict]] = defaultdict(list)
    scc_undirected: dict[int, set[int]] = defaultdict(set)
    for reachability in nav["reachabilities"]:
        source_scc = sccs[int(reachability["source_area"])]
        destination_scc = sccs[int(reachability["destination_area"])]
        if source_scc == destination_scc:
            continue
        scc_edges[(source_scc, destination_scc)].append(reachability)
        scc_undirected[source_scc].add(destination_scc)
        scc_undirected[destination_scc].add(source_scc)
    components: dict[int, int] = {}
    component_sizes: dict[int, int] = {}
    component_id = 0
    for area_id in areas:
        if area_id in components:
            continue
        component_id += 1
        members = flood([area_id], undirected)
        for member in members:
            components[member] = component_id
        component_sizes[component_id] = len(members)
    player = nav["player_bounds"]
    target_names: dict[str, list[int]] = defaultdict(list)
    for interactor in interactors.values():
        if interactor.get("targetname"):
            target_names[interactor["targetname"]].append(interactor["id"])

    activation_edges = []
    for source in interactors.values():
        for destination in target_names.get(source.get("target", ""), []):
            activation_edges.append({"source": source["id"],
                                     "destination": destination,
                                     "relation": "target"})
        for destination in target_names.get(source.get("killtarget", ""), []):
            activation_edges.append({"source": source["id"],
                                     "destination": destination,
                                     "relation": "killtarget"})

    rows = []
    for interactor in interactors.values():
        touch = region_areas(expanded_touch(interactor["bounds"], player),
                             areas, forward, reverse, components, sccs)
        row = {
            "id": interactor["id"],
            "type": interactor["type"],
            "classname": interactor["classname"],
            "model": interactor["model"],
            "activation": interactor["activation"],
            "lifetime": interactor["lifetime"],
            "target": interactor.get("target", ""),
            "targetname": interactor.get("targetname", ""),
            "killtarget": interactor.get("killtarget", ""),
            "inventory_grants": interactor.get("inventory_grants", 0),
            "inventory_requires": interactor.get("inventory_requires", 0),
            "bounds": interactor.get("bounds"),
            "endpoint_a_bounds": interactor.get("endpoint_a_bounds"),
            "endpoint_b_bounds": interactor.get("endpoint_b_bounds"),
            "touch_components": touch,
        }
        stops = interactor.get("movement_stop_bounds", [])
        endpoint_bounds = [interactor.get("endpoint_a_bounds"),
                           interactor.get("endpoint_b_bounds")]
        if all(endpoint_bounds) and endpoint_bounds[0] != endpoint_bounds[1]:
            row["endpoints"] = []
            for index, bounds in enumerate(endpoint_bounds):
                row["endpoints"].append({
                    "index": index,
                    "bounds": bounds,
                    "standing_components": region_areas(
                        standing_region(bounds, player), areas, forward, reverse,
                        components, sccs),
                })
        if stops:
            row["stops"] = []
            for index, bounds in enumerate(stops):
                row["stops"].append({
                    "index": index,
                    "bounds": bounds,
                    "standing_components": region_areas(
                        standing_region(bounds, player), areas, forward, reverse,
                        components, sccs),
                })
        links = mover_links.get(interactor["id"], [])
        if links:
            pairs = Counter((label(int(item["source_area"]), forward, reverse),
                             label(int(item["destination_area"]), forward, reverse))
                            for item in links)
            pair_modes = Counter((label(int(item["source_area"]), forward, reverse),
                                  label(int(item["destination_area"]), forward, reverse),
                                  "ride" if float(item.get("ride_time", 0)) > 0
                                  else "stopped")
                                 for item in links)
            strong_pairs = Counter((sccs[int(item["source_area"])],
                                    sccs[int(item["destination_area"])])
                                   for item in links)
            row["mover_links"] = {
                "count": len(links),
                "component_pairs": {f"{a}->{b}": count
                                    for (a, b), count in sorted(pairs.items())},
                "pair_modes": {f"{a}->{b}:{mode}": count
                               for (a, b, mode), count in sorted(pair_modes.items())},
                "strong_component_pairs": {f"{a}->{b}": count
                                           for (a, b), count in sorted(strong_pairs.items())},
                "ride_links": sum(float(item.get("ride_time", 0)) > 0
                                  for item in links),
                "board_or_exit_links": sum(float(item.get("ride_time", 0)) <= 0
                                           for item in links),
                "samples": [{
                    "id": item["id"],
                    "source_area": item["source_area"],
                    "destination_area": item["destination_area"],
                    "pair": (f"{label(int(item['source_area']), forward, reverse)}->"
                             f"{label(int(item['destination_area']), forward, reverse)}"),
                    "ride_time": item.get("ride_time", 0),
                    "start": item.get("start"),
                    "destination": item.get("destination"),
                } for item in links],
            }
        relevant = (interactor["type"] in {"door", "platform", "train", "item",
                                            "button", "trigger", "teleporter",
                                            "teleport_destination", "level_exit"} and
                    (touch["counts"].get("spawn", 0) or
                     touch["counts"].get("exit", 0) or stops or
                     interactor.get("target") or interactor.get("targetname") or
                     interactor.get("inventory_grants") or
                     interactor.get("inventory_requires")))
        if relevant or not args.relevant_only:
            rows.append(row)

    source_scc = sccs.get(args.source)
    undirected_scc_paths = []
    for exit_scc in sorted(set(sccs[area] for area in goals)):
        predecessors = {source_scc: 0}
        queue = deque([source_scc])
        while queue and exit_scc not in predecessors:
            current = queue.popleft()
            for destination in scc_undirected[current]:
                if destination not in predecessors:
                    predecessors[destination] = current
                    queue.append(destination)
        if exit_scc not in predecessors:
            continue
        path = [exit_scc]
        while path[-1] != source_scc:
            path.append(predecessors[path[-1]])
        path.reverse()
        steps = []
        for source_component, destination_component in zip(path, path[1:]):
            forward_edges = scc_edges.get((source_component, destination_component), [])
            reverse_edges = scc_edges.get((destination_component, source_component), [])

            def edge_summary(edges: list[dict]) -> dict:
                kinds = Counter(item["travel_type"] for item in edges)
                movers = Counter(str(item.get("mover_interactor", 0)) for item in edges
                                 if item.get("mover_interactor", 0))
                blockers = Counter(str(item.get("dynamic_interactor", 0)) for item in edges
                                   if item.get("dynamic_interactor", 0))
                return {
                    "count": len(edges),
                    "travel_types": dict(sorted(kinds.items())),
                    "movers": dict(sorted(movers.items(), key=lambda item: int(item[0]))),
                    "dynamic_blockers": dict(sorted(blockers.items(),
                                                    key=lambda item: int(item[0]))),
                    "samples": [{"id": item["id"],
                                 "source_area": item["source_area"],
                                 "destination_area": item["destination_area"],
                                 "travel_type": item["travel_type"],
                                 "mover": item.get("mover_interactor", 0),
                                 "blocker": item.get("dynamic_interactor", 0)}
                                for item in edges[:8]],
                    "lowest_transitions": [{
                        "id": item["id"],
                        "source_area": item["source_area"],
                        "destination_area": item["destination_area"],
                        "travel_type": item["travel_type"],
                        "height_delta": item.get("height_delta", 0),
                        "horizontal_distance": item.get("horizontal_distance", 0),
                        "start": item.get("start"),
                        "destination": item.get("destination"),
                    } for item in sorted(
                        edges,
                        key=lambda item: (abs(float(item.get("height_delta", 0))),
                                          float(item.get("horizontal_distance", 0))))[:12]],
                }

            steps.append({
                "from": source_component,
                "to": destination_component,
                "from_size": scc_sizes[source_component],
                "to_size": scc_sizes[destination_component],
                "forward": edge_summary(forward_edges),
                "reverse": edge_summary(reverse_edges),
            })
        undirected_scc_paths.append({"exit_scc": exit_scc, "path": path,
                                     "steps": steps})

    output = {
        "map": nav.get("map"),
        "source": args.source,
        "exit_areas": goals,
        "areas": len(areas),
        "reachabilities": len(nav["reachabilities"]),
        "spawn_forward": len(forward),
        "exit_reverse": len(reverse),
        "overlap": len(forward & reverse),
        "physical_component_count": component_id,
        "physical_component_sizes": component_sizes,
        "source_physical_component": components.get(args.source),
        "exit_physical_components": sorted(set(components[area]
                                               for area in goals)),
        "strong_component_count": len(scc_sizes),
        "strong_component_sizes": scc_sizes,
        "source_strong_component": sccs.get(args.source),
        "exit_strong_components": sorted(set(sccs[area] for area in goals)),
        "undirected_strong_component_paths": undirected_scc_paths,
        "frontier_portals": frontier_portals,
        "activation_edges": activation_edges,
        "interactors": rows,
    }
    print(json.dumps(output, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
