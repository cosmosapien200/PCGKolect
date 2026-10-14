#include "http_api.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ble.h"
#include "link.h"
#include "wifi_sta.h"

static const char *TAG = "http";

static httpd_handle_t s_srv;
static uint8_t s_buf[LINK_CHUNK_MAX];        /* the server task serves one request at a time */

bool http_api_running(void) { return s_srv != NULL; }

/* =========================
   HELPERS
   ========================= */

static bool authorized(httpd_req_t *req)
{
    char h[64];
    if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK ||
        strncmp(h, "Bearer ", 7) != 0 || !wifi_sta_token_ok(h + 7)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"err\":\"UNAUTHORIZED\"}");
        return false;
    }
    return true;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
    char *t = cJSON_PrintUnformatted(o);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, t ? t : "{}");
    cJSON_free(t);
    return r;
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *err)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "err", err);
    esp_err_t r = send_json(req, status, o);
    cJSON_Delete(o);
    return r;
}

/* Map an RV control status to an HTTP status line. */
static const char *http_status(const cJSON *rsp)
{
    const cJSON *st = cJSON_GetObjectItem(rsp, "st");
    const char *s = cJSON_IsString(st) ? st->valuestring : "ERROR";
    if (strcmp(s, "OK") == 0) return "200 OK";
    if (strcmp(s, "INVALID") == 0) return "400 Bad Request";
    if (strcmp(s, "BUSY") == 0) return "409 Conflict";
    if (strcmp(s, "REJECTED") == 0) {
        const cJSON *e = cJSON_GetObjectItem(rsp, "err");
        return (cJSON_IsString(e) && strcmp(e->valuestring, "NOT_FOUND") == 0) ? "404 Not Found" : "409 Conflict";
    }
    if (strcmp(s, "TIMEOUT") == 0) return "504 Gateway Timeout";
    return "502 Bad Gateway";
}

/* Forward an op to the RV; on success *rsp holds the answer (caller deletes). */
static esp_err_t rv_call(httpd_req_t *req, cJSON *rq, int timeout_ms, cJSON **rsp)
{
    *rsp = NULL;
    if (!link_is_up()) {
        send_err(req, "503 Service Unavailable", "LINK_DOWN");
        return ESP_FAIL;
    }
    if (link_request(rq, timeout_ms, 3, rsp) != ESP_OK) {
        send_err(req, "504 Gateway Timeout", "RV_TIMEOUT");
        return ESP_FAIL;
    }
    cJSON_DeleteItemFromObject(*rsp, "v");
    cJSON_DeleteItemFromObject(*rsp, "id");
    return ESP_OK;
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    buf[got] = 0;
    return (int)got;
}

/* raw socket write of all bytes */
static bool send_all(httpd_req_t *req, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        int n = httpd_send(req, p, len);
        if (n < 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* =========================
   HANDLERS
   ========================= */

static esp_err_t h_health(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "ctrl_proto", 1);
    cJSON_AddStringToObject(o, "device_id", ble_device_id());
    cJSON_AddBoolToObject(o, "link_up", link_is_up());
    esp_err_t r = send_json(req, "200 OK", o);
    cJSON_Delete(o);
    return r;
}

static esp_err_t h_forward_op(httpd_req_t *req, const char *op, int timeout_ms)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", op);
    cJSON *rsp;
    esp_err_t r = rv_call(req, rq, timeout_ms, &rsp);
    cJSON_Delete(rq);
    if (r != ESP_OK) return ESP_OK;
    return rsp ? (send_json(req, http_status(rsp), rsp), cJSON_Delete(rsp), ESP_OK) : ESP_OK;
}

static esp_err_t h_device(httpd_req_t *req) { return h_forward_op(req, "hello", 1500); }

