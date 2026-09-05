#ifndef PCM1803_H
#define PCM1803_H

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define PCM1803_SAMPLE_RATE 48000

esp_err_t pcm1803_init(void);

i2s_chan_handle_t pcm1803_get_rx_handle(void);
esp_err_t pcm1803_read(
    void *buffer,
    size_t size,
    size_t *bytes_read
);
#endif