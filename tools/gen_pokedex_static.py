#!/usr/bin/env python3
"""生成 Pokédex 离线静态数据库。

产物(在 main/ 下,随固件编译/入库):
  - pokedex_static.h / .c   : 第 1 世代 1..151 的基本数据(name/height/weight/
                              types),来源 PokeAPI(https://pokeapi.co, CC-BY 4.0)。
  - pokedex_sprites.bin     : 每只 48x48 RGB565 精灵图,raw-deflate 压缩;
                              258 字节 TOC(151 x {off,len,w,h}) + 压缩数据。

用法:python3 tools/gen_pokedex_static.py
精灵图版权归 The Pokémon Company / Nintendo(非关联项目,仅供个人学习)。
"""
import concurrent.futures
import io
import json
import struct
import unicodedata
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


def fetch(url: str, timeout: int = 30) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "ai-passport-pokedex-gen/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def sprite_to_rgb565_id(png: bytes) -> bytes:
    """把官方 PNG 缩到 48x48 最近邻并转 RGB565(透明像素混纸色 0xF7DD)。"""
    import zlib as _z

    class LodePngLite:
        pass

    # 用系统 zlib 解析 PNG IDAT 通道做最小解码(仅支持 8-bit RGBA/RGB/调色板一阶足矣——
    # 官方精灵图为 8bit RGBA 或调色板彩图,统一经 pypng? 这里复用 lodepng 需要宿主库。
    # 简化:直接用 Pillow(可选依赖)完成解码缩放,失败则报错。
    try:
        from PIL import Image
    except ImportError as e:
        raise SystemExit(f"需要 Pillow: pip install pillow ({e})")

    im = Image.open(io.BytesIO(png)).convert("RGBA")
    im = im.resize((DEX_W, DEX_H), Image.NEAREST)
    out = bytearray()
    r5 = lambda v: (v >> 3) & 0x1F
    g6 = lambda v: (v >> 2) & 0x3F
    b5 = lambda v: (v >> 3) & 0x1F
    BG = (0x24, 0x42, 0x38)  # 图鉴屏幕底 #244238(参考 pokedex.guoxudong.io),透明像素混此色
    for (r, g, b, a) in im.getdata():
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


def fetch_desc(id_: int) -> str:
    """英文图鉴描述(flavor text):取第一条英文,清理换行/分页符并截断。"""
    try:
        j = json.loads(fetch(SPECIES_API.format(id_)))
    except Exception as e:  # noqa: BLE001
        print(f"  warn: id={id_} species fetch failed: {e}")
        return ""
    for e in j.get("flavor_text_entries", []):
        if e.get("language", {}).get("name") != "en":
            continue
        t = e["flavor_text"].replace("\n", " ").replace("\x0c", " ").replace("\r", " ")
        t = " ".join(t.split())
        # LVGL 内置字体无拉丁扩展字形:去重音(POKéMON -> POKEMON)。
        t = unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode("ascii")
        return t[:DESC_MAX - 1]
    return ""


def cstr(s: str) -> str:
    """C 字符串字面量转义(描述可能含引号/反斜杠)。"""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main() -> None:
    ids = list(range(1, 152))

    def load(id_: int):
        try:
            j = json.loads(fetch(API.format(id_)))
            types = [t["type"]["name"] for t in j["types"][:2]]
            return id_, j["name"], int(j["height"]), int(j["weight"]), types, fetch_desc(id_)
        except Exception as e:  # noqa: BLE001
            print(f"  warn: id={id_} fetch failed: {e}")
            return id_, None, 0, 0, [], ""

    print(f"拉取 {len(ids)} 条宝可梦数据(含英文描述) ...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(load, ids):
            rows.append(r)
    rows.sort()

    print("拉取并压缩精灵图 ...")
    toc = []
    blob = bytearray()
    def sprites(id_):
        try:
            png = fetch(SPRITE.format(id_))
            rgb = sprite_to_rgb565_id(png)
            assert len(rgb) == SPRITE_BYTES, (id_, len(rgb))
            return id_, raw_deflate(rgb)
        except Exception as e:  # noqa: BLE001
            print(f"  warn: id={id_} sprite failed: {e}")
            return id_, None

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for id_, comp in ex.map(sprites, ids):
            if comp is None:
                comp = raw_deflate(bytes(SPRITE_BYTES))  # 黑色占位,不阻断
            toc.append((len(blob), len(comp), DEX_W, DEX_H))
            blob += comp

    with open(OUT_BIN, "wb") as f:
        for off, ln, w, h in toc:
            f.write(struct.pack("<IIHH", off, ln, w, h))
        f.write(blob)
    print(f"  {OUT_BIN}: {len(blob)+len(toc)*8} bytes (压缩后数据 {len(blob)}B)")

    types_all = sorted({t for _, _, _, _, ts, _ in rows for t in ts})
    type_idx = {t: i for i, t in enumerate(types_all)}
    print(f"  types: {len(types_all)} -> {types_all}")

    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("// main/pokedex_static.h —— 离线静态图鉴数据(生成文件,勿手改)。\n")
        f.write("// 生成:tools/gen_pokedex_static.py;数据来源 PokeAPI (CC-BY 4.0)。\n")
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write(f"#define POKEDEX_STATIC_DEX_W {DEX_W}u\n")
        f.write(f"#define POKEDEX_STATIC_DEX_H {DEX_H}u\n")
        f.write(f"#define POKEDEX_STATIC_SPRITE_BYTES {SPRITE_BYTES}u\n")
        f.write(f"#define POKEDEX_DESC_MAX {DESC_MAX}u\n\n")
        f.write("typedef struct {\n"
                "    uint16_t id;      /* 1..151 */\n"
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
        f.write("extern const pokedex_static_entry_t pokedex_static_dex[152]; /* 下标=id */\n")
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
        f.write("const pokedex_static_entry_t pokedex_static_dex[152] = {\n")
        f.write("    [0] = {0},\n")
        for id_, name, h, w, ts, desc in rows:
            t0 = type_idx.get(ts[0], 0) if ts else 0
            t1 = type_idx.get(ts[1], 0xFF) if len(ts) > 1 else 0xFF
            f.write(f'    [{id_}] = {{ {id_}, {h}, {w}, {t0}, {t1}, "{name}", "{cstr(desc)}" }},\n')
        f.write("};\n")

    print(f"  {OUT_H} / {OUT_C} 生成完成")


if __name__ == "__main__":
    main()