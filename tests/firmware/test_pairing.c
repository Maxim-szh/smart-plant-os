#include "sp_pairing.h"
#include "sp_join_crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint64_t now = 100;
static unsigned random_state = 17;
static sp_identity_t hub = {.hub = true}, node = {.hub = false}, *active;
static uint8_t hub_mac[6] = {2, 1, 2, 3, 4, 5}, node_mac[6] = {2, 6, 7, 8, 9, 10};
static uint8_t seed_blob[256], pending_blob[512], transmitted[250];
static size_t seed_size, pending_size, transmitted_size;
static bool fail_storage, fail_link;
static unsigned added;
esp_err_t sp_radio_pair_pmk(const uint8_t key[16]) {
    memcpy(active->pmk, key, 16);
    return ESP_OK;
}
int64_t esp_timer_get_time(void) {
    return (int64_t)(now * 1000);
}
void esp_fill_random(void *out, size_t n) {
    uint8_t *b = out;
    while (n--) {
        random_state = random_state * 1664525u + 1013904223u;
        *b++ = (uint8_t)(random_state >> 24);
    }
}
int esp_read_mac(uint8_t *out, int kind) {
    (void)kind;
    memcpy(out, active->hub ? hub_mac : node_mac, 6);
    return 0;
}
void sp_api_device_id(const uint8_t id[6], char out[17]) {
    snprintf(out, 17, "spo-%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5]);
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    (void)name;
    (void)mode;
    *h = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) {
    (void)h;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size) {
    (void)h;
    bool is_seed = !strcmp(key, "node_v1");
    size_t n = is_seed ? seed_size : pending_size;
    if (!n)
        return ESP_ERR_NVS_NOT_FOUND;
    if (*size < n)
        return ESP_ERR_INVALID_SIZE;
    memcpy(out, is_seed ? seed_blob : pending_blob, n);
    *size = n;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size) {
    (void)h;
    if (fail_storage)
        return ESP_FAIL;
    bool is_seed = !strcmp(key, "node_v1");
    assert(size <= (is_seed ? sizeof(seed_blob) : sizeof(pending_blob)));
    memcpy(is_seed ? seed_blob : pending_blob, in, size);
    if (is_seed)
        seed_size = size;
    else
        pending_size = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return fail_storage ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    if (fail_storage)
        return ESP_FAIL;
    if (!strcmp(key, "node_v1"))
        seed_size = 0;
    else
        pending_size = 0;
    return ESP_OK;
}
esp_err_t sp_radio_add_peer(const sp_peer_t *p) {
    if (fail_link)
        return ESP_FAIL;
    assert(active->count < SP_PEER_MAX);
    active->peers[active->count++] = *p;
    added++;
    return ESP_OK;
}
bool sp_radio_pair_send(const uint8_t *data, size_t size) {
    assert(size <= 250);
    memcpy(transmitted, data, size);
    transmitted_size = size;
    return true;
}
static void start(sp_identity_t *id) {
    active = id;
    assert(sp_pairing_start(id));
    transmitted_size = 0;
}
static cJSON *status(void) {
    cJSON *o = cJSON_CreateObject();
    sp_pairing_json(o);
    return o;
}
static bool phase(const char *text) {
    cJSON *o = status();
    bool ok = !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(o, "phase")), text);
    cJSON_Delete(o);
    return ok;
}
int main(void) {
    memcpy(hub.self, "HUB123", 6);
    memcpy(node.self, "NODE12", 6);
    start(&node);
    sp_pairing_tick();
    assert(transmitted_size == SP_JOIN_BEACON_SIZE);
    uint8_t beacon[SP_JOIN_BEACON_SIZE];
    memcpy(beacon, transmitted, sizeof(beacon));
    /* Reboot before selection preserves the Node's first-use key and nonce. */
    start(&node);
    sp_pairing_tick();
    assert(!memcmp(beacon, transmitted, sizeof(beacon)));
    start(&hub);
    assert(phase("idle"));
    assert(sp_pairing_receive(node_mac, beacon, sizeof(beacon)));
    cJSON *o = status();
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(o, "candidates")) == 0);
    cJSON_Delete(o);
    assert(sp_pairing_open() == ESP_OK);
    sp_pairing_receive(node_mac, beacon, sizeof(beacon));
    o = status();
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(o, "candidates")) == 1);
    cJSON_Delete(o);
    fail_storage = true;
    assert(sp_pairing_begin(node.self) != ESP_OK);
    assert(hub.count == 0);
    fail_storage = false;
    assert(sp_pairing_begin(node.self) == ESP_OK);
    assert(hub.count == 1 && pending_size);
    sp_pairing_tick();
    assert(transmitted_size == SP_JOIN_OFFER_SIZE);
    uint8_t offer[SP_JOIN_OFFER_SIZE];
    memcpy(offer, transmitted, sizeof(offer));
    /* Hub restart resumes exactly the committed transaction and cannot rotate keys. */
    start(&hub);
    assert(phase("connecting"));
    sp_pairing_tick();
    assert(!memcmp(offer, transmitted, sizeof(offer)));
    assert(hub.count == 1);
    start(&node);
    uint8_t altered[SP_JOIN_OFFER_SIZE];
    memcpy(altered, offer, sizeof(offer));
    altered[SP_JOIN_OFFER_PREFIX] ^= 1;
    sp_pairing_receive(hub_mac, altered, sizeof(altered));
    assert(node.count == 0);
    now += 1100;
    fail_link = true;
    sp_pairing_receive(hub_mac, offer, sizeof(offer));
    assert(node.count == 0);
    fail_link = false;
    now += 1100;
    sp_pairing_receive(hub_mac, offer, sizeof(offer));
    assert(node.count == 1 && phase("paired"));
    assert(!seed_size);
    assert(!memcmp(node.peers[0].lmk, hub.peers[0].lmk, 16));
    assert(!memcmp(node.peers[0].app_key, hub.peers[0].app_key, 16));
    sp_pairing_tick();
    assert(transmitted_size == SP_JOIN_ACK_SIZE);
    uint8_t ack[SP_JOIN_ACK_SIZE];
    memcpy(ack, transmitted, sizeof(ack));
    unsigned count = added;
    now += 1100;
    sp_pairing_receive(hub_mac, offer, sizeof(offer));
    assert(added == count);
    start(&hub);
    ack[80] ^= 1;
    sp_pairing_receive(node_mac, ack, sizeof(ack));
    assert(phase("connecting"));
    ack[80] ^= 1;
    sp_pairing_receive(node_mac, ack, sizeof(ack));
    assert(phase("paired") && !pending_size);
    assert(sp_pairing_begin(node.self) != ESP_OK);
    start(&node);
    sp_pairing_tick();
    assert(transmitted_size == 0); /* Bound Nodes never advertise again. */
    start(&hub);
    assert(sp_pairing_open() == ESP_OK);
    now += 120001;
    sp_pairing_tick();
    assert(phase("idle"));
    sp_pairing_receive(node_mac, beacon, sizeof(beacon));
    o = status();
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(o, "candidates")) == 0);
    cJSON_Delete(o);
    puts("PASS: discovery window, restart recovery, storage failure, proof rejection, pairing, idempotency "
         "and no bound advertising");
}
