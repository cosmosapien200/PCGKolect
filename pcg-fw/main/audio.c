#include "audio.h"

#include "pcm1803.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "AUDIO";

/* Source rate produced by the PCM1803A config (see pcm1803.h). */
#define AUDIO_SOURCE_RATE  PCM1803_SAMPLE_RATE

/* Ring buffer of canonical (24-bit) mono samples. 16384 samples at 48 kHz is
 * ~340 ms of headroom; 64 KB of internal SRAM. (PSRAM is not enabled; this
 * comfortably fits the ~293 KB free heap.) */
#define AUDIO_RING_SAMPLES       16384

#define AUDIO_PRODUCER_TASK_STACK 3072
#define AUDIO_PRODUCER_TASK_PRIO  6

struct audio_stream_s {
    uint32_t          sample_rate;
    audio_bit_depth_t bits;
    uint32_t          decim;          /* source_rate / sample_rate */
    uint64_t          tail;           /* next canonical sample to consume */
    bool              overrun_logged;
    SemaphoreHandle_t sem;            /* signalled by producer on new data */
    struct audio_stream_s *next;
};

static int32_t s_ring[AUDIO_RING_SAMPLES];
static uint64_t s_head = 0;           /* total canonical samples written */

static SemaphoreHandle_t s_ring_lock = NULL;
static SemaphoreHandle_t s_streams_lock = NULL;
static struct audio_stream_s *s_streams = NULL;
static bool s_inited = false;

/* Scratch buffer for one I2S DMA block (stereo int32 frames). */
static int32_t s_raw[PCM1803_DMA_FRAMES * 2];

static inline size_t audio_bytes_per_sample(audio_bit_depth_t bits)
{
    switch (bits) {
    case AUDIO_BITS_8:
        return 1;
    case AUDIO_BITS_16:
        return 2;
    case AUDIO_BITS_24:
        return 4;
    default:
        return 0;
    }
}

/* =========================
   PRODUCER
   ========================= */

static void audio_producer_task(void *arg)
{
    (void)arg;

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t ret = pcm1803_read(s_raw, sizeof(s_raw), &bytes_read);

        if (ret != ESP_OK || bytes_read < (2 * sizeof(int32_t))) {
            continue;
        }

        size_t frames = bytes_read / (2 * sizeof(int32_t));

        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        for (size_t i = 0; i < frames; i++) {
            /* PCM1803A outputs 24-bit data MSB-justified in a 32-bit slot.
             * Keep the left channel only (`s_raw[i * 2]`) and shift the valid
             * 24 bits down; the value is sign-extended into int32_t. */
            int32_t s24 = s_raw[i * 2] >> 8;
            s_ring[s_head % AUDIO_RING_SAMPLES] = s24;
            s_head++;
        }
        xSemaphoreGive(s_ring_lock);

        /* Wake every consumer that is waiting for more data. */
        xSemaphoreTake(s_streams_lock, portMAX_DELAY);
        for (struct audio_stream_s *s = s_streams; s != NULL; s = s->next) {
            xSemaphoreGive(s->sem);
        }
        xSemaphoreGive(s_streams_lock);
    }
}

/* =========================
   PUBLIC API
   ========================= */

