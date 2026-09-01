// main/pokedex_cry.c —— 叫声 blob 解析。无堆分配,可在宿主机与固件共用。
#include "pokedex_cry.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t toc_bytes(void)
{
    return (size_t)POKEDEX_DEX_SIZE * POKEDEX_CRY_TOC_ITEM;
}

bool pokedex_cry_header_ok(const uint8_t *blob, size_t blob_len,
                           pokedex_cry_header_t *out)
{
    pokedex_cry_header_t h;

    if (!blob || blob_len < POKEDEX_CRY_HEADER_SIZE) return false;
    h.magic = le32(blob);
    h.count = le32(blob + 4);
    h.sample_hz = le32(blob + 8);
    h.frame_ms = le16(blob + 12);
    h.reserved = le16(blob + 14);
    if (h.magic != POKEDEX_CRY_MAGIC) return false;
    if (h.count != POKEDEX_DEX_SIZE) return false;
    if (h.sample_hz != POKEDEX_CRY_SAMPLE_HZ) return false;
    if (h.frame_ms != POKEDEX_CRY_FRAME_MS) return false;
    if (blob_len < POKEDEX_CRY_HEADER_SIZE + toc_bytes()) return false;
    if (out) *out = h;
    return true;
}

size_t pokedex_cry_payload_off(const uint8_t *blob, size_t blob_len)
{
    if (!pokedex_cry_header_ok(blob, blob_len, NULL)) return 0;
    return POKEDEX_CRY_HEADER_SIZE + toc_bytes();
}

bool pokedex_cry_lookup_storage(const uint8_t *blob, size_t blob_len,
                                size_t storage_len, uint32_t id,
                                pokedex_cry_toc_ent_t *out)
{
    size_t payload;
    size_t toc_at;
    pokedex_cry_toc_ent_t ent;

    if (!out) return false;
    if (!pokedex_id_in_range(id)) return false;
    payload = pokedex_cry_payload_off(blob, blob_len);
    if (payload == 0) return false;

    toc_at = POKEDEX_CRY_HEADER_SIZE + (size_t)(id - 1u) * POKEDEX_CRY_TOC_ITEM;
    ent.offset = le32(blob + toc_at);
    ent.length = le32(blob + toc_at + 4);
    if (storage_len < payload) return false;
    if (ent.offset > storage_len - payload) return false;
    if ((uint64_t)payload + ent.offset + ent.length > storage_len) return false;
    *out = ent;
    return true;
}

bool pokedex_cry_lookup(const uint8_t *blob, size_t blob_len, uint32_t id,
                        pokedex_cry_toc_ent_t *out)
{
    return pokedex_cry_lookup_storage(blob, blob_len, blob_len, id, out);
}

bool pokedex_cry_next_packet(const uint8_t *clip, size_t clip_len,
                             size_t *cursor, const uint8_t **pkt,
                             uint16_t *pkt_len)
{
    size_t cur;
    uint16_t n;

    if (!clip || !cursor || !pkt || !pkt_len) return false;
    cur = *cursor;
    if (cur >= clip_len) return false;
    if (clip_len - cur < 2) return false;
    n = le16(clip + cur);
    if (n == 0) return false;
    if ((size_t)n > clip_len - cur - 2) return false;
    *pkt = clip + cur + 2;
    *pkt_len = n;
    *cursor = cur + 2 + (size_t)n;
    return true;
}
