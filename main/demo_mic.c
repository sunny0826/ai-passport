// demo_mic.c —— 麦克风页:按住 OK 说话,上/下调麦克风增益,NimBLE 推流。
//
// 交互契约(本页显式重定义了 OK 长按,见 demo_entry_t::ok_long_back):
//   OK 按住     说话(松开结束),同步经 BLE 向已订阅的电脑/手机推 PCM
//   OK 单击     回放上一段录音(说话不足 500ms 视为误触,不回放)
//   OK 长按     说话本身,不再作为返回菜单
//   上/下 单击  麦克风增益 ±10%(0..100,NVS 持久化,掉电不丢)
//   上/下 长按  返回菜单(demo_request_exit)
//
// 结构:按键回调只改状态与标志;录音/回放/BLE 推流/NVS 写入都在 mic_task;
// UI 刷新集中在 50ms 的 LVGL 定时器。可测逻辑全部在 mic_model.c(宿主机测试)。
#include "demo.h"
#include "mic_ble.h"
#include "mic_model.h"
#include "demo_radio.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "ui_pixel.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_mic";

#define MIC_SAMPLE_RATE      16000   // 16kHz/16bit/单声道,与音频演示一致
#define MIC_CHUNK_SAMPLES     160    // 10ms 一块(320 字节),控制延迟与栈占用
                                       // 录音缓冲时长:进入页面时按剩余内存自适应(4s→3s→2s)
#define MIC_TASK_STACK       6144
#define MIC_OUT_VOLUME          80   // 回放音量(%),输出增益与麦克风增益无关

// 电平条分区颜色(参照 Sound-Meter 经验:浅灰槽底,避免"莫名黑块")。
#define BAR_TRACK  0xE2E2D6

// ---------------------------------------------------------------------------
// 共享状态(按键回调/工作线程/LVGL 定时器之间的全部交互都走这些小标量)
// ---------------------------------------------------------------------------

static mic_state_t volatile s_state = MIC_IDLE;   // FSM 当前状态
static uint32_t volatile s_talk_start_ms;         // 本次说话开始的 lv_tick
static uint32_t volatile s_last_talk_ms;          // 上一段说话时长(回放门槛)
static int volatile s_level_pct;                  // 工作线程写入的平滑电平
static uint8_t volatile s_gain = MIC_GAIN_DEFAULT;// 当前增益(点击/BLE 共同写)
static bool volatile s_gain_dirty;                // 工作线程负责应用并落 NVS
static bool volatile s_stop_req;                  // 页面退出握手
static bool s_audio_ok;                           // set_format 成功才有声路
static bool s_nvs_ok;
static bool s_ring_ok;                            // 128KB 录音缓冲是否分配成功

static mic_level_t s_level;                       // 仅工作线程访问
static mic_ring_t s_ring;                         // 元数据;buf 随页面分配
static int16_t *s_ring_buf;
static size_t s_ring_seconds;                     // 实际分配到的录音时长
static nvs_handle_t s_nvs;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_task_done;

// LVGL 对象(仅 LVGL 任务/持锁上下文访问)
static lv_obj_t *s_scr;
static lv_obj_t *s_state_label, *s_ble_label, *s_batt_label, *s_gain_label;
static lv_obj_t *s_rec, *s_mic_icon, *s_wave_lo, *s_wave_hi;
static lv_obj_t *s_lvl_seg[10];
static lv_obj_t *s_gain_seg[10];
static int s_mic_icon_base_y;
static int s_bar_shown;                           // 增益条动画当前值(0..10 格)
static lv_timer_t *s_timer;

// ---------------------------------------------------------------------------
// 状态推进(按键上下文与工作线程都可能调用;事件相隔较远,标量写原子)
// ---------------------------------------------------------------------------

static uint32_t now_ms(void) { return (uint32_t)lv_tick_get(); }

