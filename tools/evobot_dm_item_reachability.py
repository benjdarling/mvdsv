#!/usr/bin/env python3
"""Audit directed EvoBot nav reachability from DM spawns to every pickup."""

from __future__ import annotations

import argparse
from collections import defaultdict, deque
import json
import math
from pathlib import Path

from quake_bsp_entities import entities, entity_text, pak_member


NOT_DEATHMATCH = 2048
PLAYER_ORIGIN_ABOVE_FLOOR = 24.0


def origin(entity: dict[str, str]) -> list[float] | None:
    values = entity.get("origin", "").split()
    if len(values) != 3:
        return None
    try:
        return [float(value) for value in values]
    except ValueError:
        return None


def point_in_area(area: dict[str, object], point: list[float], tolerance: float) -> bool:
    for plane in area.get("planes", []):
        normal = plane["normal"]
        if sum(point[axis] * normal[axis] for axis in range(3)) > \
                float(plane["distance"]) + tolerance:
            return False
    return True


def bounds_distance(bounds: dict[str, list[float]], point: list[float]) -> float:
    squared = 0.0
    for axis in range(3):
        delta = max(
            float(bounds["mins"][axis]) - point[axis],
            0.0,
            point[axis] - float(bounds["maxs"][axis]),
        )
        squared += delta * delta
    return math.sqrt(squared)


def map_point(
    areas: list[dict[str, object]],
    routing_ids: set[int],
    point: list[float],
    nearest_limit: float,
) -> tuple[list[int], str, float]:
    exact = [
        int(area["id"])
        for area in areas
        if int(area["id"]) in routing_ids and point_in_area(area, point, 0.25)
    ]
    if exact:
        return exact, "inside", 0.0
    tolerant = [
        int(area["id"])
        for area in areas
        if int(area["id"]) in routing_ids and point_in_area(area, point, 2.0)
    ]
    if tolerant:
        return tolerant, "boundary", 0.0
    ranked = sorted(
        (bounds_distance(area["bounds"], point), int(area["id"]))
        for area in areas
        if int(area["id"]) in routing_ids
    )
    if not ranked or ranked[0][0] > nearest_limit:
        return [], "unmapped", ranked[0][0] if ranked else math.inf
    best = ranked[0][0]
    return [area_id for distance, area_id in ranked if distance <= best + 0.01], \
        "nearest", best


def flood(seeds: list[int], graph: dict[int, set[int]]) -> set[int]:
    seen = set(seeds)
    pending = deque(seeds)
    while pending:
        source = pending.popleft()
        for destination in graph[source]:
            if destination not in seen:
                seen.add(destination)
                pending.append(destination)
    return seen


def pickup_entity(entity: dict[str, str]) -> bool:
    classname = entity.get("classname", "")
    if not (classname.startswith("item_") or classname.startswith("weapon_")):
        return False
    try:
        spawnflags = int(entity.get("spawnflags", "0"))
    except ValueError:
        spawnflags = 0
    return not spawnflags & NOT_DEATHMATCH


def pickup_mins_z(classname: str) -> float:
    if classname.startswith("item_artifact_") or classname in {
        "item_key1", "item_key2", "item_sigil",
    }:
        return -24.0
    return 0.0


def pickup_horizontal_samples(classname: str, position: list[float]) -> list[list[float]]:
    # Quake health/ammo entities use a [0, 32] XY box; armour, weapons, keys,
    # and powerups use a centred [-16, 16] box.  Trying both the authored
    # origin and positive-box centre also keeps this audit robust for mods.
    samples = [[position[0], position[1]]]
    if classname == "item_health" or classname in {
        "item_shells", "item_spikes", "item_rockets", "item_cells",
    }:
        samples.append([position[0] + 16.0, position[1] + 16.0])
    return samples


