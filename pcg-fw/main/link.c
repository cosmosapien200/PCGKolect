#include "link.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link";

#define LINK_PORT        UART_NUM_1
#define LINK_TX_PIN      17
#define LINK_RX_PIN      18
#define LINK_RX_BUFFER   (16 * 1024)
#define LINK_TX_BUFFER   (8 * 1024)
#define LINK_RX_TASK_STACK 4096
#define LINK_RX_TASK_PRIO  7       /* above audio consumers: it only parses */
#define LINK_MAX_PENDING   4

typedef struct {
    bool              used;
    bool              done;
    bool              is_chunk;
    uint32_t          id;
    SemaphoreHandle_t sem;
    cJSON            *rsp;         /* RSP frame (also the error answer for reads) */
    uint8_t          *buf;         /* CHUNK destination */
    size_t            cap;
    size_t            got;
} pending_t;

static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_pend_lock;
static pending_t         s_pend[LINK_MAX_PENDING];
static link_stats_t      s_stats;
static link_event_cb_t   s_evt_cb;
static link_chunk_cb_t   s_chunk_cb;
static uint32_t          s_next_id = 1;
static uint8_t           s_msg_seq;
static uint32_t          s_baud = LINK_BAUD_DEFAULT;
static volatile bool     s_up;
static volatile int64_t  s_last_rx_us;      /* last valid message from the RV */

/* RX frame buffer: header(5..6) + payload + crc */
static uint8_t s_rx[LINK_MAX_PAYLOAD + 16];
static uint8_t s_txmsg[LINK_MAX_PAYLOAD + 16];

uint16_t link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint32_t link_next_id(void)
{
    xSemaphoreTake(s_pend_lock, portMAX_DELAY);
    uint32_t id = s_next_id++;
    if (s_next_id == 0) {
        s_next_id = 1;
    }
    xSemaphoreGive(s_pend_lock);
    return id;
}

/* =========================
   TX
   ========================= */

esp_err_t link_write_frame(const uint8_t *frame, size_t len)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int n = uart_write_bytes(LINK_PORT, frame, len);
    xSemaphoreGive(s_tx_lock);
    if (n != (int)len) {
        s_stats.write_errors++;
        return ESP_FAIL;
    }
    s_stats.audio_frames_tx++;
    return ESP_OK;
}

esp_err_t link_send_msg(uint8_t type, const void *data, size_t len)
{
    if (len > LINK_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint8_t *f = s_txmsg;
    f[0] = LINK_SYNC_0;
    f[1] = LINK_SYNC_MSG;
    f[2] = (uint8_t)(len & 0xFF);
    f[3] = (uint8_t)(len >> 8);
    f[4] = type;
    f[5] = s_msg_seq++;
    memcpy(&f[6], data, len);
    uint16_t crc = link_crc16(&f[2], 4 + len);
    f[6 + len] = (uint8_t)(crc & 0xFF);
    f[7 + len] = (uint8_t)(crc >> 8);
    int n = uart_write_bytes(LINK_PORT, f, 8 + len);
    xSemaphoreGive(s_tx_lock);
    if (n != (int)(8 + len)) {
        s_stats.write_errors++;
        return ESP_FAIL;
    }
    s_stats.msg_tx++;
    return ESP_OK;
}

/* =========================
   PENDING REQUESTS
   ========================= */

static pending_t *pend_alloc(uint32_t id, bool is_chunk, uint8_t *buf, size_t cap)
{
    pending_t *p = NULL;
    xSemaphoreTake(s_pend_lock, portMAX_DELAY);
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        if (!s_pend[i].used) {
            p = &s_pend[i];
            p->used = true;
            p->done = false;
            p->is_chunk = is_chunk;
            p->id = id;
            p->rsp = NULL;
            p->buf = buf;
            p->cap = cap;
            p->got = 0;
            xSemaphoreTake(p->sem, 0);      /* clear a stale give */
            break;
        }
    }
    xSemaphoreGive(s_pend_lock);
    return p;
}

static void pend_free(pending_t *p)
{
    xSemaphoreTake(s_pend_lock, portMAX_DELAY);
    if (p->rsp) {
        cJSON_Delete(p->rsp);
        p->rsp = NULL;
    }
    p->used = false;
    xSemaphoreGive(s_pend_lock);
}

