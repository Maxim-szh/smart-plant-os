#include "sp_epochs.h"
#include <stdio.h>
static void key_for(const uint8_t id[6], char key[16]) {
    snprintf(key, 16, "r%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5]);
}
bool sp_peer_epochs_load(sp_identity_t *id) {
    uint8_t migrated = 0;
    esp_err_t e = nvs_get_u8(id->runtime, "ids_v1", &migrated);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
        return false;
    if (e == ESP_OK && migrated != 1)
        return false;
    for (unsigned i = 0; i < id->count; i++) {
        char key[16];
        key_for(id->peers[i].device, key);
        uint32_t epoch = 0;
        e = nvs_get_u32(id->runtime, key, &epoch);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
            return false;
        if (e == ESP_ERR_NVS_NOT_FOUND && !migrated) {
            char legacy[16];
            snprintf(legacy, sizeof(legacy), "p%uboot", i);
            e = nvs_get_u32(id->runtime, legacy, &epoch);
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
                return false;
            if (nvs_set_u32(id->runtime, key, epoch) != ESP_OK)
                return false;
        }
        id->peers[i].boot = epoch;
    }
    if (!migrated)
        return nvs_set_u8(id->runtime, "ids_v1", 1) == ESP_OK && nvs_commit(id->runtime) == ESP_OK;
    return true;
}
bool sp_peer_epoch_store(nvs_handle_t runtime, const uint8_t device[6], uint32_t boot) {
    char key[16];
    key_for(device, key);
    return nvs_set_u32(runtime, key, boot) == ESP_OK && nvs_commit(runtime) == ESP_OK;
}
