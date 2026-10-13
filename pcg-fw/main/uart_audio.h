#ifndef UART_AUDIO_H
#define UART_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Streams PCG audio to the RV1103 over UART1 (see uart_com).
 *
 * Wire format (100 ms frames, ~10 frames/s):
 *
 *   0        2        4      5                 5+LEN      5+LEN+2
 *   +--------+--------+------+------------------+----------+
 *   | A5 5A  | LEN    | SEQ  | PCM (int16 LE)   | CRC16 LE |
 *   | 2 B    | 2 B LE | 1 B  | LEN bytes        | 2 B      |
 *   +--------+--------+------+------------------+----------+
 *
 *   LEN         = 1600 (800 samples)
 *   CRC16       = CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over
 *                 LEN(lo)..last PCM byte (3 + LEN bytes), stored LE.
 *
 * Audio source: mono, 8000 Hz, 16-bit signed (from the audio abstraction).
 */

#define UART_AUDIO_SAMPLE_RATE    8000
#define UART_AUDIO_FRAME_SAMPLES  800                       /* 100 ms @ 8 kHz */
#define UART_AUDIO_PAYLOAD_BYTES  (UART_AUDIO_FRAME_SAMPLES * 2)      /* 1600 */
#define UART_AUDIO_FRAME_BYTES    (2 + 2 + 1 + UART_AUDIO_PAYLOAD_BYTES + 2) /* 1607 */
#define UART_AUDIO_SYNC_0         0xA5
#define UART_AUDIO_SYNC_1         0x5A

typedef struct {
    uint32_t frames_sent;
    uint32_t read_errors;
    uint32_t write_errors;
} uart_audio_stats_t;

esp_err_t uart_audio_init(void);

void uart_audio_get_stats(uart_audio_stats_t *out);

#endif
