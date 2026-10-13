#include <stdio.h>
#include <inttypes.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb_audio.h"
#include "uart_com.h"
#include "uart_audio.h"

#define TAG "MAIN"

/* =========================
   MAIN
   ========================= */

void app_main(void)
{
    // Initialize audio pipeline (pcm1803 front-end is brought up by the
    // audio abstraction layer via the first audio_setup()).
    ESP_ERROR_CHECK(usb_audio_init());

    // Initialize UART for RV1103 communication
    ESP_ERROR_CHECK(uart_com_init());

    // Stream 8 kHz / 16-bit PCG audio to the RV1103
    ESP_ERROR_CHECK(uart_audio_init());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uart_audio_stats_t st;
        uart_audio_get_stats(&st);
        ESP_LOGI(TAG, "uart_audio: frames=%" PRIu32 " read_err=%" PRIu32
                 " write_err=%" PRIu32,
                 st.frames_sent, st.read_errors, st.write_errors);
    }
}