def map_pickup(
    areas: list[dict[str, object]],
    routing_ids: set[int],
    classname: str,
    position: list[float],
) -> tuple[list[int], str, float, float | None, list[float] | None]:
    mins_z = pickup_mins_z(classname)
    highest_starting_floor = position[2] + mins_z + 0.5
    candidates: list[tuple[float, list[float]]] = []
    for xy in pickup_horizontal_samples(classname, position):
        for area in areas:
            if not area.get("supported"):
                continue
            bounds = area["bounds"]
            if not (float(bounds["mins"][0]) - 0.5 <= xy[0] <=
                    float(bounds["maxs"][0]) + 0.5 and
                    float(bounds["mins"][1]) - 0.5 <= xy[1] <=
                    float(bounds["maxs"][1]) + 0.5):
                continue
            floor = float(area["floor_height"])
            if position[2] - 256.5 <= floor <= highest_starting_floor:
                candidates.append((floor, xy))
    if not candidates:
        fallback = [position[0], position[1],
                    position[2] + PLAYER_ORIGIN_ABOVE_FLOOR]
        area_ids, mapping, distance = map_point(
            areas, routing_ids, fallback, 40.0
        )
        return area_ids, f"fallback-{mapping}", distance, None, fallback
    floor = max(candidate[0] for candidate in candidates)
    points = [
        [xy[0], xy[1], floor + PLAYER_ORIGIN_ABOVE_FLOOR]
        for candidate_floor, xy in candidates
        if abs(candidate_floor - floor) <= 0.1
    ]
    area_ids: set[int] = set()
    mappings: list[str] = []
    distances: list[float] = []
    for point in points:
        mapped, mapping, distance = map_point(areas, routing_ids, point, 40.0)
        area_ids.update(mapped)
        mappings.append(mapping)
        distances.append(distance)
    best_mapping = "inside" if "inside" in mappings else mappings[0]
    return sorted(area_ids), f"dropped-floor-{best_mapping}", min(distances), \
        floor, points[0]


def audit_map(map_name: str, nav_path: Path, pak_path: Path) -> dict[str, object]:
    nav = json.loads(nav_path.read_text(encoding="utf-8"))
    bsp_entities = entities(entity_text(pak_member(pak_path, f"maps/{map_name}.bsp")))
    areas = nav["areas"]
    graph: dict[int, set[int]] = defaultdict(set)
    routing_ids: set[int] = set()
    for reachability in nav["reachabilities"]:
        source = int(reachability["source_area"])
        destination = int(reachability["destination_area"])
        graph[source].add(destination)
        routing_ids.update((source, destination))

    spawn_records: list[dict[str, object]] = []
    for entity_index, entity in enumerate(bsp_entities):
        if entity.get("classname") != "info_player_deathmatch":
            continue
        position = origin(entity)
        if position is None:
            continue
        area_ids, mapping, distance = map_point(areas, routing_ids, position, 24.0)
        spawn_records.append({
            "entity": entity_index,
            "origin": position,
            "areas": area_ids,
            "mapping": mapping,
            "mapping_distance": round(distance, 3) if math.isfinite(distance) else None,
        })

    item_records: list[dict[str, object]] = []
    for entity_index, entity in enumerate(bsp_entities):
        if not pickup_entity(entity):
            continue
        position = origin(entity)
        if position is None:
            continue
        area_ids, mapping, distance, pickup_floor, pickup_position = map_pickup(
            areas, routing_ids, entity["classname"], position
        )
        item_records.append({
            "entity": entity_index,
            "classname": entity["classname"],
            "origin": position,
            "pickup_player_origin": pickup_position,
            "pickup_floor": round(pickup_floor, 3)
            if pickup_floor is not None else None,
            "areas": area_ids,
            "mapping": mapping,
            "mapping_distance": round(distance, 3) if math.isfinite(distance) else None,
            "spawnflags": int(entity.get("spawnflags", "0")),
            "unreachable_from_spawns": [],
        })

    per_spawn: list[dict[str, object]] = []
    for spawn in spawn_records:
        reachable = flood(spawn["areas"], graph) if spawn["areas"] else set()
        reachable_items = 0
        for item in item_records:
            if item["areas"] and reachable.intersection(item["areas"]):
                reachable_items += 1
            else:
                item["unreachable_from_spawns"].append(spawn["entity"])
        per_spawn.append({
            "entity": spawn["entity"],
            "areas": spawn["areas"],
            "reachable_areas": len(reachable),
            "reachable_items": reachable_items,
            "total_items": len(item_records),
        })

    unmapped_spawns = [record for record in spawn_records if not record["areas"]]
    unmapped_items = [record for record in item_records if not record["areas"]]
    unreachable_from_any = [
        record for record in item_records
        if len(record["unreachable_from_spawns"]) == len(spawn_records)
    ]
    not_reachable_from_all = [
        record for record in item_records if record["unreachable_from_spawns"]
    ]
    return {
        "map": map_name,
        "nav": str(nav_path),
        "nav_version": nav.get("version"),
        "areas": len(areas),
        "directed_graph_nodes": len(routing_ids),
        "reachabilities": len(nav["reachabilities"]),
        "reachability_validation": "see sweep.json (pre-save and post-load ok)",
        "spawn_count": len(spawn_records),
        "item_count": len(item_records),
        "spawns": spawn_records,
        "items": item_records,
        "per_spawn": per_spawn,
        "unmapped_spawn_count": len(unmapped_spawns),
        "unmapped_item_count": len(unmapped_items),
        "unreachable_from_any_spawn_count": len(unreachable_from_any),
        "not_reachable_from_all_spawns_count": len(not_reachable_from_all),
        "all_items_reachable_from_every_spawn": bool(spawn_records) and
            not unmapped_spawns and not not_reachable_from_all,
    }


