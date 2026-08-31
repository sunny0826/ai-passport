// main/demo_pokedex.c —— 宝可梦图鉴(Pokédex)页面【纯离线版】。
//
// 数据与图片全部内置固件(生成产物 tools/gen_pokedex_static.py,来源 PokeAPI
// CC-BY 4.0):第 1 世代 1..151 的基础数据 + 48x48 像素精灵图。
// 交互:UP/DOWN 翻阅,OK 切换"已捕捉",长按 OK 返回菜单(由 main.c 拦截)。
// 记录(捕捉/见过/上次查看)保存在 NVS(命名空间 "pokedex"),掉电不丢失。
// 无任何网络/WiFi/HTTP 依赖。
//
// 线程模型:
//   - 按键回调(main.c 的 on_key,持 LVGL 锁)只改 RAM 状态并唤醒轻量 worker;
//     NVS 落盘(flash 写入)在 worker 任务里完成 —— 按键回调不阻塞。
//   - 电量与"空闲降背光"用 lv_timer(LVGL 任务内,天然持锁)。
#include "demo.h"
#include "pokedex_core.h"
#include "pokedex_sprite.h"
#include "pokedex_static.h"
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "bsp_display.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include <string.h>

#define POKEDEX_NVS_NS       "pokedex"
#define POKEDEX_NVS_KEY_STATE "state"

#define POKEDEX_WORKER_STACK 2048
#define POKEDEX_WORKER_PRIO  5

#define POKEDEX_IDLE_DIM_S   60 /* 无按键 60s 后背光降到 25% */

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

static const uint32_t TYPE_COLORS[POKEDEX_STATIC_TYPE_COUNT] = {
    [POKEDEX_STATIC_TYPE_BUG]      = 0xA6B91A,
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

// 无边框色块(编号条/镜头/装饰)。
static lv_obj_t *flag_block(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// 属性徽章:圆角色块 + 白字(属性色保留,边框用 GB 深绿)。
static lv_obj_t *badge_create(lv_obj_t *parent, int x, int y, int w, int h,
                              uint32_t color, const char *text)
{
    lv_obj_t *o = flag_block(parent, x, y, w, h, color, 7);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(CS_FRAME), 0);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    lv_label_set_text(l, text);
    return o;
}

// ------------------------------ 页面对象(全部 LVGL 持锁访问)--------------
static lv_obj_t      *s_scr;
static lv_obj_t      *s_sprite;
static lv_obj_t      *s_sprite_hint;
static lv_obj_t      *s_name, *s_htwt, *s_battery;
static lv_obj_t      *s_no;          /* 编号条 "NO.001" */
static lv_obj_t      *s_badge[2];    /* 属性徽章 */
static lv_obj_t      *s_types;      /* 类型全名小字(GRASS / POISON) */
static lv_obj_t      *s_desc;        /* 图鉴概述(英文) */
static lv_obj_t      *s_status;      /* 状态行 */
static lv_obj_t      *s_count;       /* CAUGHT/SEEN 计数行 */
static lv_timer_t    *s_bat_timer;
static lv_timer_t    *s_idle_timer;
static uint8_t        s_idle_sec;

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

static void ui_set_status(const char *text, bool error)
{
    if (!s_scr || !s_status) return;
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status,
        lv_color_hex(error ? UI_RED : CS_TEXT), 0);
}

static void ui_update_badges(void)
{
    if (!s_scr || !s_count) return;
    uint32_t caught = pokedex_count_caught(&s_state);
    uint32_t seen = pokedex_count_seen(&s_state);
    char line[48];
    snprintf(line, sizeof(line), "CAUGHT %u/151   SEEN %u/151",
             (unsigned)caught, (unsigned)seen);
    lv_label_set_text(s_count, line);
    lv_obj_set_style_text_color(s_count, lv_color_hex(CS_TEXT), 0);
}

