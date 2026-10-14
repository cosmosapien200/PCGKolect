#ifndef BLE_H
#define BLE_H

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/*
 * BLE GATT control service (PCGKolect_app/docs/PROTOCOL.md §3).
 *
 *   Service   7a1c0001-5b6e-4e8a-9c1f-50434b4f4c54
 *   0002 Info       read           hello JSON
 *   0003 Command    write (enc)    fragmented request JSON
 *   0004 Response   notify         fragmented response JSON
 *   0005 Telemetry  notify         latest status JSON, <= 5 Hz
 *   0006 Wi-Fi      read/notify    wifi_status JSON
 *
 * Fragment = HDR u8 (bit7 FIRST, bit6 LAST, bits0-5 index) + data.
 * Requests are handled on a command task; most are forwarded to the RV1103.
 */

/* Handler for ops answered by the ESP itself (Wi-Fi). Fill rsp; return false
 * if the op is not handled locally. */
typedef bool (*ble_local_op_t)(const char *op, const cJSON *req, cJSON *rsp, bool bonded);

esp_err_t ble_init(void);
void ble_set_local_handler(ble_local_op_t fn);

/* Push the current Wi-Fi status JSON (stored for reads, notified if subscribed). */
void ble_notify_wifi(const char *json);

const char *ble_device_id(void);
bool ble_connected(void);

#endif
