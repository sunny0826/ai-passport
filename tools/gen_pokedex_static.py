#!/usr/bin/env python3
"""生成 Pokédex 离线静态数据库。

产物(在 main/ 下,随固件编译/入库):
  - pokedex_static.h / .c   : 全国图鉴 1..1025(第 I–IX 世代)的基本数据
                              (name/height/weight/types),来源 PokeAPI
                              (https://pokeapi.co, CC-BY 4.0)。
  - pokedex_sprites.bin     : 每只 48x48 RGB565 精灵图,raw-deflate 压缩;
                              TOC(1025 x {off,len,w,h}) + 压缩数据。

用法:python3 tools/gen_pokedex_static.py
可选环境变量:
  POKEDEX_CACHE  默认 /tmp/ai-passport-pokedex-cache(JSON/PNG 断点续传)
  POKEDEX_LAST   默认 1025(全国种上限)

精灵图版权归 The Pokémon Company / Nintendo(非关联项目,仅供个人学习)。
"""
from __future__ import annotations

import concurrent.futures
import io
import json
import os
import struct
import time
import unicodedata
import urllib.error
import urllib.request
import zlib

API = "https://pokeapi.co/api/v2/pokemon/{}"
SPECIES_API = "https://pokeapi.co/api/v2/pokemon-species/{}"
SPRITE = "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/{}.png"
OUT_H = "main/pokedex_static.h"
OUT_C = "main/pokedex_static.c"
OUT_BIN = "main/pokedex_sprites.bin"

DEX_W, DEX_H = 48, 48
PIXELS = DEX_W * DEX_H
BPP = 2
SPRITE_BYTES = PIXELS * BPP
DESC_MAX = 192  # 英文 flavor text 截断长度(含 NUL)
DEX_FIRST = 1
DEX_LAST = int(os.environ.get("POKEDEX_LAST", "1025"))
CACHE = os.environ.get("POKEDEX_CACHE", "/tmp/ai-passport-pokedex-cache")
UA = "ai-passport-pokedex-gen/2.0"


def cache_path(kind: str, id_: int, ext: str) -> str:
    return os.path.join(CACHE, kind, f"{id_:04d}.{ext}")


def fetch(url: str, timeout: int = 30, retries: int = 5) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    last: Exception | None = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last = e
            time.sleep(min(8.0, 0.4 * (2 ** attempt)))
    raise RuntimeError(f"fetch failed {url}: {last}") from last


