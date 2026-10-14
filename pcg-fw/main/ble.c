#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "link.h"

void ble_store_config_init(void);

static const char *TAG = "ble";

#define CMD_MAX        4096
#define TLM_MAX        1024
#define WIFI_MAX       512
#define INFO_MAX       1536
#define TLM_MIN_US     200000      /* <= 5 Hz */
#define CTRL_PROTO     1

/* 7a1c00xx-5b6e-4e8a-9c1f-50434b4f4c54, little-endian */
#define PCG_UUID(x) BLE_UUID128_INIT(0x54, 0x4c, 0x4f, 0x4b, 0x43, 0x50, 0x1f, 0x9c, \
                                     0x8a, 0x4e, 0x6e, 0x5b, (x), 0x00, 0x1c, 0x7a)

static const ble_uuid128_t s_svc_uuid  = PCG_UUID(0x01);
static const ble_uuid128_t s_info_uuid = PCG_UUID(0x02);
static const ble_uuid128_t s_cmd_uuid  = PCG_UUID(0x03);
static const ble_uuid128_t s_rsp_uuid  = PCG_UUID(0x04);
static const ble_uuid128_t s_tlm_uuid  = PCG_UUID(0x05);
static const ble_uuid128_t s_wifi_uuid = PCG_UUID(0x06);

static uint16_t s_rsp_handle, s_tlm_handle, s_wifi_handle, s_info_handle;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_sub_rsp, s_sub_tlm, s_sub_wifi;
static uint8_t s_own_addr_type;
static char s_name[24];
static char s_device_id[24];

/* command reassembly (written only from the NimBLE host task) */
static char s_cmd_buf[CMD_MAX + 1];
static size_t s_cmd_len;
static QueueHandle_t s_cmd_q;          /* char* (heap), consumed by the command task */

static SemaphoreHandle_t s_lock;
static char s_tlm[TLM_MAX];
static bool s_tlm_new;
static char s_wifi[WIFI_MAX] = "{\"state\":\"off\"}";
static char s_info[INFO_MAX] = "{}";
static ble_local_op_t s_local;

static void advertise(void);

const char *ble_device_id(void) { return s_device_id; }
bool ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
void ble_set_local_handler(ble_local_op_t fn) { s_local = fn; }

static bool link_bonded(void)
{
    struct ble_gap_conn_desc d;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(s_conn, &d) != 0) {
        return false;
    }
    return d.sec_state.encrypted && d.sec_state.bonded;
}

/* =========================
   NOTIFY (fragmented)
   ========================= */

static void notify_fragments(uint16_t handle, const char *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    size_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 4 ? mtu - 3 : 20) - 1;
    uint8_t frag[520];
    if (chunk > sizeof(frag) - 1) {
        chunk = sizeof(frag) - 1;
    }
    size_t off = 0;
    uint8_t idx = 0;
    do {
        size_t n = len - off < chunk ? len - off : chunk;
        frag[0] = (uint8_t)((off == 0 ? 0x80 : 0) | (off + n >= len ? 0x40 : 0) | (idx & 0x3F));
        memcpy(frag + 1, data + off, n);
        for (int attempt = 0; attempt < 20; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(frag, n + 1);
            if (om == NULL) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            int rc = ble_gatts_notify_custom(s_conn, handle, om);
            if (rc == 0) {
                break;
            }
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                return;                     /* disconnected */
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        off += n;
        idx++;
    } while (off < len);
}

/* =========================
   GATT
   ========================= */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)arg;
    const ble_uuid_t *u = ctxt->chr->uuid;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *src = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ble_uuid_cmp(u, &s_info_uuid.u) == 0) {
            src = s_info;
        } else if (ble_uuid_cmp(u, &s_wifi_uuid.u) == 0) {
            src = s_wifi;
        } else if (ble_uuid_cmp(u, &s_tlm_uuid.u) == 0) {
            src = s_tlm;
        }
        int rc = src ? os_mbuf_append(ctxt->om, src, strlen(src)) : 0;
        xSemaphoreGive(s_lock);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(u, &s_cmd_uuid.u) == 0) {
        uint8_t frag[520];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, frag, sizeof(frag), &n) != 0 || n < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t hdr = frag[0];
        if (hdr & 0x80) {
            s_cmd_len = 0;
        }
        if (s_cmd_len + (n - 1) > CMD_MAX) {
            s_cmd_len = 0;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        memcpy(s_cmd_buf + s_cmd_len, frag + 1, n - 1);
        s_cmd_len += n - 1;
        if (hdr & 0x40) {
            char *msg = malloc(s_cmd_len + 1);
            if (msg) {
                memcpy(msg, s_cmd_buf, s_cmd_len);
                msg[s_cmd_len] = 0;
                if (xQueueSend(s_cmd_q, &msg, 0) != pdTRUE) {
                    free(msg);              /* queue full: the app times out and retries */
                }
            }
            s_cmd_len = 0;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &s_info_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ,
              .val_handle = &s_info_handle },
            { .uuid = &s_cmd_uuid.u, .access_cb = chr_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &s_rsp_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_rsp_handle },
            { .uuid = &s_tlm_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_tlm_handle },
            { .uuid = &s_wifi_uuid.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_wifi_handle },
            { 0 },
        },
    },
    { 0 },
};

