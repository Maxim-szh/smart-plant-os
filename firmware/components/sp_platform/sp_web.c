#include "sp_web.h"
#include "sp_web_address.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_https_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
extern const uint8_t page_start[] asm("_binary_index_html_start");
extern const uint8_t page_end[] asm("_binary_index_html_end");
extern const uint8_t app_start[] asm("_binary_app_js_start");
extern const uint8_t app_end[] asm("_binary_app_js_end");
extern const uint8_t script_start[] asm("_binary_live_js_start");
extern const uint8_t script_end[] asm("_binary_live_js_end");
extern const uint8_t setup_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_end[] asm("_binary_setup_html_end");
extern const uint8_t setup_js_start[] asm("_binary_setup_js_start");
extern const uint8_t setup_js_end[] asm("_binary_setup_js_end");
extern const uint8_t pairing_js_start[] asm("_binary_pairing_js_start");
extern const uint8_t pairing_js_end[] asm("_binary_pairing_js_end");
extern const uint8_t api_js_start[] asm("_binary_api_js_start");
extern const uint8_t api_js_end[] asm("_binary_api_js_end");
extern const uint8_t style_start[] asm("_binary_style_css_start");
extern const uint8_t style_end[] asm("_binary_style_css_end");
extern const uint8_t demo_start[] asm("_binary_demo_html_start");
extern const uint8_t demo_end[] asm("_binary_demo_html_end");
extern const uint8_t demo_js_start[] asm("_binary_demo_js_start");
extern const uint8_t demo_js_end[] asm("_binary_demo_js_end");
extern const uint8_t updates_start[] asm("_binary_updates_js_start");
extern const uint8_t updates_end[] asm("_binary_updates_js_end");
static QueueHandle_t incoming, outgoing;
static char password[65], token[65];
static int64_t expires, attempt_window;
static unsigned attempts;
static uint32_t serial;

