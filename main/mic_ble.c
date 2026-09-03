// mic_ble.c —— 见 mic_ble.h。生命周期与栈收尾方式沿用 main/demo_ble.c 的模式:
// 页面进入时启动、退出时 stop/deinit,不与 Wi-Fi 等其他 radio 栈共存。
#include "mic_ble.h"
#include "mic_model.h"          // mic_gain_clamp

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static int advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static const char *TAG = "mic_ble";
static const char *DEVICE_NAME = "FoloPassport-Mic";

// 自定义 128 位 UUID 基座(FoloToy mic 页专用,无标准服务含义)。
// BLE_UUID128_INIT 按 little-endian 字节序填写,与字符串形式相反:
//   字符串 F0F0xxxx-C332-4E21-B0A0-5A5D3C1E1000
//   → INIT(0x00, 0x10, 0x1E, 0x3C, 0x5D, 0x5A, 0xA0, 0xB0,
//           0x21, 0x4E, 0x32, 0xC3, 0xx, 0x20, 0xF0, 0xF0)
// 128 位 UUID 在广播包里放不下,放进 scan response;工具端按名称连接即可。
static const ble_uuid128_t MIC_SVC_UUID =
    BLE_UUID128_INIT(0x00, 0x10, 0x1e, 0x3c, 0x5d, 0x5a, 0xa0, 0xb0,
                     0x21, 0x4e, 0x32, 0xc3, 0x01, 0x20, 0xf0, 0xf0);
static const ble_uuid128_t MIC_CHR_AUDIO_UUID =
    BLE_UUID128_INIT(0x00, 0x10, 0x1e, 0x3c, 0x5d, 0x5a, 0xa0, 0xb0,
                     0x21, 0x4e, 0x32, 0xc3, 0x02, 0x20, 0xf0, 0xf0);
static const ble_uuid128_t MIC_CHR_GAIN_UUID =
    BLE_UUID128_INIT(0x00, 0x10, 0x1e, 0x3c, 0x5d, 0x5a, 0xa0, 0xb0,
                     0x21, 0x4e, 0x32, 0xc3, 0x03, 0x20, 0xf0, 0xf0);
static const ble_uuid128_t MIC_CHR_INFO_UUID =
    BLE_UUID128_INIT(0x00, 0x10, 0x1e, 0x3c, 0x5d, 0x5a, 0xa0, 0xb0,
                     0x21, 0x4e, 0x32, 0xc3, 0x04, 0x20, 0xf0, 0xf0);

static const char *AUDIO_INFO_TEXT = "pcm;16000;16;1";

// ---------------------------------------------------------------------------
// 状态(仅 host 任务与音频工作线程通过这些小变量交互,均为标量)
// ---------------------------------------------------------------------------

static mic_ble_status_t s_status = MIC_BLE_OFF;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;   // 当前连接句柄
static bool s_subscribed;                           // 客户端已订阅 Audio Data
static uint16_t s_chr_audio_val;                    // GATT 注册后回填的值句柄
static uint8_t s_stream_seq;                        // 推流通知的序号(每连接/订阅重置)
static uint8_t s_gain_mirror = MIC_GAIN_DEFAULT;    // Mic Gain 特征镜像
static mic_ble_gain_cb_t s_gain_cb;
static bool s_initialized;
static bool s_start_requested;
static uint8_t s_addr_type;
static SemaphoreHandle_t s_host_stopped;

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------

// Audio Data 是纯 notify 特征,读写请求按协议不会路由到这里;但 NimBLE 要求
// 每个特征的 access_cb 非 NULL(ble_gatts_chr_is_sane 会拒绝 NULL,报 EINVAL)。
static int on_audio_access(uint16_t conn, uint16_t attr,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static int on_gain_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_gain_mirror, sizeof(s_gain_mirror));
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t v = MIC_GAIN_DEFAULT;
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, &v, sizeof(v), &len) != 0 || len < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_gain_mirror = mic_gain_clamp(v);       // 收敛后再镜像,读到的一定合法
        if (s_gain_cb) s_gain_cb(s_gain_mirror); // 页面负责应用与持久化,勿在此重活
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int on_info_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    return os_mbuf_append(ctxt->om, AUDIO_INFO_TEXT, strlen(AUDIO_INFO_TEXT));
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &MIC_SVC_UUID.u,
        .characteristics = (const struct ble_gatt_chr_def[]) {
            { .uuid = &MIC_CHR_AUDIO_UUID.u,
              .access_cb = on_audio_access,         // 不可为 NULL(见上)
              .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_chr_audio_val },
            { .uuid = &MIC_CHR_GAIN_UUID.u,
              .access_cb = on_gain_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &MIC_CHR_INFO_UUID.u,
              .access_cb = on_info_access,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 }                                    // 特征表结束哨兵
        },
    },
    { 0 },                                           // 服务表结束哨兵
};

// ---------------------------------------------------------------------------
// 广播与 GAP
// ---------------------------------------------------------------------------

