// tests/test_pokedex_static.c —— 宿主单测:离线精灵图 blob 解码 + dex 表完整性。
// 依赖生成产物 main/pokedex_sprites.bin(validate.sh 在仓库根运行)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pokedex_sprite.h"
#include "pokedex_static.h"
#include "vendor/lodepng.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static uint8_t *load_blob(size_t *len)
{
    FILE *f = fopen("main/pokedex_sprites.bin", "rb");
    if (!f) {
        fprintf(stderr, "FAIL: 缺少生成产物 main/pokedex_sprites.bin;"
                        "先运行 tools/gen_pokedex_static.py\n");
        s_failures++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    fread(b, 1, (size_t)n, f);
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void test_dex_table(void)
{
    /* 1..1025 连续且拿到名字/身高体重;唯一种类 NONE 只允许在第二槽。 */
    for (uint32_t id = 1; id <= POKEDEX_DEX_LAST; id++) {
        const pokedex_static_entry_t *e = &pokedex_static_dex[id];
        CHECK(e->id == id);
        CHECK(e->name[0] != '\0');
        CHECK(e->height_dm >= 0 && e->weight_hg >= 0);
        CHECK(e->type0 < POKEDEX_STATIC_TYPE_COUNT);
        if (e->type1 != POKEDEX_STATIC_TYPE_NONE) {
            CHECK(e->type1 < POKEDEX_STATIC_TYPE_COUNT);
        }
        CHECK(strlen(e->name) < sizeof(e->name));
        CHECK(e->desc[0] != '\0');
        CHECK(strlen(e->desc) < sizeof(e->desc));
        /* LVGL 内置字体无扩展字形:描述必须是纯 ASCII。 */
        for (const char *c = e->desc; *c; c++) {
            CHECK((unsigned char)*c < 0x80);
        }
    }
    /* 抽查已知事实(防生成脚本漂移)。 */
    CHECK(strcmp(pokedex_static_dex[1].name, "bulbasaur") == 0);
    CHECK(pokedex_static_dex[1].height_dm == 7);
    CHECK(pokedex_static_dex[1].weight_hg == 69);
    CHECK(pokedex_static_dex[1].type0 == POKEDEX_STATIC_TYPE_GRASS);
    CHECK(pokedex_static_dex[1].type1 == POKEDEX_STATIC_TYPE_POISON);
    CHECK(strcmp(pokedex_static_dex[25].name, "pikachu") == 0);
    CHECK(pokedex_static_dex[26].type0 == POKEDEX_STATIC_TYPE_ELECTRIC);
    CHECK(pokedex_static_dex[122].type0 == POKEDEX_STATIC_TYPE_PSYCHIC);
    CHECK(pokedex_static_dex[122].type1 == POKEDEX_STATIC_TYPE_FAIRY);
    CHECK(strcmp(pokedex_static_dex[151].name, "mew") == 0);
    CHECK(pokedex_static_dex[197].type0 == POKEDEX_STATIC_TYPE_DARK); /* umbreon */
    CHECK(strcmp(pokedex_static_dex[252].name, "treecko") == 0);
    CHECK(strcmp(pokedex_static_dex[387].name, "turtwig") == 0);
    CHECK(strcmp(pokedex_static_dex[810].name, "grookey") == 0);
    CHECK(pokedex_static_dex[1025].id == 1025);
    CHECK(pokedex_static_dex[1025].name[0] != '\0');
    CHECK(POKEDEX_STATIC_LAST == POKEDEX_DEX_LAST);
    CHECK(strcmp(pokedex_static_dex[487].name, "giratina") == 0);
}

static void test_blob_decode(void)
{
    size_t blob_len = 0;
    uint8_t *blob = load_blob(&blob_len);
    if (!blob) return;

    static uint16_t px[POKEDEX_STATIC_SPRITE_BYTES / 2];
    uint32_t w = 0, h = 0;

    /* 全部 1025 只都能解出 48x48,且不全为零(存在内容)。 */
    for (uint32_t id = 1; id <= POKEDEX_DEX_LAST; id++) {
        CHECK(pokedex_sprite_static(blob, blob_len, id, px,
                                    sizeof(px) / 2, &w, &h));
        CHECK(w == POKEDEX_STATIC_DEX_W && h == POKEDEX_STATIC_DEX_H);
        bool nonzero = false;
        for (size_t i = 0; i < POKEDEX_STATIC_SPRITE_BYTES / 2; i++) {
            if (px[i] != 0) { nonzero = true; break; }
        }
        CHECK(nonzero);
    }

    /* 越界/非法输入必须拒绝而不崩溃。 */
    CHECK(!pokedex_sprite_static(blob, blob_len, 0, px, sizeof(px) / 2, &w, &h));
    CHECK(!pokedex_sprite_static(blob, blob_len, POKEDEX_DEX_LAST + 1, px,
                                 sizeof(px) / 2, &w, &h));
    /* 只剩 TOC、数据区为空。 */
    CHECK(!pokedex_sprite_static(blob, POKEDEX_DEX_SIZE * 12u, 1, px,
                                 sizeof(px) / 2, &w, &h));
    CHECK(!pokedex_sprite_static(blob, blob_len, 1, px, 100, &w, &h));

    /* 逐只 CRC 只读校验:与生成器一致即 blob 未被静默破坏(记录基线)。 */
    uint32_t crc = 0;
    for (uint32_t id = 1; id <= POKEDEX_DEX_LAST; id += 50) {
        if (pokedex_sprite_static(blob, blob_len, id, px, sizeof(px) / 2, &w, &h)) {
            crc ^= lodepng_crc32((const unsigned char *)px, w * h * 2);
        }
    }
    printf("  sprite CRC 抽样(隔 50 只): %08x\n", (unsigned)crc);

    free(blob);
}

int main(void)
{
    test_dex_table();
    test_blob_decode();

    if (s_failures) {
        fprintf(stderr, "pokedex_static: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_static: all checks passed\n");
    return 0;
}