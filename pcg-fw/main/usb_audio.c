#include "usb_audio.h"

#include "usb_device_uac.h"
#include "pcm1803.h"

#include <string.h>
#include <stdint.h>

static int s_usb_cb_count = 0;


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

    int16_t *out = (int16_t *)buf;

    int32_t i2s_buf[960];

    size_t i2s_bytes = 0;

    esp_err_t ret =
        pcm1803_read(
            i2s_buf,
            sizeof(i2s_buf),
            &i2s_bytes
        );
    if (ret != ESP_OK || i2s_bytes == 0)
    {
        memset(buf, 0, len);

        *bytes_read = len;

        return ESP_OK;
    }

    int samples =
        i2s_bytes / sizeof(int32_t);

    int out_samples =2*len / sizeof(int16_t);

    if (samples > out_samples)
    {
        samples = out_samples;
    }

    int out_idx = 0;

    for (int i = 0; i < samples; i += 2)
    {
        out[out_idx++] =
            (int16_t)(i2s_buf[i] >> 16);
    }
    printf("\rsamples per cb: %d - i2s out: %d - buflen: %d     ",out_idx,samples, len);
    *bytes_read =
        out_idx * sizeof(int16_t);
    return ESP_OK;
}


/* =========================
   PUBLIC API
   ========================= */

esp_err_t usb_audio_init(void)
{
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