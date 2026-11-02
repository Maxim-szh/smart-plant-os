#include "sp_update.h"
#include "sp_update_wire.h"
#include "sp_image.h"
#include "sp_maintenance.h"
#include "sp_pairing.h"
#include "sp_release.h"
#include "sp_web.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
/* Only the application task mutates update state or writes flash. */
static sp_identity_t *owner;
static sp_api_t *api;
static sp_core_t *core;
static bool (*is_idle)(void *);
static bool verifying, healthy, link_seen, active, receiving, transferring, waiting, boot_scheduled, sha_live;
static uint64_t initialized_at, deadline, restart_at, validated_at, validation_ack_at;
static esp_timer_handle_t guard;
static const esp_partition_t *partition;
static esp_ota_handle_t handle;
static mbedtls_sha256_context sha;
static uint8_t session[16], expected_hash[32], prefix[128], out_data[SP_UPDATE_DATA];
static uint32_t total, received, remote_offset, old_boot;
static unsigned target, profile, attempts, out_type, out_len;
static const char *phase = "idle", *error = "";
static char version[32];
typedef struct {
    uint32_t version, address;
    uint8_t session[16], hash[32];
} update_record_t;
static update_record_t committed;
static bool committed_loaded;
static bool load_record(void) {
    size_t size = sizeof(committed);
    return nvs_get_blob(owner->runtime, "ota_commit", &committed, &size) == ESP_OK &&
           size == sizeof(committed) && committed.version == 1;
}
static bool save_record(void) {
    committed = (update_record_t){.version = 1, .address = partition->address};
    memcpy(committed.session, session, 16);
    memcpy(committed.hash, expected_hash, 32);
    return nvs_set_blob(owner->runtime, "ota_commit", &committed, sizeof(committed)) == ESP_OK &&
           nvs_commit(owner->runtime) == ESP_OK;
}

