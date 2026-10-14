idf_component_register(SRCS "main.c" "pcm1803.c" "audio.c" "usb_audio.c" "uart_audio.c" "link.c" "console.c" "ble.c" "wifi_sta.c" "http_api.c"
                       PRIV_REQUIRES driver esp_driver_i2s esp_driver_uart esp_driver_gpio esp_timer mbedtls bt nvs_flash esp_app_format esp_wifi esp_netif esp_event esp_http_server
                       INCLUDE_DIRS "")
