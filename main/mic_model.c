// mic_model.c —— 见 mic_model.h。只依赖 stdint/stdbool/stddef。
#include "mic_model.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 增益
// ---------------------------------------------------------------------------

uint8_t mic_gain_clamp(int pct) {
    if (pct < 0)   return 0;
    if (pct > MIC_GAIN_MAX) return MIC_GAIN_MAX;
    return (uint8_t)pct;
}

uint8_t mic_gain_step(uint8_t pct, int dir) {
    int next = (int)pct + (dir > 0 ? MIC_GAIN_STEP : -MIC_GAIN_STEP);
    return mic_gain_clamp(next);
}

// ---------------------------------------------------------------------------
// 电平平滑
// ---------------------------------------------------------------------------
// 10ms 更新一次;Q8 攻击/释放系数按"时间常数 ≈ 周期/alpha"选取:
//   攻击 19/256 ≈ 0.074 → 130ms 内追上 63%;释放 5/256 ≈ 0.020 → ~500ms。
#define MIC_LVL_ATTACK_NUM   19
#define MIC_LVL_RELEASE_NUM   5
#define MIC_LVL_ALPHA_DEN   256
#define MIC_LVL_FULL        25600   // 100% 的 Q8 值

void mic_level_reset(mic_level_t *lv) {
    if (lv) { lv->q8 = 0; lv->primed = false; }
}

int mic_level_update(mic_level_t *lv, uint16_t peak_0_32767) {
    if (!lv) return 0;

    // 峰值幅度 → 百分比 → Q8。
    int target_pct = ((int)peak_0_32767 * 100 + 32767 / 2) / 32767;
    int target_q8 = target_pct * 256;
    if (target_q8 > MIC_LVL_FULL) target_q8 = MIC_LVL_FULL;

    if (!lv->primed) {                       // 首帧直通:读数立即反映现实
        lv->q8 = target_q8;
        lv->primed = true;
        return target_pct;
    }

    int alpha = (target_q8 >= lv->q8) ? MIC_LVL_ATTACK_NUM : MIC_LVL_RELEASE_NUM;
    lv->q8 += ((target_q8 - lv->q8) * alpha) / MIC_LVL_ALPHA_DEN;
    int pct = lv->q8 / 256;
    return pct > 100 ? 100 : pct;
}

int mic_level_bars(int pct, int max_bars) {
    if (max_bars <= 0) return 0;
    if (pct <= 0) return 0;
    if (pct >= 100) return max_bars;
    return (pct * max_bars + 50) / 100;      // 四舍五入
}

uint16_t mic_peak_abs(const int16_t *samples, size_t n) {
    if (!samples || n == 0) return 0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = samples[i] < 0 ? -(int32_t)samples[i] : (int32_t)samples[i];
        if (v > peak) peak = v;
    }
    return peak > 32767 ? 32767 : (uint16_t)peak;
}

// ---------------------------------------------------------------------------
// 音量条动画
// ---------------------------------------------------------------------------

int mic_bar_step(int shown, int target) {
    if (shown < target) return shown + 1;
    if (shown > target) return shown - 1;
    return shown;
}

// ---------------------------------------------------------------------------
// 说话动画
// ---------------------------------------------------------------------------

bool mic_rec_blink(uint32_t elapsed_ms) {
    return (elapsed_ms / MIC_BLINK_PERIOD_MS) % 2 == 0;
}

// ---------------------------------------------------------------------------
// 录音环形缓冲
// ---------------------------------------------------------------------------

void mic_ring_init(mic_ring_t *r, int16_t *storage, size_t cap_samples) {
    if (!r) return;
    r->buf = storage;
    r->cap = cap_samples;
    r->head = 0;
    r->count = 0;
}

void mic_ring_clear(mic_ring_t *r) {
    if (!r) return;
    r->head = 0;
    r->count = 0;
}

size_t mic_ring_count(const mic_ring_t *r) {
    return r ? r->count : 0;
}

size_t mic_ring_write(mic_ring_t *r, const int16_t *src, size_t n) {
    if (!r || !r->buf || !src) return 0;
    size_t asked = n;           // 契约:即使超容量截断,返回值仍是请求值 n
    if (n > r->cap) {           // 单次写入超过容量:只保留最新的 cap 个
        src += n - r->cap;
        n = r->cap;
    }

    for (size_t done = 0; done < n; ) {
        size_t space = r->cap - r->head;             // head 到缓冲末尾的连续空间
        size_t chunk = n - done < space ? n - done : space;
        memcpy(r->buf + r->head, src + done, chunk * sizeof(int16_t));
        r->head = (r->head + chunk) % r->cap;
        done += chunk;
        // 满后覆盖最旧:count 先贴住 cap,再随继续写入保持不变(旧数据被顶掉)。
        r->count = (r->count + chunk > r->cap) ? r->cap : r->count + chunk;
    }
    return asked;
}

size_t mic_ring_read(mic_ring_t *r, int16_t *dst, size_t n) {
    if (!r || !r->buf || !dst || r->count == 0) return 0;
    if (n > r->count) n = r->count;

    // 最旧的数据起点 = head 往回退 count 个(环形回退)。
    size_t tail = (r->head + r->cap - r->count) % r->cap;
    size_t done = 0;
    while (done < n) {
        size_t span = r->cap - tail;                 // tail 到缓冲末尾的连续数据
        size_t chunk = n - done < span ? n - done : span;
        memcpy(dst + done, r->buf + tail, chunk * sizeof(int16_t));
        tail = (tail + chunk) % r->cap;
        done += chunk;
    }
    r->count -= n;
    return n;
}

// ---------------------------------------------------------------------------
// 按住说话状态机
// ---------------------------------------------------------------------------

mic_state_t mic_fsm_step(mic_state_t s, mic_event_t ev) {
    switch (s) {
    case MIC_IDLE:
        switch (ev) {
        case MIC_EV_OK_PRESS:  return MIC_TALKING;   // 按住 → 说话
        case MIC_EV_OK_CLICK:  return MIC_PLAYBACK;  // 单击 → 回放上一段
        default:               return MIC_IDLE;
        }
    case MIC_TALKING:
        switch (ev) {
        case MIC_EV_OK_RELEASE: return MIC_IDLE;     // 松开 → 停止录音
        default:                return MIC_TALKING;  // 说话中的其余事件忽略
        }
    case MIC_PLAYBACK:
        switch (ev) {
        case MIC_EV_OK_PRESS:   return MIC_TALKING;  // 回放中按 OK → 打断并说话
        case MIC_EV_PLAY_DONE:  return MIC_IDLE;     // 放完 → 空闲
        default:                return MIC_PLAYBACK; // 回放中的松开/单击忽略
        }
    default:
        return MIC_IDLE;
    }
}
