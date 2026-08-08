#!/usr/bin/env python3
"""Correlate EvoBot route frontiers with Quake BSP entities and target chains."""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
from pathlib import Path


RELEVANT_PREFIXES = ("func_", "trigger_", "target_")
RELEVANT_CLASSES = {"info_teleport_destination"}


def parse_entities(text: str) -> list[dict[str, str]]:
    entities = []
    for body in re.findall(r"\{(.*?)\}", text, re.DOTALL):
        pairs = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"\s*"([^"\\]*(?:\\.[^"\\]*)*)"', body)
        entities.append(dict(pairs))
    return entities


def vector(raw: str) -> list[float]:
    values = raw.split()
    return [float(values[i]) if i < len(values) else 0.0 for i in range(3)]


def pak_member(path: Path, member: str) -> bytes | None:
    data = path.read_bytes()
    if data[:4] != b"PACK":
        return None
    directory_offset, directory_length = struct.unpack_from("<ii", data, 4)
    for offset in range(directory_offset, directory_offset + directory_length, 64):
        name = data[offset:offset + 56].split(b"\0", 1)[0].decode("latin-1")
        file_offset, file_length = struct.unpack_from("<ii", data, offset + 56)
        if name.lower() == member.lower():
            return data[file_offset:file_offset + file_length]
    return None


def map_data(basedir: Path, map_name: str) -> bytes:
    loose = basedir / "id1" / "maps" / f"{map_name}.bsp"
    if loose.exists():
        return loose.read_bytes()
    for pak in sorted((basedir / "id1").glob("pak*.pak"), reverse=True):
        data = pak_member(pak, f"maps/{map_name}.bsp")
        if data is not None:
            return data
    raise FileNotFoundError(f"map {map_name} not found below {basedir}")


