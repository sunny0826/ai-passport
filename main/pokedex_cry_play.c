// main/pokedex_cry_play.c —— cryfs 上的 Opus 8 kbps / 16 kHz 流式播放。
// 不把整段 PCM 摊进 RAM:每次只读一个长度前缀包,解码 20 ms,再 bsp_audio_write。
#include "pokedex_cry_play.h"
#include "pokedex_cry.h"
#include "bsp_audio.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_opus_dec.h"
#include "esp_audio_dec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define CRY_TASK_STACK 16384
#define CRY_TASK_PRIO  5
#define CRY_PKT_MAX    512
#define CRY_PCM_MAX    1280 /* 20 ms @ 16 kHz 16-bit mono = 640; 留一倍余量 */

static const char *TAG = "pokedex_cry";

static const esp_partition_t *s_part;
static uint8_t s_head_toc[POKEDEX_CRY_HEADER_SIZE +
                          POKEDEX_DEX_SIZE * POKEDEX_CRY_TOC_ITEM];
static size_t s_blob_cap;
static bool s_ready;

static TaskHandle_t s_task;
static volatile uint32_t s_req_id; /* 0=空闲 */
static volatile bool s_stop;

static bool load_toc(void)
{
    pokedex_cry_header_t hdr;
    esp_err_t err;

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY, "cryfs");
    if (!s_part) {
        ESP_LOGW(TAG, "cryfs partition missing");
        return false;
    }
    s_blob_cap = s_part->size;
    err = esp_partition_read(s_part, 0, s_head_toc, sizeof(s_head_toc));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cryfs read: %s", esp_err_to_name(err));
        return false;
    }
    if (!pokedex_cry_header_ok(s_head_toc, sizeof(s_head_toc), &hdr)) {
        ESP_LOGE(TAG, "cryfs header rejected");
        return false;
    }
    ESP_LOGI(TAG, "cryfs ready, %u cries, part %u bytes",
             (unsigned)hdr.count, (unsigned)s_part->size);
    return true;
}

static bool flash_read(size_t off, void *dst, size_t n)
{
    if (!s_part || off + n > s_blob_cap) return false;
    return esp_partition_read(s_part, off, dst, n) == ESP_OK;
}

static void play_id(uint32_t id)
{
    pokedex_cry_toc_ent_t ent;
    size_t payload;
    size_t abs_off;
    size_t cursor = 0;
    void *dec = NULL;
    uint8_t pkt[CRY_PKT_MAX];
    int16_t pcm[CRY_PCM_MAX / 2];
    esp_opus_dec_cfg_t cfg = {
        .sample_rate = POKEDEX_CRY_SAMPLE_HZ,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };

    if (!pokedex_cry_lookup_storage(s_head_toc, sizeof(s_head_toc),
                                    s_blob_cap, id, &ent)) {
        ESP_LOGW(TAG, "no toc for %u", (unsigned)id);
        return;
    }
    if (ent.length == 0) return;

    payload = pokedex_cry_payload_off(s_head_toc, sizeof(s_head_toc));
    abs_off = payload + ent.offset;

    if (bsp_audio_set_format(POKEDEX_CRY_SAMPLE_HZ, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio format failed");
        return;
    }
    bsp_audio_set_volume(80);

    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "opus open failed");
        return;
    }

    while (!s_stop && cursor < ent.length) {
        uint8_t lenbuf[2];
        uint16_t n;
        size_t pkt_at;
        esp_audio_dec_in_raw_t raw;
        esp_audio_dec_out_frame_t frame;
        esp_audio_dec_info_t info;
        esp_audio_err_t derr;

        if (ent.length - cursor < 2) break;
        if (!flash_read(abs_off + cursor, lenbuf, 2)) break;
        n = (uint16_t)lenbuf[0] | ((uint16_t)lenbuf[1] << 8);
        if (n == 0 || n > CRY_PKT_MAX || (size_t)n > ent.length - cursor - 2) {
            ESP_LOGE(TAG, "bad packet id=%u", (unsigned)id);
            break;
        }
        pkt_at = cursor + 2;
        if (!flash_read(abs_off + pkt_at, pkt, n)) break;
        cursor = pkt_at + n;

        raw.buffer = pkt;
        raw.len = n;
        raw.consumed = 0;
        frame.buffer = (uint8_t *)pcm;
        frame.len = sizeof(pcm);
        derr = esp_opus_dec_decode(dec, &raw, &frame, &info);
        if (derr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGE(TAG, "pcm buffer too small (%u)",
                     (unsigned)frame.needed_size);
            break;
        }
        if (derr != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "opus decode %d", (int)derr);
            break;
        }
        if (frame.decoded_size > 0) {
            bsp_audio_write(pcm, frame.decoded_size);
        }
    }

    esp_opus_dec_close(dec);
}

static void cry_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t id = s_req_id;
        s_req_id = 0;
        s_stop = false;
        if (id == 0 || !s_ready) continue;
        play_id(id);
    }
}

bool pokedex_cry_play_init(void)
{
    if (!s_ready) s_ready = load_toc();
    if (!s_task) {
        if (xTaskCreate(cry_task, "pokedex_cry", CRY_TASK_STACK, NULL,
                        CRY_TASK_PRIO, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "cry task create failed");
            s_task = NULL;
            return false;
        }
    }
    return s_ready;
}

void pokedex_cry_play_request(uint32_t id)
{
    if (!s_ready || !s_task || !pokedex_id_in_range(id)) return;
    s_stop = true;
    s_req_id = id;
    xTaskNotifyGive(s_task);
}

void pokedex_cry_play_stop(void)
{
    s_stop = true;
    s_req_id = 0;
}

void pokedex_cry_play_release(void)
{
    pokedex_cry_play_stop();
}
