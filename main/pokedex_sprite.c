// main/pokedex_sprite.c —— 精灵图渲染管线实现。
// lodepng 解码为 RGBA8 后,先在 8bit 通道上按 alpha 混合背景色,再压缩为
// RGB565(小端:低字节在前),缩放用最近邻(像素风精灵图不需要平滑)。
#include "pokedex_sprite.h"

#include "vendor/lodepng.h"

#include <stdlib.h>
#include <string.h>

// RGB565 位域展开回 8bit 通道:用线性缩放 (v*255)/max 而非移位复制,
// 保证 pack(expand(bg)) == bg 的往返精确性(移位展开 g6=62 -> 255 -> 63 会漂移,
// 导致全透明像素混完不等于背景色)。
static uint8_t expand_rgb565(uint16_t c, unsigned shift, unsigned bits)
{
    unsigned v = (unsigned)((c >> shift) & ((1u << bits) - 1u));
    if (bits == 6) return (uint8_t)((v * 255u) / 63u);
    return (uint8_t)((v * 255u) / 31u);
}

static uint8_t r565_hi(uint16_t c) { return expand_rgb565(c, 11, 5); }
static uint8_t r565_mid(uint16_t c) { return expand_rgb565(c, 5, 6); }
static uint8_t r565_lo(uint16_t c) { return expand_rgb565(c, 0, 5); }

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5) |
                      ((uint16_t)(b >> 3)));
}

void pokedex_rgba_to_rgb565(const uint8_t *rgba, uint32_t w, uint32_t h,
                            uint16_t *rgb565_out, uint16_t bg_rgb565)
{
    /* 背景按线性缩放展开为 8bit,保证 pack(expand(bg)) == bg。 */
    const uint8_t bg_r8 = r565_hi(bg_rgb565), bg_g8 = r565_mid(bg_rgb565),
                  bg_b8 = r565_lo(bg_rgb565);

    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        if (a < 255) {
            r = (uint8_t)(((uint16_t)r * a + (uint16_t)bg_r8 * (255 - a)) / 255);
            g = (uint8_t)(((uint16_t)g * a + (uint16_t)bg_g8 * (255 - a)) / 255);
            b = (uint8_t)(((uint16_t)b * a + (uint16_t)bg_b8 * (255 - a)) / 255);
        }
        rgb565_out[i] = pack_rgb565(r, g, b);
    }
}

// 离线 blob 精灵图:raw-deflate 压缩的 RGB565(lodepng_inflate 解压)。
#define SPRITE_TOC_ITEM 12u /* off u32 + len u32 + w u16 + h u16,见生成脚本 */
#define SPRITE_TOC_LEN (POKEDEX_DEX_SIZE * SPRITE_TOC_ITEM)
#define SPRITE_TOC_OFF(i) ((i) * SPRITE_TOC_ITEM)

bool pokedex_sprite_static(const uint8_t *blob, size_t blob_len, uint32_t id,
                           uint16_t *rgb565_out, size_t out_cap_u16,
                           uint32_t *out_w, uint32_t *out_h)
{
    if (!blob || !rgb565_out || !out_w || !out_h) return false;
    if (!pokedex_id_in_range(id) || blob_len < SPRITE_TOC_LEN) return false;

    const size_t o = SPRITE_TOC_OFF(id - 1);
    uint32_t off = 0, len = 0;
    for (int i = 0; i < 4; i++) {
        off |= (uint32_t)blob[o + i] << (8 * i);
        len |= (uint32_t)blob[o + 4 + i] << (8 * i);
    }
    uint32_t w = (uint32_t)blob[o + 8] | ((uint32_t)blob[o + 9] << 8);
    uint32_t h = (uint32_t)blob[o + 10] | ((uint32_t)blob[o + 11] << 8);
    /* TOC 记录的偏移相对【数据区】起点(TOC 之后)。 */
    const uint8_t *data = blob + SPRITE_TOC_LEN;
    size_t data_len = blob_len - SPRITE_TOC_LEN;
    if (data_len < 1) return false;
    if (off > data_len || len > data_len - off) return false;
    if ((uint64_t)w * h * 2 > out_cap_u16 * 2) return false;

    /* lodepng_inflate:raw deflate,无容器头。输出由它 malloc。 */
    unsigned char *out = NULL;
    size_t out_size = 0;
    LodePNGDecompressSettings ds;
    lodepng_decompress_settings_init(&ds);
    unsigned err = lodepng_inflate(&out, &out_size, data + off, len, &ds);
    if (err != 0) {
        free(out);
        return false;
    }
    if (out_size != (size_t)w * h * 2) {
        free(out);
        return false;
    }
    memcpy(rgb565_out, out, out_size);
    free(out);
    *out_w = w;
    *out_h = h;
    return true;
}

bool pokedex_sprite_render(const uint8_t *png, size_t png_len,
                           uint16_t *rgb565_out, size_t out_cap_u16,
                           uint32_t *out_w, uint32_t *out_h,
                           uint16_t bg_rgb565)
{
    if (!png || !png_len || !rgb565_out || !out_w || !out_h) return false;
    if (out_cap_u16 < POKEDEX_SPRITE_MAX_BYTES / 2) return false;

    unsigned w = 0, h = 0;
    unsigned char *rgba = NULL;

    /* lodepng_decode_memory 总是解码为 RGBA8,由我们 free。 */
    unsigned err = lodepng_decode_memory(&rgba, &w, &h, png, png_len,
                                         LCT_RGBA, 8);
    if (err != 0) {
        free(rgba);
        return false;
    }

    uint32_t dw = w, dh = h;
    if (!pokedex_fit_scaled(w, h, POKEDEX_SPRITE_MAX_W, POKEDEX_SPRITE_MAX_H,
                            &dw, &dh)) {
        free(rgba);
        return false;
    }
    if ((uint64_t)dw * dh > out_cap_u16) {
        free(rgba);
        return false;
    }

    if (dw == w && dh == h) {
        /* 无需缩放:直接转换。 */
        pokedex_rgba_to_rgb565(rgba, w, h, rgb565_out, bg_rgb565);
    } else {
        const uint8_t bg_r8 = r565_hi(bg_rgb565), bg_g8 = r565_mid(bg_rgb565),
                      bg_b8 = r565_lo(bg_rgb565);

        /* 最近邻采样:目标像素 (x,y) 取源 (x*w/dw, y*h/dh)。 */
        for (uint32_t y = 0; y < dh; y++) {
            uint32_t sy = (y * h) / dh;
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t sx = (x * w) / dw;
                const uint8_t *px = &rgba[(sy * w + sx) * 4];
                uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
                if (a < 255) {
                    r = (uint8_t)(((uint16_t)r * a + (uint16_t)bg_r8 * (255 - a)) / 255);
                    g = (uint8_t)(((uint16_t)g * a + (uint16_t)bg_g8 * (255 - a)) / 255);
                    b = (uint8_t)(((uint16_t)b * a + (uint16_t)bg_b8 * (255 - a)) / 255);
                }
                rgb565_out[y * dw + x] = pack_rgb565(r, g, b);
            }
        }
    }

    free(rgba);
    *out_w = dw;
    *out_h = dh;
    return true;
}