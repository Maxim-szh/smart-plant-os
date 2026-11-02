#include "sp_epochs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {
    char key[16];
    uint32_t value;
} values[32];
static unsigned count;
static uint8_t marker;
static bool fail;
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) {
    (void)h;
    (void)key;
    if (!marker)
        return ESP_ERR_NVS_NOT_FOUND;
    *v = marker;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    (void)h;
    (void)key;
    marker = v;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v) {
    (void)h;
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(key, values[i].key)) {
            *v = values[i].value;
            return ESP_OK;
        }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) {
    (void)h;
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(key, values[i].key)) {
            values[i].value = v;
            return ESP_OK;
        }
    assert(count < 32);
    snprintf(values[count].key, 16, "%s", key);
    values[count++].value = v;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return fail ? ESP_FAIL : ESP_OK;
}
int main(void) {
    sp_identity_t id = {.count = 2};
    memcpy(id.peers[0].device, "AAAAAA", 6);
    memcpy(id.peers[1].device, "BBBBBB", 6);
    nvs_set_u32(0, "p0boot", 42);
    nvs_set_u32(0, "p1boot", 6);
    assert(sp_peer_epochs_load(&id));
    assert(id.peers[0].boot == 42 && id.peers[1].boot == 6 && marker == 1);
    id.peers[0] = id.peers[1];
    id.count = 1;
    id.peers[0].boot = 0;
    assert(sp_peer_epochs_load(&id));
    assert(id.peers[0].boot == 6);
    memcpy(id.peers[0].device, "CCCCCC", 6);
    assert(sp_peer_epochs_load(&id));
    assert(id.peers[0].boot == 0);
    assert(sp_peer_epoch_store(0, id.peers[0].device, 3));
    assert(sp_peer_epochs_load(&id));
    assert(id.peers[0].boot == 3);
    memcpy(id.peers[0].device, "BBBBBB", 6);
    assert(sp_peer_epochs_load(&id));
    assert(id.peers[0].boot == 6);
    fail = true;
    assert(!sp_peer_epoch_store(0, id.peers[0].device, 7));
    fail = false;
    marker = 2;
    assert(!sp_peer_epochs_load(&id));
    puts("PASS: epoch migration, slot compaction, new device isolation, persistence failure and unknown "
         "version");
}
