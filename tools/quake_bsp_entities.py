#!/usr/bin/env python3
"""Inspect the entity lump of a Quake BSP stored directly or inside a PAK."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


def pak_member(path: Path, wanted: str) -> bytes:
    data = path.read_bytes()
    if data[:4] != b"PACK":
        raise ValueError(f"{path} is not a Quake PAK")
    directory_offset, directory_length = struct.unpack_from("<ii", data, 4)
    for offset in range(directory_offset, directory_offset + directory_length, 64):
        raw_name, member_offset, member_length = struct.unpack_from("<56sii", data, offset)
        name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="replace")
        if name.lower() == wanted.lower():
            return data[member_offset:member_offset + member_length]
    raise FileNotFoundError(f"{wanted} not found in {path}")


def entity_text(bsp: bytes) -> str:
    version = struct.unpack_from("<i", bsp, 0)[0]
    if version != 29:
        raise ValueError(f"unsupported BSP version {version}")
    offset, length = struct.unpack_from("<ii", bsp, 4)
    return bsp[offset:offset + length].rstrip(b"\0").decode("latin-1")


def entities(text: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for body in re.findall(r"\{([^}]*)\}", text, re.DOTALL):
        pairs = re.findall(r'"([^"]*)"\s+"([^"]*)"', body)
        result.append(dict(pairs))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--bsp", type=Path)
    source.add_argument("--pak", type=Path)
    parser.add_argument("--member", help="PAK member such as maps/e4m8.bsp")
    parser.add_argument("--filter", default="", help="case-insensitive key/value substring")
    args = parser.parse_args()
    if args.pak and not args.member:
        parser.error("--member is required with --pak")
    bsp = args.bsp.read_bytes() if args.bsp else pak_member(args.pak, args.member)
    needle = args.filter.lower()
    for index, entity in enumerate(entities(entity_text(bsp))):
        if needle and not any(
            needle in key.lower() or needle in value.lower()
            for key, value in entity.items()
        ):
            continue
        print(f"entity {index}")
        for key, value in entity.items():
            print(f'  "{key}" "{value}"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