def read_bsp(data: bytes, label: str) -> tuple[list[dict[str, str]], dict[int, dict[str, list[float]]]]:
    if len(data) < 4 + 15 * 8:
        raise ValueError(f"short BSP: {label}")
    version = struct.unpack_from("<i", data, 0)[0]
    if version not in (29, 30):
        raise ValueError(f"unsupported BSP version {version}: {label}")
    lumps = [struct.unpack_from("<ii", data, 4 + index * 8) for index in range(15)]
    entity_offset, entity_length = lumps[0]
    entities = parse_entities(
        data[entity_offset:entity_offset + entity_length].decode("latin-1", "replace")
    )
    model_offset, model_length = lumps[14]
    models = {}
    for index in range(model_length // 64):
        values = struct.unpack_from("<9f7i", data, model_offset + index * 64)
        models[index] = {"mins": list(values[0:3]), "maxs": list(values[3:6])}
    return entities, models


def bounds(entity: dict[str, str], models: dict[int, dict[str, list[float]]]) -> dict[str, list[float]]:
    model = entity.get("model", "")
    if model.startswith("*") and model[1:].isdigit() and int(model[1:]) in models:
        return models[int(model[1:])]
    origin = vector(entity.get("origin", "0 0 0"))
    return {"mins": origin, "maxs": origin}


def bounds_distance(point: list[float], box: dict[str, list[float]]) -> float:
    squared = 0.0
    for axis in range(3):
        delta = max(box["mins"][axis] - point[axis], 0.0, point[axis] - box["maxs"][axis])
        squared += delta * delta
    return math.sqrt(squared)


def output_field(text: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}:\s*(.+)$", text, re.MULTILINE)
    return match.group(1).strip() if match else ""


def frontier_point(text: str) -> list[float] | None:
    match = re.search(
        r"^source area:.*? center \[([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\]$",
        text, re.MULTILINE,
    )
    return [float(value) for value in match.groups()] if match else None


def chain_from(entity_index: int, entities: list[dict[str, object]], depth: int = 8) -> list[dict[str, object]]:
    result = []
    queue = [(entity_index, 0)]
    visited = set()
    while queue:
        current, level = queue.pop(0)
        if current in visited or level > depth:
            continue
        visited.add(current)
        entity = entities[current]
        target = str(entity.get("target", ""))
        if not target:
            continue
        for index, candidate in enumerate(entities):
            if candidate.get("targetname") == target:
                result.append({
                    "from": current,
                    "to": index,
                    "target": target,
                    "classname": candidate.get("classname", ""),
                    "depth": level + 1,
                })
                queue.append((index, level + 1))
    return result


def classify(map_name: str, route: str, nearby: list[dict[str, object]]) -> str:
    if map_name == "end":
        return "finale/map-specific"
    close = [
        entry for entry in nearby
        if entry["distance"] is not None and float(entry["distance"]) <= 192.0
    ]
    classes = {str(entry["classname"]) for entry in close}
    if any(name in classes for name in ("func_plat", "func_train")):
        return "lift/platform"
    if "func_button" in classes and any(name.startswith("func_door") for name in classes):
        return "button-controlled door"
    if any(name.startswith("func_door") for name in classes):
        return "touch/automatic door"
    if any(name in classes for name in ("trigger_multiple", "trigger_once", "trigger_counter")):
        return "trigger-controlled geometry"
    if "water" in output_field(route, "frontier reason").lower():
        return "water traversal"
    if "no implemented reachability closes" in route:
        return "large unsupported/contextual transition"
    reason = output_field(route, "frontier reason").lower()
    if "jump" in reason or "drop" in reason or "ledge" in reason:
        return "genuine missing jump/drop"
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sweep", type=Path, required=True)
    parser.add_argument("--basedir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = json.loads(args.sweep.read_text(encoding="utf-8"))
    audit = []
    for row in rows:
        if row["result"] == "reachable":
            continue
        map_name = row["map"]
        entities, models = read_bsp(map_data(args.basedir, map_name), map_name)
        enriched: list[dict[str, object]] = []
        point = frontier_point(row["outputs"].get("frontier_report", ""))
        for index, entity in enumerate(entities):
            classname = entity.get("classname", "")
            if not (classname.startswith(RELEVANT_PREFIXES) or classname in RELEVANT_CLASSES):
                continue
            box = bounds(entity, models)
            entry: dict[str, object] = {
                "entity_index": index,
                "classname": classname,
                "model": entity.get("model", ""),
                "origin": vector(entity.get("origin", "0 0 0")),
                "bounds": box,
                "target": entity.get("target", ""),
                "targetname": entity.get("targetname", ""),
                "spawnflags": entity.get("spawnflags", "0"),
                "speed": entity.get("speed", ""),
                "wait": entity.get("wait", ""),
                "lip": entity.get("lip", ""),
                "height": entity.get("height", ""),
            }
            entry["distance"] = round(bounds_distance(point, box), 1) if point else None
            entry["activation_chain"] = chain_from(index, entities)
            enriched.append(entry)
        enriched.sort(key=lambda item: float(item["distance"]) if item["distance"] is not None else 1e30)
        route = row["outputs"].get("route", "")
        audit.append({
            "map": map_name,
            "result": row["result"],
            "source_area": output_field(route, "source area"),
            "destination_area": output_field(route, "destination area"),
            "reachable_areas": output_field(route, "reachable routing areas"),
            "frontier_area": output_field(route, "frontier area"),
            "adjacent_area": output_field(route, "adjacent area"),
            "portal": output_field(route, "portal"),
            "frontier_reason": output_field(route, "frontier reason"),
            "runtime_nearby_interactor": output_field(route, "nearby interactor"),
            "runtime_blocker": output_field(route, "blocked by interactor"),
            "frontier_point": point,
            "classification": classify(map_name, route, enriched),
            "nearby_entities": enriched[:20],
            "entity_class_counts": {
                classname: sum(entity.get("classname") == classname for entity in entities)
                for classname in sorted({entity.get("classname", "") for entity in entities})
                if classname.startswith(RELEVANT_PREFIXES) or classname in RELEVANT_CLASSES
            },
        })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(audit, indent=2), encoding="utf-8")
    print(json.dumps({
        "maps": len(audit),
        "categories": {
            category: sum(row["classification"] == category for row in audit)
            for category in sorted({row["classification"] for row in audit})
        },
        "output": str(args.output.resolve()),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
