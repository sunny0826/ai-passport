// main/pokedex_sprite.h —— 精灵图渲染管线。
// 纯 C + 内置 lodepng(main/vendor/lodepng.c, MIT),无 ESP-IDF/LVGL 依赖,
// 可在宿主机单测(tests/test_pokedex_sprite.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pokedex_core.h"

// 把 PNG 精灵图解码 → 与背景色混合 → 最近邻缩放到 ≤ POKEDEX_SPRITE_MAX_W/H
// → 输出 RGB565 像素。decode 结果在函数内临时分配并释放,调用方只需提供
// 足够大的 rgb565 输出缓冲(至少 POKEDEX_SPRITE_MAX_BYTES)。
//
// 返回 false 表示 PNG 损坏/不支持的格式/输出缓冲太小。
// 成功时 *out_w / *out_h 为实际输出尺寸(1..96)。
//
// bg_rgb565:透明像素混合用的背景色(RGB565 小端),精灵图在浅色面板上
// 显示时传 UI_PAPER 对应的 0xF4F4EA 即可,透明度按 alpha 线性混合。
bool pokedex_sprite_render(const uint8_t *png, size_t png_len,
                           uint16_t *rgb565_out, size_t out_cap_u16,
                           uint32_t *out_w, uint32_t *out_h,
                           uint16_t bg_rgb565);

// 单元测试辅助:把 RGBA 像素数组直接(不缩放)转成 RGB565 并混入背景色。
void pokedex_rgba_to_rgb565(const uint8_t *rgba, uint32_t w, uint32_t h,
                            uint16_t *rgb565_out, uint16_t bg_rgb565);

// ---------------------------------------------------------------------------
// 离线精灵图 blob(工具生成,见 tools/gen_pokedex_static.py):
//   [0..151*8)   TOC,每条 {off(u32le), len(u32le), w(u16le), h(u16le)}
//   [151*8..)    raw-deflate 压缩的 RGB565 像素(解压后 w*h*2 字节)
// ---------------------------------------------------------------------------
// 解出第 id 只的 RGB565 精灵图(48x48)。out_cap 为输出缓冲的 u16 单元数。
// 返回 false = 越界/解压失败/尺寸不符。
bool pokedex_sprite_static(const uint8_t *blob, size_t blob_len, uint32_t id,
                           uint16_t *rgb565_out, size_t out_cap_u16,
                           uint32_t *out_w, uint32_t *out_h);