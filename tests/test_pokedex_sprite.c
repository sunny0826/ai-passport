// tests/test_pokedex_sprite.c —— 宿主机单测:PNG 解码 → 混合 → RGB565 → 缩放。
// 用 lodepng 在运行时自编码一张小图,再走 pokedex_sprite_render() 全管线,
// 校验像素级输出(含 alpha 混合与最近邻采样)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pokedex_sprite.h"
#include "vendor/lodepng.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

/* 生成 RGBA8 的简单测试图:4x4,
   (0,0)=不透明红,(1,0)=不透明绿,(2,0)=不透明蓝,
   (3,0)=全透明(alpha 0,RGB 任意),
   (2,2)=半透明蓝(alpha 128),其余=不透明白。 */
static void fill_test_rgba(uint8_t *rgba, unsigned w, unsigned h)
{
    memset(rgba, 255, (size_t)w * h * 4); /* 默认不透明白 */
    rgba[0] = 255; rgba[1] = 0;   rgba[2] = 0;  /* 红 */
    rgba[4] = 0;   rgba[5] = 255; rgba[6] = 0;  /* 绿 */
    rgba[8] = 0;   rgba[9] = 0;   rgba[10] = 255; /* 蓝 */
    rgba[12] = 0; rgba[13] = 0; rgba[14] = 0; rgba[15] = 0; /* 全透明 */
    size_t half = ((size_t)2 * w + 2) * 4;
    rgba[half + 0] = 0; rgba[half + 1] = 0; rgba[half + 2] = 255;
    rgba[half + 3] = 128; /* 半透明蓝 */
}

static uint16_t read565(const uint16_t *buf, unsigned x, unsigned y, unsigned w)
{
    return buf[(size_t)y * w + x];
}

static void test_no_scale_blend(void)
{
    enum { W = 4, H = 4 };
    uint8_t rgba[W * H * 4];
    uint16_t out[POKEDEX_SPRITE_MAX_BYTES / 2];
    uint32_t ow = 0, oh = 0;
    const uint16_t bg = 0xF7DD; /* UI_PAPER 0xF4F4EA 的 RGB565 打包值 */

    fill_test_rgba(rgba, W, H);
    pokedex_rgba_to_rgb565(rgba, W, H, out, bg);

    CHECK(read565(out, 0, 0, W) == 0xF800);       /* 不透明红 */
    CHECK(read565(out, 1, 0, W) == 0x07E0);       /* 不透明绿 */
    CHECK(read565(out, 2, 0, W) == 0x001F);       /* 不透明蓝 */
    CHECK(read565(out, 3, 0, W) == bg);           /* 全透明 == 背景色 */

    /* 半透明蓝(128)混背景:期望逐通道 (src*a + bg*(255-a))/255,
       背景按线性缩放展开 (v*255)/max,保证往返打包精确。 */
    uint8_t br = (uint8_t)((bg >> 11) & 0x1F), bgg = (uint8_t)((bg >> 5) & 0x3F),
            bb = (uint8_t)(bg & 0x1F);
    uint8_t br8 = (uint8_t)((br * 255u) / 31u);
    uint8_t bg8 = (uint8_t)((bgg * 255u) / 63u);
    uint8_t bb8 = (uint8_t)((bb * 255u) / 31u);
    uint16_t expect = (uint16_t)(((uint16_t)((0 * 128 + br8 * 127) / 255) >> 3) << 11 |
                                 ((uint16_t)((0 * 128 + bg8 * 127) / 255) >> 2) << 5 |
                                 ((uint16_t)((255 * 128 + bb8 * 127) / 255) >> 3));
    CHECK(read565(out, 2, 2, W) == expect);
    CHECK(read565(out, 0, 1, W) == 0xFFFF); /* 不透明白 */

    /* 编码为 PNG 走完整管线(4x4 ≤ 96x96,应原样输出)。 */
    uint8_t *png = NULL;
    size_t png_len = 0;
    CHECK(lodepng_encode_memory(&png, &png_len, rgba, W, H, LCT_RGBA, 8) == 0);
    CHECK(pokedex_sprite_render(png, png_len, out, sizeof(out) / 2, &ow, &oh, bg));
    CHECK(ow == W && oh == H);
    CHECK(read565(out, 0, 0, W) == 0xF800);
    CHECK(read565(out, 3, 0, W) == bg);
    CHECK(read565(out, 2, 1, W) == 0xFFFF);
    free(png);
}

static void test_downscale_nearest(void)
{
    enum { W = 192, H = 96 };
    static uint8_t rgba[W * H * 4];
    static uint16_t out[POKEDEX_SPRITE_MAX_BYTES / 2];
    uint32_t ow = 0, oh = 0;

    /* 左半红(RGB 255,0,0)、右半蓝;192x96 → 96x48,
       最近邻取源 (x*192/96, y*96/48) = (2x, 2y)。 */
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t i = (y * W + x) * 4;
            rgba[i] = x < 96 ? 255 : 0;
            rgba[i + 1] = 0;
            rgba[i + 2] = x < 96 ? 0 : 255;
            rgba[i + 3] = 255;
        }
    }
    uint8_t *png = NULL;
    size_t png_len = 0;
    CHECK(lodepng_encode_memory(&png, &png_len, rgba, W, H, LCT_RGBA, 8) == 0);
    CHECK(pokedex_sprite_render(png, png_len, out, sizeof(out) / 2, &ow, &oh, 0xFFFF));
    CHECK(ow == 96 && oh == 48);
    CHECK(read565(out, 0, 0, 96) == 0xF800);   /* 左红 */
    CHECK(read565(out, 47, 0, 96) == 0xF800);  /* x=47 -> src x=94,仍左半红 */
    CHECK(read565(out, 48, 0, 96) == 0x001F);  /* x=48 -> src x=96,右半蓝 */
    CHECK(read565(out, 95, 47, 96) == 0x001F); /* 右下蓝 */
    free(png);
}

static void test_render_bad_input(void)
{
    uint16_t out[POKEDEX_SPRITE_MAX_BYTES / 2];
    uint32_t ow = 0, oh = 0;
    const uint8_t not_png[] = "this is not a png file at all";

    CHECK(!pokedex_sprite_render(NULL, 0, out, sizeof(out) / 2, &ow, &oh, 0xFFFF));
    CHECK(!pokedex_sprite_render(not_png, sizeof(not_png), out,
                                 sizeof(out) / 2, &ow, &oh, 0xFFFF));
    /* 输出缓冲过小。 */
    CHECK(!pokedex_sprite_render(not_png, sizeof(not_png), out, 8, &ow, &oh, 0xFFFF));
}

int main(void)
{
    test_no_scale_blend();
    test_downscale_nearest();
    test_render_bad_input();

    if (s_failures) {
        fprintf(stderr, "pokedex_sprite: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_sprite: all checks passed\n");
    return 0;
}