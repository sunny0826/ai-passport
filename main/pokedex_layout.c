// main/pokedex_layout.c —— 掌上图鉴布局与短文案格式化。
#include "pokedex_layout.h"
#include "pokedex_core.h"

#include <stdio.h>
#include <string.h>

void pokedex_layout_build(pokedex_layout_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* 240x320 手持图鉴:顶栏身份,右上 96px 精灵井,左列编号/名/属性,
       中部身高体重条,下部概述,底栏见过计数与按键提示。矩形之间留 4px 缝,
       左列右缘 128、精灵井左缘 132,长属性名不再能把徽章推进井里。 */
    out->screen      = (pokedex_rect_t){0, 0, 240, 320};
    out->header      = (pokedex_rect_t){0, 0, 240, 22};
    out->header_rule = (pokedex_rect_t){0, 22, 240, 2};
    out->title       = (pokedex_rect_t){8, 4, 70, 16};
    out->progress    = (pokedex_rect_t){80, 4, 94, 16}; /* "1025/1025" */
    out->battery     = (pokedex_rect_t){178, 4, 54, 16};

    out->sprite_frame = (pokedex_rect_t){132, 28, 100, 100};
    out->sprite_inner = (pokedex_rect_t){134, 30, 96, 96};
    out->sprite       = (pokedex_rect_t){134, 30, 96, 96};

    out->number_chip = (pokedex_rect_t){8, 30, 116, 18};
    out->number      = (pokedex_rect_t){12, 32, 108, 14};
    out->name        = (pokedex_rect_t){8, 52, 120, 24};
    out->badge[0]    = (pokedex_rect_t){8, 80, POKEDEX_LAYOUT_BADGE_W,
                                        POKEDEX_LAYOUT_BADGE_H};
    out->badge[1]    = (pokedex_rect_t){58, 80, POKEDEX_LAYOUT_BADGE_W,
                                        POKEDEX_LAYOUT_BADGE_H};

    out->stats      = (pokedex_rect_t){8, 134, 224, 22};
    out->stats_text = (pokedex_rect_t){8, 137, 224, 16};

    out->flavor_frame = (pokedex_rect_t){8, 160, 224, 108};
    out->flavor_inner = (pokedex_rect_t){10, 162, 220, 104};
    out->flavor_text  = (pokedex_rect_t){14, 166, 212, 96};

    out->tally_seen   = (pokedex_rect_t){8, 272, 224, 20};
    out->hint         = (pokedex_rect_t){8, 296, 224, 16};
}

bool pokedex_rect_in_bounds(pokedex_rect_t r, int w, int h)
{
    if (r.w <= 0 || r.h <= 0) return false;
    if (r.x < 0 || r.y < 0) return false;
    return (int)r.x + (int)r.w <= w && (int)r.y + (int)r.h <= h;
}

bool pokedex_rect_overlaps(pokedex_rect_t a, pokedex_rect_t b)
{
    return (int)a.x < (int)b.x + b.w && (int)b.x < (int)a.x + a.w &&
           (int)a.y < (int)b.y + b.h && (int)b.y < (int)a.y + a.h;
}

bool pokedex_rect_contains(pokedex_rect_t outer, pokedex_rect_t inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

int pokedex_layout_type_abbr(const char *type_name, char *buf, size_t cap)
{
    static const struct {
        const char *name;
        const char *abbr;
    } table[] = {
        {"bug", "BUG"},
        {"dark", "DRK"},
        {"dragon", "DRG"},
        {"electric", "ELE"},
        {"fairy", "FRY"},
        {"fighting", "FIG"},
        {"fire", "FIR"},
        {"flying", "FLY"},
        {"ghost", "GHO"},
        {"grass", "GRA"},
        {"ground", "GND"},
        {"ice", "ICE"},
        {"normal", "NOR"},
        {"poison", "PSN"},
        {"psychic", "PSY"},
        {"rock", "RCK"},
        {"steel", "STL"},
        {"water", "WAT"},
    };

    if (!buf || cap < 4) return 0;
    buf[0] = '\0';
    if (!type_name || !type_name[0]) return 0;

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(type_name, table[i].name) == 0) {
            memcpy(buf, table[i].abbr, 4);
            return 3;
        }
    }

    /* 未知属性:取前三字母并大写,保证徽章宽度仍是 3 字符。 */
    for (int i = 0; i < 3; i++) {
        char c = type_name[i];
        if (c == '\0') {
            buf[i] = '\0';
            return i;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[i] = c;
    }
    buf[3] = '\0';
    return 3;
}

