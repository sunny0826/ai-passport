#!/usr/bin/env python3
"""Encode national-dex cries into main/pokedex_cries.bin.

Source: PokeAPI latest OGG clips (PokeAPI/cries), species 1..1025.
Each clip is resampled to 16 kHz mono, encoded with libopus at 8 kbps /
20 ms, then stored as a length-prefixed raw packet stream (no Ogg).

Output layout (little-endian), matching main/pokedex_cry.h:

  u32 magic "CRY1"
  u32 count = 1025
  u32 sample_hz = 16000
  u16 frame_ms = 20
  u16 reserved = 0
  TOC[1025]: {u32 offset, u32 length} relative to the payload
  payload: concatenated clips; each packet is u16le length + bytes

Requires ffmpeg with libopus. Cries are Nintendo / The Pokémon Company
IP and are for personal learning firmware only.

Optional environment:
  POKEDEX_CRY_CACHE  default /tmp/ai-passport-pokedex-cry-cache
  POKEDEX_LAST       default 1025
  POKEDEX_CRY_JOBS   default 8
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

CRY_URL = (
    "https://raw.githubusercontent.com/PokeAPI/cries/master/"
    "cries/pokemon/latest/{}.ogg"
)
OUT_BIN = "main/pokedex_cries.bin"
DEX_FIRST = 1
DEX_LAST = int(os.environ.get("POKEDEX_LAST", "1025"))
CACHE = os.environ.get("POKEDEX_CRY_CACHE", "/tmp/ai-passport-pokedex-cry-cache")
JOBS = int(os.environ.get("POKEDEX_CRY_JOBS", "8"))
UA = "ai-passport-pokedex-cry/1.0"
MAGIC = 0x31595243
SAMPLE_HZ = 16000
FRAME_MS = 20
BITRATE = "8k"


def fetch(url: str, timeout: int = 30, retries: int = 5) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    last: Exception | None = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last = e
            time.sleep(min(8.0, 0.4 * (2**attempt)))
    raise RuntimeError(f"fetch failed {url}: {last}") from last


def load_ogg(id_: int) -> bytes:
    path = os.path.join(CACHE, "ogg", f"{id_:04d}.ogg")
    if os.path.isfile(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as f:
            return f.read()
    data = fetch(CRY_URL.format(id_))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return data


def parse_ogg_packets(data: bytes) -> list[bytes]:
    packets: list[bytes] = []
    incomplete = b""
    pos = 0
    n = len(data)
    while pos + 27 <= n:
        if data[pos : pos + 4] != b"OggS":
            pos += 1
            continue
        nsegs = data[pos + 26]
        hdr = 27 + nsegs
        if pos + hdr > n:
            break
        segs = data[pos + 27 : pos + hdr]
        payload_len = sum(segs)
        end = pos + hdr + payload_len
        if end > n:
            break
        payload = data[pos + hdr : end]
        pos = end
        i = 0
        while i < len(segs):
            plen = 0
            while True:
                s = segs[i]
                plen += s
                i += 1
                if s < 255 or i >= len(segs):
                    break
            piece = payload[:plen]
            payload = payload[plen:]
            incomplete += piece
            if segs[i - 1] < 255:
                packets.append(incomplete)
                incomplete = b""
    return packets


def encode_opus_packets(ogg: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="pokedex-cry-") as td:
        src = os.path.join(td, "in.ogg")
        dst = os.path.join(td, "out.opus")
        with open(src, "wb") as f:
            f.write(ogg)
        cmd = [
            "ffmpeg",
            "-y",
            "-loglevel",
            "error",
            "-i",
            src,
            "-ac",
            "1",
            "-ar",
            str(SAMPLE_HZ),
            "-c:a",
            "libopus",
            "-b:a",
            BITRATE,
            "-application",
            "audio",
            "-vbr",
            "on",
            "-frame_duration",
            str(FRAME_MS),
            "-f",
            "opus",
            dst,
        ]
        subprocess.check_call(cmd)
        with open(dst, "rb") as f:
            ogg_opus = f.read()
    packets = parse_ogg_packets(ogg_opus)
    audio: list[bytes] = []
    for pkt in packets:
        if pkt.startswith(b"OpusHead") or pkt.startswith(b"OpusTags"):
            continue
        if not pkt:
            continue
        if len(pkt) > 0xFFFF:
            raise RuntimeError(f"opus packet too large: {len(pkt)}")
        audio.append(pkt)
    if not audio:
        raise RuntimeError("no opus audio packets")
    out = bytearray()
    for pkt in audio:
        out += struct.pack("<H", len(pkt))
        out += pkt
    return bytes(out)


def process_one(id_: int) -> tuple[int, bytes]:
    ogg = load_ogg(id_)
    if not ogg:
        raise RuntimeError(f"empty ogg for {id_}")
    return id_, encode_opus_packets(ogg)


def build_blob(clips: dict[int, bytes]) -> bytes:
    count = DEX_LAST - DEX_FIRST + 1
    if count != 1025:
        # Header count must match firmware POKEDEX_DEX_SIZE.
        raise RuntimeError(f"encoder DEX range must stay 1..1025, got {count}")
    toc = bytearray(count * 8)
    payload = bytearray()
    for i, id_ in enumerate(range(DEX_FIRST, DEX_LAST + 1)):
        clip = clips[id_]
        struct.pack_into("<II", toc, i * 8, len(payload), len(clip))
        payload += clip
    header = struct.pack("<IIIHH", MAGIC, count, SAMPLE_HZ, FRAME_MS, 0)
    return header + bytes(toc) + bytes(payload)


def main() -> int:
    if not shutil_which("ffmpeg"):
        print("ERROR: ffmpeg with libopus is required", file=sys.stderr)
        return 1
    os.makedirs(CACHE, exist_ok=True)
    clips: dict[int, bytes] = {}
    ids = list(range(DEX_FIRST, DEX_LAST + 1))
    print(f"encoding {len(ids)} cries @ opus {BITRATE} / {SAMPLE_HZ} Hz ...")
    with ThreadPoolExecutor(max_workers=max(1, JOBS)) as pool:
        futs = {pool.submit(process_one, i): i for i in ids}
        done = 0
        for fut in as_completed(futs):
            id_ = futs[fut]
            try:
                got_id, clip = fut.result()
            except Exception as e:
                print(f"ERROR id {id_}: {e}", file=sys.stderr)
                return 1
            clips[got_id] = clip
            done += 1
            if done % 50 == 0 or done == len(ids):
                print(f"  {done}/{len(ids)}")
    blob = build_blob(clips)
    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    tmp = OUT_BIN + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, OUT_BIN)
    total_clip = sum(len(v) for v in clips.values())
    print(
        f"wrote {OUT_BIN}: {len(blob)} bytes "
        f"(payload {total_clip}, avg {total_clip / len(clips):.0f} B/cry)"
    )
    return 0


def shutil_which(name: str) -> bool:
    from shutil import which

    return which(name) is not None


if __name__ == "__main__":
    raise SystemExit(main())
