#include "sp_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    bool used;
    char ns[16], key[16];
    size_t n;
    uint8_t data[16384];
} entry_t;
static entry_t entries[8];
static bool fail_commit;
static entry_t staged;
static cJSON *response;
static const uint8_t device[6] = {1, 2, 3, 4, 5, 6};
static char current_ns[16];
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    bool exists = false;
    for (unsigned i = 0; i < 8; i++)
        if (entries[i].used && !strcmp(entries[i].ns, ns))
            exists = true;
    if (!exists && mode == NVS_READONLY)
        return ESP_ERR_NVS_NOT_FOUND;
    snprintf(current_ns, sizeof(current_ns), "%s", ns);
    *h = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) {
    (void)h;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *n) {
    (void)h;
    for (unsigned i = 0; i < 8; i++)
        if (entries[i].used && !strcmp(entries[i].ns, current_ns) && !strcmp(entries[i].key, key)) {
            if (*n < entries[i].n)
                return ESP_ERR_INVALID_SIZE;
            memcpy(data, entries[i].data, entries[i].n);
            *n = entries[i].n;
            return ESP_OK;
        }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n) {
    (void)h;
    assert(n <= sizeof(staged.data));
    memset(&staged, 0, sizeof(staged));
    staged.used = true;
    snprintf(staged.ns, sizeof(staged.ns), "%s", current_ns);
    snprintf(staged.key, sizeof(staged.key), "%s", key);
    staged.n = n;
    memcpy(staged.data, data, n);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    if (fail_commit)
        return ESP_FAIL;
    unsigned slot = 8;
    for (unsigned i = 0; i < 8; i++)
        if (entries[i].used && !strcmp(entries[i].ns, staged.ns) && !strcmp(entries[i].key, staged.key)) {
            slot = i;
            break;
        }
    if (slot == 8)
        for (unsigned i = 0; i < 8; i++)
            if (!entries[i].used) {
                slot = i;
                break;
            }
    assert(slot < 8);
    entries[slot] = staged;
    return ESP_OK;
}
void sp_web_reply(void *ctx, uint32_t id, int status, cJSON *o) {
    (void)ctx;
    (void)id;
    assert(status == 200);
    cJSON_Delete(response);
    response = o;
}
static void fixture(sp_identity_t *id, sp_api_t *api) {
    memset(id, 0, sizeof(*id));
    memset(api, 0, sizeof(*api));
    id->hub = true;
    id->count = api->count = 1;
    memcpy(id->peers[0].device, device, 6);
    memcpy(api->nodes[0].device, device, 6);
}
int main(void) {
    sp_identity_t id;
    sp_api_t api;
    fixture(&id, &api);
    sp_plant_meta_v1_t old = {.version = 1,
                              .revision = 4,
                              .plant = {1, 2, 3, 4, 5, 6, 0x40, 8, 0x80, 10, 11, 12, 13, 14, 15, 16},
                              .name = "Old plant",
                              .species = "monstera"};
    nvs_handle_t h;
    assert(nvs_open("plants", NVS_READWRITE, &h) == ESP_OK);
    assert(nvs_set_blob(h, "010203040506", &old, sizeof(old)) == ESP_OK);
    assert(nvs_commit(h) == ESP_OK);
    assert(sp_catalog_load(&id, &api));
    assert(api.nodes[0].have_plant && api.nodes[0].plant.version == 2);
    assert(api.nodes[0].plant.revision == 4 && !memcmp(api.nodes[0].plant.plant, old.plant, 16));
    sp_plant_meta_t next = api.nodes[0].plant;
    snprintf(next.name, sizeof(next.name), "Monstera");
    snprintf(next.room, sizeof(next.room), "Living room");
    next.revision++;
    next.setup_stage = 2;
    fail_commit = true;
    assert(!sp_catalog_save(device, &next));
    fail_commit = false;
    fixture(&id, &api);
    assert(sp_catalog_load(&id, &api));
    assert(api.nodes[0].plant.revision == 4);
    assert(sp_catalog_save(device, &next));
    fixture(&id, &api);
    assert(sp_catalog_load(&id, &api));
    assert(!strcmp(api.nodes[0].plant.room, "Living room"));
    assert(!strcmp(api.nodes[0].plant.name, "Monstera") && api.nodes[0].plant.setup_stage == 2);
    assert(sp_catalog_archive(device));
    sp_api_request_t q = {.id = 1, .method = "GET", .path = "/api/v1/plants"};
    assert(sp_catalog_handle(&api, &q));
    cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItem(response, "plants"), 0);
    assert(cJSON_IsNull(cJSON_GetObjectItem(row, "device_id")));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "name")), "Monstera"));
    /* The legacy blob remains intact so rollback firmware can still read it. */
    assert(nvs_open("plants", NVS_READONLY, &h) == ESP_OK);
    sp_plant_meta_v1_t preserved;
    size_t n = sizeof(preserved);
    assert(nvs_get_blob(h, "010203040506", &preserved, &n) == ESP_OK);
    assert(!memcmp(&old, &preserved, sizeof(old)));
    /* A fresh controller replaces the archived binding without replacing plant identity. */
    const uint8_t replacement[6] = {7, 8, 9, 10, 11, 12};
    sp_plant_meta_t fresh = {.version = 2,
                             .plant = {7, 8, 9, 10, 11, 12, 0x40, 1, 0x80, 1, 2, 3, 4, 5, 6, 7},
                             .name = "Fresh controller"};
    assert(sp_catalog_save(replacement, &fresh));
    memset(&api, 0, sizeof(api));
    api.count = 1;
    memcpy(api.nodes[0].device, replacement, 6);
    api.nodes[0].plant = fresh;
    api.nodes[0].have_plant = true;
    sp_api_request_t bind = {.id = 2,
                             .method = "POST",
                             .path = "/api/v1/plants/01020304-0506-4008-800a-0b0c0d0e0f10/controller",
                             .body =
                                 "{\"schema_version\":1,\"revision\":5,\"device_id\":\"spo-0708090a0b0c\"}"};
    assert(sp_catalog_handle(&api, &bind));
    assert(!memcmp(api.nodes[0].plant.plant, old.plant, 16));
    assert(!strcmp(api.nodes[0].plant.name, "Monstera") && !strcmp(api.nodes[0].plant.room, "Living room"));
    assert(api.nodes[0].plant.revision == 6 && api.nodes[0].plant.setup_stage == 2);
    memset(&id, 0, sizeof(id));
    id.hub = true;
    id.count = 1;
    memcpy(id.peers[0].device, replacement, 6);
    memset(&api, 0, sizeof(api));
    api.count = 1;
    memcpy(api.nodes[0].device, replacement, 6);
    assert(sp_catalog_load(&id, &api));
    assert(!memcmp(api.nodes[0].plant.plant, old.plant, 16));
    assert(sp_catalog_handle(&api, &q));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(response, "plants")) == 1);
    row = cJSON_GetArrayItem(cJSON_GetObjectItem(response, "plants"), 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "device_id")), "spo-0708090a0b0c"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(row, "controller_history")) == 2);
    next.setup_stage = 4;
    assert(!sp_catalog_save(device, &next));
    cJSON_Delete(response);
    puts("PASS: metadata v1 migration, durable room/setup state, failed commit, archive and legacy "
         "preservation");
}