static void deliver_rsp(const char *json, size_t len)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (o == NULL) {
        return;
    }
    const cJSON *idj = cJSON_GetObjectItem(o, "id");
    uint32_t id = cJSON_IsNumber(idj) ? (uint32_t)idj->valuedouble : 0;

    xSemaphoreTake(s_pend_lock, portMAX_DELAY);
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        pending_t *p = &s_pend[i];
        if (p->used && !p->done && p->id == id) {
            p->rsp = o;
            p->done = true;
            o = NULL;
            xSemaphoreGive(p->sem);
            break;
        }
    }
    xSemaphoreGive(s_pend_lock);
    if (o) {
        cJSON_Delete(o);    /* late duplicate of a completed request */
    }
}

static void deliver_chunk(const uint8_t *d, size_t len)
{
    if (len < 8) {
        return;
    }
    uint32_t id = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    uint32_t off = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    bool taken = false;

    xSemaphoreTake(s_pend_lock, portMAX_DELAY);
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        pending_t *p = &s_pend[i];
        if (p->used && !p->done && p->is_chunk && p->id == id) {
            size_t n = len - 8;
            if (n > p->cap) {
                n = p->cap;
            }
            memcpy(p->buf, d + 8, n);
            p->got = n;
            p->done = true;
            taken = true;
            xSemaphoreGive(p->sem);
            break;
        }
    }
    xSemaphoreGive(s_pend_lock);

    if (!taken && s_chunk_cb) {
        s_chunk_cb(id, off, d + 8, len - 8);
    }
}

/* =========================
   RX
   ========================= */

static void handle_msg(uint8_t type, const uint8_t *data, size_t len)
{
    s_stats.msg_rx++;
    s_last_rx_us = esp_timer_get_time();
    switch (type) {
    case LINK_T_RSP:
        deliver_rsp((const char *)data, len);
        break;
    case LINK_T_EVT:
        if (s_evt_cb) {
            s_evt_cb((const char *)data, len);
        }
        break;
    case LINK_T_CHUNK:
        deliver_chunk(data, len);
        break;
    default:
        break;
    }
}

static void link_rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[1024];
    enum { S_SYNC0, S_SYNC1, S_HDR, S_BODY } st = S_SYNC0;
    bool is_msg = false;
    size_t idx = 0, need = 0;

    for (;;) {
        int n = uart_read_bytes(LINK_PORT, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0) {
            continue;
        }
        s_stats.rx_bytes += (uint32_t)n;

        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            switch (st) {
            case S_SYNC0:
                if (b == LINK_SYNC_0) {
                    st = S_SYNC1;
                }
                break;
            case S_SYNC1:
                if (b == LINK_SYNC_MSG || b == LINK_SYNC_AUDIO) {
                    is_msg = (b == LINK_SYNC_MSG);
                    s_rx[0] = LINK_SYNC_0;
                    s_rx[1] = b;
                    idx = 2;
                    st = S_HDR;
                } else if (b != LINK_SYNC_0) {
                    st = S_SYNC0;
                }
                break;
            case S_HDR:
                s_rx[idx++] = b;
                if (idx == (is_msg ? 6u : 5u)) {
                    size_t len = (size_t)s_rx[2] | ((size_t)s_rx[3] << 8);
                    if (len > LINK_MAX_PAYLOAD) {
                        s_stats.rx_bad_len++;
                        st = S_SYNC0;
                        break;
                    }
                    need = idx + len + 2;
                    st = S_BODY;
                }
                break;
            case S_BODY:
                s_rx[idx++] = b;
                if (idx == need) {
                    size_t hdr = is_msg ? 6 : 5;
                    size_t len = need - hdr - 2;
                    uint16_t rx_crc = (uint16_t)s_rx[need - 2] | ((uint16_t)s_rx[need - 1] << 8);
                    if (link_crc16(&s_rx[2], hdr - 2 + len) == rx_crc) {
                        if (is_msg) {
                            handle_msg(s_rx[4], &s_rx[6], len);
                        }
                    } else {
                        s_stats.rx_crc_errors++;
                    }
                    st = S_SYNC0;
                }
                break;
            }
        }
    }
}

/* =========================
   REQUESTS
   ========================= */

