#ifndef UART_COM_H
#define UART_COM_H

#include "esp_err.h"
#include <stddef.h>

esp_err_t uart_com_init(void);

esp_err_t uart_com_write(
    const void *data,
    size_t len
);

esp_err_t uart_com_send(
    const void *data,
    size_t len
);

esp_err_t uart_com_send_string(
    const char *str
);

#endif
