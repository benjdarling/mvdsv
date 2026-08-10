#!/usr/bin/env python3
"""Find the closest boundaries between directed nav components in a botnav."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
import json
import math
from pathlib import Path


def bounds_distance(a: dict[str, list[float]], b: dict[str, list[float]]) -> float:
    total = 0.0
    for axis in range(3):
        gap = max(0.0, a["mins"][axis] - b["maxs"][axis],
                  b["mins"][axis] - a["maxs"][axis])
        total += gap * gap
    return math.sqrt(total)


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nav", type=Path, required=True)
    parser.add_argument("--source", type=int, nargs="+", required=True)
    parser.add_argument("--destination", type=int, nargs="+", required=True)
    parser.add_argument("--limit", type=int, default=30)
    args = parser.parse_args()
    nav = json.loads(args.nav.read_text(encoding="utf-8"))
    areas = {int(area["id"]): area for area in nav["areas"]}
    forward_graph: dict[int, set[int]] = defaultdict(set)
    reverse_graph: dict[int, set[int]] = defaultdict(set)
    for reachability in nav["reachabilities"]:
        source = int(reachability["source_area"])
        destination = int(reachability["destination_area"])
        forward_graph[source].add(destination)
        reverse_graph[destination].add(source)
    forward = flood(args.source, forward_graph)
    reverse = flood(args.destination, reverse_graph)
    portals: dict[tuple[int, int], list[dict[str, object]]] = defaultdict(list)
    for portal in nav["portals"]:
        key = tuple(sorted((int(portal["area_a"]), int(portal["area_b"]))))
        portals[key].append(portal)
    candidates: list[tuple[float, int, int]] = []
    for source in forward:
        if source not in areas:
            continue
        for destination in reverse:
            if destination not in areas or source == destination:
                continue
            candidates.append((
                bounds_distance(areas[source]["bounds"], areas[destination]["bounds"]),
                source,
                destination,
            ))
    candidates.sort()
    print(
        json.dumps({
            "source": args.source,
            "destination": args.destination,
            "forward_count": len(forward),
            "reverse_count": len(reverse),
            "overlap_count": len(forward & reverse),
        })
    )
    for distance, source, destination in candidates[:args.limit]:
        source_area = areas[source]
        destination_area = areas[destination]
        shared = portals.get(tuple(sorted((source, destination))), [])
        print(json.dumps({
            "distance": round(distance, 3),
            "source": source,
            "destination": destination,
            "shared_portals": [
                {"id": portal["id"], "kind": portal.get("kind", portal.get("type"))}
                for portal in shared
            ],
            "source_bounds": source_area["bounds"],
            "destination_bounds": destination_area["bounds"],
            "source_floor": source_area["floor_height"],
            "destination_floor": destination_area["floor_height"],
            "source_supported": source_area["supported"],
            "destination_supported": destination_area["supported"],
            "source_dynamic": source_area["dynamic_interactor"],
            "destination_dynamic": destination_area["dynamic_interactor"],
        }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
