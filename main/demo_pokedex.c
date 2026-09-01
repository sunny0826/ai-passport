// main/demo_pokedex.c —— 宝可梦图鉴(Pokédex)页面【纯离线版】。
//
// 数据与图片全部内置固件(生成产物 tools/gen_pokedex_static.py,来源 PokeAPI
// CC-BY 4.0):全国图鉴 1..1025(第 I–IX 世代)的基础数据 + 48x48 像素精灵图。
// 交互:UP/DOWN 单击翻 1,双击跳 10,长按跳世代;OK 单击播放当前叫声;
// 长按 OK 返回菜单(由 main.c 拦截,本页不处理 OK 长按)。
// 见过/上次查看保存在 NVS(命名空间 "pokedex"),掉电不丢失。
// 叫声在 cryfs 分区,Opus 8 kbps;解码与 I2S 写入在独立任务,按键回调不阻塞。
// 无任何网络/WiFi/HTTP 依赖。
//
// 线程模型:
//   - 按键回调(main.c 的 on_key,持 LVGL 锁)只改 RAM 状态、投递叫声编号,
//     并唤醒轻量 worker;NVS 落盘与 Opus 解码/I2S 写入都不在回调里。
//   - 电量与"空闲降背光"用 lv_timer(LVGL 任务内,天然持锁)。
#include "demo.h"
#include "pokedex_core.h"
#include "pokedex_layout.h"
#include "pokedex_sprite.h"
#include "pokedex_static.h"
#include "pokedex_cry_play.h"
#include "bsp_battery.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include <string.h>

#define POKEDEX_NVS_NS        "pokedex"
#define POKEDEX_NVS_KEY_STATE "state"

#define POKEDEX_WORKER_STACK 2048
#define POKEDEX_WORKER_PRIO  5

#define POKEDEX_IDLE_DIM_S 60 /* 无按键 60s 后背光降到 25% */

// ------------------------------ 图鉴屏幕配色 ------------------------------
// 背景取 pokedex.guoxudong.io 打开图鉴后的屏幕底 #244238;文字白/浅灰,
// 高亮用薄荷绿 #5BC0A8、分隔用亮青 #467A6A(该站"官图/展示区"用色)。
// 精灵透明像素已按 #244238 混色,无白边。
#define CS_BG     0x244238u  /* 图鉴屏幕底(参考站实测) */
#define CS_BG_DK  0x1A3730u  /* 更深一档:标题条/反色文字 */
#define CS_FRAME  0x467A6Au  /* 亮青:屏幕框/面板边 */
#define CS_BRIGHT 0x5BC0A8u  /* 薄荷绿:编号/HT chip 底 */
#define CS_TEXT   0xFFFFFFu  /* 主文字:白 */
#define CS_DIM    0x90A99Au  /* 次文字:灰绿 */
#define CS_WARN   0xF0C070u  /* 低电:暖黄,深底上仍可读 */

static const uint32_t TYPE_COLORS[POKEDEX_STATIC_TYPE_COUNT] = {
    [POKEDEX_STATIC_TYPE_BUG]      = 0xA6B91A,
    [POKEDEX_STATIC_TYPE_DARK]     = 0x705746,
    [POKEDEX_STATIC_TYPE_DRAGON]   = 0x6F35FC,
    [POKEDEX_STATIC_TYPE_ELECTRIC] = 0xF7D02C,
    [POKEDEX_STATIC_TYPE_FAIRY]    = 0xD685AD,
    [POKEDEX_STATIC_TYPE_FIGHTING] = 0xC22E28,
    [POKEDEX_STATIC_TYPE_FIRE]     = 0xEE8130,
    [POKEDEX_STATIC_TYPE_FLYING]   = 0xA98FF3,
    [POKEDEX_STATIC_TYPE_GHOST]    = 0x735797,
    [POKEDEX_STATIC_TYPE_GRASS]    = 0x7AC74C,
    [POKEDEX_STATIC_TYPE_GROUND]   = 0xE2BF65,
    [POKEDEX_STATIC_TYPE_ICE]      = 0x96D9D6,
    [POKEDEX_STATIC_TYPE_NORMAL]   = 0xA8A77A,
    [POKEDEX_STATIC_TYPE_POISON]   = 0xA33EA1,
    [POKEDEX_STATIC_TYPE_PSYCHIC]  = 0xF95587,
    [POKEDEX_STATIC_TYPE_ROCK]     = 0xB6A136,
    [POKEDEX_STATIC_TYPE_STEEL]    = 0xB7B7CE,
    [POKEDEX_STATIC_TYPE_WATER]    = 0x6390F0,
};