static void apply_state(mic_state_t next) {
    uint32_t now = now_ms();
    if (next == MIC_TALKING && s_state != MIC_TALKING) {
        mic_ring_clear(&s_ring);                  // 每次说话都重开一段
        mic_level_reset(&s_level);
        s_talk_start_ms = now;
        s_level_pct = 0;
    }
    if (next == MIC_IDLE && s_state == MIC_TALKING) {
        s_last_talk_ms = now - s_talk_start_ms;
    }
    s_state = next;
}

// ---------------------------------------------------------------------------
// 工作线程:录音 / 回放 / BLE 推流 / 增益落盘
// ---------------------------------------------------------------------------

static void gain_commit(void) {
    bsp_audio_set_mic_gain(s_gain);               // 先让声路立即生效
    if (!s_nvs_ok) return;
    esp_err_t e = nvs_set_u8(s_nvs, "gain_pct", s_gain);
    if (e == ESP_OK) e = nvs_commit(s_nvs);
    if (e != ESP_OK) ESP_LOGE(TAG, "增益保存失败: %s", esp_err_to_name(e));
}

static void mic_task(void *arg) {
    (void)arg;
    int16_t chunk[MIC_CHUNK_SAMPLES];             // 320B,栈上即可

    for (;;) {
        if (s_stop_req) break;

        if (s_gain_dirty) {                       // 点击与 BLE 写入共用一条生效路径
            s_gain_dirty = false;
            gain_commit();
        }

        switch (s_state) {
        case MIC_TALKING: {
            if (bsp_audio_read(chunk, sizeof chunk) != ESP_OK) {
                ESP_LOGE(TAG, "录音读取失败,退出说话状态");
                apply_state(MIC_IDLE);            // 单写者:工作线程改状态是安全的
                break;
            }
            mic_ring_write(&s_ring, chunk, MIC_CHUNK_SAMPLES);  // 满后覆盖最旧
            s_level_pct = mic_level_update(&s_level, mic_peak_abs(chunk, MIC_CHUNK_SAMPLES));
            mic_ble_stream(chunk, sizeof chunk);  // 未订阅/池满则丢弃,绝不阻塞
            break;
        }
        case MIC_PLAYBACK: {
            size_t n = mic_ring_read(&s_ring, chunk, MIC_CHUNK_SAMPLES);
            if (n == 0) {
                apply_state(MIC_IDLE);            // 放完
                break;
            }
            if (bsp_audio_write(chunk, n * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGE(TAG, "回放写入失败");
                apply_state(MIC_IDLE);
            }
            break;
        }
        default:
            vTaskDelay(pdMS_TO_TICKS(20));        // 空闲低频轮询(等 dirty/状态)
            break;
        }
    }
    if (s_task_done) xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

// BLE 客户端写 Mic Gain 特征的回调(NimBLE host 任务):只改标志,不碰慢 IO。
static void on_ble_gain(uint8_t pct) {
    s_gain = pct;
    s_gain_dirty = true;
}

// ---------------------------------------------------------------------------
// UI 构建(全部走 ui_pixel 像素主题)
// ---------------------------------------------------------------------------

// 像素方块助手(见下方定义)。
static lv_obj_t *page_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);

// 像素麦克风图标:ink 外框 + paper 内腔 + 拾音杆与底座。
static void mic_icon_build(lv_obj_t *parent) {
    s_mic_icon = lv_obj_create(parent);
    lv_obj_remove_flag(s_mic_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_mic_icon, 8, 8);
    lv_obj_set_size(s_mic_icon, 34, 46);
    lv_obj_set_style_bg_opa(s_mic_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mic_icon, 0, 0);
    lv_obj_set_style_pad_all(s_mic_icon, 0, 0);
    s_mic_icon_base_y = lv_obj_get_y(s_mic_icon); // 动画锚点:创建时的位置

    page_block(s_mic_icon, 5, 0, 24, 26, UI_INK);   // 拾音头外框
    page_block(s_mic_icon, 9, 4, 16, 18, UI_PAPER); // 内腔(挖空成环)
    page_block(s_mic_icon, 14, 26, 6, 10, UI_INK);  // 杆
    page_block(s_mic_icon, 7, 36, 20, 4, UI_INK);   // 底座
}

static lv_obj_t *page_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static void ui_build(void) {
    s_scr = ui_pixel_screen_create("MIC");

    // 电量(右上,云朵 y8..25 之下;读不到显示 --)
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -8, 27);

    // 主面板:图标 + REC + 状态/说明 + BLE 行
    lv_obj_t *main = ui_pixel_panel_create(s_scr, 18, 52, 204, 116, UI_PAPER);
    mic_icon_build(main);
    s_rec = page_block(main, 44, 2, 12, 12, UI_RED);
    lv_obj_add_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
    s_wave_lo = page_block(main, 44, 18, 8, 10, UI_YELLOW);
    s_wave_hi = page_block(main, 44, 32, 8, 10, UI_ORANGE);
    lv_obj_add_flag(s_wave_lo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wave_hi, LV_OBJ_FLAG_HIDDEN);

    s_state_label = lv_label_create(main);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_pos(s_state_label, 62, 4);
    lv_obj_set_width(s_state_label, 116);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_WRAP);

    s_ble_label = lv_label_create(main);
    lv_obj_set_style_text_font(s_ble_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ble_label, lv_color_hex(0x2E5A73), 0);
    lv_obj_set_pos(s_ble_label, 62, 56);
    lv_obj_set_width(s_ble_label, 116);
    lv_label_set_long_mode(s_ble_label, LV_LABEL_LONG_WRAP);

    // 电平条:10 根,浅灰槽底,点亮色按区间绿/黄/红
    lv_obj_t *lvl = ui_pixel_panel_create(s_scr, 18, 176, 204, 40, UI_PAPER);
    for (int i = 0; i < 10; i++) {
        s_lvl_seg[i] = page_block(lvl, i * 18, 1, 16, 16, BAR_TRACK);
    }

    // 增益条:10 格 × 10%,与增益步长一致;百分比标签定宽右对齐
    lv_obj_t *gp = ui_pixel_panel_create(s_scr, 18, 224, 204, 56, UI_PAPER);
    lv_obj_t *gt = ui_pixel_label(gp, "GAIN", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(gt, 0, 0);
    s_gain_label = lv_label_create(gp);
    lv_obj_set_style_text_font(s_gain_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_gain_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(s_gain_label, 44);
    lv_label_set_text_fmt(s_gain_label, "%d%%", (int)s_gain);
    lv_obj_set_style_text_align(s_gain_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_gain_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    for (int i = 0; i < 10; i++) {
        s_gain_seg[i] = page_block(gp, i * 16, 20, 14, 12, BAR_TRACK);
    }
    s_bar_shown = (int)s_gain / 10;

    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 50ms LVGL 定时器:全部界面动画集中在这里
// ---------------------------------------------------------------------------

static void seg_color(lv_obj_t *seg, bool lit, uint32_t lit_color) {
    lv_obj_set_style_bg_color(seg, lv_color_hex(lit ? lit_color : BAR_TRACK), 0);
}

static uint32_t lvl_seg_color(int i) {
    return i < 6 ? UI_GRASS : (i < 8 ? UI_YELLOW : UI_RED);
}

static void tick(lv_timer_t *timer) {
    (void)timer;
    mic_state_t st = s_state;
    uint32_t now = now_ms();

    // 状态与说明文字
    if (st == MIC_TALKING)      lv_label_set_text(s_state_label, "TALKING ...");
    else if (st == MIC_PLAYBACK) lv_label_set_text(s_state_label, "PLAYING ...");
    else if (!s_audio_ok)        lv_label_set_text(s_state_label, "AUDIO FAIL");
    else                         lv_label_set_text(s_state_label, "HOLD OK: TALK\nTAP OK: PLAY");

    // REC 闪烁 + 麦克风图标随电平轻跳(从创建锚点直接定位,不累积动画)
    bool talking = st == MIC_TALKING;
    int pct = talking ? s_level_pct : 0;
    if (talking && mic_rec_blink(now - s_talk_start_ms)) {
        lv_obj_remove_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_rec, LV_OBJ_FLAG_HIDDEN);
    }
    int lift = mic_level_bars(pct, 4);
    lv_obj_set_y(s_mic_icon, s_mic_icon_base_y - lift);
    if (mic_level_bars(pct, 10) > 3) lv_obj_remove_flag(s_wave_lo, LV_OBJ_FLAG_HIDDEN);
    else                             lv_obj_add_flag(s_wave_lo, LV_OBJ_FLAG_HIDDEN);
    if (mic_level_bars(pct, 10) > 6) lv_obj_remove_flag(s_wave_hi, LV_OBJ_FLAG_HIDDEN);
    else                             lv_obj_add_flag(s_wave_hi, LV_OBJ_FLAG_HIDDEN);

    // 电平条
    int bars = mic_level_bars(pct, 10);
    for (int i = 0; i < 10; i++) seg_color(s_lvl_seg[i], i < bars, lvl_seg_color(i));

    // 增益条动画:每 tick 向目标挪 1 格;未到位前百分比标签亮黄
    uint8_t g = s_gain;
    s_bar_shown = mic_bar_step(s_bar_shown, (int)g / 10);
    for (int i = 0; i < 10; i++) seg_color(s_gain_seg[i], i < s_bar_shown, UI_GRASS);
    lv_label_set_text_fmt(s_gain_label, "%d%%", (int)g);
    lv_obj_set_style_text_color(s_gain_label,
        lv_color_hex(s_bar_shown != (int)g / 10 ? UI_ORANGE : UI_INK), 0);

    // BLE 状态行
    switch (mic_ble_status()) {
    case MIC_BLE_LIVE:  lv_label_set_text(s_ble_label, "BLE: LIVE"); break;
    case MIC_BLE_CONN:  lv_label_set_text(s_ble_label, "BLE: CONNECTED"); break;
    case MIC_BLE_ADV:   lv_label_set_text(s_ble_label, "BLE: ADVERTISING"); break;
    default:            lv_label_set_text(s_ble_label, "BLE: OFF"); break;
    }
    if (!s_ring_ok) lv_label_set_text(s_ble_label, "BLE ONLY (LOW MEM)");

    // 电量
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_batt_label, "--");
    else         lv_label_set_text_fmt(s_batt_label, "%d%%", soc);
}

// ---------------------------------------------------------------------------
// 页面生命周期
// ---------------------------------------------------------------------------

void demo_mic_enter(void) {
    memset(&s_level, 0, sizeof(s_level));
    s_state = MIC_IDLE;
    s_last_talk_ms = 0;
    s_level_pct = 0;
    s_stop_req = false;
    s_audio_ok = false;

    ui_build();

    // 声路:16k/16bit/单声道;失败则本页只读不发声
    s_audio_ok = bsp_audio_set_format(MIC_SAMPLE_RATE, 16, 1) == ESP_OK;
    if (s_audio_ok) {
        bsp_audio_set_volume(MIC_OUT_VOLUME);
        bsp_audio_set_mic_gain(s_gain);
    }

    // 增益持久化:NVS 读出(写由工作线程按 dirty 标志完成)
    s_nvs_ok = demo_radio_nvs_prepare() == ESP_OK &&
               nvs_open("mic_demo", NVS_READWRITE, &s_nvs) == ESP_OK;
    if (s_nvs_ok) {
        uint8_t saved = 0;
        if (nvs_get_u8(s_nvs, "gain_pct", &saved) == ESP_OK) {
            s_gain = mic_gain_clamp(saved);       // 旧数据可能越界,收敛后再用
        }
        bsp_audio_set_mic_gain(s_gain);
    } else {
        ESP_LOGW(TAG, "NVS 不可用,增益不会掉电保存");
    }

    // BLE:可连接广播 + GATT(Audio Data / Mic Gain / Audio Info)。
    // 先于录音缓冲启动:BLE 的控制/宿主内存池是硬需求,不能被大块缓冲挤掉。
    esp_err_t ble_err = mic_ble_start(on_ble_gain);
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 启动失败: %s", esp_err_to_name(ble_err));
    }

    // 本地录音缓冲:先试 4 秒(128KB),分配不到或分配后剩余内存不足就降到
    // 3/2/1 秒。BLE 连接建立时 NimBLE 还要申请数 KB,必须留出安全垫,否则
    // 会出现"连上即断(Malloc failed)"。
    static const size_t TRY_SEC[] = { 4, 3, 2, 1 };
    for (size_t i = 0; i < sizeof TRY_SEC / sizeof TRY_SEC[0]; i++) {
        size_t bytes = (size_t)MIC_SAMPLE_RATE * TRY_SEC[i] * sizeof(int16_t);
        int16_t *p = malloc(bytes);
        if (!p) continue;
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16 * 1024) {
            free(p);                                  // 剩余太少,BLE 连接会失败
            continue;
        }
        s_ring_buf = p;
        s_ring_ok = true;
        s_ring_seconds = TRY_SEC[i];
        mic_ring_init(&s_ring, s_ring_buf, (size_t)MIC_SAMPLE_RATE * TRY_SEC[i]);
        break;
    }
    if (!s_ring_ok) {
        ESP_LOGW(TAG, "录音缓冲分配失败,只支持 BLE 实时推流");
        mic_ring_init(&s_ring, NULL, 0);
    }

    s_task_done = xSemaphoreCreateBinary();
    if (xTaskCreate(mic_task, "demo_mic", MIC_TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "mic_task 创建失败");
        s_task = NULL;
        if (s_task_done) { vSemaphoreDelete(s_task_done); s_task_done = NULL; }
    }

    s_timer = lv_timer_create(tick, 50, NULL);
    ESP_LOGI(TAG, "mic 页进入: gain=%d%% ring=%us heap=%u largest=%u",
             (int)s_gain, (unsigned)s_ring_seconds,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void demo_mic_exit(void) {
    // 1) 先停工作线程(显式握手,不在 codec IO 阻塞点上杀任务)
    s_stop_req = true;
    if (s_task) {
        xSemaphoreTake(s_task_done, pdMS_TO_TICKS(1000));
        s_task = NULL;
    }
    if (s_task_done) { vSemaphoreDelete(s_task_done); s_task_done = NULL; }

    // 2) 停 BLE(host 收尾有信号量握手,不碰 LVGL,此处持锁安全)
    mic_ble_stop();

    // 3) 删定时器与屏幕,再释放页面内存
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr)   { lv_obj_delete(s_scr); s_scr = NULL; }
    s_state_label = s_ble_label = s_batt_label = s_gain_label = NULL;
    s_rec = s_mic_icon = s_wave_lo = s_wave_hi = NULL;
    memset(s_lvl_seg, 0, sizeof s_lvl_seg);
    memset(s_gain_seg, 0, sizeof s_gain_seg);

    if (s_ring_buf) { free(s_ring_buf); s_ring_buf = NULL; }
    s_ring_ok = false;
    if (s_nvs_ok) { nvs_close(s_nvs); s_nvs_ok = false; }
    s_gain_dirty = false;
    s_state = MIC_IDLE;
}

void demo_mic_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK) {
        switch (ev) {
        case BSP_BTN_PRESS:
            apply_state(mic_fsm_step(s_state, MIC_EV_OK_PRESS));
            break;
        case BSP_BTN_RELEASE:
            apply_state(mic_fsm_step(s_state, MIC_EV_OK_RELEASE));
            break;
        case BSP_BTN_CLICK:
            // 说话太短视为误触,不回放(避免"啪"的一声)
            if (s_last_talk_ms >= MIC_MIN_PLAY_MS && mic_ring_count(&s_ring) > 0) {
                apply_state(mic_fsm_step(s_state, MIC_EV_OK_CLICK));
            }
            break;
        default:                              // LONG:按住说话本身,忽略
            break;
        }
        return;
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (ev == BSP_BTN_CLICK) {
            s_gain = mic_gain_step(s_gain, btn == BSP_BTN_UP ? +1 : -1);
            mic_ble_set_gain(s_gain);         // 保持特征镜像一致
            s_gain_dirty = true;              // 工作线程应用 + 落盘
        } else if (ev == BSP_BTN_LONG) {
            demo_request_exit();              // 长按上/下返回菜单
        }
    }
}