static esp_err_t h_stats(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", "stats");
    cJSON *rsp;
    esp_err_t r = rv_call(req, rq, 1500, &rsp);
    cJSON_Delete(rq);
    if (r != ESP_OK) return ESP_OK;
    link_stats_t ls;
    link_get_stats(&ls);
    cJSON *e = cJSON_AddObjectToObject(rsp, "esp");
    cJSON_AddNumberToObject(e, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(e, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(e, "heap_internal_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(e, "wifi_rssi", wifi_sta_rssi());
    cJSON_AddBoolToObject(e, "ble_connected", ble_connected());
    cJSON *l = cJSON_AddObjectToObject(e, "link");
    cJSON_AddNumberToObject(l, "baud", ls.baud);
    cJSON_AddNumberToObject(l, "audio_frames_tx", ls.audio_frames_tx);
    cJSON_AddNumberToObject(l, "rx_crc_errors", ls.rx_crc_errors);
    cJSON_AddNumberToObject(l, "req_timeouts", ls.req_timeouts);
    cJSON_AddNumberToObject(l, "req_retries", ls.req_retries);
    cJSON_AddNumberToObject(l, "link_downs", ls.link_downs);
    send_json(req, http_status(rsp), rsp);
    cJSON_Delete(rsp);
    return ESP_OK;
}

/* GET /v1/recordings?offset=&limit= : pages through the RV manifest (6 per link frame) */
static esp_err_t h_manifest(httpd_req_t *req)
{
    char q[64] = "", v[12];
    int offset = 0, limit = 1000;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "offset", v, sizeof(v)) == ESP_OK) offset = atoi(v);
        if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) limit = atoi(v);
    }
    cJSON *all = cJSON_CreateArray();
    int total = 0;
    for (int off = offset; off < offset + limit;) {
        cJSON *rq = cJSON_CreateObject();
        cJSON_AddStringToObject(rq, "op", "manifest");
        cJSON_AddNumberToObject(rq, "offset", off);
        cJSON_AddNumberToObject(rq, "limit", 6);
        cJSON *rsp;
        esp_err_t r = rv_call(req, rq, 1500, &rsp);
        cJSON_Delete(rq);
        if (r != ESP_OK) {
            cJSON_Delete(all);
            return ESP_OK;
        }
        const cJSON *tj = cJSON_GetObjectItem(rsp, "total");
        cJSON *items = cJSON_GetObjectItem(rsp, "items");
        if (!cJSON_IsNumber(tj) || !cJSON_IsArray(items)) {
            send_json(req, http_status(rsp), rsp);
            cJSON_Delete(rsp);
            cJSON_Delete(all);
            return ESP_OK;
        }
        total = (int)tj->valuedouble;
        int n = cJSON_GetArraySize(items);
        while (cJSON_GetArraySize(items) > 0 && cJSON_GetArraySize(all) < limit) {
            cJSON_AddItemToArray(all, cJSON_DetachItemFromArray(items, 0));
        }
        cJSON_Delete(rsp);
        off += n;
        if (n == 0 || off >= total) break;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "offset", offset);
    cJSON_AddItemToObject(o, "items", all);
    send_json(req, "200 OK", o);
    cJSON_Delete(o);
    return ESP_OK;
}

/* Stream one recording file with Content-Length, Range and SHA-256 headers. */
static esp_err_t stream_file(httpd_req_t *req, const char *rid, const char *kind)
{
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", "open");
    cJSON_AddStringToObject(rq, "recording_id", rid);
    cJSON_AddStringToObject(rq, "kind", kind);
    cJSON *rsp;
    esp_err_t r = rv_call(req, rq, 1500, &rsp);
    cJSON_Delete(rq);
    if (r != ESP_OK) return ESP_OK;
    const cJSON *h = cJSON_GetObjectItem(rsp, "handle");
    if (!cJSON_IsNumber(h)) {
        send_json(req, http_status(rsp), rsp);
        cJSON_Delete(rsp);
        return ESP_OK;
    }
    uint32_t handle = (uint32_t)h->valuedouble;
    uint32_t size = (uint32_t)cJSON_GetObjectItem(rsp, "size")->valuedouble;
    char sha[65] = "";
    const cJSON *sj = cJSON_GetObjectItem(rsp, "sha256");
    if (cJSON_IsString(sj)) snprintf(sha, sizeof(sha), "%s", sj->valuestring);
    cJSON_Delete(rsp);

    /* Range: bytes=a-b | bytes=a- */
    uint32_t start = 0, end = size ? size - 1 : 0;
    bool partial = false;
    char range[48];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK &&
        strncmp(range, "bytes=", 6) == 0) {
        char *dash = strchr(range + 6, '-');
        if (dash) {
            start = (uint32_t)strtoul(range + 6, NULL, 10);
            if (dash[1]) end = (uint32_t)strtoul(dash + 1, NULL, 10);
            if (end >= size) end = size - 1;
            partial = true;
        }
        if (start >= size || start > end) {
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_sendstr(req, "");
            goto done;
        }
    }
    {
        const char *ctype = strcmp(kind, "audio") == 0 ? "audio/wav"
                          : strcmp(kind, "meta") == 0 ? "application/json" : "text/csv";
        uint32_t len = size ? end - start + 1 : 0;
        char hdr[384];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %" PRIu32 "\r\n"
                         "Accept-Ranges: bytes\r\nX-Content-SHA256: %s\r\nX-File-Size: %" PRIu32 "\r\n",
                         partial ? "206 Partial Content" : "200 OK", ctype, len, sha, size);
        if (partial) {
            n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Range: bytes %" PRIu32 "-%" PRIu32 "/%" PRIu32 "\r\n",
                          start, end, size);
        }
        n += snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
        if (!send_all(req, hdr, (size_t)n)) goto done;

        uint32_t off = start;
        int64_t t0 = esp_timer_get_time();
        while (off <= end && len) {
            uint32_t want = end - off + 1;
            if (want > LINK_CHUNK_MAX) want = LINK_CHUNK_MAX;
            size_t got = 0;
            cJSON *err = NULL;
            if (link_read(handle, off, want, s_buf, &got, &err) != ESP_OK || got == 0) {
                cJSON_Delete(err);
                ESP_LOGW(TAG, "read failed at %" PRIu32 "; closing connection", off);
                break;          /* short body: the client sees the length mismatch and resumes */
            }
            if (!send_all(req, s_buf, got)) break;
            off += got;
        }
        double dt = (esp_timer_get_time() - t0) / 1e6;
        ESP_LOGI(TAG, "%s %s %" PRIu32 " B in %.2f s", kind, partial ? "range" : "full", off - start, dt);
    }
