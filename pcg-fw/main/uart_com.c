#include "uart_com.h"

#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "uart_com";

#define UART_COM_PORT      UART_NUM_1
#define UART_COM_TX_PIN    17
#define UART_COM_RX_PIN    18
#define UART_COM_BAUDRATE  460800

/* TX ring buffer must hold at least one uart_audio frame (1607 B) so that
 * uart_write_bytes() never blocks on the wire and delays the audio task. */
#define UART_COM_RX_BUFFER 256
#define UART_COM_TX_BUFFER 4096

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
            UART_COM_RX_BUFFER,
            UART_COM_TX_BUFFER,
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

esp_err_t uart_com_write(
    const void *data,
    size_t len
)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Blocks only when the TX ring buffer is full; with UART_COM_TX_BUFFER
    // sized to hold a full audio frame this returns immediately and the
    // driver drains the bytes in the background.
    int written = uart_write_bytes(UART_COM_PORT, data, len);

    if (written < 0) {
        ESP_LOGE(TAG, "UART write failed");
        return ESP_FAIL;
    }

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
