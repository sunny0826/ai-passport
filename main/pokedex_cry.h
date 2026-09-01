// main/pokedex_cry.h —— 全国图鉴叫声 blob 的只读解析。
// 纯 C,不依赖 ESP-IDF/LVGL,可在宿主机单测(tests/test_pokedex_cry.c)。
// 固件把同一份 blob 放在 cryfs 数据分区,按帧流式读取,不把整段 PCM 摊进 RAM。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pokedex_core.h"

// 小端 "CRY1"。改布局必须升版本并同步 tools/gen_pokedex_cries.py。
#define POKEDEX_CRY_MAGIC       0x31595243u
#define POKEDEX_CRY_SAMPLE_HZ   16000u
#define POKEDEX_CRY_FRAME_MS    20u
#define POKEDEX_CRY_HEADER_SIZE 16u
#define POKEDEX_CRY_TOC_ITEM    8u

// 定长文件头。后接 count 个 TOC 项,再接 payload。
typedef struct {
    uint32_t magic;
    uint32_t count;       /* 必须等于 POKEDEX_DEX_SIZE */
    uint32_t sample_hz;   /* 16000 */
    uint16_t frame_ms;    /* 20 */
    uint16_t reserved;
} pokedex_cry_header_t;

typedef struct {
    uint32_t offset; /* 相对 payload 起点 */
    uint32_t length; /* 该只叫声字节数(长度前缀包流) */
} pokedex_cry_toc_ent_t;

// 校验头:magic/count/采样率/帧长,且 blob 至少能放下 TOC。
bool pokedex_cry_header_ok(const uint8_t *blob, size_t blob_len,
                           pokedex_cry_header_t *out);

// payload 起点(头 + TOC)。blob 过短返回 0。
size_t pokedex_cry_payload_off(const uint8_t *blob, size_t blob_len);

// 查 id 的 TOC。blob_len 是 TOC 可读长度;storage_len 是整份存储
// (分区或完整文件)的字节数,用来核对 payload 是否越界。
// pokedex_cry_lookup 等价于 storage_len == blob_len(完整文件)。
bool pokedex_cry_lookup_storage(const uint8_t *blob, size_t blob_len,
                                size_t storage_len, uint32_t id,
                                pokedex_cry_toc_ent_t *out);
bool pokedex_cry_lookup(const uint8_t *blob, size_t blob_len, uint32_t id,
                        pokedex_cry_toc_ent_t *out);

// 从 clip 的 cursor 处读下一包。成功后 *cursor 前进;
// 结束返回 false 且不改 pkt。包长 0 或越界视为损坏,返回 false。
bool pokedex_cry_next_packet(const uint8_t *clip, size_t clip_len,
                             size_t *cursor, const uint8_t **pkt,
                             uint16_t *pkt_len);
