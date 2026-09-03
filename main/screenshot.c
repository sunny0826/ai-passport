// main/screenshot.c —— FAP_SCREENSHOT_V1 串口屏幕截图(仅观测,不改动设备状态)。
//
// 协议(AI Passport 发布技能 references/serial-screenshot.md):
//   主机打开 USB 串口(115200)后发送一行 "FAP_SCREENSHOT_V1\n";
//   固件先回一行 ASCII 头 "FAP_SCREENSHOT_V1 <宽> <高> RGB565LE <字节数>\n",
//   紧跟 宽*高*2 字节小端 RGB565、自上而下逐行的屏幕帧数据。
//
// 实现要点:
//   - 控制台就是 USB Serial/JTAG(VFS no-driver 模式,读操作天然非阻塞):
//     独立低优先级任务轮询 fd 0 匹配命令行,未连接或无数据时小睡,不空转。
//   - 截帧不打断 UI 结构:持 LVGL 锁后临时包装 flush 回调,主动 invalidate
//     全屏并由本任务驱动 lv_timer_handler。LVGL 用 240×20 行缓冲,全屏刷新
//     会被拆成 16 条自上而下、整宽 20 行的条带;回调里把"正好是下一条带"的
//     像素(换字节前的小端原始数据)按顺序写进串口,顺序即帧序。条带之外的
//     局部刷新(如吉祥物眨眼动画)照常上屏但不进帧流。
//   - VFS 默认 TX 行尾 CRLF 会把像素数据里的 0x0A 改写成 0x0D 0x0A;截帧
//     期间把 TX 行尾临时切到 LF(不改写)并全局静音日志,防止日志混入像素流,
//     结束后恢复。命令头恰好在首条带到达时才发出:首带不来就什么都不发,
//     主机只会得到超时,不会收到半截垃圾。
//   - 只读屏幕内容:不重启、不烧录、不擦存储、不改设置、不输出任何凭据。
//     截帧窗口内按键回调可能因拿不到 LVGL 锁而丢键,属预期内的短暂窗口。
//
// 已知边界:若截帧窗口内恰好出现"整宽且起点等于下一条带 y1"的局部刷新,
// 帧流会错位(产物花屏,肉眼可辨),重新截一次即可;发布流程会人工检查截图。
#include "screenshot.h"

#include "bsp_display.h"
#include "bsp_pins.h"

#include "display/lv_display_private.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "screenshot";

#define SCREENSHOT_CMD       "FAP_SCREENSHOT_V1"
#define SCREENSHOT_LINE_MAX  40    // 命令行缓冲上限,超长行整行丢弃
#define SCREENSHOT_STACK     7168  // 与 LVGL port 任务同规格:截帧时在本任务栈上跑 lv_timer_handler
#define SCREENSHOT_PRIO      3     // 低于 LVGL port(4)与叫声任务(5):UI 与音频永远优先
#define SCREENSHOT_BAND_MAX  500   // 截帧总超时:500 轮 × 2ms ≈ 1s,防条带永远不到时卡死

// ---- 截帧窗口状态:仅 screenshot 任务与 flush 回调触碰 --------------------
// (LVGL 渲染在 screenshot 任务内运行,回调与状态机天然同线程,无需加锁。)
static lv_display_flush_cb_t s_orig_flush;   // 被包装的原 flush 回调
static volatile bool     s_capture_armed;    // true=正在截帧,回调按条带搬运像素
static volatile uint32_t s_rows_done;        // 已搬运行数(同时是下一条带的 y1)

