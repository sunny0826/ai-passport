// mic_ble.h —— 麦克风页的 BLE 侧:NimBLE 可连接广播 + GATT 服务。
//
// 对外提供(电脑/手机用 nRF Connect、LightBlue、Web Bluetooth 等通用 BLE 工具即可):
//   服务  MIC_SVC_UUID
//     - Audio Data   (notify)   说话期间按 chunk 推送 16kHz/16bit/单声道 PCM
//     - Mic Gain     (read/write) 0..100,写入口会回调页面并持久化
//     - Audio Info   (read)     ASCII "pcm;16000;16;1"
//
// 线程约定:所有回调都在 NimBLE host 任务里,禁止阻塞、禁止直接操作 LVGL 或
// 调用慢速 IO(见 demo_mic.c 里注册的 gain 回调实现)。mic_ble_stream() 可从
// 音频工作线程调用(NimBLE 宿主 API 线程安全);池耗尽时丢弃并计流失败,
// 绝不阻塞录音。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 增益被 BLE 客户端写入时回调(参数已收敛到 0..100)。运行于 NimBLE host 任务。
typedef void (*mic_ble_gain_cb_t)(uint8_t pct);

typedef enum {
    MIC_BLE_OFF = 0,   // 未初始化
    MIC_BLE_ADV,       // 广播中,等待电脑/手机连接
    MIC_BLE_CONN,      // 已连接,但未订阅 Audio Data
    MIC_BLE_LIVE,      // 已连接且已订阅(说话时会推流)
} mic_ble_status_t;

// 启动 NimBLE、注册 GATT 并开始可连接广播。依赖 demo_radio_nvs_prepare()。
// gain_cb:客户端写 Mic Gain 特征时的回调,可为 NULL。重复调用返回 INVALID_STATE。
esp_err_t mic_ble_start(mic_ble_gain_cb_t gain_cb);

// 停止广播并卸载 NimBLE(host 任务收尾用信号量握手,参照 demo_ble.c)。
void mic_ble_stop(void);

// 当前 BLE 状态(给 UI 状态行)。
mic_ble_status_t mic_ble_status(void);

// 页面侧增益变化时同步镜像(供客户端读取);不会反过来调 gain_cb。
void mic_ble_set_gain(uint8_t pct);

// 推流一段 PCM(任意长度,内部按当前 MTU 分包 notify)。
// 未连接/未订阅/mbuf 不足时返回 false(调用方直接丢弃该段)。
bool mic_ble_stream(const void *pcm, size_t bytes);