static int advertise(void) {
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (const uint8_t *)DEVICE_NAME;
    adv.name_len = strlen(DEVICE_NAME);
    adv.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) return rc;

    // 128 位服务 UUID 放 scan response(21 字节,主包放不下)。
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = &MIC_SVC_UUID;            // 该字段要 ble_uuid128_t*,直接取本体
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) return rc;

    // 可连接广播:30~60ms 间隔,直到被连接或显式停止。
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(60);
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) s_status = s_subscribed ? MIC_BLE_LIVE : MIC_BLE_ADV;
    return rc;
}

int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_status = MIC_BLE_CONN;
            s_stream_seq = 0;                       // 新连接,序号从 0 重新开始
            ESP_LOGI(TAG, "central 已连接 conn=%d", s_conn);
        } else {
            // 连接被拒/失败:重新广播,等待下一次连接。
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        ESP_LOGI(TAG, "central 断开 reason=%d,恢复广播", event->disconnect.reason);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_start_requested) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        // 只关心 Audio Data 特征的 notify 订阅状态。
        if (event->subscribe.attr_handle == s_chr_audio_val) {
            s_subscribed = event->subscribe.cur_notify != 0;
            if (s_subscribed) s_stream_seq = 0;     // 每次订阅,客户端从头对齐序号
            s_status = (s_conn != BLE_HS_CONN_HANDLE_NONE)
                       ? (s_subscribed ? MIC_BLE_LIVE : MIC_BLE_CONN)
                       : MIC_BLE_ADV;
            ESP_LOGI(TAG, "Audio Data 订阅=%d", s_subscribed);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商为 %d(conn=%d)",
                 event->mtu.value, event->mtu.conn_handle);
        return 0;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// host 生命周期(与 demo_ble.c 相同的握手收尾模式)
// ---------------------------------------------------------------------------

static void on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE host 复位 reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0 && s_start_requested) rc = advertise();
    if (rc != 0) {
        s_status = MIC_BLE_OFF;              // 广播没起来就不谎报状态
        ESP_LOGE(TAG, "同步后启动广播失败 rc=%d", rc);
    }
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();               // 返回即 host 已停止
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

esp_err_t mic_ble_start(mic_ble_gain_cb_t gain_cb) {
    if (s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc == 0) rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc == 0) rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc != 0) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
        nimble_port_deinit();
        s_initialized = false;
        ESP_LOGE(TAG, "GATT 服务注册失败 rc=%d", rc);
        return ESP_FAIL;
    }

    s_gain_cb = gain_cb;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    // 状态保持 OFF,直到 on_sync 里广播真正启动才置 ADV,避免"显示在广播其实没起"。
    s_start_requested = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void mic_ble_stop(void) {
    s_start_requested = false;
    if (!s_initialized) return;

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc == 0 && s_host_stopped) {
        // host 回调不碰 LVGL;即使调用方(页面 exit)已持 LVGL 锁也不会成环。
        xSemaphoreTake(s_host_stopped, portMAX_DELAY);
    }
    if (rc == 0) {
        nimble_port_deinit();
        s_initialized = false;
    } else {
        ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
    }
    if (!s_initialized && s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_gain_cb = NULL;
    s_status = MIC_BLE_OFF;
}

// ---------------------------------------------------------------------------
// 对外小接口
// ---------------------------------------------------------------------------

mic_ble_status_t mic_ble_status(void) {
    return s_status;
}

void mic_ble_set_gain(uint8_t pct) {
    s_gain_mirror = mic_gain_clamp(pct);
}

bool mic_ble_stream(const void *pcm, size_t bytes) {
    if (!pcm || !bytes) return false;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed || !s_chr_audio_val) {
        return false;
    }

    const uint8_t *p = pcm;
    // 每条 notify 的有效载荷 = ATT MTU - 3;MTU 未协商时至少剩 20 字节也能发。
    size_t payload = ble_att_mtu(s_conn);
    payload = payload > 3 ? payload - 3 : 20;

    // 关键设计:每条 notify = [seq 1B][整数个 16bit 样本]。
    //   - mbuf 紧张时只可能"整条丢弃",空洞恒为偶数字节 → 客户端听到的只是一声
    //     "咔",不会像按字节硬切(奇数载荷)那样永久错位半样本、之后全是杂音;
    //   - seq 供客户端检测丢包并统计。
    size_t piece = (payload - 1) & ~(size_t)1;   // 扣掉 seq,再按整样本对齐
    if (piece == 0) return false;

    uint8_t frame[513];                          // 517 MTU - 3 - 1 = 513 上限
    while (bytes > 0) {
        size_t n = bytes < piece ? bytes : piece;
        if (n == 0) break;
        frame[0] = s_stream_seq++;
        memcpy(frame + 1, p, n);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, n + 1);
        if (!om) return false;                   // mbuf 池耗尽:整条丢弃,绝不阻塞录音
        if (ble_gattc_notify_custom(s_conn, s_chr_audio_val, om) != 0) {
            ESP_LOGD(TAG, "notify 失败,丢弃 %u 字节", (unsigned)(n + 1));
            return false;
        }
        p += n;
        bytes -= n;
    }
    return true;
}
