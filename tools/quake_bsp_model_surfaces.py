#!/usr/bin/env python3
"""List upward-facing surfaces for a Quake BSP brush model."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from quake_bsp_entities import pak_member


def lump(data: bytes, index: int) -> bytes:
    offset, length = struct.unpack_from("<ii", data, 4 + index * 8)
    return data[offset:offset + length]


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--bsp", type=Path)
    source.add_argument("--pak", type=Path)
    parser.add_argument("--member")
    parser.add_argument("--model", type=int, required=True)
    parser.add_argument("--minimum-normal-z", type=float, default=0.7)
    parser.add_argument("--bounds", type=float, nargs=6, metavar=(
        "MIN_X", "MIN_Y", "MIN_Z", "MAX_X", "MAX_Y", "MAX_Z"),
        help="only list surfaces whose bounds overlap this box")
    args = parser.parse_args()
    if args.pak and not args.member:
        parser.error("--member is required with --pak")
    data = args.bsp.read_bytes() if args.bsp else pak_member(args.pak, args.member)
    if struct.unpack_from("<i", data, 0)[0] != 29:
        raise ValueError("only Quake BSP version 29 is supported")
    planes_raw = lump(data, 1)
    vertices_raw = lump(data, 3)
    faces_raw = lump(data, 7)
    edges_raw = lump(data, 12)
    surfedges_raw = lump(data, 13)
    models_raw = lump(data, 14)
    planes = [struct.unpack_from("<4fi", planes_raw, offset)
              for offset in range(0, len(planes_raw), 20)]
    vertices = [struct.unpack_from("<3f", vertices_raw, offset)
                for offset in range(0, len(vertices_raw), 12)]
    edges = [struct.unpack_from("<HH", edges_raw, offset)
             for offset in range(0, len(edges_raw), 4)]
    surfedges = [struct.unpack_from("<i", surfedges_raw, offset)[0]
                 for offset in range(0, len(surfedges_raw), 4)]
    model_offset = args.model * 64
    values = struct.unpack_from("<9f7i", models_raw, model_offset)
    first_face, face_count = values[-2], values[-1]
    print(f"model *{args.model}: faces {first_face}..{first_face + face_count - 1}")
    for face_index in range(first_face, first_face + face_count):
        face = struct.unpack_from("<Hhihh4Bi", faces_raw, face_index * 20)
        plane_index, side, first_edge, edge_count = face[:4]
        normal = list(planes[plane_index][:3])
        if side:
            normal = [-value for value in normal]
        if normal[2] < args.minimum_normal_z:
            continue
        points = []
        for index in range(first_edge, first_edge + edge_count):
            surfedge = surfedges[index]
            edge = edges[abs(surfedge)]
            vertex_index = edge[0] if surfedge >= 0 else edge[1]
            points.append(vertices[vertex_index])
        mins = [min(point[axis] for point in points) for axis in range(3)]
        maxs = [max(point[axis] for point in points) for axis in range(3)]
        if args.bounds:
            query_mins = args.bounds[:3]
            query_maxs = args.bounds[3:]
            if any(maxs[axis] < query_mins[axis] or
                   mins[axis] > query_maxs[axis] for axis in range(3)):
                continue
        print(f"face {face_index}: normal {normal}, bounds {mins} {maxs}, "
              f"vertices {len(points)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
