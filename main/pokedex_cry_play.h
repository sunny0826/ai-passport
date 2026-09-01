// main/pokedex_cry_play.h —— 从 cryfs 分区流式解码 Opus 叫声。
// 仅固件侧;按键回调只投递编号,解码与 bsp_audio_write 在独立任务里。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 查找 cryfs、读入 TOC、拉起常驻播放任务。分区缺失时返回 false,
// 图鉴页仍可浏览,只是 OK 无声。
bool pokedex_cry_play_init(void);

// 非阻塞:请求播放 id(1..1025)。正在播的会被打断。未 init 则忽略。
void pokedex_cry_play_request(uint32_t id);

// 非阻塞:打断当前播放。
void pokedex_cry_play_stop(void);

// 页面退出时调用:打断播放。任务保持休眠,下次 enter 再 init 是幂等的。
void pokedex_cry_play_release(void);