static void apply_rect(lv_obj_t *o, pokedex_rect_t r)
{
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
}

// 无边框色块(编号条/镜头/装饰)。
static lv_obj_t *flag_block(lv_obj_t *parent, pokedex_rect_t r,
                            uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    apply_rect(o, r);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *label_at(lv_obj_t *parent, pokedex_rect_t r,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    apply_rect(l, r);
    return l;
}

// 属性徽章:固定 3 字母宽,圆角色块 + 白字。
static lv_obj_t *badge_create(lv_obj_t *parent, pokedex_rect_t r, uint32_t color)
{
    lv_obj_t *o = flag_block(parent, r, color, 6);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(CS_FRAME), 0);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    lv_label_set_text(l, "-");
    return o;
}

// ------------------------------ 页面对象(全部 LVGL 持锁访问)--------------
static lv_obj_t      *s_scr;
static lv_obj_t      *s_sprite;
static lv_obj_t      *s_sprite_hint;
static lv_obj_t      *s_name, *s_htwt, *s_battery, *s_progress;
static lv_obj_t      *s_no;          /* 编号条 "NO.001" */
static lv_obj_t      *s_badge[2];    /* 属性徽章 */
static lv_obj_t      *s_desc;        /* 图鉴概述(英文) */
static lv_obj_t      *s_tally_seen;
static lv_timer_t    *s_bat_timer;
static lv_timer_t    *s_idle_timer;
static uint8_t        s_idle_sec;
static pokedex_layout_t s_lay;

// 精灵图输出缓冲与 LVGL 图像描述(静态分配,避免碎片化)。
static uint16_t       s_sprite_pixels[POKEDEX_SPRITE_MAX_BYTES / 2];
static lv_image_dsc_t s_sprite_dsc;

// ------------------------------ 状态与并发 --------------------------------
static TaskHandle_t    s_worker;      /* 轻量落盘 worker(页面常驻) */
static volatile bool   s_exit;        /* 1 = 页面已退出 */
static pokedex_state_t s_state;       /* RAM 图鉴状态;按键侧改,worker 落盘 */
static volatile bool   s_state_dirty; /* 有改动待落盘 */

static const char *TAG = "pokedex";

// ------------------------------ NVS 持久化 --------------------------------
static void state_load(void)
{
    pokedex_state_init(&s_state);
    nvs_handle_t h;
    if (nvs_open(POKEDEX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[POKEDEX_STATE_BLOB_SIZE];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, POKEDEX_NVS_KEY_STATE, blob, &len);
    if (err == ESP_OK) {
        pokedex_state_t tmp;
        if (pokedex_state_deserialize(&tmp, blob, len)) s_state = tmp;
    }
    nvs_close(h);
}

// worker 任务里调用:加锁快照 RAM 状态,再在锁外写 NVS(flash 写入较慢)。
static void state_persist(void)
{
    if (!s_state_dirty) return;
    uint8_t blob[POKEDEX_STATE_BLOB_SIZE];
    bool captured = false;
    if (bsp_lvgl_lock(200)) {
        if (pokedex_state_serialize(&s_state, blob, sizeof(blob)) == sizeof(blob)) {
            s_state.save_seq++; /* 仅排障可见 */
            s_state_dirty = false;
            captured = true;
        }
        bsp_lvgl_unlock();
    }
    if (!captured) return;

    nvs_handle_t h;
    esp_err_t err = nvs_open(POKEDEX_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, POKEDEX_NVS_KEY_STATE, blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs save: %s", esp_err_to_name(err));
    nvs_close(h);
}

// ------------------------------ UI 小工具 --------------------------------
static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
    }
}