static uint64_t now(void) {
    return (uint64_t)esp_timer_get_time() / 1000;
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (8 * i));
}
static void guard_timeout(void *arg) {
    (void)arg;
    esp_restart();
}
void sp_update_boot_guard(void) {
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    verifying = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                state == ESP_OTA_IMG_PENDING_VERIFY;
    if (verifying) {
        esp_timer_create_args_t args = {.callback = guard_timeout, .name = "ota_guard"};
        if (esp_timer_create(&args, &guard) != ESP_OK || esp_timer_start_once(guard, 90000000) != ESP_OK)
            esp_restart();
    }
}
bool sp_update_verifying(void) {
    return verifying;
}
bool sp_update_busy(void) {
    return active || verifying;
}
void sp_update_init(sp_identity_t *id, sp_api_t *a, sp_core_t *c, bool (*idle)(void *), bool startup_ready) {
    owner = id;
    api = a;
    core = c;
    is_idle = idle;
    initialized_at = now();
    healthy = startup_ready;
    committed_loaded = load_record();
    esp_ota_img_states_t state;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p && esp_ota_get_state_partition(p, &state) == ESP_OK && state == ESP_OTA_IMG_VALID)
        validated_at = now();
}
void sp_update_link_seen(unsigned peer) {
    (void)peer;
    link_seen = true;
}
static void end_sha(void) {
    if (sha_live) {
        mbedtls_sha256_free(&sha);
        sha_live = false;
    }
}
static void fail(const char *why) {
    if (handle) {
        esp_ota_abort(handle);
        handle = 0;
    }
    end_sha();
    active = receiving = transferring = waiting = false;
    phase = "failed";
    error = why;
}
static bool local_ready(void) {
    return !sp_maintenance_busy() && !sp_pairing_pending() && (!is_idle || is_idle(NULL)) &&
           (!core || (core->storage_ok && core->active_slot < 0 && !core->soak_pending &&
                      !core->saved.config.automatic));
}
static bool local_begin(void) {
    if (!partition || total > partition->size || total < 128)
        return false;
    if (!(owner->hub && profile == 0) &&
        esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK)
        return false;
    mbedtls_sha256_init(&sha);
    sha_live = true;
    if (mbedtls_sha256_starts(&sha, 0)) {
        end_sha();
        return false;
    }
    received = 0;
    receiving = active = true;
    deadline = now() + 60000;
    phase = "uploading";
    error = "";
    return true;
}
static bool append(const uint8_t *p, size_t len) {
    if (!receiving || !len || len > total - received)
        return false;
    if (!received && (len < 128 || !sp_image_header(p, profile, version)))
        return false;
    if (!handle) {
        uint32_t start = (received + 4095u) & ~4095u, end = (received + (uint32_t)len + 4095u) & ~4095u;
        if (start < end && esp_partition_erase_range(partition, start, end - start) != ESP_OK)
            return false;
    }
    esp_err_t e = handle ? esp_ota_write(handle, p, len) : esp_partition_write(partition, received, p, len);
    if (e != ESP_OK || mbedtls_sha256_update(&sha, p, len))
        return false;
    received += (uint32_t)len;
    deadline = now() + 60000;
    return true;
}
static bool finish_local(void) {
    uint8_t hash[32];
    if (!receiving || received != total || mbedtls_sha256_finish(&sha, hash))
        return false;
    end_sha();
    if (memcmp(hash, expected_hash, 32))
        return false;
    if (handle) {
        esp_ota_handle_t h = handle;
        handle = 0;
        if (esp_ota_end(h) != ESP_OK)
            return false;
        if (!save_record())
            return false;
        if (esp_ota_set_boot_partition(partition) != ESP_OK)
            return false;
    }
    receiving = false;
    return true;
}
static bool radio_send(unsigned peer, unsigned type, uint32_t offset, const uint8_t *data, size_t n) {
    if (!owner || peer >= owner->count || !owner->peers[peer].boot || owner->tx_sequence == UINT32_MAX)
        return false;
    sp_update_header_t h = {.role = owner->hub,
                            .type = type,
                            .boot = owner->boot,
                            .sequence = ++owner->tx_sequence,
                            .target_boot = owner->peers[peer].boot,
                            .offset = offset};
    memcpy(h.session, session, 16);
    uint8_t wire[SP_UPDATE_MAX];
    size_t size = 0;
    return sp_update_seal(owner->peers[peer].app_key, &h, data, n, wire, &size) &&
           sp_radio_update_send(peer, wire, size);
}
static void queue_packet(unsigned type, uint32_t offset, const uint8_t *data, size_t n) {
    out_type = type;
    remote_offset = offset;
    out_len = (unsigned)n;
    if (n)
        memcpy(out_data, data, n);
    attempts = 0;
    waiting = true;
    deadline = 0;
}
static void transfer_next(void) {
    if (remote_offset == total) {
        queue_packet(SP_U_FINISH, total, NULL, 0);
        return;
    }
    size_t len = total - remote_offset;
    if (len > SP_UPDATE_DATA)
        len = SP_UPDATE_DATA;
    uint8_t data[SP_UPDATE_DATA];
    if (esp_partition_read(partition, remote_offset, data, len) != ESP_OK) {
        fail("stage_read_failed");
        return;
    }
    queue_packet(SP_U_DATA, remote_offset, data, len);
}
static void ack(unsigned peer, uint8_t code, unsigned stage) {
    uint8_t data[2] = {code, (uint8_t)stage};
    (void)radio_send(peer, SP_U_ACK, received, data, 2);
}
bool sp_update_receive(unsigned peer, const uint8_t *wire, size_t size) {
    if (size < 4 || memcmp(wire, "SPU\1", 4))
        return false;
    if (!owner || peer >= owner->count)
        return true;
    sp_peer_t *p = &owner->peers[peer];
    sp_update_header_t h;
    uint8_t data[SP_UPDATE_DATA];
    size_t len;
    if (!sp_update_open(p->app_key, wire, size, &h, data, &len) || h.role == owner->hub ||
        h.boot != p->boot || h.target_boot != owner->boot || h.sequence <= p->rx_sequence)
        return true;
    p->rx_sequence = h.sequence;
    if (owner->hub) {
        if (!active || peer != target || memcmp(h.session, session, 16) || h.type != SP_U_ACK || len != 2)
            return true;
        if (data[0]) {
            fail(data[0] == 3   ? "node_rolled_back"
                 : data[0] == 1 ? "node_busy_or_automatic"
                                : "node_update_failed");
            return true;
        }
        if (!transferring) {
            if (data[1] == 4 && p->boot != old_boot) {
                active = false;
                phase = "completed";
                error = "";
            }
            return true;
        }
        if (!waiting)
            return true;
        if ((out_type == SP_U_BEGIN && data[1] != 1) || (out_type == SP_U_DATA && data[1] != 2) ||
            (out_type == SP_U_FINISH && data[1] != 3))
            return true;
        uint32_t next = out_type == SP_U_DATA ? remote_offset + out_len : out_type == SP_U_FINISH ? total : 0;
        if (h.offset != next)
            return true;
        waiting = false;
        if (out_type == SP_U_FINISH) {
            transferring = false;
            phase = "rebooting_node";
            deadline = now() + 120000;
        } else {
            remote_offset = next;
            transfer_next();
        }
        return true;
    }
    if (h.type == SP_U_BEGIN) {
        if (active && memcmp(session, h.session, 16) == 0) {
            ack(peer, 0, 1);
            return true;
        }
        if (active || verifying || !local_ready() || len != 166 || h.offset || data[4] != 0 ||
            data[5] != SP_DATA_ABI) {
            uint8_t old[16];
            memcpy(old, session, 16);
            memcpy(session, h.session, 16);
            ack(peer, 1, 1);
            memcpy(session, old, 16);
            return true;
        }
        total = u32(data);
        profile = 0;
        memcpy(expected_hash, data + 6, 32);
        memcpy(prefix, data + 38, 128);
        memcpy(session, h.session, 16);
        partition = esp_ota_get_next_update_partition(NULL);
        if (!sp_image_header(prefix, 0, version) || !local_begin()) {
            fail("node_begin_failed");
            ack(peer, 2, 1);
        } else
            ack(peer, 0, 1);
        return true;
    }
    if (memcmp(session, h.session, 16))
        return true;
    if (h.type == SP_U_ABORT) {
        if (!boot_scheduled)
            fail("cancelled");
        return true;
    }
    if (h.type == SP_U_DATA && receiving) {
        /* A retry carries a fresh authenticated sequence but the same offset. */
        if (h.offset == received) {
            if (!append(data, len)) {
                fail("node_write_failed");
                ack(peer, 2, 2);
                return true;
            }
        } else if (h.offset > received || h.offset + len != received) {
            ack(peer, 2, 2);
            return true;
        }
        uint8_t actual[SP_UPDATE_DATA];
        if (esp_partition_read(partition, h.offset, actual, len) != ESP_OK || memcmp(actual, data, len)) {
            fail("node_chunk_mismatch");
            ack(peer, 2, 2);
            return true;
        }
        ack(peer, 0, 2);
        return true;
    }
    if (h.type == SP_U_FINISH && h.offset == total && !len) {
        if (boot_scheduled) {
            ack(peer, 0, 3);
            return true;
        }
        if (!finish_local()) {
            fail("node_validation_failed");
            ack(peer, 2, 3);
            return true;
        }
        phase = "rebooting";
        boot_scheduled = true;
        restart_at = now() + 1500;
        ack(peer, 0, 3);
    }
    return true;
}
static bool hex(const char *s, uint8_t *out, size_t cap, size_t *size) {
    if (!s || strlen(s) % 2 || strlen(s) / 2 > cap)
        return false;
    *size = strlen(s) / 2;
    for (size_t i = 0; i < *size; i++) {
        unsigned x = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = s[2 * i + j];
            unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
            if (d == 16)
                return false;
            x = x * 16 + d;
        }
        out[i] = (uint8_t)x;
    }
    return true;
}
static void reply(const sp_api_request_t *q, int status, const char *why) {
    cJSON *o = why ? sp_api_error(why, why) : cJSON_CreateObject();
    if (!why)
        cJSON_AddNumberToObject(o, "schema_version", 1);
    sp_web_reply(NULL, q->id, status, o);
}
static void status_reply(const sp_api_request_t *q) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *last = "none";
    if (committed_loaded && !active) {
        memcpy(session, committed.session, 16);
        last = running && running->address == committed.address ? verifying ? "verifying" : "validated"
                                                                : "rolled_back";
    }
    cJSON_AddStringToObject(o, "last_result", last);
    cJSON_AddStringToObject(o, "phase", phase);
    cJSON_AddStringToObject(o, "error", error);
    cJSON_AddBoolToObject(o, "active", active);
    cJSON_AddBoolToObject(o, "verifying", verifying);
    cJSON_AddBoolToObject(o, "validated", !verifying && validated_at != 0);
    cJSON_AddNumberToObject(o, "received_bytes", transferring ? remote_offset : received);
    cJSON_AddNumberToObject(o, "total_bytes", total);
    cJSON_AddStringToObject(o, "firmware_version", SP_FIRMWARE_VERSION);
    cJSON_AddNumberToObject(o, "data_abi", SP_DATA_ABI);
    cJSON_AddStringToObject(o, "layout", esp_ota_get_next_update_partition(NULL) ? "ota-v1" : "factory");
    const char *prof = "c3";
    if (owner->hub) {
        uint32_t flash = 0;
        esp_flash_get_size(NULL, &flash);
        prof = flash == 0x800000 ? "n8r2" : "n16r8";
    }
    cJSON_AddStringToObject(o, "profile", prof);
    char id[37];
    sp_api_uuid(session, id);
    cJSON_AddStringToObject(o, "session_id", id);
    sp_web_reply(NULL, q->id, 200, o);
}
bool sp_update_handle(const sp_api_request_t *q) {
    if (!owner || !owner->hub)
        return false;
    bool ours = !strncmp(q->path, "/api/v1/updates", 15);
    if (!ours) {
        if (sp_update_busy() && strcmp(q->method, "GET")) {
            reply(q, 409, verifying ? "firmware_verifying" : "update_busy");
            return true;
        }
        if (active && strstr(q->path, "/nodes/") &&
            (strstr(q->path, "/config") || strstr(q->path, "/events"))) {
            reply(q, 409, "update_busy");
            return true;
        }
        return false;
    }
    if (!strcmp(q->path, "/api/v1/updates") && !strcmp(q->method, "GET")) {
        status_reply(q);
        return true;
    }
    if (strcmp(q->method, "POST")) {
        reply(q, 405, "method_not_allowed");
        return true;
    }
    cJSON *o = cJSON_ParseWithOpts(q->body, NULL, true);
    if (!cJSON_IsObject(o) || strstr(q->body, "\\u0000")) {
        cJSON_Delete(o);
        reply(q, 400, "invalid_body");
        return true;
    }
    const char *why = NULL;
    int code = 200;
    if (!strcmp(q->path, "/api/v1/updates/begin")) {
        const char *dest = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "device_id"));
        const char *prof = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "profile"));
        double bytes = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "size"));
        size_t n = 0;
        uint8_t hash[32], head[128];
        unsigned selected = prof && !strcmp(prof, "c3")      ? 0
                            : prof && !strcmp(prof, "n8r2")  ? 1
                            : prof && !strcmp(prof, "n16r8") ? 2
                                                             : 3;
        unsigned slot = owner->count;
        for (unsigned i = 0; dest && i < owner->count; i++) {
            char id[17];
            sp_api_device_id(owner->peers[i].device, id);
            if (!strcmp(dest, id))
                slot = i;
        }
        if (active || verifying || !local_ready())
            why = "update_busy";
        else if (cJSON_GetArraySize(o) != 7 ||
                 cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "schema_version")) != 1 || !dest ||
                 selected > 2 || !isfinite(bytes) || bytes != floor(bytes) || bytes < 128 ||
                 bytes > (selected ? 0x200000 : 0x1e0000) ||
                 !hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "sha256")), hash, 32, &n) ||
                 n != 32 ||
                 !hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "header")), head, 128, &n) ||
                 n != 128 ||
                 !sp_api_parse_uuid(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "session_id")),
                                    session))
            why = "invalid_manifest";
        else if ((selected == 0 && (slot >= owner->count || !api ||
                                    !sp_registry_online(&api->nodes[slot].registry, api->now) ||
                                    !(api->nodes[slot].hardware_flags & 16))) ||
                 (selected != 0 && strcmp(dest, "hub")))
            why = "unsupported_target";
        else {
            uint32_t flash = 0;
            esp_flash_get_size(NULL, &flash);
            if ((selected == 1 && flash != 0x800000) || (selected == 2 && flash != 0x1000000) ||
                !sp_image_header(head, selected, version))
                why = "incompatible_image";
            else {
                target = slot;
                profile = selected;
                total = (uint32_t)bytes;
                memcpy(expected_hash, hash, 32);
                memcpy(prefix, head, 128);
                partition = selected
                                ? esp_ota_get_next_update_partition(NULL)
                                : esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "update_stage");
                if (!local_begin())
                    why = "begin_failed";
                else
                    code = 202;
            }
        }
    } else {
        uint8_t sid[16];
        if (!active ||
            !sp_api_parse_uuid(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "session_id")),
                               sid) ||
            memcmp(sid, session, 16))
            why = "unknown_session";
        else if (!strcmp(q->path, "/api/v1/updates/chunk")) {
            uint8_t data[768];
            size_t size = 0;
            double offset = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "offset"));
            if (cJSON_GetArraySize(o) != 3 ||
                !hex(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "data")), data, sizeof(data),
                     &size) ||
                !size || !isfinite(offset) || offset != floor(offset) || offset < 0 || offset > total)
                why = "invalid_chunk";
            else if (offset == received) {
                if (!append(data, size)) {
                    fail("write_failed");
                    why = "write_failed";
                }
            } else if (offset + size != received || !receiving)
                why = "offset_mismatch";
            /* Duplicate chunk retries verify flash contents, never write twice. */
            else {
                uint8_t actual[768];
                if (esp_partition_read(partition, (size_t)offset, actual, size) != ESP_OK ||
                    memcmp(actual, data, size))
                    why = "chunk_mismatch";
            }
        } else if (!strcmp(q->path, "/api/v1/updates/finish")) {
            if (cJSON_GetArraySize(o) != 1 || !finish_local()) {
                fail("image_validation_failed");
                why = "image_validation_failed";
            } else if (profile) {
                phase = "rebooting";
                boot_scheduled = true;
                restart_at = now() + 1500;
                code = 202;
            } else {
                uint8_t begin[166];
                put(begin, total);
                begin[4] = 0;
                begin[5] = SP_DATA_ABI;
                memcpy(begin + 6, expected_hash, 32);
                memcpy(begin + 38, prefix, 128);
                old_boot = owner->peers[target].boot;
                transferring = true;
                phase = "transferring";
                queue_packet(SP_U_BEGIN, 0, begin, sizeof(begin));
                code = 202;
            }
        } else if (!strcmp(q->path, "/api/v1/updates/abort")) {
            if (boot_scheduled)
                why = "already_committed";
            else {
                if (!profile)
                    (void)radio_send(target, SP_U_ABORT, 0, NULL, 0);
                fail("cancelled");
            }
        } else
            why = "not_found";
    }
    cJSON_Delete(o);
    reply(q, why ? 409 : code, why);
    return true;
}
void sp_update_tick(void) {
    if (!owner)
        return;
    uint64_t ms = now();
    if (verifying && healthy && ms - initialized_at >= 30000 && (owner->hub || !owner->count || link_seen)) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            verifying = false;
            validated_at = ms;
            if (guard)
                esp_timer_stop(guard);
            puts("OTA: running image validated; settings preserved");
        }
    }
    if (boot_scheduled && ms >= restart_at)
        esp_restart();
    if (!owner->hub && !active && validated_at && owner->count && link_seen && ms >= validation_ack_at &&
        ms - validated_at < 120000) {
        validation_ack_at = ms + 3000;
        if (committed_loaded) {
            const esp_partition_t *running = esp_ota_get_running_partition();
            memcpy(session, committed.session, 16);
            ack(0, running && running->address == committed.address ? 0 : 3, 4);
        }
    }
    if (!active)
        return;
    if (transferring && waiting && ms >= deadline) {
        if (attempts++ >= 12) {
            fail("radio_timeout");
            return;
        }
        (void)radio_send(target, out_type, remote_offset, out_data, out_len);
        deadline = ms + 700;
    } else if (!transferring && !boot_scheduled && ms >= deadline)
        fail(receiving ? "upload_timeout" : "node_validation_timeout");
}
