#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "ble.h"
#include "http_api.h"

static const char *TAG = "wifi";

#define NVS_NS        "wifi"
#define MAX_RETRY_S   30

typedef enum { W_OFF, W_CONNECTING, W_CONNECTED, W_FAILED } wstate_t;
static const char *const kState[] = {"off", "connecting", "connected", "failed"};

static SemaphoreHandle_t s_lock;
static wstate_t s_state = W_OFF;
static char s_ssid[33];
static char s_ip[16];
static char s_err[32] = "";
static char s_token[33];
static int s_retries;
static esp_netif_t *s_netif;
static bool s_started;

bool wifi_sta_connected(void) { return s_state == W_CONNECTED; }

int wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;
    return (s_state == W_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

void wifi_sta_rotate_token(void)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < 16; i++) {
        snprintf(s_token + 2 * i, 3, "%02x", r[i]);
    }
    xSemaphoreGive(s_lock);
}

bool wifi_sta_token_ok(const char *bearer)
{
    if (bearer == NULL || strlen(bearer) != 32) {
        return false;
    }
    uint8_t diff = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(bearer[i] ^ s_token[i]);
    }
    xSemaphoreGive(s_lock);
    return diff == 0;
}

/* Status JSON. The token is included only for bonded command answers. */
static cJSON *status_json(bool with_token)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(o, "state", kState[s_state]);
    cJSON_AddStringToObject(o, "ssid", s_ssid);
    if (s_state == W_CONNECTED) {
        cJSON_AddStringToObject(o, "ip", s_ip);
        cJSON_AddNumberToObject(o, "port", HTTP_PORT);
    }
    if (with_token) {
        cJSON_AddStringToObject(o, "token", s_token);
    }
    if (s_err[0]) {
        cJSON_AddStringToObject(o, "err", s_err);
    }
    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(o, "rssi", wifi_sta_rssi());
    cJSON_AddBoolToObject(o, "http_ready", s_state == W_CONNECTED && http_api_running());
    return o;
}

static void publish(void)
{
    cJSON *o = status_json(false);
    char *t = cJSON_PrintUnformatted(o);
    if (t) {
        ble_notify_wifi(t);
        cJSON_free(t);
    }
    cJSON_Delete(o);
}

static void set_state(wstate_t st, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    snprintf(s_err, sizeof(s_err), "%s", err ? err : "");
    xSemaphoreGive(s_lock);
    publish();
}

static const char *reason_name(int r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "AUTH_FAILED";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAILED";
    default: return "DISCONNECTED";
    }
}

static void reconnect_task(void *arg)
{
    int delay_s = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    if (s_ssid[0] && s_state != W_CONNECTED) {
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        http_api_stop();
        if (!s_ssid[0]) {
            set_state(W_OFF, NULL);
            return;
        }
        s_retries++;
        /* keep trying with backoff (the operator may enable the hotspot later) */
        int delay_s = s_retries < 5 ? 2 : (s_retries < 10 ? 10 : MAX_RETRY_S);
        set_state(s_retries >= 3 ? W_FAILED : W_CONNECTING, reason_name(d->reason));
        ESP_LOGW(TAG, "disconnected (%s), retry in %d s", reason_name(d->reason), delay_s);
        xTaskCreate(reconnect_task, "wifi_rc", 2048, (void *)(intptr_t)delay_s, 2, NULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_lock);
        s_retries = 0;
        http_api_start(HTTP_PORT);
        ESP_LOGI(TAG, "connected, ip %s, http :%d", s_ip, HTTP_PORT);
        set_state(W_CONNECTED, NULL);
    }
}

static void connect_with(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    xSemaphoreGive(s_lock);
    s_retries = 0;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));               /* do not keep the password on the stack */
    if (!s_started) {
        esp_wifi_start();
        s_started = true;
    }
    set_state(W_CONNECTING, NULL);
    esp_wifi_connect();
}

static esp_err_t nvs_creds(bool write, char *ssid, char *pass)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, write ? NVS_READWRITE : NVS_READONLY, &h);
    if (r != ESP_OK) {
        return r;
    }
    if (write) {
        r = nvs_set_str(h, "ssid", ssid);
        if (r == ESP_OK) {
            r = nvs_set_str(h, "pass", pass);
        }
        if (r == ESP_OK) {
            r = nvs_commit(h);
        }
    } else {
        size_t n = 33;
        r = nvs_get_str(h, "ssid", ssid, &n);
        n = 65;
        if (r == ESP_OK) {
            r = nvs_get_str(h, "pass", pass, &n);
        }
    }
    nvs_close(h);
    return r;
}

bool wifi_sta_handle_op(const char *op, const cJSON *req, cJSON *rsp, bool bonded)
{
    if (strcmp(op, "wifi_set") == 0) {
        const cJSON *s = cJSON_GetObjectItem(req, "ssid");
        const cJSON *p = cJSON_GetObjectItem(req, "pass");
        if (!cJSON_IsString(s) || strlen(s->valuestring) == 0 || strlen(s->valuestring) > 32 ||
            (p && (!cJSON_IsString(p) || strlen(p->valuestring) > 63 ||
                   (strlen(p->valuestring) > 0 && strlen(p->valuestring) < 8)))) {
            cJSON_AddStringToObject(rsp, "st", "INVALID");
            cJSON_AddStringToObject(rsp, "err", "CREDENTIALS");
            return true;
        }
        char ssid[33], pass[65] = "";
        snprintf(ssid, sizeof(ssid), "%s", s->valuestring);
        if (cJSON_IsString(p)) {
            snprintf(pass, sizeof(pass), "%s", p->valuestring);
        }
        nvs_creds(true, ssid, pass);
        connect_with(ssid, pass);
        memset(pass, 0, sizeof(pass));
        ESP_LOGI(TAG, "credentials received for a %u-char SSID", (unsigned)strlen(ssid));
        cJSON_AddStringToObject(rsp, "state", "connecting");
        return true;
    }
    if (strcmp(op, "wifi_forget") == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ssid[0] = 0;
        s_ip[0] = 0;
        xSemaphoreGive(s_lock);
        http_api_stop();
        esp_wifi_disconnect();
        wifi_config_t empty = { 0 };
        esp_wifi_set_config(WIFI_IF_STA, &empty);
        set_state(W_OFF, NULL);
        return true;
    }
    if (strcmp(op, "wifi_status") == 0) {
        cJSON *st = status_json(bonded);
        for (cJSON *it = st->child; it;) {
            cJSON *next = it->next;
            cJSON_DetachItemViaPointer(st, it);
            cJSON_AddItemToObject(rsp, it->string, it);
            it = next;
        }
        cJSON_Delete(st);
        return true;
    }
    return false;
}

esp_err_t wifi_sta_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    wifi_sta_rotate_token();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "pcgkolect");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    /* creds live in our NVS namespace */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);                         /* required with BLE coexistence */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ble_set_local_handler(wifi_sta_handle_op);

    char ssid[33] = "", pass[65] = "";
    if (nvs_creds(false, ssid, pass) == ESP_OK && ssid[0]) {
        ESP_LOGI(TAG, "rejoining the stored network");
        connect_with(ssid, pass);
    } else {
        publish();
    }
    memset(pass, 0, sizeof(pass));
    return ESP_OK;
}
