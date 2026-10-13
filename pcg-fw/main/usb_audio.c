#include "usb_audio.h"

#include "usb_device_uac.h"
#include "audio.h"

#include <string.h>
#include <stdint.h>

static int s_usb_cb_count = 0;

/* Microphone stream: 48 kHz, 16-bit mono, matching the UAC descriptor. */
static audio_stream_t s_mic_stream = NULL;


/* =========================
   REQUIRED CALLBACKS
   ========================= */

static esp_err_t usb_uac_device_output_cb(
    uint8_t *buf,
    size_t len,
    void *arg)
{
    (void)buf;
    (void)len;
    (void)arg;

    return ESP_OK;
}

static void usb_uac_device_set_mute_cb(
    uint32_t mute,
    void *arg)
{
    (void)mute;
    (void)arg;
}

static void usb_uac_device_set_volume_cb(
    uint32_t volume,
    void *arg)
{
    (void)volume;
    (void)arg;
}

static esp_err_t usb_uac_device_input_cb(
    uint8_t *buf,
    size_t len,
    size_t *bytes_read,
    void *arg)
{
    (void)arg;

    s_usb_cb_count++;

    size_t got = 0;
    esp_err_t ret = audio_read(s_mic_stream, buf, len, &got);

    if (ret != ESP_OK || got < len) {
        /* Never hand an underrun to the host: pad with silence. */
        if (ret == ESP_OK && got < len) {
            memset(buf + got, 0, len - got);
        } else if (ret != ESP_OK) {
            memset(buf, 0, len);
            got = 0;
        }
        *bytes_read = len;
        return ESP_OK;
    }

    *bytes_read = got;
    return ESP_OK;
}


/* =========================
   PUBLIC API
   ========================= */

esp_err_t usb_audio_init(void)
{
    audio_config_t mic_cfg = {
        .sample_rate = 48000,
        .bits = AUDIO_BITS_16,
    };

    s_mic_stream = audio_setup(&mic_cfg);
    if (s_mic_stream == NULL) {
        return ESP_FAIL;
    }

    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = usb_uac_device_input_cb,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };

    return uac_device_init(&config);
}

int usb_audio_get_callback_count(void)
{
    return s_usb_cb_count;
}

void usb_audio_reset_callback_count(void)
{
    s_usb_cb_count = 0;
}