// fd 1 全量写:USB 未连接会立刻返回 -1,断连则重试若干次后放弃本次截帧。
static bool write_all(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int retries = 0;
    while (len > 0) {
        ssize_t n = write(1, p, len);
        if (n <= 0) {
            if (++retries > 100) return false;   // ≈500ms 仍写不出去:主机已不在读
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        retries = 0;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// flush 包装:截帧期间把"正好是下一条带"的整宽刷新按帧序写进串口(此时
// 像素尚未换字节,即协议要的小端 RGB565),再交给原回调换字节并推到面板。
static void screenshot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_capture_armed && area->x1 == 0 && area->x2 == BSP_LCD_W - 1 &&
        (uint32_t)area->y1 == s_rows_done && area->y2 >= area->y1) {
        uint32_t rows = (uint32_t)(area->y2 - area->y1 + 1);
        bool ok = true;
        if (s_rows_done == 0) {   // 协议头跟在首条带前,避免空帧时输出悬空头
            char header[64];
            int n = snprintf(header, sizeof(header), "%s %d %d RGB565LE %u\n",
                             SCREENSHOT_CMD, BSP_LCD_W, BSP_LCD_H,
                             (unsigned)(BSP_LCD_W * BSP_LCD_H * 2));
            ok = write_all(header, (size_t)n);
        }
        if (ok) ok = write_all(px_map, (size_t)rows * BSP_LCD_W * 2);
        if (ok) {
            s_rows_done += rows;
            if (s_rows_done >= BSP_LCD_H) s_capture_armed = false;
        } else {
            s_capture_armed = false;   // 主机断开:中止,剩余条带只上屏不外发
        }
    }
    if (s_orig_flush) s_orig_flush(disp, area, px_map);
}

// 执行一次截帧:全程持 LVGL 锁,由本任务驱动刷新;结束恢复一切并汇报。
static void screenshot_run(void)
{
    if (!bsp_lvgl_lock(2000)) return;   // LVGL 忙:本次忽略,主机可重试

    // 截帧窗口:静音日志 + TX 行尾切 LF。CONFIG_LOG_DEFAULT_LEVEL 是构建期
    // 默认档,结束后恢复它即可还原全局日志行为。
    esp_log_level_set("*", ESP_LOG_NONE);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    lv_display_t *disp = lv_display_get_default();
    // LVGL 9.5 没有 lv_display_get_flush_cb(),只能从私有结构体取原回调
    // (dependencies.lock 把 lvgl 锁在 9.5.0;升级 LVGL 时需复核这一行)。
    s_orig_flush  = disp->flush_cb;
    s_rows_done   = 0;
    s_capture_armed = true;
    lv_display_set_flush_cb(disp, screenshot_flush_cb);

    // 持锁期间 port 任务被挡在外面,由本任务安全地驱动 LVGL:全屏失效后
    // 一个刷新周期就会按条带顺序逐个进回调。
    lv_obj_invalidate(lv_screen_active());
    int guard = 0;
    while (s_capture_armed && guard++ < SCREENSHOT_BAND_MAX) {
        lv_timer_handler();
        if (s_capture_armed) vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_capture_armed = false;
    lv_display_set_flush_cb(disp, s_orig_flush);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "screenshot %s (%lu/%d rows)",
             (s_rows_done >= BSP_LCD_H) ? "ok" : "incomplete",
             (unsigned long)s_rows_done, BSP_LCD_H);
}

// 命令行监听:逐字节拼行,整行等于命令字才触发截帧;其余输入一律忽略。
static void screenshot_task(void *arg)
{
    (void)arg;
    char line[SCREENSHOT_LINE_MAX];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        ssize_t n = read(0, &ch, 1);   // no-driver VFS:无数据/未连接立即返回 -1
        if (n == 1) {
            if (ch == '\n' || ch == '\r') {
                if (len == sizeof(SCREENSHOT_CMD) - 1 &&
                    memcmp(line, SCREENSHOT_CMD, len) == 0) {
                    screenshot_run();
                }
                len = 0;
            } else if (len < sizeof(line)) {
                line[len++] = (char)ch;
            } else {
                len = 0;   // 超长行:丢弃重来,防止缓冲被无关输入填满
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));   // 未连接或无数据:小睡再试
        }
    }
}

void screenshot_init(void)
{
    if (xTaskCreate(screenshot_task, "screenshot", SCREENSHOT_STACK,
                    NULL, SCREENSHOT_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "screenshot task 创建失败,串口截图不可用");
    }
}
