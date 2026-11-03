#include "sp_system.h"
#include "sp_release.h"
#include "sp_maintenance.h"
#include "sp_pairing.h"
#include "sp_web.h"
#include <stdio.h>
#include <string.h>
const char *sp_system_reset_name(unsigned reason) {
    static const char *names[] = {"unknown",       "power_on", "software",   "panic",    "interrupt_watchdog",
                                  "task_watchdog", "watchdog", "deep_sleep", "brownout", "usb",
                                  "other"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}
static void reply(const sp_api_request_t *q, int status, const char *error) {
    cJSON *o = error ? sp_api_error(error, error) : cJSON_CreateObject();
    if (!error)
        cJSON_AddNumberToObject(o, "schema_version", 1);
    sp_web_reply(NULL, q->id, status, o);
}
static bool empty(const sp_api_request_t *q) {
    cJSON *o = cJSON_ParseWithOpts(q->body, NULL, true);
    bool ok = cJSON_IsObject(o) && !cJSON_GetArraySize(o);
    cJSON_Delete(o);
    return ok;
}
static void diagnostics(sp_identity_t *id, sp_api_t *api, const sp_api_request_t *q,
                        const sp_system_io_t *io) {
    uint64_t now = io->now_ms();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "schema_version", 1);
    cJSON_AddStringToObject(out, "format", "smartplantos.diagnostics");
    cJSON_AddNumberToObject(out, "generated_uptime_ms", now);
    cJSON *hub = cJSON_AddObjectToObject(out, "hub");
    char device[17];
    sp_api_device_id(id->self, device);
    cJSON_AddStringToObject(hub, "device_id", device);
    cJSON_AddStringToObject(hub, "firmware_version", SP_FIRMWARE_VERSION);
    cJSON_AddNumberToObject(hub, "uptime_ms", now);
    cJSON_AddStringToObject(hub, "reset_reason", sp_system_reset_name(io->reset_reason()));
    cJSON_AddNumberToObject(hub, "free_heap_bytes", io->free_heap());
    cJSON_AddNumberToObject(hub, "minimum_free_heap_bytes", io->min_heap());
    cJSON *full = cJSON_CreateObject();
    io->network_json(full);
    cJSON *network = cJSON_AddObjectToObject(out, "network");
    const char *fields[] = {"wifi_connected",         "wifi_phase",      "channel", "rssi_dbm",
                            "last_disconnect_reason", "setup_ap_enabled"};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(full, fields[i]);
        if (v)
            cJSON_AddItemToObject(network, fields[i], cJSON_Duplicate(v, true));
    }
    cJSON_Delete(full);
    cJSON *radio = cJSON_AddObjectToObject(out, "radio");
    cJSON *source_radio = cJSON_CreateObject();
    io->radio_json(source_radio);
    const char *radio_fields[] = {"enabled",           "channel",
                                  "received_packets",  "unknown_packets",
                                  "header_rejections", "authentication_rejections",
                                  "accepted_messages", "tx_attempts",
                                  "tx_completed",      "tx_callback_timeouts"};
    for (unsigned i = 0; i < sizeof(radio_fields) / sizeof(radio_fields[0]); i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(source_radio, radio_fields[i]);
        if (v)
            cJSON_AddItemToObject(radio, radio_fields[i], cJSON_Duplicate(v, true));
    }
    cJSON_Delete(source_radio);
    cJSON *source_update = io->update_json(), *update = cJSON_AddObjectToObject(out, "update");
    const char *update_fields[] = {
        "phase",  "active",         "verifying",   "validated",        "last_result",
        "error",  "received_bytes", "total_bytes", "firmware_version", "data_abi",
        "layout", "profile",        "session_id",  "target_device_id", "image_sha256"};
    for (unsigned i = 0; i < sizeof(update_fields) / sizeof(update_fields[0]); i++) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(source_update, update_fields[i]);
        if (v)
            cJSON_AddItemToObject(update, update_fields[i], cJSON_Duplicate(v, true));
    }
    cJSON_Delete(source_update);
    cJSON *devices = cJSON_AddArrayToObject(out, "devices");
    for (unsigned i = 0; i < api->count; i++) {
        sp_api_node_t *n = &api->nodes[i];
        cJSON *row = cJSON_CreateObject();
        sp_api_device_id(n->device, device);
        cJSON_AddStringToObject(row, "device_id", device);
        cJSON_AddBoolToObject(row, "online", sp_registry_online(&n->registry, now));
        if (n->registry.have_state && now >= n->registry.received_ms)
            cJSON_AddNumberToObject(row, "last_telemetry_age_ms", now - n->registry.received_ms);
        else
            cJSON_AddNullToObject(row, "last_telemetry_age_ms");
        char version[32];
        if (n->hardware_known)
            snprintf(version, sizeof(version), "%u.%u.%u", (unsigned)(n->firmware_number >> 16) & 255,
                     (unsigned)(n->firmware_number >> 8) & 255, (unsigned)n->firmware_number & 255);
        else
            snprintf(version, sizeof(version), "unknown");
        cJSON_AddStringToObject(row, "firmware_version", version);
        if (n->have_rssi) {
            cJSON_AddNumberToObject(row, "rssi_dbm", n->rssi);
            cJSON_AddNumberToObject(row, "rssi_age_ms", now >= n->rssi_at_ms ? now - n->rssi_at_ms : 0);
        } else {
            cJSON_AddNullToObject(row, "rssi_dbm");
            cJSON_AddNullToObject(row, "rssi_age_ms");
        }
        if (n->health.known) {
            cJSON *health = cJSON_AddObjectToObject(row, "health");
            cJSON_AddNumberToObject(health, "age_ms",
                                    now >= n->health.received_ms ? now - n->health.received_ms : 0);
            cJSON_AddNumberToObject(health, "uptime_ms", n->health.uptime_ms);
            cJSON_AddStringToObject(health, "reset_reason", sp_system_reset_name(n->health.reset_reason));
            cJSON_AddNumberToObject(health, "free_heap_bytes", n->health.free_heap);
            cJSON_AddNumberToObject(health, "minimum_free_heap_bytes", n->health.min_heap);
            cJSON_AddBoolToObject(health, "radio_enabled", n->health.radio_enabled);
            cJSON_AddNumberToObject(health, "accepted_messages", n->health.rx_valid);
            cJSON_AddNumberToObject(health, "authentication_rejections", n->health.rx_auth);
            cJSON_AddNumberToObject(health, "tx_callback_timeouts", n->health.tx_timeouts);
        } else
            cJSON_AddNullToObject(row, "health");
        cJSON_AddItemToArray(devices, row);
    }
    sp_web_reply(NULL, q->id, 200, out);
}
bool sp_system_handle(sp_identity_t *id, sp_api_t *api, const sp_api_request_t *q, const sp_system_io_t *io) {
    if (!id->hub)
        return false;
    if (!strcmp(q->path, "/api/v1/diagnostics")) {
        if (strcmp(q->method, "GET"))
            reply(q, 405, "method_not_allowed");
        else
            diagnostics(id, api, q, io);
        return true;
    }
    bool scan = !strcmp(q->path, "/api/v1/setup/wifi/scan"),
         recovery = !strcmp(q->path, "/api/v1/setup/recovery");
    bool backup = !strncmp(q->path, "/api/v1/backup", 14) && !strcmp(q->method, "POST");
    if (!scan && !recovery && !backup)
        return false;
    if (scan && !strcmp(q->method, "GET")) {
        cJSON *o = cJSON_CreateObject();
        io->scan_json(o);
        sp_web_reply(NULL, q->id, 200, o);
        return true;
    }
    if (strcmp(q->method, "POST")) {
        reply(q, 405, "method_not_allowed");
        return true;
    }
    if (sp_maintenance_busy() || sp_pairing_pending() || (io->idle && !io->idle(NULL))) {
        reply(q, 409, "system_busy");
        return true;
    }
    if (backup)
        return false;
    if (!empty(q)) {
        reply(q, 400, "invalid_body");
        return true;
    }
    if (scan) {
        esp_err_t e = io->scan_begin();
        reply(q, e == ESP_OK ? 202 : 409, e == ESP_OK ? NULL : "scan_busy");
    } else {
        bool opened = io->recovery_open();
        reply(q, opened ? 202 : 409, opened ? NULL : "recovery_unavailable");
    }
    return true;
}