static bool origin_allowed(httpd_req_t *r) {
    size_t origin_len = httpd_req_get_hdr_value_len(r, "Origin");
    if (!origin_len)
        return true;
    char origin[96], host[64], expected[80];
    if (origin_len >= sizeof(origin) ||
        httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    size_t host_len = strlen(host);
    if (host_len > 4 && !strcmp(host + host_len - 4, ":443"))
        host[host_len - 4] = 0;
    if (snprintf(expected, sizeof(expected), "https://%s", host) >= (int)sizeof(expected))
        return false;
    return !strcmp(origin, expected);
}
typedef struct {
    uint32_t id;
    int status;
    char *json;
} response_t;
static char *setting(nvs_handle_t h, const char *name) {
    size_t size = 0;
    if (nvs_get_str(h, name, NULL, &size) != ESP_OK || size > 8192)
        return NULL;
    char *p = malloc(size);
    if (p && nvs_get_str(h, name, p, &size) != ESP_OK) {
        free(p);
        p = NULL;
    }
    return p;
}
bool sp_web_ap(unsigned channel) {
    nvs_handle_t h;
    if (nvs_open("web", NVS_READONLY, &h) != ESP_OK)
        return false;
    char *admin = setting(h, "admin");
    nvs_close(h);
    size_t admin_len = admin ? strlen(admin) : 0;
    bool valid = admin && (admin_len == 32 || admin_len == 64);
    if (valid) {
        wifi_config_t c = {0};
        snprintf((char *)c.ap.ssid, sizeof(c.ap.ssid), "SmartPlantOS");
        snprintf((char *)c.ap.password, sizeof(c.ap.password), "%s", SP_SETUP_AP_PASSWORD);
        c.ap.channel = channel;
        c.ap.max_connection = 3;
        c.ap.authmode = WIFI_AUTH_WPA2_PSK;
        valid = esp_netif_create_default_wifi_ap() && esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
                esp_wifi_set_config(WIFI_IF_AP, &c) == ESP_OK;
        if (valid)
            snprintf(password, sizeof(password), "%s", admin);
    }
    free(admin);
    if (!valid)
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
    return valid;
}
static bool equal(const char *a, const char *b) {
    size_t len = strlen(a);
    if (len != strlen(b) || (len != 32 && len != 64))
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return !diff;
}
static esp_err_t send_json(httpd_req_t *r, int status, const char *s) {
    char code[24];
    snprintf(code, sizeof(code), "%d Response", status);
    httpd_resp_set_status(r, code);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    return httpd_resp_sendstr(r, s);
}
static esp_err_t fail(httpd_req_t *r, int status, const char *why) {
    char text[180];
    snprintf(text, sizeof(text), "{\"schema_version\":1,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", why,
             why);
    return send_json(r, status, text);
}
static bool body(httpd_req_t *r, char *out) {
    if (r->content_len > SP_API_BODY_MAX)
        return false;
    size_t n = 0;
    while (n < r->content_len) {
        int got = httpd_req_recv(r, out + n, r->content_len - n);
        if (got <= 0)
            return false;
        n += (size_t)got;
    }
    if (memchr(out, 0, n))
        return false;
    out[n] = 0;
    return true;
}
/* Decide by the accepted socket, never by an untrusted Host header.
 * The setup AP always serves the wizard, including during recovery. */
static bool home_interface(httpd_req_t *r) {
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.ip.addr ||
        getsockname(httpd_req_to_sockfd(r), (struct sockaddr *)&local, &length) != 0)
        return false;
    if (local.ss_family == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&local;
        return sp_web_home_address((const uint8_t *)&v4->sin_addr, 4, (const uint8_t *)&ip.ip.addr);
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        return sp_web_home_address((const uint8_t *)&v6->sin6_addr, 16, (const uint8_t *)&ip.ip.addr);
    }
#endif
    return false;
}
static esp_err_t handle(httpd_req_t *r) {
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src "
                       "'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    if (r->method == HTTP_GET && (!strcmp(r->uri, "/demo") || !strcmp(r->uri, "/demo.html"))) {
        httpd_resp_set_hdr(r, "Content-Security-Policy",
                           "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                           "connect-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        return httpd_resp_send(r, (const char *)demo_start, demo_end - demo_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/demo.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)demo_js_start, demo_js_end - demo_js_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/api.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)api_js_start, api_js_end - api_js_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/style.css")) {
        httpd_resp_set_type(r, "text/css; charset=utf-8");
        return httpd_resp_send(r, (const char *)style_start, style_end - style_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/setup")) {
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        return httpd_resp_send(r, (const char *)setup_start, setup_end - setup_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/setup.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)setup_js_start, setup_js_end - setup_js_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/")) {
        if (!home_interface(r)) {
            httpd_resp_set_status(r, "303 See Other");
            httpd_resp_set_hdr(r, "Location", "/setup");
            return httpd_resp_sendstr(r, "");
        }
        httpd_resp_set_type(r, "text/html; charset=utf-8");
        return httpd_resp_send(r, (const char *)page_start, page_end - page_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/live.js")) {
        httpd_resp_set_type(r, "application/javascript");
        return httpd_resp_send(r, (const char *)script_start, script_end - script_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/pairing.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)pairing_js_start, pairing_js_end - pairing_js_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/app.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)app_start, app_end - app_start);
    }
    if (r->method == HTTP_GET && !strcmp(r->uri, "/updates.js")) {
        httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        return httpd_resp_send(r, (const char *)updates_start, updates_end - updates_start);
    }
    if (!origin_allowed(r))
        return fail(r, 403, "origin_rejected");
    if (r->method != HTTP_GET && r->method != HTTP_DELETE) {
        char type[48];
        if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
            strcmp(type, "application/json"))
            return fail(r, 415, "json_required");
    }
    sp_api_request_t *q = calloc(1, sizeof(*q));
    if (!q)
        return fail(r, 503, "busy");
    if (!body(r, q->body)) {
        free(q);
        return fail(r, 400, "invalid_body");
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (r->method == HTTP_POST && !strcmp(r->uri, "/api/v1/session")) {
        if (now - attempt_window >= 60000) {
            attempt_window = now;
            attempts = 0;
        }
        if (attempts++ >= 5) {
            free(q);
            return fail(r, 429, "login_rate_limit");
        }
        cJSON *j = cJSON_ParseWithOpts(q->body, NULL, true);
        const char *p = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "password"));
        bool ok = cJSON_IsObject(j) && cJSON_GetArraySize(j) == 1 && p && equal(p, password);
        cJSON_Delete(j);
        free(q);
        if (!ok)
            return fail(r, 401, "unauthorized");
        uint8_t random[32];
        esp_fill_random(random, sizeof(random));
        for (unsigned i = 0; i < 32; i++)
            snprintf(token + 2 * i, 3, "%02x", random[i]);
        expires = now + 900000;
        char cookie[160];
        snprintf(cookie, sizeof(cookie),
                 "sp_session=%s; Path=/; Max-Age=900; HttpOnly; Secure; SameSite=Strict", token);
        httpd_resp_set_hdr(r, "Set-Cookie", cookie);
        char result[150];
        snprintf(result, sizeof(result), "{\"schema_version\":1,\"token\":\"%s\",\"expires_in\":900}", token);
        return send_json(r, 200, result);
    }
    char auth[80], cookie[256];
    bool authorized = httpd_req_get_hdr_value_str(r, "Authorization", auth, sizeof(auth)) == ESP_OK &&
                      !strncmp(auth, "Bearer ", 7) && equal(auth + 7, token);
    if (!authorized && httpd_req_get_hdr_value_str(r, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
        char *p = cookie;
        while (*p) {
            while (*p == ' ' || *p == ';')
                p++;
            char *end = strchr(p, ';');
            if (end)
                *end = 0;
            if (!strncmp(p, "sp_session=", 11) && equal(p + 11, token))
                authorized = true;
            if (!end)
                break;
            p = end + 1;
        }
    }
    if (now >= expires || !authorized) {
        free(q);
        return fail(r, 401, "unauthorized");
    }
    if (r->method == HTTP_DELETE && !strcmp(r->uri, "/api/v1/session")) {
        token[0] = 0;
        expires = 0;
        httpd_resp_set_hdr(r, "Set-Cookie",
                           "sp_session=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict");
        free(q);
        return send_json(r, 200, "{\"schema_version\":1}");
    }
    if (strlen(r->uri) >= sizeof(q->path)) {
        free(q);
        return fail(r, 414, "path_too_long");
    }
    strcpy(q->path, r->uri);
    snprintf(q->method, sizeof(q->method), "%s",
             r->method == HTTP_GET    ? "GET"
             : r->method == HTTP_POST ? "POST"
             : r->method == HTTP_PUT  ? "PUT"
                                      : "DELETE");
    if (httpd_req_get_hdr_value_len(r, "If-Match") &&
        httpd_req_get_hdr_value_str(r, "If-Match", q->if_match, sizeof(q->if_match)) != ESP_OK) {
        free(q);
        return fail(r, 400, "invalid_revision");
    }
    q->id = ++serial;
    if (!q->id)
        q->id = ++serial;
    uint32_t id = q->id;
    response_t previous;
    while (xQueueReceive(outgoing, &previous, 0) == pdTRUE)
        free(previous.json);
    bool queued = xQueueSend(incoming, q, 0) == pdTRUE;
    free(q);
    if (!queued)
        return fail(r, 503, "busy");
    int64_t deadline = now + 1000;
    while (esp_timer_get_time() / 1000 < deadline) {
        response_t result;
        if (xQueueReceive(outgoing, &result, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (result.id != id) {
                free(result.json);
                continue;
            }
            esp_err_t err = send_json(r, result.status, result.json);
            free(result.json);
            return err;
        }
    }
    return fail(r, 503, "request_outcome_unknown");
}
bool sp_web_start(void) {
    if (!password[0])
        return false;
    nvs_handle_t h;
    if (nvs_open("web", NVS_READONLY, &h) != ESP_OK)
        return false;
    char *cert = setting(h, "cert"), *key = setting(h, "key");
    nvs_close(h);
    if (!cert || !key) {
        free(cert);
        free(key);
        return false;
    }
    incoming = xQueueCreate(2, sizeof(sp_api_request_t));
    outgoing = xQueueCreate(2, sizeof(response_t));
    if (!incoming || !outgoing) {
        free(cert);
        free(key);
        return false;
    }
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.servercert = (const unsigned char *)cert;
    config.servercert_len = strlen(cert) + 1;
    config.prvtkey_pem = (const unsigned char *)key;
    config.prvtkey_len = strlen(key) + 1;
    config.httpd.stack_size = 12288;
    config.httpd.max_open_sockets = 3;
    config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 2;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server;
    esp_err_t err = httpd_ssl_start(&server, &config);
    /* Server config keeps certificate pointers for future TLS connections. */
    if (err != ESP_OK) {
        free(cert);
        free(key);
        return false;
    }
    const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE};
    for (unsigned i = 0; i < 4; i++) {
        httpd_uri_t uri = {.uri = "/*", .method = methods[i], .handler = handle};
        if (httpd_register_uri_handler(server, &uri) != ESP_OK)
            return false;
    }
    return true;
}
bool sp_web_take(sp_api_request_t *r) {
    return incoming && xQueueReceive(incoming, r, 0) == pdTRUE;
}
void sp_web_reply(void *ctx, uint32_t id, int status, cJSON *o) {
    (void)ctx;
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json)
        return;
    if (strlen(json) > SP_API_REPLY_MAX) {
        free(json);
        return;
    }
    response_t r = {.id = id, .status = status, .json = json};
    if (!outgoing || xQueueSend(outgoing, &r, 0) != pdTRUE)
        free(json);
}
#else
bool sp_web_ap(unsigned channel) {
    (void)channel;
    return false;
}
bool sp_web_start(void) {
    return false;
}
bool sp_web_take(sp_api_request_t *r) {
    (void)r;
    return false;
}
void sp_web_reply(void *ctx, uint32_t id, int status, cJSON *o) {
    (void)ctx;
    (void)id;
    (void)status;
    cJSON_Delete(o);
}
#endif
