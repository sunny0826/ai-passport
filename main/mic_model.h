// mic_model.h —— 麦克风页的纯逻辑模型:不依赖 ESP-IDF / LVGL / FreeRTOS,
// 全部可在宿主机(cc + tests/test_mic_model.c)上测试。
//
// 覆盖四块可测逻辑:
//   1. 麦克风增益的步进与收敛(0..100%,步长 10)。
//   2. 说话电平的非对称 EMA 平滑(快攻 ~130ms / 慢放 ~500ms,10ms 一次更新,
//      参照 docs/reference/y2lin/meter-ui-smoothing-and-layout.md 的经验)。
//   3. 录音环形缓冲的写入/回读账目(满后覆盖最旧,回放按最旧→最新顺序)。
//   4. 按住说话的状态机(空闲/说话/回放)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 增益
// ---------------------------------------------------------------------------

#define MIC_GAIN_MAX     100   // 增益上限(%),100% 对应 BSP 的 30 dB 基线
#define MIC_GAIN_STEP     10   // 上/下键每击的步长(%)
#define MIC_GAIN_DEFAULT 100   // 出厂默认:与产品基线 30 dB 一致

// 把任意整数收敛到 0..MIC_GAIN_MAX。
uint8_t mic_gain_clamp(int pct);

// 按方向步进增益:dir>0 加一步,dir<0 减一步,边界收敛不回绕。
uint8_t mic_gain_step(uint8_t pct, int dir);

// ---------------------------------------------------------------------------
// 电平平滑(非对称 EMA,Q8 定点)
// ---------------------------------------------------------------------------

typedef struct {
    int  q8;      // 平滑后的电平,0..25600 即 0..100%(Q8 定点)
    bool primed;  // 首帧直通标志,避免开机从 0 缓慢爬升
} mic_level_t;

// 复位为未预热状态(每次开始说话时调用,防止沿用上次的余量)。
void mic_level_reset(mic_level_t *lv);

// 输入一段 10ms 采样的峰值幅度(0..32767),返回平滑后的电平 0..100(%)。
// 攻击快、释放慢:读数跟着说话快速上冲,停顿时缓慢回落,不抖动。
int mic_level_update(mic_level_t *lv, uint16_t peak_0_32767);

// 把平滑电平(0..100)映射到 max_bars 根电平条中应点亮的根数(四舍五入)。
int mic_level_bars(int pct, int max_bars);

// 取一段 PCM 的峰值幅度(绝对值,饱和到 32767)。给 EMA 提供输入。
uint16_t mic_peak_abs(const int16_t *samples, size_t n);

// ---------------------------------------------------------------------------
// 音量条动画:shown 每步向 target 靠近 1 格
// ---------------------------------------------------------------------------

int mic_bar_step(int shown, int target);

// ---------------------------------------------------------------------------
// 说话动画
// ---------------------------------------------------------------------------

#define MIC_BLINK_PERIOD_MS 250   // 说话时 REC 方块的闪烁半周期

// 说话期间的 REC 方块:按 elapsed_ms 以 250ms 半周期闪烁。
bool mic_rec_blink(uint32_t elapsed_ms);

// ---------------------------------------------------------------------------
// 录音环形缓冲(满后覆盖最旧;回放按最旧→最新的顺序消费)
// ---------------------------------------------------------------------------

typedef struct {
    int16_t *buf;      // 外部提供的存储(页负责分配/释放)
    size_t   cap;      // 容量(采样数)
    size_t   head;     // 下一个写入位置
    size_t   count;    // 有效采样数(≤ cap)
} mic_ring_t;

void   mic_ring_init(mic_ring_t *r, int16_t *storage, size_t cap_samples);
void   mic_ring_clear(mic_ring_t *r);
size_t mic_ring_count(const mic_ring_t *r);
// 写入 n 个采样,返回实际保存的数量(满后覆盖最旧,返回值恒为 n,除非 n>cap)。
size_t mic_ring_write(mic_ring_t *r, const int16_t *src, size_t n);
// 从最旧→最新读出最多 n 个采样并消费,返回实际读到的数量。
size_t mic_ring_read(mic_ring_t *r, int16_t *dst, size_t n);

// ---------------------------------------------------------------------------
// 按住说话状态机
// ---------------------------------------------------------------------------

typedef enum {
    MIC_IDLE = 0,     // 空闲:等 OK 按下(说话)或单击(回放)
    MIC_TALKING,      // 按住 OK 说话中
    MIC_PLAYBACK,     // 回放上一段录音
} mic_state_t;

typedef enum {
    MIC_EV_OK_PRESS = 0,   // OK 按下
    MIC_EV_OK_RELEASE,     // OK 抬起
    MIC_EV_OK_CLICK,       // OK 单击(页保证:仅当上一段说话足够长才发)
    MIC_EV_PLAY_DONE,      // 回放自然放完(工作线程通知)
} mic_event_t;

// 单步状态转移。输入当前状态与事件,返回新状态;非法组合保持原状态。
mic_state_t mic_fsm_step(mic_state_t s, mic_event_t ev);

// 说话短于该时长时,OK 单击不触发回放(视为误触,避免回放"啪"的一声)。
#define MIC_MIN_PLAY_MS 500