esp_err_t audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_ring_lock = xSemaphoreCreateMutex();
    s_streams_lock = xSemaphoreCreateMutex();
    if (s_ring_lock == NULL || s_streams_lock == NULL) {
        ESP_LOGE(TAG, "failed to create mutexes");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = pcm1803_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcm1803_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (xTaskCreate(audio_producer_task, "audio_producer",
                    AUDIO_PRODUCER_TASK_STACK, NULL,
                    AUDIO_PRODUCER_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create producer task");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "audio front-end ready (source %d Hz)", AUDIO_SOURCE_RATE);

    return ESP_OK;
}

audio_stream_t audio_setup(const audio_config_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "config is NULL");
        return NULL;
    }

    if (cfg->sample_rate == 0 ||
        (AUDIO_SOURCE_RATE % cfg->sample_rate) != 0) {
        ESP_LOGE(TAG, "sample rate %" PRIu32 " must divide %d",
                 cfg->sample_rate, AUDIO_SOURCE_RATE);
        return NULL;
    }

    if (audio_bytes_per_sample(cfg->bits) == 0) {
        ESP_LOGE(TAG, "unsupported bit depth %d", cfg->bits);
        return NULL;
    }

    if (audio_init() != ESP_OK) {
        return NULL;
    }

    struct audio_stream_s *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }

    s->sample_rate = cfg->sample_rate;
    s->bits        = cfg->bits;
    s->decim       = AUDIO_SOURCE_RATE / cfg->sample_rate;
    s->sem         = xSemaphoreCreateBinary();
    if (s->sem == NULL) {
        free(s);
        return NULL;
    }

    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s->tail = s_head;
    xSemaphoreGive(s_ring_lock);

    xSemaphoreTake(s_streams_lock, portMAX_DELAY);
    s->next   = s_streams;
    s_streams = s;
    xSemaphoreGive(s_streams_lock);

    ESP_LOGI(TAG, "stream open: %" PRIu32 " Hz, %d-bit, decim=%" PRIu32,
             s->sample_rate, s->bits, s->decim);

    return s;
}

esp_err_t audio_read(
    audio_stream_t stream,
    void *buf,
    size_t size,
    size_t *bytes_read
)
{
    if (stream == NULL || buf == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *bytes_read = 0;

    size_t bps  = audio_bytes_per_sample(stream->bits);
    size_t want = size / bps;
    if (want == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t need = (uint64_t)want * stream->decim;
    if (need > AUDIO_RING_SAMPLES) {
        ESP_LOGE(TAG, "request of %u samples exceeds ring capacity", (unsigned)want);
        return ESP_ERR_INVALID_SIZE;
    }

    for (;;) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);

        uint64_t head  = s_head;
        uint64_t avail = head - stream->tail;

        /* Producer lapped a slow consumer: the oldest samples are gone. */
        if (avail > AUDIO_RING_SAMPLES) {
            if (!stream->overrun_logged) {
                ESP_LOGW(TAG, "stream %" PRIu32 " Hz overrun, dropping old data",
                         stream->sample_rate);
                stream->overrun_logged = true;
            }
            stream->tail = head - need;
            avail = need;
        }

        if (avail >= need) {
            const uint64_t tail  = stream->tail;
            const uint32_t decim = stream->decim;
            const audio_bit_depth_t bits = stream->bits;

            for (size_t k = 0; k < want; k++) {
                uint64_t base = tail + (uint64_t)k * decim;

                int64_t sum = 0;
                for (uint32_t j = 0; j < decim; j++) {
                    sum += s_ring[(base + j) % AUDIO_RING_SAMPLES];
                }
                int32_t avg = (int32_t)(sum / (int64_t)decim);

                switch (bits) {
                case AUDIO_BITS_24:
                    ((int32_t *)buf)[k] = avg;
                    break;
                case AUDIO_BITS_16:
                    ((int16_t *)buf)[k] = (int16_t)(avg >> 8);
                    break;
                case AUDIO_BITS_8:
                    ((int8_t *)buf)[k] = (int8_t)(avg >> 16);
                    break;
                default:
                    break;
                }
            }

            stream->tail = tail + need;
            stream->overrun_logged = false;
            xSemaphoreGive(s_ring_lock);

            *bytes_read = want * bps;
            return ESP_OK;
        }

        xSemaphoreGive(s_ring_lock);

        /* Wait for the producer to push another block (periodic wake-up so a
         * stalled producer cannot deadlock the caller forever). */
        xSemaphoreTake(stream->sem, pdMS_TO_TICKS(100));
    }
}

void audio_stream_close(audio_stream_t stream)
{
    if (stream == NULL) {
        return;
    }

    xSemaphoreTake(s_streams_lock, portMAX_DELAY);
    struct audio_stream_s **pp = &s_streams;
    while (*pp != NULL && *pp != stream) {
        pp = &(*pp)->next;
    }
    if (*pp != NULL) {
        *pp = stream->next;
    }
    xSemaphoreGive(s_streams_lock);

    if (stream->sem != NULL) {
        vSemaphoreDelete(stream->sem);
    }
    free(stream);
}