static esp_err_t send_json(cJSON *req)
{
    char *txt = cJSON_PrintUnformatted(req);
    if (txt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = link_send_msg(LINK_T_REQ, txt, strlen(txt));
    cJSON_free(txt);
    return r;
}

esp_err_t link_request(cJSON *req, int timeout_ms, int attempts, cJSON **rsp)
{
    *rsp = NULL;
    uint32_t id = link_next_id();
    cJSON_DeleteItemFromObject(req, "v");
    cJSON_DeleteItemFromObject(req, "id");
    cJSON_AddNumberToObject(req, "v", 1);
    cJSON_AddNumberToObject(req, "id", id);

    pending_t *p = pend_alloc(id, false, NULL, 0);
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int a = 0; a < attempts; a++) {
        if (a > 0) {
            s_stats.req_retries++;
        }
        if (send_json(req) != ESP_OK) {
            continue;
        }
        if (xSemaphoreTake(p->sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            xSemaphoreTake(s_pend_lock, portMAX_DELAY);
            *rsp = p->rsp;
            p->rsp = NULL;
            xSemaphoreGive(s_pend_lock);
            ret = ESP_OK;
            break;
        }
    }
    if (ret == ESP_ERR_TIMEOUT) {
        s_stats.req_timeouts++;
    }
    pend_free(p);
    return ret;
}

esp_err_t link_request_op(const char *op, int timeout_ms, cJSON **rsp)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", op);
    esp_err_t r = link_request(req, timeout_ms, 3, rsp);
    cJSON_Delete(req);
    return r;
}

esp_err_t link_read(uint32_t handle, uint32_t offset, uint32_t len,
                    uint8_t *buf, size_t *got, cJSON **rsp_err)
{
    *got = 0;
    *rsp_err = NULL;
    if (len > LINK_CHUNK_MAX) {
        len = LINK_CHUNK_MAX;
    }
    uint32_t id = link_next_id();
    char txt[112];
    int tl = snprintf(txt, sizeof(txt),
                      "{\"v\":1,\"id\":%lu,\"op\":\"read\",\"handle\":%lu,\"offset\":%lu,\"len\":%lu}",
                      (unsigned long)id, (unsigned long)handle, (unsigned long)offset, (unsigned long)len);

    pending_t *p = pend_alloc(id, true, buf, len);
    if (p == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_ERR_TIMEOUT;
    for (int a = 0; a < 4; a++) {
        if (a > 0) {
            s_stats.req_retries++;
        }
        link_send_msg(LINK_T_REQ, txt, (size_t)tl);
        if (xSemaphoreTake(p->sem, pdMS_TO_TICKS(250)) == pdTRUE) {
            xSemaphoreTake(s_pend_lock, portMAX_DELAY);
            if (p->rsp) {                 /* RV answered with an error RSP */
                *rsp_err = p->rsp;
                p->rsp = NULL;
                ret = ESP_FAIL;
            } else {
                *got = p->got;
                ret = ESP_OK;
            }
            xSemaphoreGive(s_pend_lock);
            break;
        }
    }
    if (ret == ESP_ERR_TIMEOUT) {
        s_stats.req_timeouts++;
    }
    pend_free(p);
    return ret;
}

/* RSP frames for chunk reads only arrive on error; let deliver_rsp match them too. */

esp_err_t link_set_baud(uint32_t baud)
{
    uint32_t old = s_baud;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "baud");
    cJSON_AddNumberToObject(req, "baud", baud);
    cJSON *rsp = NULL;
    esp_err_t r = link_request(req, 400, 3, &rsp);
    cJSON_Delete(req);
    if (r != ESP_OK) {
        return r;
    }
    const cJSON *st = cJSON_GetObjectItem(rsp, "st");
    bool ok = cJSON_IsString(st) && strcmp(st->valuestring, "OK") == 0;
    cJSON_Delete(rsp);
    if (!ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* RV switches after its response is on the wire; follow it. */
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_wait_tx_done(LINK_PORT, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(30));
    uart_set_baudrate(LINK_PORT, baud);
    /* No uart_flush_input(): it waits on the driver RX mutex that the
     * higher-priority link_rx task re-takes continuously (starvation). The
     * parser resynchronises on garbage by itself. */
    s_baud = baud;
    s_stats.baud = baud;
    xSemaphoreGive(s_tx_lock);

    /* Confirm both directions at the new rate; otherwise fall back (the RV
     * reverts by itself when it hears no valid frame for 2 s). */
    vTaskDelay(pdMS_TO_TICKS(50));
    cJSON *pong = NULL;
    if (link_request_op("ping", 200, &pong) == ESP_OK) {
        cJSON_Delete(pong);
        ESP_LOGI(TAG, "baud %lu confirmed", (unsigned long)baud);
        return ESP_OK;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_set_baudrate(LINK_PORT, old);
    s_baud = old;
    s_stats.baud = old;
    xSemaphoreGive(s_tx_lock);
    ESP_LOGW(TAG, "baud %lu failed, back to %lu", (unsigned long)baud, (unsigned long)old);
    vTaskDelay(pdMS_TO_TICKS(2500));    /* let the RV revert too */
    return ESP_FAIL;
}

bool link_is_up(void) { return s_up; }

static void set_local_baud(uint32_t baud)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_wait_tx_done(LINK_PORT, pdMS_TO_TICKS(100));
    uart_set_baudrate(LINK_PORT, baud);
    s_baud = baud;
    s_stats.baud = baud;
    xSemaphoreGive(s_tx_lock);
}

/*
 * Link supervisor: both ends boot at LINK_BAUD_DEFAULT. When the RV answers,
 * negotiate LINK_BAUD_TARGET. If the RV goes silent (it falls back to the
 * default after 3 s without valid frames, e.g. after its own restart), drop to
 * the default here as well and negotiate again.
 */
static void link_supervisor_task(void *arg)
{
    (void)arg;
    int fails = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_up && esp_timer_get_time() - s_last_rx_us < 1500000) {
            fails = 0;
            continue;                       /* traffic proves the link is alive */
        }
        cJSON *rsp = NULL;
        if (link_request_op("ping", 300, &rsp) == ESP_OK) {
            cJSON_Delete(rsp);
            fails = 0;
            if (!s_up) {
                ESP_LOGI(TAG, "RV link up @ %lu", (unsigned long)s_baud);
            }
            s_up = true;
            if (s_baud != LINK_BAUD_TARGET) {
                link_set_baud(LINK_BAUD_TARGET);
            }
            continue;
        }
        if (++fails >= 2) {
            if (s_up) {
                s_stats.link_downs++;
                ESP_LOGW(TAG, "RV link down");
            }
            s_up = false;
            if (s_baud != LINK_BAUD_DEFAULT) {
                vTaskDelay(pdMS_TO_TICKS(3500));    /* let the RV fall back first */
                set_local_baud(LINK_BAUD_DEFAULT);
            }
        }
    }
}