static void ui_update_tally(void)
{
    char line[20];
    if (!s_scr || !s_tally_seen) return;
    pokedex_layout_format_seen(pokedex_count_seen(&s_state),
                               line, sizeof(line));
    lv_label_set_text(s_tally_seen, line);
}

// 左列固定两枚 3 字母徽章;无第二属性则隐藏第二枚。
static void layout_badges(uint8_t t0, uint8_t t1)
{
    uint8_t idx[2] = { t0, t1 };
    for (int i = 0; i < 2; i++) {
        if (idx[i] >= POKEDEX_STATIC_TYPE_COUNT) {
            lv_obj_add_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char abbr[8];
        pokedex_layout_type_abbr(pokedex_static_type_names[idx[i]],
                                 abbr, sizeof(abbr));
        lv_obj_t *l = lv_obj_get_child(s_badge[i], 0);
        uint32_t bg = TYPE_COLORS[idx[i]];
        if (l) {
            lv_label_set_text(l, abbr);
            lv_obj_set_style_text_color(l,
                lv_color_hex(pokedex_layout_dark_ink(bg) ? CS_BG_DK : 0xFFFFFFu), 0);
        }
        lv_obj_set_style_bg_color(s_badge[i], lv_color_hex(bg), 0);
        apply_rect(s_badge[i], s_lay.badge[i]);
        lv_obj_clear_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_show_sprite_locked(uint32_t dw, uint32_t dh)
{
    if (!s_scr || !s_sprite) return;
    s_sprite_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_sprite_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_sprite_dsc.header.w = (uint16_t)dw;
    s_sprite_dsc.header.h = (uint16_t)dh;
    s_sprite_dsc.header.stride = (uint16_t)(dw * 2);
    s_sprite_dsc.data_size = dw * dh * 2;
    s_sprite_dsc.data = (const uint8_t *)s_sprite_pixels;

    /* 右上精灵井:软件最近邻 2x 后 1:1 贴图,避免 LVGL 缩放把像素风抹糊。 */
    apply_rect(s_sprite, s_lay.sprite);
    lv_image_set_src(s_sprite, &s_sprite_dsc);
    lv_obj_remove_flag(s_sprite, LV_OBJ_FLAG_HIDDEN);
    if (s_sprite_hint) lv_obj_add_flag(s_sprite_hint, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------ 本地离线显示 ------------------------------
// 全国图鉴 1..1025 的基础数据与 48x48 像素精灵图内置在固件,切换零等待。

static void ui_apply_static_locked(uint32_t id)
{
    const pokedex_static_entry_t *e = &pokedex_static_dex[id];
    char buf[64];

    int n = pokedex_pretty_name(e->name, buf, sizeof(buf));
    if (n > 0) upper(buf);
    lv_label_set_text(s_name, buf);

    pokedex_layout_format_no(id, buf, sizeof(buf));
    lv_label_set_text(s_no, buf);

    pokedex_layout_format_progress(id, buf, sizeof(buf));
    lv_label_set_text(s_progress, buf);

    layout_badges(e->type0, e->type1);

    pokedex_layout_format_stats(e->height_dm, e->weight_hg, buf, sizeof(buf));
    lv_label_set_text(s_htwt, buf);

    {
        char clip[POKEDEX_LAYOUT_DESC_MAX + 4];
        pokedex_layout_clip_desc(e->desc, clip, sizeof(clip), 0);
        lv_label_set_text(s_desc, clip);
    }

    ui_update_tally();

    uint32_t w = 0, h = 0;
    if (pokedex_sprite_static(_binary_pokedex_sprites_bin_start,
                              (size_t)(_binary_pokedex_sprites_bin_end -
                                       _binary_pokedex_sprites_bin_start),
                              id, s_sprite_pixels,
                              sizeof(s_sprite_pixels) / 2, &w, &h) &&
        pokedex_layout_scale2x_rgb565(s_sprite_pixels, w, h, s_sprite_pixels,
                                      sizeof(s_sprite_pixels) / 2, &w, &h)) {
        ui_show_sprite_locked(w, h);
    } else if (s_sprite_hint) {
        lv_obj_add_flag(s_sprite, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sprite_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// ------------------------------ 轻量落盘 worker ---------------------------
static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_exit) continue; /* 页面已退出,等待下一次进入 */
        state_persist();
    }
}

// ------------------------------ 按键 --------------------------------------
static void goto_id(uint32_t id)
{
    s_state.last_id = id;
    pokedex_mark_seen(&s_state, id);
    s_state_dirty = true;
    ui_apply_static_locked(id);
    if (s_worker) xTaskNotifyGive(s_worker); /* 落盘(见过/位置) */
}

void demo_pokedex_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    /* OK 长按由 main.c 拦截返回菜单,本页不处理。 */
    if (btn == BSP_BTN_OK && ev != BSP_BTN_CLICK) return;
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG) return;

    s_idle_sec = 0;
    bsp_display_backlight(100);

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int32_t dir = (btn == BSP_BTN_UP) ? -1 : 1;
        uint32_t id = s_state.last_id;
        if (ev == BSP_BTN_CLICK) {
            id = pokedex_step(id, dir);
        } else if (ev == BSP_BTN_DOUBLE) {
            id = pokedex_step(id, dir * 10);
        } else {
            id = pokedex_step_gen(id, dir);
        }
        pokedex_cry_play_stop();
        goto_id(id);
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        pokedex_cry_play_request(s_state.last_id);
    }
}

// ------------------------------ LVGL 定时器 --------------------------------
static void bat_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_scr || !s_battery) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery, "--");
        lv_obj_set_style_text_color(s_battery, lv_color_hex(CS_DIM), 0);
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
        /* 深色顶栏必须用浅字;低电改暖黄,避免 UI_INK 几乎看不见。 */
        lv_obj_set_style_text_color(s_battery,
            lv_color_hex(soc < 20 ? CS_WARN : CS_TEXT), 0);
    }
}

