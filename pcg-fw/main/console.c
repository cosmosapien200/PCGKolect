#include "console.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link.h"
#include "psa/crypto.h"

/*
 * Line-based debug console on UART0 (the USB-serial bridge), for bring-up and
 * tests from a PC. Commands:
 *   req <json>       send a control request to the RV, print the response
 *   baud <rate>      switch the link baud rate
 *   flood <bytes>    RV streams <bytes> of CHUNK frames; report throughput/loss
 *   rbench <bytes>   request/response read of a virtual RV file; verify pattern
 *   fetch <rid> <kind>  download a recording file over the link, verify SHA-256
 *   stats            link counters
 *   evt on|off       print RV events
 */

static const char *TAG = "console";

#define CON_PORT UART_NUM_0

static volatile bool s_print_evt;
static volatile uint32_t s_flood_id, s_flood_bytes, s_flood_frames, s_flood_bad, s_flood_next;

static void on_evt(const char *json, size_t len)
{
    if (s_print_evt) {
        printf("EVT %.*s\n", (int)len, json);
    }
}

static void on_chunk(uint32_t id, uint32_t off, const uint8_t *d, size_t len)
{
    if (id != s_flood_id) {
        return;
    }
    if (off != s_flood_next) {
        s_flood_bad++;          /* missing chunk(s) before this one */
    }
    s_flood_next = off + len;
    s_flood_bytes += len;
    s_flood_frames++;
}

static void print_rsp(esp_err_t r, cJSON *rsp)
{
    if (r != ESP_OK) {
        printf("RSP error %s\n", esp_err_to_name(r));
        return;
    }
    char *t = cJSON_PrintUnformatted(rsp);
    printf("RSP %s\n", t ? t : "?");
    cJSON_free(t);
    cJSON_Delete(rsp);
}

static void cmd_flood(uint32_t bytes)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "flood");
    cJSON_AddNumberToObject(req, "bytes", bytes);
    s_flood_id = link_next_id() + 1;    /* link_request takes the next id */
    s_flood_bytes = s_flood_frames = s_flood_bad = s_flood_next = 0;
    link_stats_t s0;
    link_get_stats(&s0);
    int64_t t0 = esp_timer_get_time();
    cJSON *rsp = NULL;
    esp_err_t r = link_request(req, 400, 1, &rsp);
    cJSON_Delete(req);
    if (r != ESP_OK) {
        printf("FLOOD start failed %s\n", esp_err_to_name(r));
        return;
    }
    cJSON_Delete(rsp);
    /* wait until the stream stops for 300 ms */
    uint32_t last = 0;
    int idle = 0;
    while (idle < 15) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (s_flood_bytes == last) {
            idle++;
        } else {
            idle = 0;
            last = s_flood_bytes;
        }
    }
    double dt = (double)(esp_timer_get_time() - t0) / 1e6 - 0.3;
    link_stats_t s1;
    link_get_stats(&s1);
    printf("FLOOD bytes=%" PRIu32 "/%" PRIu32 " frames=%" PRIu32 " gaps=%" PRIu32
           " crc_err=%" PRIu32 " time=%.2fs rate=%.1f KB/s\n",
           s_flood_bytes, bytes, s_flood_frames, s_flood_bad,
           s1.rx_crc_errors - s0.rx_crc_errors, dt, s_flood_bytes / dt / 1024.0);
}

static void cmd_rbench(uint32_t bytes)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "open");
    cJSON_AddStringToObject(req, "kind", "bench");
    cJSON_AddNumberToObject(req, "size", bytes);
    cJSON *rsp = NULL;
    esp_err_t r = link_request(req, 400, 3, &rsp);
    cJSON_Delete(req);
    if (r != ESP_OK) {
        printf("RBENCH open failed %s\n", esp_err_to_name(r));
        return;
    }
    const cJSON *h = cJSON_GetObjectItem(rsp, "handle");
    if (!cJSON_IsNumber(h)) {
        print_rsp(ESP_OK, rsp);
        return;
    }
    uint32_t handle = (uint32_t)h->valuedouble;
    cJSON_Delete(rsp);

    static uint8_t buf[LINK_CHUNK_MAX];
    link_stats_t s0;
    link_get_stats(&s0);
    int64_t t0 = esp_timer_get_time();
    uint32_t off = 0, bad = 0;
    while (off < bytes) {
        size_t got = 0;
        cJSON *err = NULL;
        r = link_read(handle, off, LINK_CHUNK_MAX, buf, &got, &err);
        if (r != ESP_OK || got == 0) {
            printf("RBENCH read failed at %" PRIu32 ": %s\n", off, esp_err_to_name(r));
            if (err) {
                print_rsp(ESP_OK, err);
            }
            break;
        }
        for (size_t i = 0; i < got; i++) {
            if (buf[i] != (uint8_t)((off + i) * 31u + 7u)) {
                bad++;
            }
        }
        off += got;
    }
    double dt = (double)(esp_timer_get_time() - t0) / 1e6;
    link_stats_t s1;
    link_get_stats(&s1);
    printf("RBENCH bytes=%" PRIu32 " bad_bytes=%" PRIu32 " retries=%" PRIu32 " crc_err=%" PRIu32
           " time=%.2fs rate=%.1f KB/s\n",
           off, bad, s1.req_retries - s0.req_retries, s1.rx_crc_errors - s0.rx_crc_errors,
           dt, off / dt / 1024.0);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "close");
    cJSON_AddNumberToObject(req, "handle", handle);
    if (link_request(req, 400, 3, &rsp) == ESP_OK) {
        cJSON_Delete(rsp);
    }
    cJSON_Delete(req);
}

