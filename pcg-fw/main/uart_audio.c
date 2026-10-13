#include "uart_audio.h"

#include "audio.h"
#include "uart_com.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "UART_AUDIO";

#define UART_AUDIO_TASK_STACK 4096
#define UART_AUDIO_TASK_PRIO  5

static audio_stream_t s_stream = NULL;
static volatile uart_audio_stats_t s_stats;

/* Static buffers: the task stack is small and these must not live on it. */
static int16_t s_pcm[UART_AUDIO_FRAME_SAMPLES];
static uint8_t s_frame[UART_AUDIO_FRAME_BYTES];

/* =========================
   FRAMING
   ========================= */

static uint16_t crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/* Build a frame into `out` and return its total size in bytes.
 * `len` is the PCM payload length in bytes (even). */
static size_t build_frame(uint8_t *out, const int16_t *pcm,
                          uint16_t len, uint8_t seq)
{
    out[0] = UART_AUDIO_SYNC_0;
    out[1] = UART_AUDIO_SYNC_1;
    out[2] = (uint8_t)(len & 0xFF);
    out[3] = (uint8_t)(len >> 8);
    out[4] = seq;

    /* ESP32 is little-endian, so int16 samples are already on the wire in the
     * required order. */
    memcpy(&out[5], pcm, len);

    uint16_t crc = crc16_ccitt_false(&out[2], (size_t)3 + len);
    out[5 + len]     = (uint8_t)(crc & 0xFF);
    out[5 + len + 1] = (uint8_t)(crc >> 8);

    return (size_t)5 + len + 2;
}

/* Validate CRC + framing against the receiver spec's test vectors. */
static void uart_audio_selftest(void)
{
    bool ok = crc16_ccitt_false((const uint8_t *)"123456789", 9) == 0x29B1;

    static const uint8_t expect[] = {
        0xA5, 0x5A, 0x04, 0x00, 0x01, 0x01, 0x00, 0x02, 0x00, 0x8F, 0x8A
    };
    const int16_t samples[2] = { 1, 2 };
    uint8_t frame[sizeof(expect)];

    size_t n = build_frame(frame, samples, 4, 1);
    ok = ok && (n == sizeof(expect)) && (memcmp(frame, expect, n) == 0);

    ESP_LOGI(TAG, "protocol self-test: %s", ok ? "PASS" : "FAIL");
}

/* =========================
   TASK
   ========================= */

static void uart_audio_task(void *arg)
{
    (void)arg;

    uint8_t seq = 0;

    for (;;) {
        size_t got = 0;
        esp_err_t ret = audio_read(s_stream, s_pcm, sizeof(s_pcm), &got);

        if (ret != ESP_OK || got != sizeof(s_pcm)) {
            s_stats.read_errors++;
            continue;
        }

        size_t n = build_frame(s_frame, s_pcm, UART_AUDIO_PAYLOAD_BYTES, seq++);

        if (uart_com_write(s_frame, n) != ESP_OK) {
            s_stats.write_errors++;
            continue;
        }

        s_stats.frames_sent++;
    }
}

/* =========================
   PUBLIC API
   ========================= */

esp_err_t uart_audio_init(void)
{
    audio_config_t cfg = {
        .sample_rate = UART_AUDIO_SAMPLE_RATE,
        .bits        = AUDIO_BITS_16,
    };

    s_stream = audio_setup(&cfg);
    if (s_stream == NULL) {
        ESP_LOGE(TAG, "audio_setup failed");
        return ESP_FAIL;
    }

    uart_audio_selftest();

    if (xTaskCreate(uart_audio_task, "uart_audio",
                    UART_AUDIO_TASK_STACK, NULL,
                    UART_AUDIO_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "streaming %d Hz, %d-bit, %d samples/frame (%d B frames)",
             UART_AUDIO_SAMPLE_RATE, AUDIO_BITS_16,
             UART_AUDIO_FRAME_SAMPLES, UART_AUDIO_FRAME_BYTES);

    return ESP_OK;
}

void uart_audio_get_stats(uart_audio_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->frames_sent  = s_stats.frames_sent;
    out->read_errors  = s_stats.read_errors;
    out->write_errors = s_stats.write_errors;
}
