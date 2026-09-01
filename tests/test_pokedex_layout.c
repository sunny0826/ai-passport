// tests/test_pokedex_layout.c —— 宿主机单测:掌上图鉴布局几何与短文案。
#include <stdio.h>
#include <string.h>

#include "pokedex_layout.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void check_no_overlap(pokedex_rect_t a, pokedex_rect_t b,
                             const char *an, const char *bn)
{
    if (pokedex_rect_overlaps(a, b)) {
        fprintf(stderr, "FAIL overlap %s (%d,%d %dx%d) vs %s (%d,%d %dx%d)\n",
                an, a.x, a.y, a.w, a.h, bn, b.x, b.y, b.w, b.h);
        s_failures++;
    }
}

static void test_bounds_and_nesting(void)
{
    pokedex_layout_t L;
    pokedex_layout_build(&L);

    CHECK(pokedex_rect_in_bounds(L.screen, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.header, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.header_rule, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.title, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.progress, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.battery, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.sprite_frame, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.sprite_inner, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.sprite, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.number_chip, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.number, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.name, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.badge[0], POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.badge[1], POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.stats, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.stats_text, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.flavor_frame, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.flavor_inner, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.flavor_text, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.tally_seen, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));
    CHECK(pokedex_rect_in_bounds(L.hint, POKEDEX_LAYOUT_W, POKEDEX_LAYOUT_H));

    CHECK(pokedex_rect_contains(L.header, L.title));
    CHECK(pokedex_rect_contains(L.header, L.progress));
    CHECK(pokedex_rect_contains(L.header, L.battery));
    CHECK(pokedex_rect_contains(L.sprite_frame, L.sprite_inner));
    CHECK(pokedex_rect_contains(L.sprite_inner, L.sprite));
    CHECK(pokedex_rect_contains(L.number_chip, L.number));
    CHECK(pokedex_rect_contains(L.stats, L.stats_text));
    CHECK(pokedex_rect_contains(L.flavor_frame, L.flavor_inner));
    CHECK(pokedex_rect_contains(L.flavor_inner, L.flavor_text));

    CHECK(L.sprite.w == POKEDEX_LAYOUT_SPRITE_PX);
    CHECK(L.sprite.h == POKEDEX_LAYOUT_SPRITE_PX);
    CHECK(L.name.w == 120);
    CHECK(L.name.h >= 32); /* 14px 两行,长种名折行而不进精灵井 */
    CHECK(L.name.x + L.name.w <= L.sprite_frame.x);
    CHECK(L.flavor_text.h >= 80); /* 至少约 5 行 14px 字 */
}

static void test_no_sibling_overlap(void)
{
    pokedex_layout_t L;
    pokedex_layout_build(&L);

    check_no_overlap(L.header, L.sprite_frame, "header", "sprite");
    check_no_overlap(L.header_rule, L.sprite_frame, "rule", "sprite");
    check_no_overlap(L.title, L.progress, "title", "progress");
    check_no_overlap(L.progress, L.battery, "progress", "battery");
    check_no_overlap(L.title, L.battery, "title", "battery");

    check_no_overlap(L.number_chip, L.sprite_frame, "number", "sprite");
    check_no_overlap(L.name, L.sprite_frame, "name", "sprite");
    check_no_overlap(L.badge[0], L.sprite_frame, "badge0", "sprite");
    check_no_overlap(L.badge[1], L.sprite_frame, "badge1", "sprite");
    check_no_overlap(L.badge[0], L.badge[1], "badge0", "badge1");
    check_no_overlap(L.number_chip, L.name, "number", "name");
    check_no_overlap(L.name, L.badge[0], "name", "badge0");
    check_no_overlap(L.name, L.badge[1], "name", "badge1");
    check_no_overlap(L.badge[0], L.stats, "badge0", "stats");
    check_no_overlap(L.badge[1], L.stats, "badge1", "stats");
    check_no_overlap(L.name, L.stats, "name", "stats");
    check_no_overlap(L.sprite_frame, L.stats, "sprite", "stats");
    check_no_overlap(L.stats, L.flavor_frame, "stats", "flavor");
    check_no_overlap(L.flavor_frame, L.tally_seen, "flavor", "seen");
    check_no_overlap(L.tally_seen, L.hint, "seen", "hint");
}

static void test_type_abbr(void)
{
    char buf[8];

    CHECK(pokedex_layout_type_abbr("dark", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "DRK") == 0);
    CHECK(pokedex_layout_type_abbr("electric", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "ELE") == 0);
    CHECK(pokedex_layout_type_abbr("fighting", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "FIG") == 0);
    CHECK(pokedex_layout_type_abbr("grass", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "GRA") == 0);
    CHECK(pokedex_layout_type_abbr("poison", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "PSN") == 0);
    CHECK(pokedex_layout_type_abbr("water", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "WAT") == 0);

    CHECK(pokedex_layout_type_abbr("unknown", buf, sizeof(buf)) == 3);
    CHECK(strcmp(buf, "UNK") == 0);
    CHECK(pokedex_layout_type_abbr("ab", buf, sizeof(buf)) == 2);
    CHECK(strcmp(buf, "AB") == 0);
    CHECK(pokedex_layout_type_abbr("", buf, sizeof(buf)) == 0);
    CHECK(buf[0] == '\0');
    CHECK(pokedex_layout_type_abbr("fire", buf, 3) == 0);
}

