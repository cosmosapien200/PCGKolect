#include "uart_com.h"

#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "uart_com";

#define UART_COM_PORT      UART_NUM_1
#define UART_COM_TX_PIN    17
#define UART_COM_RX_PIN    18
#define UART_COM_BAUDRATE  460800

// No handle needed in ESP-IDF v6.x — UART APIs use uart_port_t directly

esp_err_t uart_com_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_COM_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Install UART driver so we can get the driver handle
    ESP_ERROR_CHECK(
        uart_driver_install(
            UART_COM_PORT,
            256, // RX buffer size (0 = default 256)
            256, // TX buffer size (0 = default 256)
            0, // Queue size (not used in simple mode)
            NULL,
            0
        )
    );

    // Configure UART parameters
    ESP_ERROR_CHECK(
        uart_param_config(
            UART_COM_PORT,
            &uart_config
        )
    );

    // Set pins (TX, RX, RTS, CTS)
    ESP_ERROR_CHECK(
        uart_set_pin(
            UART_COM_PORT,
            UART_COM_TX_PIN,
            UART_COM_RX_PIN,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    // ESP-IDF v6.x uses uart_port_t directly — no handle needed

    ESP_LOGI(TAG, "UART initialized: port=%d, TX=%d, RX=%d, baud=%d",
             UART_COM_PORT, UART_COM_TX_PIN, UART_COM_RX_PIN,
             UART_COM_BAUDRATE);

    return ESP_OK;
}

esp_err_t uart_com_send(
    const void *data,
    size_t len
)
{
    size_t written = 0;
    esp_err_t ret = uart_write_bytes(
        UART_COM_PORT,
        data,
        len
    );

    if (ret < 0) {
        ESP_LOGE(TAG, "UART write failed");
        return ret;
    }

    written = (size_t)ret;

    // Flush TX buffer to ensure all bytes are transmitted
    uart_flush(UART_COM_PORT);

    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, written, ESP_LOG_DEBUG);

    return ESP_OK;
}

esp_err_t uart_com_send_string(
    const char *str
)
{
    if (str == NULL) {
        ESP_LOGE(TAG, "String is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    return uart_com_send(str, strlen(str));
}
