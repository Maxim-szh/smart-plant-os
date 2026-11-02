#include "sp_setup.h"
#include "sp_web.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

static bool hex(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != n * 2)
        return false;
    unsigned any = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = s[2 * i + j];
            if (c >= '0' && c <= '9')
                value = value * 16 + c - '0';
            else if (c >= 'a' && c <= 'f')
                value = value * 16 + c - 'a' + 10;
            else
                return false;
        }
        out[i] = value;
        any |= value;
    }
    return any != 0;
}
static bool device(const char *s, uint8_t out[6]) {
    return s && !strncmp(s, "spo-", 4) && hex(s + 4, out, 6);
}
static void reply(const sp_api_request_t *q, int status, const char *error) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    if (error) {
        cJSON *e = cJSON_AddObjectToObject(o, "error");
        cJSON_AddStringToObject(e, "code", error);
        cJSON_AddStringToObject(e, "message", error);
    }
    sp_web_reply(NULL, q->id, status, o);
}
bool sp_setup_handle(sp_identity_t *id, sp_api_t *api, const sp_api_request_t *q) {
    if (strncmp(q->path, "/api/v1/setup", 13))
        return false;
    if (!strcmp(q->method, "GET") && !strcmp(q->path, "/api/v1/setup")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "schema_version", 1);
        cJSON_AddStringToObject(o, "firmware_version", "0.1.0");
        sp_radio_network_json(o);
        cJSON_AddStringToObject(o, "update_method", "usb_app_only");
        cJSON *peers = cJSON_AddArrayToObject(o, "nodes");
        for (unsigned i = 0; i < id->count; i++) {
            char name[17];
            sp_api_device_id(id->peers[i].device, name);
            cJSON *node = cJSON_CreateObject();
            cJSON_AddStringToObject(node, "device_id", name);
            cJSON_AddItemToArray(peers, node);
        }
        sp_web_reply(NULL, q->id, 200, o);
        return true;
    }
    if (!strcmp(q->method, "GET") && !strcmp(q->path, "/api/v1/setup/passport")) {
        cJSON *o = cJSON_CreateObject();
        uint8_t mac[6];
        char address[18], name[17];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        sp_api_device_id(id->self, name);
        cJSON_AddNumberToObject(o, "format", 1);
        cJSON_AddStringToObject(o, "hub_id", name);
        cJSON_AddStringToObject(o, "hub_mac", address);
        cJSON_AddStringToObject(o, "country", id->country);
        cJSON_AddNumberToObject(o, "channel", sp_radio_channel());
        sp_web_reply(NULL, q->id, 200, o);
        return true;
    }
    if (strcmp(q->method, "POST")) {
        reply(q, 404, "not_found");
        return true;
    }
    cJSON *o = cJSON_ParseWithOpts(q->body, NULL, true);
    if (!cJSON_IsObject(o) || strstr(q->body, "\\u0000")) {
        cJSON_Delete(o);
        reply(q, 400, "invalid_body");
        return true;
    }
    if (!strcmp(q->path, "/api/v1/setup/wifi")) {
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "ssid"));
        const char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "password"));
        bool valid = cJSON_GetArraySize(o) == 2 && ssid && pass && strlen(ssid) > 0 && strlen(ssid) <= 32 &&
                     strlen(pass) >= 8 && strlen(pass) <= 63;
        for (const char *p = pass; valid && *p; p++)
            if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
                valid = false;
        esp_err_t err = valid ? sp_radio_network_begin(ssid, pass) : ESP_ERR_INVALID_ARG;
        reply(q,
              err == ESP_OK                  ? 202
              : err == ESP_ERR_INVALID_STATE ? 409
                                             : 400,
              err == ESP_OK                  ? NULL
              : err == ESP_ERR_INVALID_STATE ? "setup_busy"
                                             : "wifi_invalid");
    } else if (!strcmp(q->path, "/api/v1/setup/peers")) {
        sp_peer_t peer = {0};
        uint8_t target[6];
        cJSON *format = cJSON_GetObjectItemCaseSensitive(o, "format");
        const char *address = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "node_mac"));
        char compact[13] = {0};
        unsigned k = 0;
        if (address && strlen(address) == 17)
            for (unsigned i = 0; i < 17; i++) {
                if (i % 3 == 2) {
                    if (address[i] != ':')
                        break;
                } else if (k < 12)
                    compact[k++] = address[i];
            }
        bool valid =
            cJSON_GetArraySize(o) == 6 && cJSON_IsNumber(format) && format->valuedouble == 1 &&
            device(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "hub_id")), target) &&
            !memcmp(target, id->self, 6) &&
            device(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "node_id")), peer.device) &&
            k == 12 && hex(compact, peer.mac, 6) && !(peer.mac[0] & 1) &&
            hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "lmk")), peer.lmk, 16) &&
            hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "app_key")), peer.app_key, 16) &&
            memcmp(peer.lmk, peer.app_key, 16);
        unsigned slot = id->count;
        esp_err_t err = valid ? sp_radio_add_peer(&peer) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && api) {
            memcpy(api->nodes[slot].device, peer.device, 6);
            api->count = id->count;
        }
        reply(q,
              err == ESP_OK                  ? 201
              : err == ESP_ERR_INVALID_STATE ? 409
                                             : 400,
              err == ESP_OK                  ? NULL
              : err == ESP_ERR_INVALID_STATE ? "peer_exists_or_full"
                                             : "pairing_rejected");
    } else
        reply(q, 404, "not_found");
    cJSON_Delete(o);
    return true;
}
