#ifndef HTTP_API_H
#define HTTP_API_H

#include <stdbool.h>

#include "esp_err.h"

/*
 * Token-authenticated local HTTP API on the hotspot LAN (PROTOCOL.md §4).
 * Recording bytes are proxied from the RV1103 in 4 KB link reads; nothing is
 * buffered whole in ESP RAM. One request is served at a time.
 */
esp_err_t http_api_start(int port);
void http_api_stop(void);
bool http_api_running(void);

#endif