static void cmd_fetch(const char *rid, const char *kind)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "open");
    cJSON_AddStringToObject(req, "recording_id", rid);
    cJSON_AddStringToObject(req, "kind", kind);
    cJSON *rsp = NULL;
    esp_err_t r = link_request(req, 1500, 3, &rsp);
    cJSON_Delete(req);
    if (r != ESP_OK) {
        printf("FETCH open failed %s\n", esp_err_to_name(r));
        return;
    }
    const cJSON *h = cJSON_GetObjectItem(rsp, "handle");
    const cJSON *sz = cJSON_GetObjectItem(rsp, "size");
    const cJSON *sh = cJSON_GetObjectItem(rsp, "sha256");
    if (!cJSON_IsNumber(h)) {
        print_rsp(ESP_OK, rsp);
        return;
    }
    uint32_t handle = (uint32_t)h->valuedouble, size = (uint32_t)sz->valuedouble;
    char expect[65] = "";
    if (cJSON_IsString(sh)) {
        strncpy(expect, sh->valuestring, 64);
    }
    cJSON_Delete(rsp);

    static uint8_t buf[LINK_CHUNK_MAX];
    psa_crypto_init();
    psa_hash_operation_t ctx = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&ctx, PSA_ALG_SHA_256);
    int64_t t0 = esp_timer_get_time();
    uint32_t off = 0;
    while (off < size) {
        size_t got = 0;
        cJSON *err = NULL;
        r = link_read(handle, off, LINK_CHUNK_MAX, buf, &got, &err);
        if (r != ESP_OK || got == 0) {
            printf("FETCH read failed at %" PRIu32 ": %s\n", off, esp_err_to_name(r));
            if (err) {
                print_rsp(ESP_OK, err);
            }
            break;
        }
        psa_hash_update(&ctx, buf, got);
        off += got;
    }
    uint8_t dig[32];
    size_t dlen = 0;
    psa_hash_finish(&ctx, dig, sizeof(dig), &dlen);
    char hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(hex + 2 * i, "%02x", dig[i]);
    }
    double dt = (double)(esp_timer_get_time() - t0) / 1e6;
    printf("FETCH %s bytes=%" PRIu32 "/%" PRIu32 " sha256=%s %s time=%.2fs rate=%.1f KB/s\n", kind, off, size, hex,
           strcmp(hex, expect) == 0 ? "MATCH" : "MISMATCH", dt, off / dt / 1024.0);

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "op", "close");
    cJSON_AddNumberToObject(req, "handle", handle);
    if (link_request(req, 400, 3, &rsp) == ESP_OK) {
        cJSON_Delete(rsp);
    }
    cJSON_Delete(req);
}

static void handle_line(char *line)
{
    if (strncmp(line, "req ", 4) == 0) {
        cJSON *req = cJSON_Parse(line + 4);
        if (req == NULL) {
            printf("ERR bad json\n");
            return;
        }
        cJSON *rsp = NULL;
        esp_err_t r = link_request(req, 1500, 3, &rsp);
        cJSON_Delete(req);
        print_rsp(r, rsp);
    } else if (strncmp(line, "baud ", 5) == 0) {
        esp_err_t r = link_set_baud((uint32_t)strtoul(line + 5, NULL, 10));
        printf("BAUD %s\n", esp_err_to_name(r));
    } else if (strncmp(line, "flood ", 6) == 0) {
        cmd_flood((uint32_t)strtoul(line + 6, NULL, 10));
    } else if (strncmp(line, "rbench ", 7) == 0) {
        cmd_rbench((uint32_t)strtoul(line + 7, NULL, 10));
    } else if (strncmp(line, "fetch ", 6) == 0) {
        char rid[65] = "", kind[16] = "audio";
        sscanf(line + 6, "%64s %15s", rid, kind);
        cmd_fetch(rid, kind);
    } else if (strcmp(line, "stats") == 0) {
        link_stats_t s;
        link_get_stats(&s);
        printf("STATS baud=%" PRIu32 " audio_tx=%" PRIu32 " msg_tx=%" PRIu32 " msg_rx=%" PRIu32
               " rx_bytes=%" PRIu32 " crc_err=%" PRIu32 " bad_len=%" PRIu32 " timeouts=%" PRIu32
               " retries=%" PRIu32 " write_err=%" PRIu32 " up=%d downs=%" PRIu32 "\n",
               s.baud, s.audio_frames_tx, s.msg_tx, s.msg_rx, s.rx_bytes, s.rx_crc_errors,
               s.rx_bad_len, s.req_timeouts, s.req_retries, s.write_errors, link_is_up(), s.link_downs);
    } else if (strcmp(line, "evt on") == 0) {
        s_print_evt = true;
    } else if (strcmp(line, "evt off") == 0) {
        s_print_evt = false;
    } else if (line[0]) {
        printf("ERR unknown command\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[1024];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CON_PORT, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) {
                handle_line(line);
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

esp_err_t console_init(void)
{
    if (!uart_is_driver_installed(CON_PORT)) {
        ESP_ERROR_CHECK(uart_driver_install(CON_PORT, 2048, 0, 0, NULL, 0));
    }
    link_add_event_cb(on_evt);
    link_set_chunk_cb(on_chunk);
    if (xTaskCreate(console_task, "console", 6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "debug console ready on UART0");
    return ESP_OK;
}
