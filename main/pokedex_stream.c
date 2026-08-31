// main/pokedex_stream.c —— gzip 流式解压实现(见 pokedex_stream.h)。
// 输入分块到达即解压,不累积压缩字节;输出环 open() 时分配一次。
#include "pokedex_stream.h"

#include <stdlib.h>
#include <string.h>

/* miniz 配置:只要 tinfl/tdefl 解压相关,不要 zip/stdio/time。 */
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "vendor/miniz/miniz_tinfl.h"

#define PST_HDR   0u /* 等 gzip 头 */
#define PST_INFL  1u /* 解压中 */
#define PST_DONE  2u /* 已到流尾 */

bool pokedex_stream_header_skip(const uint8_t *data, size_t len, size_t *off)
{
    /* 标准 gzip 头:1f 8b | CM=8 | FLG | MTIME(4) | XFL | OS,然后按 FLG:
       FEXTRA(0x04): XLEN(2) + 数据;FNAME(0x08)/FCOMMENT(0x10): NUL 结尾;
       FHCRC(0x02): CRC16(2)。 */
    if (!data || len < 10u) return false;
    if (data[0] != 0x1Fu || data[1] != 0x8Bu || data[2] != 8u) return false;

    size_t p = 10u;
    uint8_t flg = data[3];
    if (flg & 0x04u) { /* FEXTRA */
        if (p + 2u > len) return false;
        size_t xlen = (size_t)data[p] | ((size_t)data[p + 1] << 8);
        p += 2u + xlen;
        if (p > len) return false;
    }
    if (flg & 0x08u) { /* FNAME */
        while (p < len && data[p] != 0) p++;
        if (p >= len) return false;
        p++;
    }
    if (flg & 0x10u) { /* FCOMMENT */
        while (p < len && data[p] != 0) p++;
        if (p >= len) return false;
        p++;
    }
    if (flg & 0x02u) { /* FHCRC */
        if (p + 2u > len) return false;
        p += 2u;
    }
    if (off) *off = p;
    return true;
}

/* 用给定的输入窗口跑一次 tinfl,把环里新产生的字节交给 sink。
   *done=true 表示本步不会再消费更多输入(等下一块/已结束)。 */
static bool inflate_step(pokedex_stream_t *s, const uint8_t *in,
                         size_t in_len, size_t *consumed, uint32_t flags,
                         bool *done)
{
    size_t in_size = in_len;
    size_t out_size = s->ring_cap;
    tinfl_status st = tinfl_decompress(&s->dec, in, &in_size, s->ring,
                                       s->ring, &out_size, flags);
    *consumed = in_size;
    *done = false;
    s->last_status = (int)st;

    s->out_total += out_size;
    if (out_size > 0) {
        bool abort = false;
        s->sink(s->sink_user, s->ring, out_size, &abort);
        if (abort) {
            s->state = PST_DONE; /* 调用方提前终止,视为成功 */
            *done = true;
            return true;
        }
    }

    if (st == TINFL_STATUS_DONE) {
        s->state = PST_DONE;
        *done = true;
        return true;
    }
    if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
        return true; /* 环满已排空,可继续消费剩余输入 */
    }
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT) {
        *done = true; /* 本块输入耗尽,等下一块 */
        return true;
    }
    s->fail_reason = 5;
    return false;
}

bool pokedex_stream_open(pokedex_stream_t *s, pokedex_stream_sink_t sink,
                         void *user)
{
    if (!s || !sink) return false;
    memset(s, 0, sizeof(*s));
    s->sink = sink;
    s->sink_user = user;
    s->ring = malloc(POKEDEX_STREAM_RING_SIZE);
    if (!s->ring) {
        s->fail_reason = 3;
        return false;
    }
    s->ring_cap = POKEDEX_STREAM_RING_SIZE;
    tinfl_init(&s->dec);
    s->state = PST_HDR;
    return true;
}

bool pokedex_stream_feed(pokedex_stream_t *s, const uint8_t *data, size_t len)
{
    if (!s || (!data && len)) return false;
    if (!s->ring) return false;
    if (s->state == PST_DONE) return true; /* 尾部 CRC/ISIZE 等忽略 */

    size_t pos = 0;

    if (s->state == PST_HDR) {
        /* 累积到能解析出完整 gzip 头(可能跨传输块)。 */
        while (pos < len && s->state == PST_HDR) {
            size_t room = sizeof(s->hdr) - s->hdr_len;
            size_t take = len - pos;
            if (take > room) take = room;
            memcpy(s->hdr + s->hdr_len, data + pos, take);
            s->hdr_len += (unsigned)take;
            pos += take;

            size_t off = 0;
            if (pokedex_stream_header_skip(s->hdr, s->hdr_len, &off)) {
                s->state = PST_INFL;
                if (s->hdr_len > off) {
                    /* 头之后紧跟的字节已暂存在 hdr,直接解压。 */
                    size_t consumable = s->hdr_len - off;
                    bool done = false;
                    size_t consumed = 0;
                    if (!inflate_step(s, s->hdr + off, consumable, &consumed,
                                      TINFL_FLAG_HAS_MORE_INPUT, &done)) {
                        return false;
                    }
                }
            } else if (s->hdr_len >= sizeof(s->hdr)) {
                s->fail_reason = 2; /* 头超长/损坏(正常头 <24B) */
                return false;
            }
        }
        if (s->state == PST_HDR) return true; /* 头还没齐,等下一块 */
    }

    /* PST_INFL:逐块立即解压(含本块头之后的剩余字节)。 */
    while (pos < len && s->state == PST_INFL) {
        bool done = false;
        size_t consumed = 0;
        if (!inflate_step(s, data + pos, len - pos, &consumed,
                          TINFL_FLAG_HAS_MORE_INPUT, &done)) {
            return false;
        }
        pos += consumed;
        if (s->state == PST_DONE) break;  /* 流尾 */
        if (done) break;                  /* 输入耗尽,等下一块 */
    }
    return true;
}

bool pokedex_stream_close(pokedex_stream_t *s)
{
    if (!s || !s->ring) return false;
    if (s->state == PST_HDR) { /* 头都没齐 = 截断 */
        s->fail_reason = 2;
        return false;
    }
    if (s->state == PST_DONE) return true;

    /* 最后一次调用:明确"无更多输入"(flags=0),让 tinfl 校验流结束。 */
    for (;;) {
        bool done = false;
        size_t consumed = 0;
        if (!inflate_step(s, NULL, 0, &consumed, 0u, &done)) return false;
        if (s->state == PST_DONE) return true;
        if (done) { /* NEEDS_MORE_INPUT 且再无输入 = 截断 */
            s->fail_reason = 4;
            return false;
        }
    }
}

void pokedex_stream_deinit(pokedex_stream_t *s)
{
    if (!s) return;
    free(s->ring);
    memset(s, 0, sizeof(*s));
}