#include <stdio.h>
#include <inttypes.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "usb_audio.h"
#include "link.h"
#include "console.h"
#include "ble.h"
#include "wifi_sta.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "uart_audio.h"

#define TAG "MAIN"

/* =========================
   MAIN
   ========================= */

void app_main(void)
{
    // NVS holds BLE bonds and Wi-Fi credentials
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nv = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nv);

    // Initialize audio pipeline (pcm1803 front-end is brought up by the
    // audio abstraction layer via the first audio_setup()).
    ESP_ERROR_CHECK(usb_audio_init());

    // RV1103 link: audio + typed control/file frames on UART1
    ESP_ERROR_CHECK(link_init());

    // Stream 8 kHz / 16-bit PCG audio to the RV1103
    ESP_ERROR_CHECK(uart_audio_init());

    // PC debug console on UART0
    ESP_ERROR_CHECK(console_init());

    // BLE control service for the phone app
    ESP_ERROR_CHECK(ble_init());

    // Wi-Fi station on the phone hotspot + local HTTP API (credentials via BLE)
    ESP_ERROR_CHECK(wifi_sta_init());
    ble_set_connect_cb(wifi_sta_rotate_token);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        uart_audio_stats_t st;
        uart_audio_get_stats(&st);
        size_t txfree = 0;
        uart_get_tx_buffer_free_size(UART_NUM_1, &txfree);
        ESP_LOGI(TAG, "uart_audio: frames=%" PRIu32 " read_err=%" PRIu32
                 " write_err=%" PRIu32 " txfree=%u",
                 st.frames_sent, st.read_errors, st.write_errors, (unsigned)txfree);
    }
}