def markdown(results: list[dict[str, object]]) -> str:
    passed = sum(bool(result["all_items_reachable_from_every_spawn"])
                 for result in results)
    lines = [
        "# EvoBot DM direct-graph item reachability audit",
        "",
        f"Result: **{passed} / {len(results)} maps have a direct generated-graph "
        "path from every deathmatch spawn to every pickup.**",
        "",
        "| Map | Areas | Directed graph nodes | Links | Spawns | Pickups | Unmapped | "
        "Unreachable from any spawn | Not reachable from every spawn | Result |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |",
    ]
    for result in results:
        unmapped = int(result["unmapped_spawn_count"]) + \
            int(result["unmapped_item_count"])
        status = "DIRECT PASS" if result["all_items_reachable_from_every_spawn"] \
            else "CONDITIONAL/GAP"
        lines.append(
            f"| {result['map']} | {result['areas']} | {result['directed_graph_nodes']} | "
            f"{result['reachabilities']} | {result['spawn_count']} | "
            f"{result['item_count']} | {unmapped} | "
            f"{result['unreachable_from_any_spawn_count']} | "
            f"{result['not_reachable_from_all_spawns_count']} | {status} |"
        )
    for result in results:
        lines += ["", f"## {str(result['map']).upper()}", ""]
        lines.append(
            f"Direct graph path to every pickup from every spawn: "
            f"**{'yes' if result['all_items_reachable_from_every_spawn'] else 'no'}**."
        )
        failures = [item for item in result["items"]
                    if item["unreachable_from_spawns"]]
        if not failures:
            lines.append("No direct-graph coverage gaps.")
        else:
            lines += ["", "| Entity | Class | Origin | Nav areas | Unreachable spawns |",
                      "| ---: | --- | --- | --- | --- |"]
            for item in failures:
                lines.append(
                    f"| {item['entity']} | {item['classname']} | "
                    f"`{item['origin']}` | `{item['areas']}` | "
                    f"`{item['unreachable_from_spawns']}` |"
                )
    lines += [
        "",
        "## Method",
        "",
        "- Fresh `.botnav` files were generated, validated, saved, reloaded, and "
        "validated again by MVDSV.",
        "- Pickup scope is every deathmatch-enabled BSP entity whose classname starts "
        "with `item_` or `weapon_`.",
        "- Each deathmatch spawn is mapped at its player origin. Each pickup is mapped "
        "after reproducing Quake's downward item placement against generated support "
        "floors, then at standing player-origin height 24 units above that floor.",
        "- Reachability is directed and tested independently from every "
        "`info_player_deathmatch` spawn.",
        "- The audit proves direct graph coverage. Gameplay interaction sequences, "
        "dynamic brush state, advanced strafe/circle jumps, and physical route execution "
        "are separate runtime concerns; a reported gap can therefore be conditionally "
        "reachable in the real map.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nav-dir", type=Path, required=True)
    parser.add_argument("--pak", type=Path, required=True)
    parser.add_argument("--maps", nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    results = [
        audit_map(map_name.lower(), args.nav_dir / f"{map_name.lower()}.botnav",
                  args.pak)
        for map_name in args.maps
    ]
    payload = {
        "result": f"{sum(bool(item['all_items_reachable_from_every_spawn']) for item in results)} / {len(results)} maps have complete direct graph coverage",
        "criterion": "direct generated-graph path from every info_player_deathmatch spawn to every deathmatch-enabled item_* and weapon_*",
        "maps": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(markdown(results), encoding="utf-8")
    print(payload["result"])
    for result in results:
        print(
            f"{result['map']}: items={result['item_count']} "
            f"unmapped={result['unmapped_item_count']} "
            f"unreachable-any={result['unreachable_from_any_spawn_count']} "
            f"not-all-spawns={result['not_reachable_from_all_spawns_count']} "
            f"pass={result['all_items_reachable_from_every_spawn']}"
        )
    return 0 if all(item["all_items_reachable_from_every_spawn"]
                    for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
