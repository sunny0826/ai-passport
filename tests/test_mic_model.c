// tests/test_mic_model.c —— mic_model 的宿主机测试:cc -Imain 编译,无 ESP-IDF 依赖。
#include <assert.h>
#include "mic_model.h"

static void test_gain(void) {
    assert(mic_gain_clamp(-5) == 0);
    assert(mic_gain_clamp(0) == 0);
    assert(mic_gain_clamp(55) == 55);
    assert(mic_gain_clamp(100) == 100);
    assert(mic_gain_clamp(120) == 100);

    assert(mic_gain_step(100, +1) == 100);   // 顶格不再加
    assert(mic_gain_step(95, +1) == 100);    // 越界收敛
    assert(mic_gain_step(95, -1) == 85);
    assert(mic_gain_step(5, -1) == 0);
    assert(mic_gain_step(0, -1) == 0);       // 底格不再减
}

static void test_level_ema(void) {
    mic_level_t lv;
    mic_level_reset(&lv);

    // 首帧直通:立即反映输入,而不是从 0 爬升。
    assert(mic_level_update(&lv, 32767) == 100);
    assert(mic_level_update(&lv, 0) < 100);  // 释放开始回落

    // 攻击快:满幅输入约 130ms(13 次 10ms 更新)追上 63% 以上。
    mic_level_reset(&lv);
    mic_level_update(&lv, 0);
    int pct = 0;
    for (int i = 0; i < 13; i++) pct = mic_level_update(&lv, 32767);
    assert(pct >= 63);

    // 释放慢:满幅后 10 次(100ms)空输入仍明显高于 0。
    for (int i = 0; i < 10; i++) pct = mic_level_update(&lv, 0);
    assert(pct > 40);

    // 最终收敛到 0,过程永不超界。
    for (int i = 0; i < 400; i++) pct = mic_level_update(&lv, 0);
    assert(pct == 0);

    // 中等输入不超 100。
    mic_level_reset(&lv);
    for (int i = 0; i < 50; i++) pct = mic_level_update(&lv, 16000);
    assert(pct <= 100 && pct > 0);
}

static void test_bars_and_anim(void) {
    int16_t quiet[4] = {0, -5, 5, -3};
    assert(mic_peak_abs(quiet, 4) == 5);
    int16_t loud[3] = {-32000, 32767, 100};
    assert(mic_peak_abs(loud, 3) == 32767);
    int16_t full[2] = {32767, -32768};       // INT16_MIN 的绝对值饱和到 32767
    assert(mic_peak_abs(full, 2) == 32767);
    assert(mic_peak_abs(NULL, 3) == 0);
    assert(mic_peak_abs(quiet, 0) == 0);

    assert(mic_level_bars(0, 10) == 0);
    assert(mic_level_bars(100, 10) == 10);
    assert(mic_level_bars(55, 10) == 6);     // 四舍五入
    assert(mic_level_bars(54, 10) == 5);
    assert(mic_level_bars(50, 10) == 5);
    assert(mic_level_bars(120, 10) == 10);
    assert(mic_level_bars(50, 0) == 0);

    assert(mic_bar_step(3, 6) == 4);         // 每步向目标靠近 1 格
    assert(mic_bar_step(6, 3) == 5);
    assert(mic_bar_step(5, 5) == 5);

    assert(mic_rec_blink(0) == true);
    assert(mic_rec_blink(249) == true);
    assert(mic_rec_blink(250) == false);
    assert(mic_rec_blink(500) == true);
}

