#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/*
 * Wi-Fi station on the phone's hotspot (PROTOCOL.md §4). Credentials arrive
 * over bonded BLE (wifi_set), are kept in NVS so the device rejoins after a
 * reboot, and are erased by wifi_forget. Once an IP is assigned the local
 * HTTP API starts; its bearer token is random and rotated on every new BLE
 * connection. The token is only returned in wifi_status answers on a bonded
 * link, never in notifications, adverts or logs.
 */

#define HTTP_PORT 8080

esp_err_t wifi_sta_init(void);

/* BLE local-op handler: wifi_set, wifi_forget, wifi_status. */
bool wifi_sta_handle_op(const char *op, const cJSON *req, cJSON *rsp, bool bonded);

/* New random HTTP token (call on each BLE connection). */
void wifi_sta_rotate_token(void);

/* Constant-time token check for the HTTP server. */
bool wifi_sta_token_ok(const char *bearer);

bool wifi_sta_connected(void);
int wifi_sta_rssi(void);

#endif
