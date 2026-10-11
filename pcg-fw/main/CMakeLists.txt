idf_component_register(SRCS "main.c" "pcm1803.c" "usb_audio.c" "uart_com.c"
                       PRIV_REQUIRES driver esp_driver_i2s esp_driver_uart esp_driver_gpio
                       INCLUDE_DIRS "")