done:
    rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", "close");
    cJSON_AddNumberToObject(rq, "handle", handle);
    if (link_request(rq, 400, 2, &rsp) == ESP_OK) cJSON_Delete(rsp);
    cJSON_Delete(rq);
    return ESP_OK;
}

static bool valid_id(const char *s, size_t n)
{
    if (n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
    }
    return true;
}

/* GET /v1/recordings/{id}/{metadata|audio|sidecars/beats|sidecars/quality} */
static esp_err_t h_recording(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const char *p = req->uri + strlen("/v1/recordings/");
    const char *slash = strchr(p, '/');
    if (!slash || !valid_id(p, (size_t)(slash - p))) return send_err(req, "404 Not Found", "NOT_FOUND");
    char rid[65];
    snprintf(rid, sizeof(rid), "%.*s", (int)(slash - p), p);
    const char *what = slash + 1;
    const char *kind = strcmp(what, "audio") == 0 ? "audio"
                     : strcmp(what, "metadata") == 0 ? "meta"
                     : strcmp(what, "sidecars/beats") == 0 ? "beats"
                     : strcmp(what, "sidecars/quality") == 0 ? "quality" : NULL;
    if (!kind) return send_err(req, "404 Not Found", "NOT_FOUND");
    return stream_file(req, rid, kind);
}

/* POST /v1/transfers/{id}/{ack|delete} body {"sha256":...} */
static esp_err_t h_transfer(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const char *p = req->uri + strlen("/v1/transfers/");
    const char *slash = strchr(p, '/');
    if (!slash || !valid_id(p, (size_t)(slash - p))) return send_err(req, "404 Not Found", "NOT_FOUND");
    const char *op = strcmp(slash + 1, "ack") == 0 ? "ack" : strcmp(slash + 1, "delete") == 0 ? "delete" : NULL;
    if (!op) return send_err(req, "404 Not Found", "NOT_FOUND");
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) return send_err(req, "400 Bad Request", "BODY");
    cJSON *b = cJSON_Parse(body);
    const cJSON *sha = cJSON_GetObjectItem(b, "sha256");
    if (!cJSON_IsString(sha)) {
        cJSON_Delete(b);
        return send_err(req, "400 Bad Request", "sha256");
    }
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", op);
    char rid[65];
    snprintf(rid, sizeof(rid), "%.*s", (int)(slash - p), p);
    cJSON_AddStringToObject(rq, "recording_id", rid);
    cJSON_AddStringToObject(rq, "sha256", sha->valuestring);
    cJSON_Delete(b);
    cJSON *rsp;
    if (rv_call(req, rq, 2000, &rsp) == ESP_OK) {
        send_json(req, http_status(rsp), rsp);
        cJSON_Delete(rsp);
    }
    cJSON_Delete(rq);
    return ESP_OK;
}

/* POST /v1/recordings/clear body {"force":false} */
static esp_err_t h_clear(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char body[128] = "{}";
    if (req->content_len && read_body(req, body, sizeof(body)) < 0) return send_err(req, "400 Bad Request", "BODY");
    cJSON *b = cJSON_Parse(body);
    cJSON *rq = cJSON_CreateObject();
    cJSON_AddStringToObject(rq, "op", "clear");
    cJSON_AddBoolToObject(rq, "force", cJSON_IsTrue(cJSON_GetObjectItem(b, "force")));
    cJSON_Delete(b);
    cJSON *rsp;
    if (rv_call(req, rq, 8000, &rsp) == ESP_OK) {
        send_json(req, http_status(rsp), rsp);
        cJSON_Delete(rsp);
    }
    cJSON_Delete(rq);
    return ESP_OK;
}

/* =========================
   SERVER
   ========================= */

esp_err_t http_api_start(int port)
{
    if (s_srv) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = (uint16_t)port;
    cfg.ctrl_port = (uint16_t)(port + 1);
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.stack_size = 6144;
    cfg.task_priority = 4;            /* below audio/link tasks: transfer yields to acquisition */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        s_srv = NULL;
        return ESP_FAIL;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/v1/health", .method = HTTP_GET, .handler = h_health },
        { .uri = "/v1/device", .method = HTTP_GET, .handler = h_device },
        { .uri = "/v1/stats", .method = HTTP_GET, .handler = h_stats },
        { .uri = "/v1/recordings", .method = HTTP_GET, .handler = h_manifest },
        { .uri = "/v1/recordings/clear", .method = HTTP_POST, .handler = h_clear },
        { .uri = "/v1/recordings/*", .method = HTTP_GET, .handler = h_recording },
        { .uri = "/v1/transfers/*", .method = HTTP_POST, .handler = h_transfer },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_srv, &uris[i]);
    }
    return ESP_OK;
}

void http_api_stop(void)
{
    if (s_srv) {
        httpd_stop(s_srv);
        s_srv = NULL;
    }
}
