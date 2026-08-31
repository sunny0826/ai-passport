// main/pokedex_stream.h —— gzip 流式解压(miniz tinfl,纯 C)。
//
// 背景:ESP-IDF 5.5 的 esp_http_client 不支持 gzip,而 PokeAPI 在
// Accept-Encoding: gzip 时把 ~290KB JSON 压成 ~8KB 传输。本模块把到达的
// 压缩块【逐块立即解压】(tinfl 流式模式),输出回调 sink;不累积压缩字节,
// 也不驻留解压结果 —— 全程只有 open() 时分配的 32KB 输出环 ≈ 词典型内存。
//
// 内存纪律:32KB 环在 open() 时一次性分配。调用方应在堆连续块最充裕的
// 时刻(HTTP 客户端已释放/抓取启动前)调用 open();传输期不再有任何分配。
// 不依赖 ESP-IDF,可在宿主机单测(tests/test_pokedex_stream.c)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* tinfl 要求输出缓冲 ≥ 词典大小(32KB),同时用作流式输出的环。 */
#define POKEDEX_STREAM_RING_SIZE 32768u

typedef void (*pokedex_stream_sink_t)(void *user, const uint8_t *data,
                                      size_t len, bool *abort);

/* 需要 tinfl 完整类型(结构体内嵌其状态,约 10.6KB)。 */
#include "vendor/miniz/miniz_tinfl.h"

/* 结构体公开(定长);请只通过函数访问。tinfl 状态内置,避免调用方
   把它放到小栈上。ring 指针在 open() 时分配。 */
typedef struct pokedex_stream {
    pokedex_stream_sink_t sink;
    void *sink_user;

    uint8_t *ring;      /* 输出环 ≥32KB;open() 分配,deinit 释放 */
    size_t ring_cap;
    tinfl_decompressor dec; /* 约 10.6KB,内置 */

    uint8_t hdr[24];    /* gzip 头暂存(gzip 头可能跨传输块) */
    unsigned hdr_len;
    unsigned state;     /* 0=等头,1=解压中,2=完成 */

    /* 诊断(只读):解压输出总字节、最后一次 tinfl 状态码与失败原因码
       (1=无效参数 2=gzip头坏/超长 3=输出环分配失败 4=输入截断 5=tinfl错误)。 */
    size_t out_total;
    int last_status;
    int fail_reason;
} pokedex_stream_t;

// 打开流:分配输出环、初始化解压器,进入"等 gzip 头"阶段。
// sink 每收到一个输出块被调用一次;sink 设 *abort 可提前结束(视为成功)。
bool pokedex_stream_open(pokedex_stream_t *s, pokedex_stream_sink_t sink,
                         void *user);

// 喂入一块压缩字节(任意长度,顺序保持)。返回 false = 流已损坏/失败。
bool pokedex_stream_feed(pokedex_stream_t *s, const uint8_t *data, size_t len);

// 输入全部喂完后调用:用"无更多输入"的最后一次 tinfl 调用校验流结束。
// 成功(或调用方提前 abort)返回 true;截断返回 false(reason=4)。
bool pokedex_stream_close(pokedex_stream_t *s);

// 释放环。之后可重新 open。
void pokedex_stream_deinit(pokedex_stream_t *s);

// 供测试:独立解析 gzip 头,成功返回 deflate 数据起始偏移。
bool pokedex_stream_header_skip(const uint8_t *data, size_t len, size_t *off);