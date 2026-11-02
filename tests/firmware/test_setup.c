#include "sp_setup.h"
#include "sp_web.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void sp_pairing_json(cJSON *o) {
    cJSON_AddStringToObject(o, "phase", "idle");
}
esp_err_t sp_pairing_open(void) {
    return ESP_OK;
}
esp_err_t sp_pairing_retry(void) {
    return ESP_OK;
}
esp_err_t sp_pairing_begin(const uint8_t device[6]) {
    (void)device;
    return ESP_OK;
}
static cJSON *output;
static int status, wifi_calls, pair_calls;
static esp_err_t result;
uint8_t sp_radio_channel(void) {
    return 6;
}
int esp_read_mac(uint8_t *mac, int kind) {
    (void)kind;
    memcpy(mac, "\x02\x00\x00\x00\x00\x01", 6);
    return 0;
}
void sp_web_reply(void *ctx, uint32_t id, int code, cJSON *o) {
    (void)ctx;
    (void)id;
    cJSON_Delete(output);
    output = o;
    status = code;
}
void sp_radio_network_json(cJSON *o) {
    cJSON_AddStringToObject(o, "wifi_phase", "idle");
}
esp_err_t sp_radio_network_begin(const char *ssid, const char *password) {
    assert(ssid && password);
    wifi_calls++;
    return result;
}
esp_err_t sp_radio_add_peer(const sp_peer_t *p) {
    assert(p->mac[0] == 2);
    assert(p->lmk[0] == 0x11);
    pair_calls++;
    return result;
}
static void request(sp_identity_t *id, sp_api_t *api, const char *method, const char *path,
                    const char *body) {
    sp_api_request_t q = {0};
    snprintf(q.method, sizeof(q.method), "%s", method);
    snprintf(q.path, sizeof(q.path), "%s", path);
    snprintf(q.body, sizeof(q.body), "%s", body ? body : "");
    assert(sp_setup_handle(id, api, &q));
}
int main(void) {
    sp_identity_t id = {.hub = true, .channel = 6};
    memcpy(id.self, "\x01\x02\x03\x04\x05\x06", 6);
    memcpy(id.country, "01", 3);
    sp_api_t api = {0};
    request(&id, &api, "GET", "/api/v1/setup", NULL);
    assert(status == 200);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(output, "nodes")) == 0);
    assert(!cJSON_GetObjectItem(output, "password"));
    request(&id, &api, "GET", "/api/v1/setup/passport", NULL);
    assert(status == 200);
    assert(cJSON_GetArraySize(output) == 5);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(output, "hub_id")), "spo-010203040506"));
    request(&id, &api, "POST", "/api/v1/setup/wifi", "{\"ssid\":\"example\",\"password\":\"example-pass\"}");
    assert(status == 202 && wifi_calls == 1);
    result = ESP_ERR_INVALID_STATE;
    request(&id, &api, "POST", "/api/v1/setup/wifi", "{\"ssid\":\"example\",\"password\":\"example-pass\"}");
    assert(status == 409);
    request(&id, &api, "POST", "/api/v1/setup/wifi", "{\"ssid\":\"example\",\"password\":\"short\"}");
    assert(status == 400 && wifi_calls == 2);
    request(&id, &api, "POST", "/api/v1/setup/wifi",
            "{\"ssid\":\"example\",\"password\":\"example-pass\",\"extra\":true}");
    assert(status == 400 && wifi_calls == 2);
    const char *valid = "{\"format\":1,\"hub_id\":\"spo-010203040506\",\"node_id\":\"spo-111213141516\","
                        "\"node_mac\":\"02:00:00:00:00:02\",\"lmk\":\"11111111111111111111111111111111\","
                        "\"app_key\":\"22222222222222222222222222222222\"}";
    result = ESP_OK;
    request(&id, &api, "POST", "/api/v1/setup/peers", valid);
    assert(status == 201 && pair_calls == 1);
    char bad[512];
    snprintf(bad, sizeof(bad), "%s", valid);
    char *hub = strstr(bad, "spo-010203040506");
    hub[4] = '9';
    request(&id, &api, "POST", "/api/v1/setup/peers", bad);
    assert(status == 400 && pair_calls == 1);
    snprintf(bad, sizeof(bad), "%s", valid);
    char *mac = strstr(bad, "02:00:00:00:00:02");
    mac[1] = '3';
    request(&id, &api, "POST", "/api/v1/setup/peers", bad);
    assert(status == 400 && pair_calls == 1);
    snprintf(bad, sizeof(bad), "%s", valid);
    char *key = strstr(bad, "22222222222222222222222222222222");
    memset(key, '1', 32);
    request(&id, &api, "POST", "/api/v1/setup/peers", bad);
    assert(status == 400 && pair_calls == 1);
    result = ESP_ERR_INVALID_STATE;
    request(&id, &api, "POST", "/api/v1/setup/peers", valid);
    assert(status == 409 && pair_calls == 2);
    request(&id, &api, "POST", "/api/v1/setup/peers", "{}");
    assert(status == 400 && pair_calls == 2);
    request(&id, &api, "GET", "/api/v1/setup/unknown", NULL);
    assert(status == 404);
    request(&id, &api, "GET", "/api/v1/setup/pairing", NULL);
    assert(status == 200);
    request(&id, &api, "POST", "/api/v1/setup/pairing/search", "{}");
    assert(status == 202);
    request(&id, &api, "POST", "/api/v1/setup/pairing/search", "{\"extra\":1}");
    assert(status == 400);
    request(&id, &api, "POST", "/api/v1/setup/pairing/connect", "{\"device_id\":\"spo-010203040506\"}");
    assert(status == 202);
    request(&id, &api, "POST", "/api/v1/setup/pairing/connect", "{\"device_id\":\"spo-000000000000\"}");
    assert(status == 400);
    request(&id, &api, "POST", "/api/v1/setup/pairing/retry", "{}");
    assert(status == 202);
    cJSON_Delete(output);
    puts("PASS: setup parser, empty Hub, no exported keys, Wi-Fi validation/busy, pairing owner/MAC "
         "validation, duplicate and unknown route");
}