int pokedex_layout_format_no(uint32_t id, char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    return snprintf(buf, cap, "NO.%03u", (unsigned)id);
}

int pokedex_layout_format_progress(uint32_t id, char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    return snprintf(buf, cap, "%03u/%u", (unsigned)id, (unsigned)POKEDEX_DEX_LAST);
}

int pokedex_layout_format_caught(uint32_t n, char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    return snprintf(buf, cap, "%u CAUGHT", (unsigned)n);
}

int pokedex_layout_format_seen(uint32_t n, char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    return snprintf(buf, cap, "%u SEEN", (unsigned)n);
}

int pokedex_layout_format_stats(int height_dm, int weight_hg,
                                char *buf, size_t cap)
{
    char h[16];
    char w[16];

    if (!buf || !cap) return 0;
    pokedex_format_height(height_dm, h, sizeof(h));
    pokedex_format_weight(weight_hg, w, sizeof(w));
    return snprintf(buf, cap, "HT %s    WT %s", h, w);
}

size_t pokedex_layout_clip_desc(const char *src, char *dst, size_t dst_cap,
                                size_t max_chars)
{
    size_t n;
    size_t limit;

    if (!dst || dst_cap == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;

    if (max_chars == 0) max_chars = POKEDEX_LAYOUT_DESC_MAX;
    limit = max_chars;
    if (limit + 1 > dst_cap) limit = dst_cap - 1;

    n = strlen(src);
    if (n <= limit) {
        memcpy(dst, src, n + 1);
        return n;
    }
    if (limit < 3) {
        memcpy(dst, src, limit);
        dst[limit] = '\0';
        return limit;
    }
    memcpy(dst, src, limit - 3);
    memcpy(dst + (limit - 3), "...", 4);
    return limit;
}

bool pokedex_layout_scale2x_rgb565(const uint16_t *src, uint32_t w, uint32_t h,
                                   uint16_t *dst, size_t dst_cap_u16,
                                   uint32_t *out_w, uint32_t *out_h)
{
    uint32_t dw;
    uint32_t dh;
    uint32_t y;
    uint32_t x;

    if (!src || !dst || !out_w || !out_h || w == 0 || h == 0) return false;
    if (w > (UINT32_MAX / 2) || h > (UINT32_MAX / 2)) return false;
    dw = w * 2u;
    dh = h * 2u;
    if ((uint64_t)dw * dh > dst_cap_u16) return false;

    /* 从右下往左上写,src==dst 时也不会覆盖尚未读取的源像素。 */
    for (y = h; y-- > 0; ) {
        for (x = w; x-- > 0; ) {
            uint16_t px = src[y * w + x];
            uint32_t d0 = (y * 2u) * dw + (x * 2u);
            dst[d0] = px;
            dst[d0 + 1] = px;
            dst[d0 + dw] = px;
            dst[d0 + dw + 1] = px;
        }
    }
    *out_w = dw;
    *out_h = dh;
    return true;
}

bool pokedex_layout_dark_ink(uint32_t rgb888)
{
    /* Rec. 601 粗略亮度;阈值偏高,让电黄/冰蓝/钢灰走深字。 */
    unsigned r = (rgb888 >> 16) & 0xFFu;
    unsigned g = (rgb888 >> 8) & 0xFFu;
    unsigned b = rgb888 & 0xFFu;
    unsigned y = (r * 299u + g * 587u + b * 114u) / 1000u;
    return y >= 160u;
}