static void test_formatters(void)
{
    char buf[64];

    CHECK(pokedex_layout_format_no(1, buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "NO.001") == 0);
    pokedex_layout_format_no(151, buf, sizeof(buf));
    CHECK(strcmp(buf, "NO.151") == 0);

    pokedex_layout_format_progress(25, buf, sizeof(buf));
    CHECK(strcmp(buf, "025/1025") == 0);
    pokedex_layout_format_progress(1025, buf, sizeof(buf));
    CHECK(strcmp(buf, "1025/1025") == 0);

    pokedex_layout_format_caught(3, buf, sizeof(buf));
    CHECK(strcmp(buf, "3 CAUGHT") == 0);
    pokedex_layout_format_seen(12, buf, sizeof(buf));
    CHECK(strcmp(buf, "12 SEEN") == 0);
    pokedex_layout_format_caught(151, buf, sizeof(buf));
    CHECK(strcmp(buf, "151 CAUGHT") == 0);

    pokedex_layout_format_stats(4, 60, buf, sizeof(buf));
    CHECK(strcmp(buf, "HT 0.4 m    WT 6.0 kg") == 0);
    pokedex_layout_format_stats(20, 1000, buf, sizeof(buf));
    CHECK(strcmp(buf, "HT 2.0 m    WT 100.0 kg") == 0);
}

static void test_clip_desc(void)
{
    char dst[32];
    const char *short_txt = "A strange seed was planted.";
    const char *long_txt =
        "When several of these POKeMON gather, their electricity could "
        "build and cause lightning storms.";

    CHECK(pokedex_layout_clip_desc(short_txt, dst, sizeof(dst), 0) ==
          strlen(short_txt));
    CHECK(strcmp(dst, short_txt) == 0);

    CHECK(pokedex_layout_clip_desc(long_txt, dst, sizeof(dst), 20) == 20);
    CHECK(strlen(dst) == 20);
    CHECK(strcmp(dst + 17, "...") == 0);
    CHECK(strncmp(dst, long_txt, 17) == 0);

    CHECK(pokedex_layout_clip_desc(long_txt, dst, 8, 20) == 7);
    CHECK(strlen(dst) == 7);
    CHECK(strcmp(dst + 4, "...") == 0);

    CHECK(pokedex_layout_clip_desc(NULL, dst, sizeof(dst), 10) == 0);
    CHECK(dst[0] == '\0');
}

static void test_scale2x(void)
{
    uint16_t buf[16];
    uint32_t dw = 0, dh = 0;

    buf[0] = 0xF800;
    buf[1] = 0x07E0;
    buf[2] = 0x001F;
    buf[3] = 0xFFFF;
    CHECK(pokedex_layout_scale2x_rgb565(buf, 2, 2, buf, 16, &dw, &dh));
    CHECK(dw == 4 && dh == 4);
    CHECK(buf[0] == 0xF800 && buf[1] == 0xF800);
    CHECK(buf[4] == 0xF800 && buf[5] == 0xF800);
    CHECK(buf[2] == 0x07E0 && buf[3] == 0x07E0);
    CHECK(buf[6] == 0x07E0 && buf[7] == 0x07E0);
    CHECK(buf[8] == 0x001F && buf[9] == 0x001F);
    CHECK(buf[10] == 0xFFFF && buf[11] == 0xFFFF);
    CHECK(buf[12] == 0x001F && buf[13] == 0x001F);
    CHECK(buf[14] == 0xFFFF && buf[15] == 0xFFFF);

    CHECK(!pokedex_layout_scale2x_rgb565(buf, 2, 2, buf, 15, &dw, &dh));
    CHECK(!pokedex_layout_scale2x_rgb565(buf, 0, 2, buf, 16, &dw, &dh));
}

static void test_dark_ink(void)
{
    CHECK(pokedex_layout_dark_ink(0xF7D02C));  /* electric */
    CHECK(pokedex_layout_dark_ink(0x96D9D6));  /* ice */
    CHECK(pokedex_layout_dark_ink(0xB7B7CE));  /* steel */
    CHECK(pokedex_layout_dark_ink(0xE2BF65));  /* ground */
    CHECK(!pokedex_layout_dark_ink(0xC22E28)); /* fighting */
    CHECK(!pokedex_layout_dark_ink(0x6390F0)); /* water */
    CHECK(!pokedex_layout_dark_ink(0xA33EA1)); /* poison */
}

int main(void)
{
    test_bounds_and_nesting();
    test_no_sibling_overlap();
    test_type_abbr();
    test_formatters();
    test_clip_desc();
    test_scale2x();
    test_dark_ink();

    if (s_failures) {
        fprintf(stderr, "pokedex_layout: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_layout: all checks passed\n");
    return 0;
}
