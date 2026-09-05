#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pcm1803.h"
#include "usb_audio.h"

#define SAMPLE_RATE 48000
#define TAG "PSRAM TEST"

/* =========================
   MAIN
   ========================= */

void app_main(void)
{
    
    
    ESP_ERROR_CHECK(pcm1803_init());
    ESP_ERROR_CHECK(
        usb_audio_init()
    );
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf(
            "\nUSB CB COUNT: %d\n",
            usb_audio_get_callback_count()
        );

        usb_audio_reset_callback_count();
    }
}