// 左信息列排放 1..2 枚属性徽章(从 x=16 起)。
static void layout_badges(uint8_t t0, uint8_t t1)
{
    uint8_t idx[2] = { t0, t1 };
    int shown = 0;
    int widths[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        if (idx[i] >= POKEDEX_STATIC_TYPE_COUNT) break;
        const char *tn = pokedex_static_type_names[idx[i]];
        widths[i] = (int)strlen(tn) * 7 + 22;
        shown++;
    }
    if (shown == 0) {
        for (int i = 0; i < 2; i++) lv_obj_add_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int x = 16;
    for (int i = 0; i < shown; i++) {
        uint8_t ti = idx[i];
        char up[16];
        snprintf(up, sizeof(up), "%s", pokedex_static_type_names[ti]);
        upper(up);
        lv_obj_t *l = lv_obj_get_child(s_badge[i], 0);
        if (l) lv_label_set_text(l, up);
        lv_obj_set_style_bg_color(s_badge[i], lv_color_hex(TYPE_COLORS[ti]), 0);
        lv_obj_set_pos(s_badge[i], x, 100);
        lv_obj_set_size(s_badge[i], widths[i], 20);
        lv_obj_clear_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
        x += widths[i] + 10;
    }
    for (int i = shown; i < 2; i++) lv_obj_add_flag(s_badge[i], LV_OBJ_FLAG_HIDDEN);
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

    /* 右上角缩略图:48px 像素图放大 1.5 倍(72px),置于精灵框内。 */
    lv_image_set_scale(s_sprite, 384);
    lv_obj_set_size(s_sprite, 72, 72);
    lv_obj_set_pos(s_sprite, 149, 43);
    lv_image_set_src(s_sprite, &s_sprite_dsc);
    lv_obj_remove_flag(s_sprite, LV_OBJ_FLAG_HIDDEN);
    if (s_sprite_hint) {
        lv_obj_add_flag(s_sprite_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// ------------------------------ 本地离线显示 ------------------------------
// 第 1 世代 151 只的基础数据与 48x48 像素精灵图内置在固件,切换零等待。

static void ui_apply_static_locked(uint32_t id)
{
    const pokedex_static_entry_t *e = &pokedex_static_dex[id];
    char buf[64];

    int n = pokedex_pretty_name(e->name, buf, sizeof(buf));
    if (n > 0) {
        for (size_t i = 0; buf[i]; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
        }
    }
    lv_label_set_text(s_name, buf);
    lv_label_set_text_fmt(s_no, "NO.%03u", (unsigned)id);

    layout_badges(e->type0, e->type1);

    {
        char tl[40];
        snprintf(tl, sizeof(tl), "%s", pokedex_static_type_names[e->type0]);
        upper(tl);
        if (e->type1 != POKEDEX_STATIC_TYPE_NONE) {
            char t2[16];
            snprintf(t2, sizeof(t2), "%s", pokedex_static_type_names[e->type1]);
            upper(t2);
            snprintf(buf, sizeof(buf), "%s / %s", tl, t2);
        } else {
            snprintf(buf, sizeof(buf), "%s", tl);
        }
        lv_label_set_text(s_types, buf);
    }

    snprintf(buf, sizeof(buf), "HT ");
    pokedex_format_height(e->height_dm, buf + 3, sizeof(buf) - 3);
    size_t used2 = strlen(buf);
    snprintf(buf + used2, sizeof(buf) - used2, "   WT ");
    pokedex_format_weight(e->weight_hg, buf + strlen(buf), sizeof(buf) - strlen(buf));
    lv_label_set_text(s_htwt, buf);

    /* 概述:14px 三行约 84 字符,超长截断加 "..."。 */
    size_t dl = strlen(e->desc);
    if (dl > 84) {
        char clip[90];
        memcpy(clip, e->desc, 84);
        memcpy(clip + 84, "...", 4);
        lv_label_set_text(s_desc, clip);
    } else {
        lv_label_set_text(s_desc, e->desc);
    }

    ui_update_badges();

    uint32_t w = 0, h = 0;
    if (pokedex_sprite_static(_binary_pokedex_sprites_bin_start,
                              (size_t)(_binary_pokedex_sprites_bin_end - _binary_pokedex_sprites_bin_start),
                              id, s_sprite_pixels, sizeof(s_sprite_pixels) / 2,
                              &w, &h)) {
        ui_show_sprite_locked(w, h);
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
    if (id == POKEDEX_DEX_FIRST || id == POKEDEX_DEX_LAST) {
        ui_set_status("EDGE OF DEX", false);
    } else {
        ui_set_status("POKEDEX 1-151", false);
    }
    if (s_worker) xTaskNotifyGive(s_worker); /* 落盘(见过/位置) */
}

void demo_pokedex_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    s_idle_sec = 0;
    bsp_display_backlight(100);

    if (btn == BSP_BTN_UP) {
        goto_id(pokedex_step(s_state.last_id, -1));
    } else if (btn == BSP_BTN_DOWN) {
        goto_id(pokedex_step(s_state.last_id, 1));
    } else if (btn == BSP_BTN_OK) {
        uint32_t id = s_state.last_id;
        pokedex_set_caught(&s_state, id, !pokedex_is_caught(&s_state, id));
        s_state_dirty = true;
        ui_update_badges();
        if (s_worker) xTaskNotifyGive(s_worker); /* 触发落盘 */
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
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
        lv_obj_set_style_text_color(s_battery,
            lv_color_hex(soc < 20 ? UI_RED : UI_INK), 0);
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
    if (!s_worker) {
        xTaskCreate(worker_main, "pokedex", POKEDEX_WORKER_STACK, NULL,
                    POKEDEX_WORKER_PRIO, &s_worker);
    }

    /* ---------- 早期 Game Boy 绿屏风格(整屏 = 一台 GB 图鉴) ---------- */
    /* 早期绿色电子屏:整机暗墨绿底,荧光绿字,屏幕区带亮绿细框;
       左上基础信息(编号/名字/属性徽章),右上精灵缩略图,
       屏下身高体重条与概述(DEX ENTRY)。精灵透明底已混 CS_BG,无白边。 */
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(CS_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    /* 标题条:更暗的绿 + 亮绿细框线,荧光绿字 */
    flag_block(s_scr, 0, 0, 240, 22, CS_BG_DK, 0);
    flag_block(s_scr, 0, 22, 240, 2, CS_FRAME, 0);
    lv_obj_t *heading = lv_label_create(s_scr);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(CS_TEXT), 0);
    lv_obj_set_pos(heading, 10, 4);
    lv_label_set_text(heading, "POKEDEX");

    s_battery = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(CS_TEXT), 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_label_set_text(s_battery, "--");

    /* ---------- 电子屏显示区(亮绿细框 + 暗绿芯) ---------- */
    flag_block(s_scr, 6, 26, 228, 130, CS_FRAME, 0);
    flag_block(s_scr, 8, 28, 224, 126, CS_BG, 0);

    /* 左列:编号 chip(荧光亮绿底、深绿字) */
    flag_block(s_scr, 16, 34, 96, 18, CS_BRIGHT, 0);
    s_no = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_no, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_no, lv_color_hex(CS_BG_DK), 0);
    lv_obj_set_pos(s_no, 20, 36);
    lv_label_set_text(s_no, "NO.001");

    /* 左列:名字 */
    s_name = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(CS_TEXT), 0);
    lv_obj_set_pos(s_name, 14, 60);
    lv_obj_set_width(s_name, 118);
    lv_label_set_text(s_name, "---");

    /* 左列:属性徽章 + 类型全名小字(GRASS / POISON) */
    s_badge[0] = badge_create(s_scr, 0, 100, 40, 20, TYPE_COLORS[0], "-");
    s_badge[1] = badge_create(s_scr, 0, 100, 40, 20, TYPE_COLORS[0], "-");
    lv_obj_add_flag(s_badge[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_badge[1], LV_OBJ_FLAG_HIDDEN);
    s_types = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_types, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_types, lv_color_hex(CS_DIM), 0);
    lv_obj_set_pos(s_types, 16, 124);
    lv_obj_set_width(s_types, 110);
    lv_label_set_text(s_types, "");

    /* 右上:精灵缩略图框(透明底混屏色,无白边) */
    flag_block(s_scr, 140, 30, 90, 90, CS_FRAME, 0);
    flag_block(s_scr, 142, 32, 86, 86, CS_BG, 0);
    s_sprite = lv_image_create(s_scr);
    lv_obj_add_flag(s_sprite, LV_OBJ_FLAG_HIDDEN);
    s_sprite_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_sprite_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sprite_hint, lv_color_hex(CS_DIM), 0);
    lv_obj_set_pos(s_sprite_hint, 151, 70);
    lv_obj_set_width(s_sprite_hint, 68);
    lv_obj_set_style_text_align(s_sprite_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_sprite_hint, "NO IMAGE");

    /* ---------- 身高体重条(荧光亮绿底) ---------- */
    flag_block(s_scr, 10, 162, 220, 20, CS_BRIGHT, 0);
    s_htwt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_htwt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_htwt, lv_color_hex(CS_BG_DK), 0);
    lv_obj_set_pos(s_htwt, 14, 165);
    lv_obj_set_width(s_htwt, 212);
    lv_obj_set_style_text_align(s_htwt, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_htwt, "HT -.- m   WT -.- kg");

    /* ---------- 概述面板(DEX ENTRY) ---------- */
    flag_block(s_scr, 10, 188, 220, 72, CS_FRAME, 0);
    flag_block(s_scr, 12, 190, 216, 68, CS_BG, 0);
    lv_obj_t *dexl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(dexl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dexl, lv_color_hex(CS_DIM), 0);
    lv_obj_set_pos(dexl, 18, 194);
    lv_label_set_text(dexl, "DEX ENTRY");
    s_desc = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_desc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_desc, lv_color_hex(CS_TEXT), 0);
    lv_obj_set_pos(s_desc, 18, 209);
    lv_obj_set_width(s_desc, 204);
    lv_label_set_long_mode(s_desc, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_desc, "");

    /* ---------- 底部状态 / 收集计数 / 操作提示 ---------- */
    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(CS_TEXT), 0);
    lv_obj_set_pos(s_status, 4, 264);
    lv_obj_set_width(s_status, 232);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, "OFFLINE DEX");

    s_count = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_count, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_count, lv_color_hex(CS_TEXT), 0);
    lv_obj_set_pos(s_count, 4, 282);
    lv_obj_set_width(s_count, 232);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_count, "CAUGHT 0/151   SEEN 0/151");

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(CS_DIM), 0);
    lv_obj_set_pos(hint, 4, 302);
    lv_obj_set_width(hint, 232);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "UP/DOWN BROWSE   OK CATCH");

    s_idle_sec = 0;
    s_bat_timer = lv_timer_create(bat_tick, 2000, NULL);
    s_idle_timer = lv_timer_create(idle_tick, 1000, NULL);
    bat_tick(NULL);

    lv_screen_load(s_scr);

    s_exit = false;
    ui_update_badges();
    goto_id(s_state.last_id);
}

void demo_pokedex_exit(void)
{
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
    s_types = NULL;
    s_htwt = NULL;
    s_desc = NULL;
    s_status = NULL;
    s_count = NULL;
    s_battery = NULL;

    /* worker 会把未落盘的改动持久化。 */
}