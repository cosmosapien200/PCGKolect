#ifndef LINK_H
#define LINK_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

/*
 * ESP32 <-> RV1103 UART link (UART1, TX GPIO17 / RX GPIO18).
 * Wire format and semantics: PCGKolect_app/docs/PROTOCOL.md §1–2.
 *
 *   audio   (ESP->RV): A5 5A | LEN u16 | SEQ u8  | PCM[LEN]     | CRC16
 *   message (both)   : A5 5C | LEN u16 | TYPE u8 | SEQ u8 | DATA | CRC16
 *
 * Every frame is written atomically, so audio and messages never interleave.
 */

#define LINK_PROTO          1
#define LINK_BAUD_DEFAULT   460800      /* both sides boot here */
#define LINK_BAUD_TARGET    4000000     /* negotiated after boot; benchmarked 333 KB/s, 0 errors */

#define LINK_SYNC_0         0xA5
#define LINK_SYNC_AUDIO     0x5A
#define LINK_SYNC_MSG       0x5C
#define LINK_MAX_PAYLOAD    4200
#define LINK_CHUNK_MAX      4096

#define LINK_T_REQ          0x01
#define LINK_T_RSP          0x02
#define LINK_T_EVT          0x03
#define LINK_T_CHUNK        0x10

typedef struct {
    uint32_t audio_frames_tx;
    uint32_t msg_tx;
    uint32_t msg_rx;
    uint32_t rx_crc_errors;
    uint32_t rx_bad_len;
    uint32_t rx_bytes;
    uint32_t req_timeouts;
    uint32_t req_retries;
    uint32_t write_errors;
    uint32_t baud;
    uint32_t link_downs;
} link_stats_t;

/* Called from the link RX task for every EVT frame (JSON text, not NUL-terminated). */
typedef void (*link_event_cb_t)(const char *json, size_t len);

/* Called for CHUNK frames that match no pending read (bench / streaming). */
typedef void (*link_chunk_cb_t)(uint32_t req_id, uint32_t offset, const uint8_t *data, size_t len);

esp_err_t link_init(void);

/* Write one complete, already-framed byte sequence (used for audio frames). */
esp_err_t link_write_frame(const uint8_t *frame, size_t len);

/* Frame and send a message. */
esp_err_t link_send_msg(uint8_t type, const void *data, size_t len);

/*
 * Send a request and wait for its response. `req` gets "v" and "id" set (any
 * existing "id" is replaced). Retries with the same id on timeout.
 * On success *rsp is a new cJSON object the caller must cJSON_Delete().
 */
esp_err_t link_request(cJSON *req, int timeout_ms, int attempts, cJSON **rsp);

/* Convenience: request with only an op name. */
esp_err_t link_request_op(const char *op, int timeout_ms, cJSON **rsp);

/*
 * Read a chunk of an open RV file handle into buf (cap >= len).
 * Returns ESP_OK and *got (0 = EOF), ESP_ERR_TIMEOUT, or ESP_FAIL with
 * *rsp_err set to the RV error JSON (caller deletes) when the RV refused.
 */
esp_err_t link_read(uint32_t handle, uint32_t offset, uint32_t len,
                    uint8_t *buf, size_t *got, cJSON **rsp_err);

/* true while the RV answers requests (updated by the link supervisor). */
bool link_is_up(void);

/* Switch the link baud rate (negotiated with the RV, reverts on failure). */
esp_err_t link_set_baud(uint32_t baud);

void link_set_event_cb(link_event_cb_t cb);
void link_set_chunk_cb(link_chunk_cb_t cb);
uint32_t link_next_id(void);
void link_get_stats(link_stats_t *out);

uint16_t link_crc16(const uint8_t *data, size_t len);

#endif
