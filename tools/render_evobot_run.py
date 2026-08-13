#!/usr/bin/env python3
"""Render EvoBot or human-capture JSONL as a labelled top-down run map."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


def load_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    names = (["C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/arialbd.ttf"]
             if bold else
             ["C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"])
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def read_run(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    with path.open(encoding="utf-8") as source:
        for line in source:
            try:
                record = json.loads(line)
            except (ValueError, TypeError):
                continue
            origin = record.get("origin")
            if not (isinstance(origin, list) and len(origin) >= 3):
                continue
            if "total_elapsed" not in record and "elapsed" not in record:
                continue
            records.append(record)
    # Quake moves the player to a remote intermission camera after the exit.
    # That is not traversed movement and must not draw a diagonal across the map.
    for index in range(1, len(records)):
        before = records[index - 1]["origin"]
        after = records[index]["origin"]
        if math.dist([float(before[0]), float(before[1])],
                     [float(after[0]), float(after[1])]) > 768.0:  # type: ignore[index]
            return records[:index]
    return records


def read_supported_floors(path: Path | None) -> list[tuple[float, list[tuple[float, float]]]]:
    if not path or not path.exists():
        return []
    vertices: list[tuple[float, float, float]] = []
    floors: list[tuple[float, list[tuple[float, float]]]] = []
    group = ""
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            if line.startswith("g "):
                group = line[2:].strip()
                if group != "supported_areas" and floors:
                    break
            elif line.startswith("v "):
                fields = line.split()
                if len(fields) >= 4:
                    vertices.append(tuple(map(float, fields[1:4])))
            elif group == "supported_areas" and line.startswith("f "):
                indices = [int(field.split("/")[0]) - 1 for field in line.split()[1:]]
                points = [vertices[index] for index in indices if 0 <= index < len(vertices)]
                if len(points) < 3:
                    continue
                zs = [point[2] for point in points]
                if max(zs) - min(zs) > 0.25:
                    continue
                polygon = [(point[0], point[1]) for point in points]
                area = abs(sum(polygon[i][0] * polygon[(i + 1) % len(polygon)][1] -
                               polygon[(i + 1) % len(polygon)][0] * polygon[i][1]
                               for i in range(len(polygon)))) * 0.5
                if area >= 4.0:
                    floors.append((sum(zs) / len(zs), polygon))
    return floors


def column_name(index: int) -> str:
    result = ""
    index += 1
    while index:
        index, remainder = divmod(index - 1, 26)
        result = chr(65 + remainder) + result
    return result


def elapsed(record: dict[str, object]) -> float:
    return float(record.get("total_elapsed", record.get("elapsed", 0.0)))


def view_yaw(record: dict[str, object]) -> float | None:
    view = record.get("view")
    if isinstance(view, list) and view:
        return float(view[0])
    return None


def draw_run_map(runs: list[tuple[str, list[dict[str, object]]]],
                 floors: list[tuple[float, list[tuple[float, float]]]],
                 output: Path, grid_size: int, requested_cell: str | None) -> Path:
    all_records = [record for _, records in runs for record in records]
    if not all_records:
        raise ValueError("no telemetry records containing origin and elapsed time")
    xs = [float(record["origin"][0]) for record in all_records]  # type: ignore[index]
    ys = [float(record["origin"][1]) for record in all_records]  # type: ignore[index]
    zs = [float(record["origin"][2]) for record in all_records]  # type: ignore[index]
    world_min_x = math.floor((min(xs) - grid_size * 0.35) / grid_size) * grid_size
    world_max_x = math.ceil((max(xs) + grid_size * 0.35) / grid_size) * grid_size
    world_min_y = math.floor((min(ys) - grid_size * 0.35) / grid_size) * grid_size
    world_max_y = math.ceil((max(ys) + grid_size * 0.35) / grid_size) * grid_size
    full_min_x, full_max_x = world_min_x, world_max_x
    full_min_y, full_max_y = world_min_y, world_max_y

    if requested_cell:
        import re
        match = re.fullmatch(r"([A-Za-z]+)([1-9][0-9]*)", requested_cell.strip())
        if not match:
            raise ValueError("--cell must look like D7")
        column = 0
        for character in match.group(1).upper():
            column = column * 26 + ord(character) - 64
        column -= 1
        row = int(match.group(2)) - 1
        cell_min_x = full_min_x + column * grid_size
        cell_max_y = full_max_y - row * grid_size
        world_min_x = cell_min_x - grid_size
        world_max_x = cell_min_x + grid_size * 2
        world_min_y = cell_max_y - grid_size * 2
        world_max_y = cell_max_y + grid_size

    margin_left, margin_top, margin_right, margin_bottom = 100, 90, 55, 105
    plot_ratio = (world_max_x - world_min_x) / max(1.0, world_max_y - world_min_y)
    plot_height = 1450
    plot_width = max(720, min(1800, int(plot_height * plot_ratio)))
    canvas = Image.new("RGB", (plot_width + margin_left + margin_right,
                               plot_height + margin_top + margin_bottom), "#111820")
    draw = ImageDraw.Draw(canvas, "RGBA")
    font = load_font(18)
    small = load_font(14)
    label_font = load_font(17, True)
    title_font = load_font(28, True)

    scale_x = plot_width / (world_max_x - world_min_x)
    scale_y = plot_height / (world_max_y - world_min_y)

    def project(x: float, y: float) -> tuple[int, int]:
        return (round(margin_left + (x - world_min_x) * scale_x),
                round(margin_top + (world_max_y - y) * scale_y))

    plot_box = (margin_left, margin_top, margin_left + plot_width,
                margin_top + plot_height)
    draw.rectangle(plot_box, fill="#18222c", outline="#8a9aaa", width=2)

    z_min, z_max = min(zs), max(zs)
    for floor_z, polygon in floors:
        if floor_z < z_min - 96 or floor_z > z_max + 96:
            continue
        poly_x = [point[0] for point in polygon]
        poly_y = [point[1] for point in polygon]
        if (max(poly_x) < world_min_x or min(poly_x) > world_max_x or
                max(poly_y) < world_min_y or min(poly_y) > world_max_y):
            continue
        shade = int(52 + 62 * ((floor_z - z_min) / max(1.0, z_max - z_min)))
        draw.polygon([project(*point) for point in polygon],
                     fill=(shade, shade + 10, shade + 15, 72))

    start_grid_x = math.floor(world_min_x / grid_size) * grid_size
    end_grid_x = math.ceil(world_max_x / grid_size) * grid_size
    start_grid_y = math.floor(world_min_y / grid_size) * grid_size
    end_grid_y = math.ceil(world_max_y / grid_size) * grid_size
    x = start_grid_x
    while x <= end_grid_x:
        px, _ = project(x, world_min_y)
        draw.line((px, margin_top, px, margin_top + plot_height),
                  fill="#7d8b9955", width=2)
        draw.text((px + 4, margin_top + plot_height + 8), f"x {int(x)}",
                  font=small, fill="#b9c6d2")
        x += grid_size
    y = start_grid_y
    while y <= end_grid_y:
        _, py = project(world_min_x, y)
        draw.line((margin_left, py, margin_left + plot_width, py),
                  fill="#7d8b9955", width=2)
        draw.text((8, py - 9), f"y {int(y)}", font=small, fill="#b9c6d2")
        y += grid_size

    columns = int(round((full_max_x - full_min_x) / grid_size))
    rows = int(round((full_max_y - full_min_y) / grid_size))
    cell_manifest: dict[str, dict[str, int]] = {}
    for row in range(rows):
        for column in range(columns):
            code = f"{column_name(column)}{row + 1}"
            left = full_min_x + column * grid_size
            right = left + grid_size
            top = full_max_y - row * grid_size
            bottom = top - grid_size
            cell_manifest[code] = {
                "x_min": int(left), "x_max": int(right),
                "y_min": int(bottom), "y_max": int(top),
            }
            if (right < world_min_x or left > world_max_x or
                    top < world_min_y or bottom > world_max_y):
                continue
            px, py = project(left, top)
            draw.text((px + 7, py + 5), code, font=label_font,
                      fill="#f1f5f9b8", stroke_width=2, stroke_fill="#111820aa")

    palette = ["#ff9f43", "#41b6e6", "#b983ff", "#66d17a"]
    for run_index, (label, records) in enumerate(runs):
        colour = palette[run_index % len(palette)]
        points = [project(float(record["origin"][0]), float(record["origin"][1]))  # type: ignore[index]
                  for record in records]
        if len(points) > 1:
            draw.line(points, fill="#090c10e6", width=10, joint="curve")
            draw.line(points, fill=colour, width=5, joint="curve")
        draw.ellipse((points[0][0] - 8, points[0][1] - 8,
                      points[0][0] + 8, points[0][1] + 8),
                     fill="#66d17a", outline="#071008", width=2)
        draw.rectangle((points[-1][0] - 8, points[-1][1] - 8,
                        points[-1][0] + 8, points[-1][1] + 8),
                       fill="#ff5d5d", outline="#180707", width=2)

        next_time = math.ceil(elapsed(records[0]) / 5.0) * 5.0
        previous_drop = float(records[0].get("bot_dropped_seconds", 0.0))
        next_gaze = elapsed(records[0])
        for record, point in zip(records, points):
            moment = elapsed(record)
            if moment >= next_time:
                draw.ellipse((point[0] - 5, point[1] - 5, point[0] + 5, point[1] + 5),
                             fill=colour, outline="#0a0d10", width=2)
                draw.text((point[0] + 7, point[1] - 18), f"{next_time:.0f}s",
                          font=small, fill="#e8eef4", stroke_width=2,
                          stroke_fill="#111820")
                next_time += 5.0
            yaw = view_yaw(record)
            if yaw is not None and moment >= next_gaze:
                radians = math.radians(yaw)
                origin = record["origin"]  # type: ignore[assignment]
                gaze = project(float(origin[0]) + math.cos(radians) * 100.0,
                               float(origin[1]) + math.sin(radians) * 100.0)
                draw.line((point, gaze), fill="#60d8d888", width=2)
                next_gaze = moment + 1.0
            current_drop = float(record.get("bot_dropped_seconds", previous_drop))
            dropped = max(0.0, current_drop - previous_drop)
            command = float(record.get("command_last_seconds", 0.0))
            if dropped >= 0.015 or command >= 0.020:
                radius = 9
                draw.line((point[0] - radius, point[1] - radius,
                           point[0] + radius, point[1] + radius), fill="#ff4d4d", width=4)
                draw.line((point[0] - radius, point[1] + radius,
                           point[0] + radius, point[1] - radius), fill="#ff4d4d", width=4)
            previous_drop = current_drop

    title = "EvoBot run map — 256-unit reference grid" if grid_size == 256 else \
        f"EvoBot run map — {grid_size}-unit reference grid"
    draw.text((margin_left, 22), title, font=title_font, fill="#f5f8fb")
    legend_x = margin_left
    legend_y = margin_top + plot_height + 42
    for run_index, (label, records) in enumerate(runs):
        colour = palette[run_index % len(palette)]
        draw.line((legend_x, legend_y + 8, legend_x + 30, legend_y + 8), fill=colour, width=5)
        duration = elapsed(records[-1]) - elapsed(records[0])
        draw.text((legend_x + 38, legend_y - 4), f"{label} ({duration:.2f}s)",
                  font=font, fill="#e8eef4")
        legend_x += 245
    draw.text((margin_left, legend_y + 25),
              "red × = server/command hitch   cyan line = sampled gaze",
              font=small, fill="#c6d0da")

    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)
    manifest_path = output.with_name(output.stem + "-grid.json")
    manifest_path.write_text(json.dumps({
        "grid_size": grid_size,
        "row_1_is_north": True,
        "world_bounds": {"x_min": int(full_min_x), "x_max": int(full_max_x),
                         "y_min": int(full_min_y), "y_max": int(full_max_y)},
        "cells": cell_manifest,
    }, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path,
                        help="bot execution or human capture JSONL")
    parser.add_argument("--label", action="append", default=[],
                        help="legend label, repeated in run order")
    parser.add_argument("--nav-obj", type=Path,
                        help="EvoBot navigation OBJ for the floor backdrop")
    parser.add_argument("--output", type=Path, help="output PNG")
    parser.add_argument("--grid-size", type=int, default=256)
    parser.add_argument("--cell", help="zoom to a labelled cell, such as D7")
    args = parser.parse_args()
    if args.grid_size < 32:
        parser.error("--grid-size must be at least 32")
    loaded = [(args.label[index] if index < len(args.label) else path.stem,
               read_run(path)) for index, path in enumerate(args.runs)]
    output = args.output or args.runs[0].with_name(args.runs[0].stem + "-grid.png")
    manifest = draw_run_map(loaded, read_supported_floors(args.nav_obj), output,
                            args.grid_size, args.cell)
    print(output.resolve())
    print(manifest.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