static void test_ring(void) {
    int16_t store[8];
    mic_ring_t r;
    int16_t out[16];

    mic_ring_init(&r, store, 8);
    assert(mic_ring_count(&r) == 0);
    assert(mic_ring_read(&r, out, 4) == 0);  // 空读安全

    // 顺序写入 1..5,原序读回。
    int16_t in[5] = {1, 2, 3, 4, 5};
    assert(mic_ring_write(&r, in, 5) == 5);
    assert(mic_ring_count(&r) == 5);
    assert(mic_ring_read(&r, out, 5) == 5);
    for (int i = 0; i < 5; i++) assert(out[i] == i + 1);
    assert(mic_ring_count(&r) == 0);

    // 跨越末尾的写入(head 落在 5):1..5 之后继续 6..10,应环绕。
    int16_t seq[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    mic_ring_clear(&r);
    assert(mic_ring_write(&r, seq, 10) == 10);   // 超容量,保留最新 8 个 = 3..10
    assert(mic_ring_count(&r) == 8);
    assert(mic_ring_read(&r, out, 8) == 8);
    for (int i = 0; i < 8; i++) assert(out[i] == i + 3);

    // 满 8 后继续小块写,覆盖最旧。
    mic_ring_clear(&r);
    int16_t fill[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    assert(mic_ring_write(&r, fill, 8) == 8);
    int16_t one = 99;
    assert(mic_ring_write(&r, &one, 1) == 1);    // 覆盖 10,内容变 11..17,99
    assert(mic_ring_count(&r) == 8);
    assert(mic_ring_read(&r, out, 16) == 8);     // 读超量被截断
    assert(out[0] == 11 && out[7] == 99);

    // 分段读:写 0..7,每次读 3。
    mic_ring_clear(&r);
    int16_t eight[8];
    for (int i = 0; i < 8; i++) eight[i] = i * 10;
    mic_ring_write(&r, eight, 8);
    assert(mic_ring_read(&r, out, 3) == 3);
    assert(out[0] == 0 && out[2] == 20);
    assert(mic_ring_read(&r, out, 3) == 3);
    assert(out[0] == 30 && out[2] == 50);
    assert(mic_ring_read(&r, out, 3) == 2);      // 只剩 2 个
    assert(out[0] == 60 && out[1] == 70);
    assert(mic_ring_count(&r) == 0);

    // 单次写入超过容量:只保留最新 cap 个。
    mic_ring_clear(&r);
    assert(mic_ring_write(&r, seq, 10) == 10);
    assert(mic_ring_count(&r) == 8);
    mic_ring_read(&r, out, 8);
    assert(out[0] == 3);
}

static void test_fsm(void) {
    // 按住说话:IDLE → TALKING → IDLE。
    assert(mic_fsm_step(MIC_IDLE, MIC_EV_OK_PRESS) == MIC_TALKING);
    assert(mic_fsm_step(MIC_TALKING, MIC_EV_OK_RELEASE) == MIC_IDLE);

    // 单击回放:IDLE → PLAYBACK → PLAY_DONE → IDLE。
    assert(mic_fsm_step(MIC_IDLE, MIC_EV_OK_CLICK) == MIC_PLAYBACK);
    assert(mic_fsm_step(MIC_PLAYBACK, MIC_EV_PLAY_DONE) == MIC_IDLE);

    // 回放中按 OK:打断并进入说话,再松开回 IDLE。
    assert(mic_fsm_step(MIC_PLAYBACK, MIC_EV_OK_PRESS) == MIC_TALKING);
    assert(mic_fsm_step(MIC_TALKING, MIC_EV_OK_RELEASE) == MIC_IDLE);

    // 非法/干扰事件保持原状态。
    assert(mic_fsm_step(MIC_IDLE, MIC_EV_OK_RELEASE) == MIC_IDLE);
    assert(mic_fsm_step(MIC_IDLE, MIC_EV_PLAY_DONE) == MIC_IDLE);
    assert(mic_fsm_step(MIC_TALKING, MIC_EV_OK_CLICK) == MIC_TALKING);
    assert(mic_fsm_step(MIC_TALKING, MIC_EV_PLAY_DONE) == MIC_TALKING);
    assert(mic_fsm_step(MIC_PLAYBACK, MIC_EV_OK_RELEASE) == MIC_PLAYBACK);
    assert(mic_fsm_step(MIC_PLAYBACK, MIC_EV_OK_CLICK) == MIC_PLAYBACK);
}

int main(void) {
    test_gain();
    test_level_ema();
    test_bars_and_anim();
    test_ring();
    test_fsm();
    return 0;
}
