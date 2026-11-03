#include "sp_system.h"
#include "sp_web.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static cJSON *result;
static int status, scans, recoveries;
static bool busy, idle = true, recovery = true;
void sp_web_reply(void *ctx, uint32_t id, int code, cJSON *out) {
    (void)ctx;
    (void)id;
    cJSON_Delete(result);
    result = out;
    status = code;
}
bool sp_maintenance_busy(void) {
    return busy;
}
bool sp_pairing_pending(void) {
    return false;
}
static uint64_t now(void) {
    return 50000;
}
static uint32_t heap(void) {
    return 123456;
}
static unsigned reset(void) {
    return 9;
}
static bool available(void *ctx) {
    (void)ctx;
    return idle;
}
static void network(cJSON *o) {
    cJSON_AddBoolToObject(o, "wifi_connected", true);
    cJSON_AddStringToObject(o, "ssid", "PRIVATE SSID");
    cJSON_AddStringToObject(o, "ip", "192.168.30.152");
    cJSON_AddStringToObject(o, "password", "NEVER EXPORT");
    cJSON_AddStringToObject(o, "wifi_phase", "connected");
    cJSON_AddNumberToObject(o, "channel", 6);
    cJSON_AddNumberToObject(o, "rssi_dbm", -49);
    cJSON_AddStringToObject(o, "last_disconnect_reason", "NONE");
    cJSON_AddBoolToObject(o, "setup_ap_enabled", false);
}
static void radio(cJSON *o) {
    cJSON_AddBoolToObject(o, "enabled", true);
    cJSON_AddStringToObject(o, "private_key", "NEVER EXPORT");
}
static cJSON *update(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "phase", "idle");
    cJSON_AddStringToObject(o, "password", "NEVER EXPORT");
    return o;
}
static esp_err_t scan(void) {
    scans++;
    return ESP_OK;
}
static void scan_json(cJSON *o) {
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddStringToObject(o, "phase", "idle");
    cJSON_AddStringToObject(o, "password", "NEVER EXPORT");
}
static bool recover(void) {
    recoveries++;
    return recovery;
}
int main(void) {
    sp_identity_t id = {.hub = true, .count = 1, .self = {1, 2, 3, 4, 5, 6}};
    sp_api_t api = {.count = 1};
    memcpy(api.nodes[0].device, id.self, 6);
    api.nodes[0].health.known = true;
    api.nodes[0].health.reset_reason = 1;
    api.nodes[0].health.received_ms = 40000;
    api.nodes[0].health.uptime_ms = 4000;
    sp_system_io_t io = {.now_ms = now,
                         .free_heap = heap,
                         .min_heap = heap,
                         .reset_reason = reset,
                         .idle = available,
                         .network_json = network,
                         .radio_json = radio,
                         .update_json = update,
                         .scan_begin = scan,
                         .scan_json = scan_json,
                         .recovery_open = recover};
    sp_api_request_t q = {.method = "GET", .path = "/api/v1/diagnostics"};
    assert(sp_system_handle(&id, &api, &q, &io) && status == 200);
    char *json = cJSON_PrintUnformatted(result);
    assert(!strstr(json, "PRIVATE") && !strstr(json, "NEVER EXPORT") && !strstr(json, "192.168") &&
           !strstr(json, "password") && !strstr(json, "ssid"));
    free(json);
    assert(
        !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(result, "hub"), "reset_reason")),
                "usb"));
    assert(!strcmp(sp_system_reset_name(255), "unknown"));
    snprintf(q.path, sizeof(q.path), "/api/v1/setup/wifi/scan");
    snprintf(q.method, sizeof(q.method), "POST");
    snprintf(q.body, sizeof(q.body), "{}");
    busy = true;
    assert(sp_system_handle(&id, &api, &q, &io) && status == 409 && !scans);
    busy = false;
    idle = false;
    assert(sp_system_handle(&id, &api, &q, &io) && status == 409 && !scans);
    idle = true;
    assert(sp_system_handle(&id, &api, &q, &io) && status == 202 && scans == 1);
    snprintf(q.body, sizeof(q.body), "{\"password\":\"bad\"}");
    assert(sp_system_handle(&id, &api, &q, &io) && status == 400 && scans == 1);
    snprintf(q.method, sizeof(q.method), "GET");
    assert(sp_system_handle(&id, &api, &q, &io) && status == 200 && scans == 1);
    snprintf(q.path, sizeof(q.path), "/api/v1/setup/recovery");
    snprintf(q.method, sizeof(q.method), "POST");
    snprintf(q.body, sizeof(q.body), "{}");
    assert(sp_system_handle(&id, &api, &q, &io) && status == 202 && recoveries == 1);
    recovery = false;
    assert(sp_system_handle(&id, &api, &q, &io) && status == 409 && recoveries == 2);
    snprintf(q.path, sizeof(q.path), "/api/v1/backup/restore");
    assert(!sp_system_handle(&id, &api, &q, &io));
    busy = true;
    assert(sp_system_handle(&id, &api, &q, &io) && status == 409);
    cJSON_Delete(result);
    puts("PASS: diagnostic privacy, reset labels, scan/recovery validation and exclusive maintenance");
}
