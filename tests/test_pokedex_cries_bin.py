#!/usr/bin/env python3
"""Host check for the generated national-dex cry blob."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLOB = ROOT / "main" / "pokedex_cries.bin"
MAGIC = 0x31595243
COUNT = 1025
SAMPLE_HZ = 16000
FRAME_MS = 20
HEADER = 16
TOC_ITEM = 8
MAX_SIZE = 0x3A6000  # cryfs partition


def main() -> int:
    if not BLOB.is_file():
        print(f"ERROR: missing {BLOB}", file=sys.stderr)
        return 1
    data = BLOB.read_bytes()
    if len(data) < HEADER + COUNT * TOC_ITEM:
        print(f"ERROR: cry blob too small ({len(data)} bytes)", file=sys.stderr)
        return 1
    if len(data) > MAX_SIZE:
        print(f"ERROR: cry blob {len(data)} exceeds cryfs {MAX_SIZE}", file=sys.stderr)
        return 1
    magic, count, hz, frame_ms, _reserved = struct.unpack_from("<IIIHH", data, 0)
    if magic != MAGIC or count != COUNT or hz != SAMPLE_HZ or frame_ms != FRAME_MS:
        print(
            f"ERROR: bad cry header magic=0x{magic:x} count={count} hz={hz} ms={frame_ms}",
            file=sys.stderr,
        )
        return 1
    payload = HEADER + COUNT * TOC_ITEM
    empty = 0
    for i in range(COUNT):
        off, length = struct.unpack_from("<II", data, HEADER + i * TOC_ITEM)
        if payload + off + length > len(data):
            print(f"ERROR: cry {i + 1} overruns blob", file=sys.stderr)
            return 1
        if length == 0:
            empty += 1
    if empty:
        print(f"ERROR: {empty} empty cry clips", file=sys.stderr)
        return 1
    print(
        f"pokedex_cries.bin: PASS ({len(data)} bytes, {COUNT} cries, "
        f"payload {len(data) - payload})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