void link_set_event_cb(link_event_cb_t cb) { s_evt_cb = cb; }
void link_set_chunk_cb(link_chunk_cb_t cb) { s_chunk_cb = cb; }

void link_get_stats(link_stats_t *out)
{
    *out = s_stats;
}

/* =========================
   INIT
   ========================= */

static bool link_selftest(void)
{
    if (link_crc16((const uint8_t *)"123456789", 9) != 0x29B1) {
        return false;
    }
    /* message vector: TYPE REQ, SEQ 0, payload "{}" */
    const uint8_t hdr[] = { 0x02, 0x00, 0x01, 0x00, '{', '}' };
    uint16_t c = link_crc16(hdr, sizeof(hdr));
    ESP_LOGI(TAG, "msg test vector: A5 5C 02 00 01 00 7B 7D %02X %02X", c & 0xFF, c >> 8);
    return true;
}

esp_err_t link_init(void)
{
    /* Random id base per boot: the RV caches responses by id, so ids must not
     * repeat across ESP resets. */
    s_next_id = (esp_random() & 0x3FFFFFFF) | 1;
    s_tx_lock = xSemaphoreCreateMutex();
    s_pend_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < LINK_MAX_PENDING; i++) {
        s_pend[i].sem = xSemaphoreCreateBinary();
    }

    uart_config_t cfg = {
        .baud_rate = LINK_BAUD_DEFAULT,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_PORT, LINK_RX_BUFFER, LINK_TX_BUFFER, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_PORT, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_stats.baud = LINK_BAUD_DEFAULT;

    ESP_LOGI(TAG, "self-test: %s", link_selftest() ? "PASS" : "FAIL");

    if (xTaskCreate(link_rx_task, "link_rx", LINK_RX_TASK_STACK, NULL, LINK_RX_TASK_PRIO, NULL) != pdPASS ||
        xTaskCreate(link_supervisor_task, "link_sup", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d @ %d", LINK_PORT, LINK_TX_PIN, LINK_RX_PIN, LINK_BAUD_DEFAULT);
    return ESP_OK;
}
