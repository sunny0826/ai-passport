// tests/test_pokedex_stream.c —— 宿主机单测:gzip 头解析 + tinfl 流式解压。
// 用 lodepng 的 deflate 编码器在运行时构造合法的 gzip 流,再分块走
// pokedex_stream 的 open/feed/close 还原,校验逐字节一致;
// 覆盖跨块 gzip 头、截断、坏输入等路径。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pokedex_stream.h"
#include "pokedex_core.h"
#include "vendor/lodepng.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} sink_buf_t;

static void sink_cb(void *user, const uint8_t *data, size_t len, bool *abort)
{
    (void)abort;
    sink_buf_t *b = (sink_buf_t *)user;
    if (b->len + len > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        while (ncap < b->len + len) ncap *= 2;
        b->buf = realloc(b->buf, ncap);
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

/* 用 lodepng 构造 gzip:魔数头(含可选扩展)+ deflate 流 + CRC32 + ISIZE。 */
static uint8_t *make_gzip(const uint8_t *plain, size_t plain_len, size_t *out_len,
                          uint8_t flg, const uint8_t *extra, size_t extra_len)
{
    unsigned char *def = NULL;
    size_t def_len = 0;
    LodePNGCompressSettings cs;
    lodepng_compress_settings_init(&cs);
    cs.btype = 2;
    if (lodepng_deflate(&def, &def_len, plain, plain_len, &cs) != 0) return NULL;

    size_t head = 10u + extra_len + 8u;
    uint8_t *g = malloc(head + def_len);
    if (!g) { free(def); return NULL; }

    size_t p = 0;
    g[p++] = 0x1F;
    g[p++] = 0x8B;
    g[p++] = 8;          /* CM = deflate */
    g[p++] = flg;
    g[p++] = 0;          /* MTIME x4 */
    g[p++] = 0;
    g[p++] = 0;
    g[p++] = 0;
    g[p++] = 0;          /* XFL */
    g[p++] = 0;          /* OS */
    memcpy(g + p, extra, extra_len);
    p += extra_len;
    memcpy(g + p, def, def_len);
    p += def_len;
    free(def);

    uint32_t crc = lodepng_crc32(plain, plain_len);
    g[p++] = (uint8_t)(crc & 0xFF);
    g[p++] = (uint8_t)((crc >> 8) & 0xFF);
    g[p++] = (uint8_t)((crc >> 16) & 0xFF);
    g[p++] = (uint8_t)((crc >> 24) & 0xFF);
    uint32_t isize = (uint32_t)plain_len;
    g[p++] = (uint8_t)(isize & 0xFF);
    g[p++] = (uint8_t)((isize >> 8) & 0xFF);
    g[p++] = (uint8_t)((isize >> 16) & 0xFF);
    g[p++] = (uint8_t)((isize >> 24) & 0xFF);

    *out_len = p;
    return g;
}

/* 以若干小块喂入,验证 chunk 边界处理(小块覆盖跨 gzip 头)。 */
static void run_pipe(pokedex_stream_t *s, const uint8_t *gz, size_t gz_len,
                     size_t chunk, sink_buf_t *out)
{
    CHECK(pokedex_stream_open(s, sink_cb, out));
    size_t pos = 0;
    while (pos < gz_len) {
        size_t n = gz_len - pos;
        if (n > chunk) n = chunk;
        CHECK(pokedex_stream_feed(s, gz + pos, n));
        pos += n;
    }
    CHECK(pokedex_stream_close(s));
}

static void test_roundtrip_big(void)
{
    /* >32KB 输入:强制走多次 HAS_MORE_OUTPUT / 环复用;数据需可压缩。 */
    size_t n = 200000;
    uint8_t *plain = malloc(n);
    for (size_t i = 0; i < n; i++) plain[i] = (uint8_t)((i / 512) % 251);

    size_t gz_len = 0;
    uint8_t *gz = make_gzip(plain, n, &gz_len, 0, NULL, 0);
    CHECK(gz != NULL);
    if (!gz) { free(plain); return; }

    pokedex_stream_t s;
    sink_buf_t out = { NULL, 0, 0 };
    run_pipe(&s, gz, gz_len, 4096, &out);
    CHECK(out.len == n);
    CHECK(memcmp(out.buf, plain, n) == 0);
    CHECK(s.fail_reason == 0);

    pokedex_stream_deinit(&s);
    free(out.buf);
    free(gz);
    free(plain);
}

static void test_chunked_small(void)
{
    /* 小文档 + 1 字节/块喂入,覆盖 gzip 头跨块。 */
    const char *plain = "tiny gzip document for chunk tests";
    size_t plain_len = strlen(plain);

    size_t gz_len = 0;
    uint8_t *gz = make_gzip((const uint8_t *)plain, plain_len, &gz_len, 0,
                            NULL, 0);
    CHECK(gz != NULL);

    pokedex_stream_t s;
    sink_buf_t out = { NULL, 0, 0 };
    run_pipe(&s, gz, gz_len, 1, &out);
    CHECK(out.len == plain_len);
    CHECK(memcmp(out.buf, plain, plain_len) == 0);

    pokedex_stream_deinit(&s);
    free(out.buf);
    free(gz);
}

static void test_header_flg_fname(void)
{
    /* FEXTRA(0x04) + FNAME(0x08):extra 布局 = XLEN(2 小端)+ 数据 + NUL 名。 */
    const char *plain = "hello gzip header";
    size_t plain_len = strlen(plain);
    uint8_t extra[9] = { 0x02, 0x00, 'a', 'b', 'n', 'a', 'm', 'e', 0 };
    uint8_t flg = 0x04 | 0x08;

    size_t gz_len = 0;
    uint8_t *gz = make_gzip((const uint8_t *)plain, plain_len, &gz_len, flg,
                            extra, sizeof(extra));
    CHECK(gz != NULL);

    size_t off = 0;
    CHECK(pokedex_stream_header_skip(gz, gz_len, &off));
    CHECK(off == 19); /* 10 固定 + XLEN(2+2) + FNAME(5) */

    pokedex_stream_t s;
    sink_buf_t out = { NULL, 0, 0 };
    run_pipe(&s, gz, gz_len, 5, &out); /* 5B/块跨 FEXTRA/FNAME 边界 */
    CHECK(out.len == plain_len);
    CHECK(memcmp(out.buf, plain, plain_len) == 0);

    pokedex_stream_deinit(&s);
    free(out.buf);
    free(gz);

    /* 坏头。 */
    CHECK(!pokedex_stream_header_skip((const uint8_t *)"XX", 2, &off));
    CHECK(!pokedex_stream_header_skip((const uint8_t *)"\x1f\x8b\x08", 3, &off));
}

static void test_truncated_and_junk(void)
{
    const char *plain = "truncate me please";
    size_t plain_len = strlen(plain);
    size_t gz_len = 0;
    uint8_t *gz = make_gzip((const uint8_t *)plain, plain_len, &gz_len, 0,
                            NULL, 0);
    CHECK(gz != NULL);

    /* 只喂一半:close 应判定截断。 */
    pokedex_stream_t s;
    sink_buf_t out = { NULL, 0, 0 };
    CHECK(pokedex_stream_open(&s, sink_cb, &out));
    CHECK(pokedex_stream_feed(&s, gz, gz_len / 2));
    CHECK(!pokedex_stream_close(&s));
    /* 截断的 deflate 可能被 tinfl 判为"需要更多输入"(4)或直接失败(5)。 */
    CHECK(s.fail_reason == 4 || s.fail_reason == 5);
    pokedex_stream_deinit(&s);
    free(out.buf);

    /* 非 gzip 输入:头暂存 24 字节后仍无法解析 = 判死。 */
    const uint8_t junk[24] = { 0x1f, 0x8b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                               0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
                               0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    CHECK(pokedex_stream_open(&s, sink_cb, &out));
    CHECK(!pokedex_stream_feed(&s, junk, sizeof(junk)));
    CHECK(s.fail_reason == 2);
    pokedex_stream_deinit(&s);
    free(out.buf);

    free(gz);
}

int main(void)
{
    test_roundtrip_big();
    test_chunked_small();
    test_header_flg_fname();
    test_truncated_and_junk();

    if (s_failures) {
        fprintf(stderr, "pokedex_stream: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_stream: all checks passed\n");
    return 0;
}