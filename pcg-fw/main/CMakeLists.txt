idf_component_register(SRCS "main.c" "pcm1803.c" "audio.c" "usb_audio.c" "uart_audio.c" "link.c" "console.c"
                       PRIV_REQUIRES driver esp_driver_i2s esp_driver_uart esp_driver_gpio esp_timer mbedtls
                       INCLUDE_DIRS "")