def load_or_fetch(kind: str, id_: int, url: str, ext: str) -> bytes:
    path = cache_path(kind, id_, ext)
    if os.path.isfile(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as f:
            return f.read()
    data = fetch(url)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return data


def sprite_to_rgb565_id(png: bytes) -> bytes:
    """把官方 PNG 缩到 48x48 最近邻并转 RGB565(透明像素混纸色)。"""
    try:
        from PIL import Image
    except ImportError as e:
        raise SystemExit(f"需要 Pillow: pip install pillow ({e})") from e

    im = Image.open(io.BytesIO(png)).convert("RGBA")
    im = im.resize((DEX_W, DEX_H), Image.NEAREST)
    out = bytearray()
    r5 = lambda v: (v >> 3) & 0x1F
    g6 = lambda v: (v >> 2) & 0x3F
    b5 = lambda v: (v >> 3) & 0x1F
    BG = (0x24, 0x42, 0x38)  # 图鉴屏幕底 #244238
    for (r, g, b, a) in list(im.getdata()):
        if a < 255:
            k = a / 255.0
            r = int(r * k + BG[0] * (1 - k))
            g = int(g * k + BG[1] * (1 - k))
            b = int(b * k + BG[2] * (1 - k))
        px = (r5(r) << 11) | (g6(g) << 5) | b5(b)
        out += struct.pack("<H", px)
    return bytes(out)


def raw_deflate(data: bytes, level: int = 9) -> bytes:
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def parse_desc_from_obj(j: dict) -> str:
    for e in j.get("flavor_text_entries", []):
        if e.get("language", {}).get("name") != "en":
            continue
        t = e["flavor_text"].replace("\n", " ").replace("\x0c", " ").replace("\r", " ")
        t = " ".join(t.split())
        t = unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode("ascii")
        return t[: DESC_MAX - 1]
    return ""


def cstr(s: str) -> str:
    """C 字符串字面量转义(描述可能含引号/反斜杠)。"""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def load_row(id_: int):
    try:
        j = json.loads(load_or_fetch("pokemon", id_, API.format(id_), "json"))
        types = [t["type"]["name"] for t in j["types"][:2]]
        species = json.loads(load_or_fetch("species", id_, SPECIES_API.format(id_), "json"))
        name = species.get("name") or j["name"]
        if len(name) >= 16:
            name = name[:15]
        desc = parse_desc_from_obj(species)
        return id_, name, int(j["height"]), int(j["weight"]), types, desc
    except Exception as e:  # noqa: BLE001
        print(f"  warn: id={id_} fetch failed: {e}")
        return id_, None, 0, 0, [], ""


def load_sprite(id_: int):
    try:
        png = load_or_fetch("sprite", id_, SPRITE.format(id_), "png")
        rgb = sprite_to_rgb565_id(png)
        assert len(rgb) == SPRITE_BYTES, (id_, len(rgb))
        return id_, raw_deflate(rgb)
    except Exception as e:  # noqa: BLE001
        print(f"  warn: id={id_} sprite failed: {e}")
        return id_, None


def main() -> None:
    os.makedirs(CACHE, exist_ok=True)
    ids = list(range(DEX_FIRST, DEX_LAST + 1))

    print(f"拉取 {len(ids)} 条宝可梦数据(含英文描述,cache={CACHE}) ...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for i, r in enumerate(ex.map(load_row, ids), 1):
            rows.append(r)
            if i % 50 == 0 or i == len(ids):
                print(f"  data {i}/{len(ids)}")
    rows.sort()
    missing = [id_ for id_, name, *_ in rows if not name]
    if missing:
        raise SystemExit(f"缺少 {len(missing)} 条数据,例如 {missing[:8]}")

    print("拉取并压缩精灵图 ...")
    sprites: dict[int, bytes] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for i, (id_, comp) in enumerate(ex.map(load_sprite, ids), 1):
            sprites[id_] = comp if comp is not None else raw_deflate(bytes(SPRITE_BYTES))
            if i % 50 == 0 or i == len(ids):
                print(f"  sprite {i}/{len(ids)}")

    toc = []
    blob = bytearray()
    for id_ in ids:
        comp = sprites[id_]
        toc.append((len(blob), len(comp), DEX_W, DEX_H))
        blob += comp

    os.makedirs(os.path.dirname(OUT_BIN) or ".", exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        for off, ln, w, h in toc:
            f.write(struct.pack("<IIHH", off, ln, w, h))
        f.write(blob)
    print(f"  {OUT_BIN}: {len(blob) + len(toc) * 12} bytes (压缩后数据 {len(blob)}B)")

    types_all = sorted({t for _, _, _, _, ts, _ in rows for t in ts})
    type_idx = {t: i for i, t in enumerate(types_all)}
    print(f"  types: {len(types_all)} -> {types_all}")

    n_entries = DEX_LAST + 1  # 下标=id,含 [0]
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// main/pokedex_static.h —— 离线静态图鉴数据(生成文件,勿手改)。\n")
        f.write("// 生成:tools/gen_pokedex_static.py;数据来源 PokeAPI (CC-BY 4.0)。\n")
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write(f"#define POKEDEX_STATIC_DEX_W {DEX_W}u\n")
        f.write(f"#define POKEDEX_STATIC_DEX_H {DEX_H}u\n")
        f.write(f"#define POKEDEX_STATIC_SPRITE_BYTES {SPRITE_BYTES}u\n")
        f.write(f"#define POKEDEX_DESC_MAX {DESC_MAX}u\n")
        f.write(f"#define POKEDEX_STATIC_LAST {DEX_LAST}u\n\n")
        f.write("typedef struct {\n"
                f"    uint16_t id;      /* 1..{DEX_LAST} */\n"
                "    int16_t  height_dm;\n"
                "    int16_t  weight_hg;\n"
                "    uint8_t  type0;   /* POKEDEX_STATIC_TYPE_* 索引 */\n"
                "    uint8_t  type1;   /* 0xFF = 无第二属性 */\n"
                "    char     name[16];\n"
                "    char     desc[POKEDEX_DESC_MAX]; /* 英文图鉴描述 */\n"
                "} pokedex_static_entry_t;\n\n")
        f.write("enum {\n")
        for i, t in enumerate(types_all):
            f.write(f"    POKEDEX_STATIC_TYPE_{t.upper().replace('-', '_')} = {i},\n")
        f.write("    POKEDEX_STATIC_TYPE_COUNT,\n    POKEDEX_STATIC_TYPE_NONE = 0xFF,\n};\n\n")
        f.write(f"extern const pokedex_static_entry_t pokedex_static_dex[{n_entries}]; /* 下标=id */\n")
        f.write("extern const char *pokedex_static_type_names[POKEDEX_STATIC_TYPE_COUNT];\n\n")
        f.write("// 精灵图 blob 的 TOC/数据位于 pokedex_sprites.bin(由 CMake 嵌入):\n")
        f.write("extern const uint8_t _binary_pokedex_sprites_bin_start[];\n")
        f.write("extern const uint8_t _binary_pokedex_sprites_bin_end[];\n")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("// main/pokedex_static.c —— 离线静态图鉴数据(生成文件,勿手改)。\n")
        f.write("// 生成:tools/gen_pokedex_static.py;数据来源 PokeAPI (CC-BY 4.0)。\n")
        f.write("#include \"pokedex_static.h\"\n\n")
        f.write("const char *pokedex_static_type_names[POKEDEX_STATIC_TYPE_COUNT] = {\n")
        for t in types_all:
            f.write(f'    "{t}",\n')
        f.write("};\n\n")
        f.write(f"const pokedex_static_entry_t pokedex_static_dex[{n_entries}] = {{\n")
        f.write("    [0] = {0},\n")
        for id_, name, h, w, ts, desc in rows:
            t0 = type_idx.get(ts[0], 0) if ts else 0
            t1 = type_idx.get(ts[1], 0xFF) if len(ts) > 1 else 0xFF
            f.write(f'    [{id_}] = {{ {id_}, {h}, {w}, {t0}, {t1}, "{name}", "{cstr(desc)}" }},\n')
        f.write("};\n")

    print(f"  {OUT_H} / {OUT_C} 生成完成")


if __name__ == "__main__":
    main()
