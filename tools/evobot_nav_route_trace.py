#!/usr/bin/env python3
"""Reconstruct an offline botnav route under an explicit interactor state."""

from __future__ import annotations

import argparse
import heapq
import json
from pathlib import Path


def intersects(a: dict, b: dict) -> bool:
    return all(a["maxs"][axis] >= b["mins"][axis] and
               a["mins"][axis] <= b["maxs"][axis] for axis in range(3))


def expanded(bounds: dict, player: dict) -> dict:
    return {
        "mins": [bounds["mins"][i] - player["maxs"][i] - 1 for i in range(3)],
        "maxs": [bounds["maxs"][i] - player["mins"][i] + 1 for i in range(3)],
    }


def initial_stop(reach: dict, mover: dict, player: dict) -> bool:
    board = reach.get("board_region", {})
    if not board or "mins" not in board:
        return False
    for axis in range(2):
        board_center = (board["mins"][axis] + board["maxs"][axis]) * 0.5
        mover_center = (mover["bounds"]["mins"][axis] +
                        mover["bounds"]["maxs"][axis]) * 0.5
        if abs(board_center - mover_center) > 1:
            return False
    if mover["type"] == "train":
        origin_z = (board["mins"][2] + board["maxs"][2]) * 0.5
        return (origin_z >= mover["bounds"]["mins"][2] + player["mins"][2] - 8 and
                origin_z <= mover["bounds"]["maxs"][2] - player["mins"][2] + 8)
    return abs((board["mins"][2] + 4) -
               (mover["bounds"]["maxs"][2] - player["mins"][2])) <= 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nav", type=Path, required=True)
    parser.add_argument("--source", type=int, required=True)
    parser.add_argument("--destination", type=int, nargs="*")
    parser.add_argument("--enable", type=int, nargs="*", default=[])
    parser.add_argument("--block-unlisted", action="store_true")
    args = parser.parse_args()
    nav = json.loads(args.nav.read_text(encoding="utf-8"))
    interactors = {int(item["id"]): item for item in nav["interactors"]}
    enabled = set(args.enable)
    goals = set(args.destination or [])
    if not goals:
        areas = {int(area["id"]): area for area in nav["areas"]}
        for interactor in nav["interactors"]:
            if interactor["type"] != "level_exit":
                continue
            region = expanded(interactor["bounds"], nav["player_bounds"])
            goals.update(area_id for area_id, area in areas.items()
                         if (area["supported"] or area["water_level"] > 0) and
                         intersects(area["bounds"], region))
    outgoing: dict[int, list[dict]] = {}
    for reach in nav["reachabilities"]:
        outgoing.setdefault(int(reach["source_area"]), []).append(reach)
    costs = {args.source: 0.0}
    predecessors: dict[int, dict] = {}
    queue = [(0.0, args.source)]
    destination = None
    while queue:
        cost, area = heapq.heappop(queue)
        if cost != costs.get(area):
            continue
        if area in goals:
            destination = area
            break
        for reach in outgoing.get(area, []):
            dynamic = int(reach.get("dynamic_interactor", -1))
            if args.block_unlisted and dynamic > 0 and dynamic not in enabled:
                blocker = interactors.get(dynamic)
                if not blocker or int(blocker.get("activation", 0)) != 1:
                    continue
            mover_id = int(reach.get("mover_interactor", 0))
            if args.block_unlisted and reach["travel_type"] == "platform" and mover_id:
                mover = interactors.get(mover_id)
                if (mover and (int(mover.get("activation", 0)) == 4 or
                               int(mover.get("inventory_requires", 0))) and
                        mover_id not in enabled):
                    if float(reach.get("ride_time", 0)) > 0 or not initial_stop(
                            reach, mover, nav["player_bounds"]):
                        continue
            next_area = int(reach["destination_area"])
            next_cost = cost + float(reach.get("base_travel_time", 0))
            if next_cost < costs.get(next_area, float("inf")):
                costs[next_area] = next_cost
                predecessors[next_area] = reach
                heapq.heappush(queue, (next_cost, next_area))
    steps = []
    current = destination
    while current is not None and current != args.source:
        reach = predecessors.get(current)
        if reach is None:
            break
        steps.append({
            "id": reach["id"],
            "source": reach["source_area"],
            "destination": reach["destination_area"],
            "type": reach["travel_type"],
            "dynamic": reach.get("dynamic_interactor", -1),
            "mover": reach.get("mover_interactor", 0),
            "ride_time": reach.get("ride_time", 0),
            "height_delta": reach.get("height_delta", 0),
            "horizontal_distance": reach.get("horizontal_distance", 0),
            "portal": reach.get("portal_id"),
            "start": reach.get("start"),
            "end": reach.get("destination"),
        })
        current = int(reach["source_area"])
    steps.reverse()
    print(json.dumps({
        "source": args.source,
        "goals": sorted(goals),
        "destination": destination,
        "cost": costs.get(destination) if destination is not None else None,
        "reachable": len(costs),
        "steps": steps,
    }, indent=2))
    return 0 if destination is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
