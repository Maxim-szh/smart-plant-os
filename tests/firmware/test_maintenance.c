#include "sp_maintenance.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t now = 100;
static uint8_t blob[256], staged[256];
static size_t size, staged_size;
static bool fail_store, allow_remove = true, idle = true;
static sp_identity_t hub, node;
static sp_api_t api;
static unsigned sends, removed, archived, prepared, restarted;
static sp_message_t sent;
static cJSON *response;
static int http;
int64_t esp_timer_get_time(void) {
    return (int64_t)(now * 1000);
}
void esp_restart(void) {
    restarted++;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)ns;
    (void)mode;
    *h = 1;
    return size || mode == NVS_READWRITE ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t h) {
    (void)h;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h;
    (void)key;
    if (!size)
        return ESP_ERR_NVS_NOT_FOUND;
    assert(*n >= size);
    memcpy(out, blob, size);
    *n = size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h;
    (void)key;
    assert(n <= sizeof(staged));
    memcpy(staged, in, n);
    staged_size = n;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    if (fail_store)
        return ESP_FAIL;
    memcpy(blob, staged, staged_size);
    size = staged_size;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    (void)key;
    staged_size = 0;
    return ESP_OK;
}
bool sp_pairing_pending(void) {
    return false;
}
bool sp_catalog_archive(const uint8_t device[6]) {
    assert(!memcmp(device, "AAAAAA", 6));
    archived++;
    return true;
}
bool sp_radio_send(unsigned peer, const sp_message_t *m) {
    assert(peer == 0);
    sent = *m;
    sends++;
    return true;
}
esp_err_t sp_radio_prepare_unpair(const uint8_t operation[16]) {
    (void)operation;
    prepared++;
    return fail_store ? ESP_FAIL : ESP_OK;
}
void sp_web_reply(void *ctx, uint32_t id, int code, cJSON *o) {
    (void)ctx;
    (void)id;
    cJSON_Delete(response);
    response = o;
    http = code;
}
static bool is_idle(void *ctx) {
    (void)ctx;
    return idle;
}
static bool remove_slot(void *ctx, unsigned slot) {
    (void)ctx;
    assert(slot == 0);
    if (!allow_remove)
        return false;
    hub.peers[0] = hub.peers[1];
    hub.count--;
    api.nodes[0] = api.nodes[1];
    api.count--;
    removed++;
    return true;
}
static bool save_core(void *ctx, const sp_persist_t *p) {
    (void)ctx;
    (void)p;
    return true;
}
static void pump(void *ctx, bool on, uint32_t duration) {
    (void)ctx;
    (void)duration;
    assert(!on);
}
static const uint8_t uuid[16] = {1, 2, 3, 4, 5, 6, 0x40, 8, 0x80, 10, 11, 12, 13, 14, 15, 16};
static void hub_init(void) {
    assert(
        sp_maintenance_init(&hub, &api, NULL, (sp_maintenance_io_t){.idle = is_idle, .remove = remove_slot}));
}
static void request(const char *path, const char *body) {
    sp_api_request_t q = {.id = 1, .method = "POST"};
    snprintf(q.path, sizeof(q.path), "%s", path);
    snprintf(q.body, sizeof(q.body), "%s", body);
    assert(sp_maintenance_handle(&q));
}
int main(void) {
    sp_core_t core;
    assert(sp_core_init(&core, NULL, save_core, pump, NULL, now));
    node.count = 1;
    assert(sp_maintenance_init(&node, NULL, &core, (sp_maintenance_io_t){0}));
    sp_message_t m, r;
    sp_message_init(&m, SP_MANAGE);
    sp_message_set(&m, SP_K_ACTION, 1);
    sp_message_bytes(&m, SP_K_ID, uuid);
    core.active_slot = 0;
    assert(sp_maintenance_node(&m, &r) && r.value[6] == 1 && prepared == 0);
    core.active_slot = -1;
    core.saved.config.automatic = true;
    core.saved.cooldown_until_utc = 123456;
    assert(sp_maintenance_node(&m, &r) && r.value[6] == 0 && prepared == 1);
    assert(!core.saved.config.automatic && core.saved.cooldown_until_utc == 123456);
    assert(sp_maintenance_node(&m, &r) && prepared == 1);
    now += 1300;
    sp_maintenance_tick();
    assert(restarted == 1);
    memset(&hub, 0, sizeof(hub));
    hub.hub = true;
    hub.count = api.count = 2;
    memcpy(hub.self, "HUB123", 6);
    memcpy(hub.peers[0].device, "AAAAAA", 6);
    memcpy(hub.peers[1].device, "BBBBBB", 6);
    for (unsigned i = 0; i < 2; i++) {
        memcpy(api.nodes[i].device, hub.peers[i].device, 6);
        api.nodes[i].hardware_known = true;
        api.nodes[i].hardware_flags = 32;
        api.nodes[i].registry.have_state = true;
        api.nodes[i].registry.received_ms = now;
    }
    api.now = now;
    hub_init();
    const char *path = "/api/v1/nodes/spo-414141414141/detach";
    const char *body = "{\"schema_version\":1,\"operation_id\":\"01020304-0506-4008-800a-0b0c0d0e0f10\","
                       "\"acknowledge_autonomy\":true}";
    idle = false;
    request(path, body);
    assert(http == 409 && !size);
    idle = true;
    fail_store = true;
    request(path, body);
    assert(http == 503 && !size);
    fail_store = false;
    request(path, body);
    assert(http == 202 && size);
    sp_maintenance_tick();
    assert(sends == 1 && sent.value[0] == SP_MANAGE && !memcmp(sent.bytes[0], uuid, 16));
    sp_message_init(&r, SP_MANAGE_RESULT);
    sp_message_set(&r, SP_K_STATUS, 0);
    sp_message_bytes(&r, SP_K_ID, uuid);
    allow_remove = false;
    assert(sp_maintenance_receive(0, &r));
    assert(!removed && size);
    allow_remove = true;
    hub_init();
    assert(removed == 1 && archived == 1 && !size);
    assert(hub.count == 1 && !memcmp(hub.peers[0].device, "BBBBBB", 6));
    assert(!memcmp(api.nodes[0].device, "BBBBBB", 6));
    cJSON_Delete(response);
    puts("PASS: safe Node detach, busy pump rejection, idempotency, storage failures and Hub cleanup "
         "recovery preserving other peers");
}