/* =========================
   GAP
   ========================= */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_sub_rsp = s_sub_tlm = s_sub_wifi = false;
        s_cmd_len = 0;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_rsp_handle) {
            s_sub_rsp = ev->subscribe.cur_notify;
        } else if (ev->subscribe.attr_handle == s_tlm_handle) {
            s_sub_tlm = ev->subscribe.cur_notify;
        } else if (ev->subscribe.attr_handle == s_wifi_handle) {
            s_sub_wifi = ev->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption %s", ev->enc_change.status == 0 ? "on" : "failed");
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone lost its bond: drop ours and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields rf = { 0 };     /* service UUID in the scan response */
    rf.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    rf.num_uuids128 = 1;
    rf.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rf);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv start failed %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* =========================
   COMMANDS
   ========================= */

static void add_esp_info(cJSON *o)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char fw[48];
    snprintf(fw, sizeof(fw), "%s", d->version);
    cJSON_DeleteItemFromObject(o, "device_id");
    cJSON_AddStringToObject(o, "device_id", s_device_id);
    cJSON_AddStringToObject(o, "esp_fw", fw);
    cJSON_AddStringToObject(o, "esp_idf", d->idf_ver);
    cJSON_AddBoolToObject(o, "link_up", link_is_up());
    link_stats_t ls;
    link_get_stats(&ls);
    cJSON_AddNumberToObject(o, "link_baud", ls.baud);
}

static void refresh_info(void)
{
    cJSON *rsp = NULL;
    cJSON *info;
    if (link_is_up() && link_request_op("hello", 800, &rsp) == ESP_OK) {
        info = rsp;
        cJSON_DeleteItemFromObject(info, "v");
        cJSON_DeleteItemFromObject(info, "id");
        cJSON_DeleteItemFromObject(info, "st");
    } else {
        info = cJSON_CreateObject();
        cJSON_AddNumberToObject(info, "ctrl_proto", CTRL_PROTO);
        cJSON_AddNumberToObject(info, "link_proto", LINK_PROTO);
    }
    add_esp_info(info);
    char *t = cJSON_PrintUnformatted(info);
    if (t) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strncpy(s_info, t, INFO_MAX - 1);
        xSemaphoreGive(s_lock);
        cJSON_free(t);
    }
    cJSON_Delete(info);
}

static bool is_destructive(const char *op)
{
    return strcmp(op, "delete") == 0 || strcmp(op, "clear") == 0 || strcmp(op, "wifi_set") == 0 ||
           strcmp(op, "wifi_forget") == 0;
}

static int op_timeout_ms(const char *op)
{
    if (strcmp(op, "clear") == 0) {
        return 8000;               /* removes many directories */
    }
    if (strcmp(op, "stop") == 0) {
        return 4000;               /* finalisation hashes the files */
    }
    return 1500;
}

static void reply(cJSON *rsp)
{
    char *t = cJSON_PrintUnformatted(rsp);
    if (t) {
        if (s_sub_rsp) {
            notify_fragments(s_rsp_handle, t, strlen(t));
        }
        cJSON_free(t);
    }
}