static void idle_tick(lv_timer_t *t)
{
    (void)t;
    if (s_idle_sec < POKEDEX_IDLE_DIM_S) {
        s_idle_sec++;
        if (s_idle_sec == POKEDEX_IDLE_DIM_S) bsp_display_backlight(25);
    }
}

// ------------------------------ 页面入口/出口 ------------------------------
void demo_pokedex_enter(void)
{
    nvs_flash_init();
    state_load();
    pokedex_cry_play_init();
    if (!s_worker) {
        xTaskCreate(worker_main, "pokedex", POKEDEX_WORKER_STACK, NULL,
                    POKEDEX_WORKER_PRIO, &s_worker);
    }

    pokedex_layout_build(&s_lay);

    /* 早期绿色电子屏:整机暗墨绿底,荧光绿字;
       左列身份(编号/名字/3 字母属性),右上 96px 精灵井,
       中部身高体重,下部概述,底栏见过计数与按键提示。 */
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(CS_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    flag_block(s_scr, s_lay.header, CS_BG_DK, 0);
    flag_block(s_scr, s_lay.header_rule, CS_FRAME, 0);

    lv_obj_t *heading = label_at(s_scr, s_lay.title, &lv_font_montserrat_14, CS_TEXT);
    lv_label_set_text(heading, "POKEDEX");

    s_progress = label_at(s_scr, s_lay.progress, &lv_font_montserrat_14, CS_BRIGHT);
    lv_obj_set_style_text_align(s_progress, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_progress, "001/1025");

    s_battery = label_at(s_scr, s_lay.battery, &lv_font_montserrat_14, CS_TEXT);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_battery, "--");

    /* 右上精灵井 */
    flag_block(s_scr, s_lay.sprite_frame, CS_FRAME, 0);
    flag_block(s_scr, s_lay.sprite_inner, CS_BG, 0);
    s_sprite = lv_image_create(s_scr);
    lv_obj_add_flag(s_sprite, LV_OBJ_FLAG_HIDDEN);
    s_sprite_hint = label_at(s_scr, s_lay.sprite, &lv_font_montserrat_14, CS_DIM);
    lv_obj_set_style_text_align(s_sprite_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_sprite_hint, "NO IMAGE");

    /* 左列:编号 chip */
    flag_block(s_scr, s_lay.number_chip, CS_BRIGHT, 0);
    s_no = label_at(s_scr, s_lay.number, &lv_font_montserrat_14, CS_BG_DK);
    lv_label_set_text(s_no, "NO.001");

    s_name = label_at(s_scr, s_lay.name, &lv_font_montserrat_20, CS_TEXT);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_name, "---");

    s_badge[0] = badge_create(s_scr, s_lay.badge[0], TYPE_COLORS[0]);
    s_badge[1] = badge_create(s_scr, s_lay.badge[1], TYPE_COLORS[0]);
    lv_obj_add_flag(s_badge[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_badge[1], LV_OBJ_FLAG_HIDDEN);

    flag_block(s_scr, s_lay.stats, CS_BRIGHT, 0);
    s_htwt = label_at(s_scr, s_lay.stats_text, &lv_font_montserrat_14, CS_BG_DK);
    lv_obj_set_style_text_align(s_htwt, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_htwt, "HT -.- m    WT -.- kg");

    flag_block(s_scr, s_lay.flavor_frame, CS_FRAME, 0);
    flag_block(s_scr, s_lay.flavor_inner, CS_BG_DK, 0);
    s_desc = label_at(s_scr, s_lay.flavor_text, &lv_font_montserrat_14, CS_TEXT);
    lv_label_set_long_mode(s_desc, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_desc, "");

    lv_obj_t *seen_chip = flag_block(s_scr, s_lay.tally_seen, CS_BG_DK, 0);
    s_tally_seen = lv_label_create(seen_chip);
    lv_obj_set_style_text_font(s_tally_seen, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_tally_seen, lv_color_hex(CS_TEXT), 0);
    lv_obj_center(s_tally_seen);
    lv_label_set_text(s_tally_seen, "0 SEEN");

    lv_obj_t *hint = label_at(s_scr, s_lay.hint, &lv_font_montserrat_14, CS_DIM);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "OK CRY   UP/DN 1/10/GEN");

    s_idle_sec = 0;
    s_bat_timer = lv_timer_create(bat_tick, 2000, NULL);
    s_idle_timer = lv_timer_create(idle_tick, 1000, NULL);
    bat_tick(NULL);

    lv_screen_load(s_scr);

    s_exit = false;
    ui_update_tally();
    goto_id(s_state.last_id);
}

void demo_pokedex_exit(void)
{
    pokedex_cry_play_release();
    if (s_worker) xTaskNotifyGive(s_worker);

    s_exit = true;

    lv_timer_delete(s_bat_timer);
    s_bat_timer = NULL;
    lv_timer_delete(s_idle_timer);
    s_idle_timer = NULL;

    lv_obj_delete(s_scr);
    s_scr = NULL;
    s_sprite = NULL;
    s_sprite_hint = NULL;
    s_name = NULL;
    s_no = NULL;
    s_badge[0] = NULL;
    s_badge[1] = NULL;
    s_htwt = NULL;
    s_desc = NULL;
    s_tally_seen = NULL;
    s_battery = NULL;
    s_progress = NULL;

    /* worker 会把未落盘的改动持久化。 */
}
