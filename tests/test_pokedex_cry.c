// tests/test_pokedex_cry.c —— 宿主机单测:叫声 blob 头/TOC/包流。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pokedex_cry.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                                    \
        }                                                                    \
    } while (0)

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void test_header_reject(void)
{
    uint8_t tiny[8] = {0};
    pokedex_cry_header_t h;

    CHECK(!pokedex_cry_header_ok(NULL, 64, &h));
    CHECK(!pokedex_cry_header_ok(tiny, sizeof(tiny), &h));
}

static void test_roundtrip_packets(void)
{
    /* 最小合法 blob:1025 个 TOC,只有 id=1 有两包,其余 length=0。 */
    const size_t toc = (size_t)POKEDEX_DEX_SIZE * POKEDEX_CRY_TOC_ITEM;
    uint8_t pkt_a[3] = {1, 2, 3};
    uint8_t pkt_b[1] = {9};
    const size_t clip_len = 2 + sizeof(pkt_a) + 2 + sizeof(pkt_b);
    uint8_t blob[POKEDEX_CRY_HEADER_SIZE + 1025 * 8 + 16];
    pokedex_cry_header_t h;
    pokedex_cry_toc_ent_t ent;
    const uint8_t *pkt = NULL;
    uint16_t n = 0;
    size_t cur = 0;
    const uint8_t *clip;
    size_t payload;

    memset(blob, 0, sizeof(blob));
    wr32(blob + 0, POKEDEX_CRY_MAGIC);
    wr32(blob + 4, POKEDEX_DEX_SIZE);
    wr32(blob + 8, POKEDEX_CRY_SAMPLE_HZ);
    wr16(blob + 12, POKEDEX_CRY_FRAME_MS);
    wr16(blob + 14, 0);

    wr32(blob + POKEDEX_CRY_HEADER_SIZE + 0, 0);
    wr32(blob + POKEDEX_CRY_HEADER_SIZE + 4, (uint32_t)clip_len);

    payload = POKEDEX_CRY_HEADER_SIZE + toc;
    wr16(blob + payload, (uint16_t)sizeof(pkt_a));
    memcpy(blob + payload + 2, pkt_a, sizeof(pkt_a));
    wr16(blob + payload + 2 + sizeof(pkt_a), (uint16_t)sizeof(pkt_b));
    memcpy(blob + payload + 2 + sizeof(pkt_a) + 2, pkt_b, sizeof(pkt_b));

    CHECK(pokedex_cry_header_ok(blob, payload + clip_len, &h));
    CHECK(h.count == POKEDEX_DEX_SIZE);
    CHECK(h.sample_hz == 16000);
    CHECK(pokedex_cry_payload_off(blob, payload + clip_len) == payload);

    CHECK(pokedex_cry_lookup(blob, payload + clip_len, 1, &ent));
    CHECK(ent.offset == 0 && ent.length == clip_len);
    CHECK(pokedex_cry_lookup(blob, payload + clip_len, 2, &ent));
    CHECK(ent.length == 0);
    CHECK(!pokedex_cry_lookup(blob, payload + clip_len, 0, &ent));
    CHECK(!pokedex_cry_lookup(blob, payload + clip_len, 1026, &ent));

    /* 固件只把 TOC 留在 RAM,payload 在分区上:用 storage_len 核对。 */
    CHECK(pokedex_cry_lookup_storage(blob, payload, payload + clip_len, 1, &ent));
    CHECK(ent.offset == 0 && ent.length == clip_len);
    CHECK(!pokedex_cry_lookup(blob, payload, 1, &ent));

    clip = blob + payload + ent.offset;
    /* re-lookup id 1 */
    CHECK(pokedex_cry_lookup(blob, payload + clip_len, 1, &ent));
    clip = blob + payload + ent.offset;
    CHECK(pokedex_cry_next_packet(clip, ent.length, &cur, &pkt, &n));
    CHECK(n == 3 && pkt[0] == 1 && pkt[2] == 3);
    CHECK(pokedex_cry_next_packet(clip, ent.length, &cur, &pkt, &n));
    CHECK(n == 1 && pkt[0] == 9);
    CHECK(!pokedex_cry_next_packet(clip, ent.length, &cur, &pkt, &n));

    /* 损坏:声明长度超过 blob */
    wr32(blob + POKEDEX_CRY_HEADER_SIZE + 4, 0x100000u);
    CHECK(!pokedex_cry_lookup(blob, payload + clip_len, 1, &ent));
}

static void test_bad_packet(void)
{
    uint8_t clip[4] = {3, 0, 1, 2}; /* len=3 但只剩 2 字节 */
    const uint8_t *pkt = NULL;
    uint16_t n = 0;
    size_t cur = 0;
    uint8_t zclip[2] = {0, 0};

    CHECK(!pokedex_cry_next_packet(clip, sizeof(clip), &cur, &pkt, &n));
    cur = 0;
    CHECK(!pokedex_cry_next_packet(zclip, sizeof(zclip), &cur, &pkt, &n));
}

int main(void)
{
    test_header_reject();
    test_roundtrip_packets();
    test_bad_packet();
    if (s_failures) {
        fprintf(stderr, "pokedex_cry: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("pokedex_cry: all checks passed\n");
    return 0;
}