static void handle_command(const char *msg)
{
    cJSON *req = cJSON_Parse(msg);
    if (req == NULL) {
        return;                    /* no id to answer */
    }
    const cJSON *idj = cJSON_GetObjectItem(req, "id");
    double app_id = cJSON_IsNumber(idj) ? idj->valuedouble : 0;
    const cJSON *opj = cJSON_GetObjectItem(req, "op");
    const char *op = cJSON_IsString(opj) ? opj->valuestring : "";
    const cJSON *vj = cJSON_GetObjectItem(req, "v");
    bool bonded = link_bonded();

    cJSON *rsp = NULL;
    if (!cJSON_IsNumber(vj) || vj->valuedouble != CTRL_PROTO) {
        rsp = cJSON_CreateObject();
        cJSON_AddStringToObject(rsp, "st", "UNSUPPORTED");
        cJSON_AddStringToObject(rsp, "err", "CTRL_PROTO");
    } else if (is_destructive(op) && !bonded) {
        rsp = cJSON_CreateObject();
        cJSON_AddStringToObject(rsp, "st", "REJECTED");
        cJSON_AddStringToObject(rsp, "err", "NOT_BONDED");
    } else if (strcmp(op, "hello") == 0) {
        refresh_info();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        rsp = cJSON_Parse(s_info);
        xSemaphoreGive(s_lock);
        if (rsp == NULL) {
            rsp = cJSON_CreateObject();
        }
        cJSON_AddStringToObject(rsp, "st", "OK");
        cJSON_AddBoolToObject(rsp, "bonded", bonded);
    } else {
        rsp = cJSON_CreateObject();
        if (s_local && s_local(op, req, rsp, bonded)) {
            if (!cJSON_GetObjectItem(rsp, "st")) {
                cJSON_AddStringToObject(rsp, "st", "OK");
            }
        } else {
            cJSON_Delete(rsp);
            rsp = NULL;
            if (!link_is_up()) {
                rsp = cJSON_CreateObject();
                cJSON_AddStringToObject(rsp, "st", "ERROR");
                cJSON_AddStringToObject(rsp, "err", "LINK_DOWN");
            } else if (link_request(req, op_timeout_ms(op), 3, &rsp) != ESP_OK) {
                rsp = cJSON_CreateObject();
                cJSON_AddStringToObject(rsp, "st", "TIMEOUT");
                cJSON_AddStringToObject(rsp, "err", "RV_TIMEOUT");
            }
        }
    }
    /* answer with the app's id, not the link id */
    cJSON_DeleteItemFromObject(rsp, "v");
    cJSON_DeleteItemFromObject(rsp, "id");
    cJSON_AddNumberToObject(rsp, "v", CTRL_PROTO);
    cJSON_AddNumberToObject(rsp, "id", app_id);
    reply(rsp);
    cJSON_Delete(rsp);
    cJSON_Delete(req);
}

static void cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        char *msg = NULL;
        if (xQueueReceive(s_cmd_q, &msg, portMAX_DELAY) == pdTRUE && msg) {
            handle_command(msg);
            free(msg);
        }
    }
}

/* =========================
   TELEMETRY
   ========================= */

static void on_link_event(const char *json, size_t len)
{
    /* runs on link_rx: copy + flag only */
    if (len >= TLM_MAX - 40) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_tlm, json, len);
    s_tlm[len] = 0;
    s_tlm_new = true;
    xSemaphoreGive(s_lock);
}

static void tlm_task(void *arg)
{
    (void)arg;
    static char out[TLM_MAX + 64];
    int64_t last = 0, last_down = 0;
    bool was_up = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        bool up = link_is_up();
        if (up && !was_up) {
            refresh_info();
        }
        was_up = up;

        if (!up) {
            if (s_sub_tlm && now - last_down > 2000000) {
                last_down = now;
                const char *m = "{\"ev\":\"status\",\"src\":\"esp\",\"state\":\"LINK_DEGRADED\",\"link_up\":false}";
                notify_fragments(s_tlm_handle, m, strlen(m));
            }
            continue;
        }
        if (now - last < TLM_MIN_US) {
            continue;
        }
        bool send = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_tlm_new) {
            s_tlm_new = false;
            /* mark the source: this state was confirmed by the RV */
            size_t n = strlen(s_tlm);
            if (n > 1 && s_tlm[n - 1] == '}') {
                snprintf(out, sizeof(out), "%.*s,\"src\":\"rv\",\"link_up\":true}", (int)(n - 1), s_tlm);
                send = true;
            }
        }
        xSemaphoreGive(s_lock);
        if (send && s_sub_tlm) {
            notify_fragments(s_tlm_handle, out, strlen(out));
            last = now;
        }
    }
}

void ble_notify_wifi(const char *json)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_wifi, json, WIFI_MAX - 1);
    xSemaphoreGive(s_lock);
    if (s_sub_wifi) {
        notify_fragments(s_wifi_handle, json, strlen(json));
    }
}

/* =========================
   INIT
   ========================= */

esp_err_t ble_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(8, sizeof(char *));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "PCGKolect-%02X%02X", mac[4], mac[5]);
    snprintf(s_device_id, sizeof(s_device_id), "pcgk-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_err_t r = nimble_port_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed %s", esp_err_to_name(r));
        return r;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;     /* Just Works, LE Secure Connections */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt setup failed %d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    link_add_event_cb(on_link_event);
    xTaskCreate(cmd_task, "ble_cmd", 6144, NULL, 4, NULL);
    xTaskCreate(tlm_task, "ble_tlm", 4096, NULL, 3, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "device id %s", s_device_id);
    return ESP_OK;
}
