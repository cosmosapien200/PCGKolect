#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "pcm1803.h"
#include "usb_audio.h"
#include "uart_com.h"

#define SAMPLE_RATE 48000
#define TAG "PSRAM TEST"

/* =========================
   MAIN
   ========================= */

void app_main(void)
{
    // Initialize audio pipeline
    ESP_ERROR_CHECK(pcm1803_init());
    ESP_ERROR_CHECK(usb_audio_init());

    // Initialize UART for RV1103 communication
    ESP_ERROR_CHECK(uart_com_init());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG,"Sending hello world! in uart");       
        uart_com_send_string("Hello World\r\n");
    }
}