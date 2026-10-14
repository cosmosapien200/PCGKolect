#ifndef CONSOLE_H
#define CONSOLE_H

#include "esp_err.h"

/* Debug console on UART0 for PC-driven link tests (see console.c). */
esp_err_t console_init(void);

#endif
