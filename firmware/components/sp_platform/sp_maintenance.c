#include "sp_maintenance.h"
#include "sp_catalog.h"
#include "sp_web.h"
#include "sp_pairing.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
typedef struct {
    uint32_t version, kind, stage;
    uint8_t owner[6], device[6], operation[16];
} transaction_t;
static sp_identity_t *owner;
static sp_api_t *api;
static sp_core_t *core;
static sp_maintenance_io_t io;
static transaction_t transaction;
static bool persisted, node_detaching, ready;
static uint8_t node_operation[16];
static unsigned attempts;
static uint64_t due, restart_at;
static const char *phase = "idle", *message = "";
static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000;
}
static int find_peer(void) {
    for (unsigned i = 0; i < owner->count; i++)
        if (!memcmp(owner->peers[i].device, transaction.device, 6))
            return (int)i;
    return -1;
}
static bool save_transaction(void) {
    nvs_handle_t h;
    if (nvs_open("maint", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok =
        nvs_set_blob(h, "hub_v1", &transaction, sizeof(transaction)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
static bool clear_transaction(void) {
    nvs_handle_t h;
    if (nvs_open("maint", NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t e = nvs_erase_key(h, "hub_v1");
    bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
static void cleanup(void) {
    int slot = find_peer();
    if ((slot >= 0 && (!io.remove || !io.remove(io.ctx, (unsigned)slot))) ||
        !sp_catalog_archive(transaction.device) || !clear_transaction()) {
        phase = "storage_error";
        message =
            "Не удалось завершить сохранение. Повторите продолжение; при ошибке радио перезапустите Hub.";
        return;
    }
    persisted = false;
    phase = "completed";
    message = "Контроллер отвязан. Растение сохранено без контроллера.";
}
bool sp_maintenance_init(sp_identity_t *id, sp_api_t *a, sp_core_t *c, sp_maintenance_io_t callbacks) {
    owner = id;
    api = a;
    core = c;
    io = callbacks;
    persisted = node_detaching = ready = false;
    attempts = 0;
    due = restart_at = 0;
    phase = "idle";
    message = "";
    memset(&transaction, 0, sizeof(transaction));
    if (id->hub) {
        nvs_handle_t h;
        esp_err_t e = nvs_open("maint", NVS_READONLY, &h);
        if (e == ESP_OK) {
            size_t n = sizeof(transaction);
            e = nvs_get_blob(h, "hub_v1", &transaction, &n);
            nvs_close(h);
            if (e != ESP_ERR_NVS_NOT_FOUND &&
                (e != ESP_OK || n != sizeof(transaction) || transaction.version != 1 ||
                 transaction.kind < 1 || transaction.kind > 2 || transaction.stage < 1 ||
                 transaction.stage > 2 || memcmp(transaction.owner, id->self, 6)))
                return false;
        } else if (e != ESP_ERR_NVS_NOT_FOUND)
            return false;
        if (e == ESP_OK) {
            persisted = true;
            if (transaction.stage == 2)
                cleanup();
            else {
                phase = "unknown";
                message = "Предыдущая отвязка не подтверждена. Проверьте контроллер перед повтором.";
            }
        }
    }
    ready = true;
    return true;
}
bool sp_maintenance_busy(void) {
    return !ready || node_detaching || (owner && owner->hub && persisted);
}
static void reply(const sp_api_request_t *q, int status, const char *code) {
    cJSON *o = code ? sp_api_error(code, code) : cJSON_CreateObject();
    if (!code)
        cJSON_AddNumberToObject(o, "schema_version", 1);
    sp_web_reply(NULL, q->id, status, o);
}
static void status(const sp_api_request_t *q) {
    cJSON *o = cJSON_CreateObject();
    char id[37], device[17];
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddStringToObject(o, "phase", ready ? phase : "unavailable");
    cJSON_AddStringToObject(o, "message", message);
    if (transaction.version) {
        sp_api_uuid(transaction.operation, id);
        sp_api_device_id(transaction.device, device);
        cJSON_AddStringToObject(o, "operation_id", id);
        cJSON_AddStringToObject(o, "device_id", device);
    } else {
        cJSON_AddNullToObject(o, "operation_id");
        cJSON_AddNullToObject(o, "device_id");
    }
    sp_web_reply(NULL, q->id, 200, o);
}
bool sp_maintenance_handle(const sp_api_request_t *q) {
    if (!strcmp(q->method, "GET") && !strcmp(q->path, "/api/v1/maintenance")) {
        status(q);
        return true;
    }
    if (!strcmp(q->path, "/api/v1/maintenance/retry") && !strcmp(q->method, "POST")) {
        if (!ready || !persisted) {
            reply(q, 409, "maintenance_unavailable");
            return true;
        }
        if (transaction.stage == 2)
            cleanup();
        else {
            attempts = 0;
            due = 0;
            phase = "waiting";
            message = "Ожидаем подтверждение контроллера.";
        }
        reply(q, 202, NULL);
        return true;
    }
    const char *prefix = "/api/v1/nodes/";
    bool detach = false, forget = false;
    int slot = -1;
    if (!strncmp(q->path, prefix, strlen(prefix))) {
        const char *p = q->path + strlen(prefix);
        if (strlen(p) >= 17 && p[16] == '/') {
            detach = !strcmp(p + 17, "detach");
            forget = !strcmp(p + 17, "forget");
            for (unsigned i = 0; i < owner->count; i++) {
                char id[17];
                sp_api_device_id(owner->peers[i].device, id);
                if (!strncmp(p, id, 16)) {
                    slot = (int)i;
                    break;
                }
            }
        }
    }
    if ((detach || forget) && !strcmp(q->method, "POST")) {
        if (!ready || slot < 0) {
            reply(q, 404, "not_found");
            return true;
        }
        bool online = api && sp_registry_online(&api->nodes[slot].registry, api->now);
        cJSON *o = cJSON_ParseWithOpts(q->body, NULL, true);
        uint8_t op[16];
        bool valid =
            cJSON_IsObject(o) && cJSON_GetArraySize(o) == 3 && !strstr(q->body, "\\u0000") &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "acknowledge_autonomy")) &&
            cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "schema_version")) == 1 &&
            sp_api_parse_uuid(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "operation_id")), op);
        cJSON_Delete(o);
        if (!valid) {
            reply(q, 400, "invalid_body");
            return true;
        }
        if ((detach &&
             (!online || !api->nodes[slot].hardware_known || !(api->nodes[slot].hardware_flags & 32))) ||
            (forget && online)) {
            reply(q, 409, online ? "use_safe_detach" : "node_offline");
            return true;
        }
        if ((io.idle && !io.idle(io.ctx)) || sp_pairing_pending() ||
            (persisted && (!forget || memcmp(transaction.device, owner->peers[slot].device, 6)))) {
            reply(q, 409, "maintenance_busy");
            return true;
        }
        memset(&transaction, 0, sizeof(transaction));
        transaction.version = 1;
        transaction.kind = forget ? 2 : 1;
        transaction.stage = forget ? 2 : 1;
        memcpy(transaction.owner, owner->self, 6);
        memcpy(transaction.device, owner->peers[slot].device, 6);
        memcpy(transaction.operation, op, 16);
        if (!save_transaction()) {
            reply(q, 503, "storage_fault");
            return true;
        }
        persisted = true;
        attempts = 0;
        due = 0;
        phase = "waiting";
        message = "Ожидаем подтверждение контроллера.";
        reply(q, 202, NULL);
        return true;
    }
    if (sp_maintenance_busy() && strncmp(q->path, "/api/v1/maintenance", 19)) {
        bool read = !strcmp(q->method, "GET");
        bool cached = read && (!strcmp(q->path,"/api/v1/diagnostics") || !strcmp(q->path, "/api/v1/nodes") || !strcmp(q->path, "/api/v1/plants") ||
                               strstr(q->path, "/state") || strstr(q->path, "/device") ||
                               strstr(q->path, "/plant") || !strcmp(q->path, "/api/v1/setup"));
        if (!cached) {
            reply(q, 409, "maintenance_busy");
            return true;
        }
    }
    return false;
}
void sp_maintenance_tick(void) {
    if (!ready)
        return;
    uint64_t now = now_ms();
    if (!owner->hub) {
        if (node_detaching && now >= restart_at)
            esp_restart();
        return;
    }
    if (!persisted || strcmp(phase, "waiting"))
        return;
    if (transaction.stage == 2) {
        cleanup();
        return;
    }
    if (now < due)
        return;
    int slot = find_peer();
    if (slot < 0 || attempts >= 3) {
        phase = "unknown";
        message = "Подтверждение не получено. Не считаем контроллер отключённым.";
        return;
    }
    sp_message_t m;
    sp_message_init(&m, SP_MANAGE);
    sp_message_set(&m, SP_K_ACTION, 1);
    sp_message_bytes(&m, SP_K_ID, transaction.operation);
    (void)sp_radio_send((unsigned)slot, &m);
    attempts++;
    due = now + 1500;
}
bool sp_maintenance_receive(unsigned peer, const sp_message_t *m) {
    if (m->value[0] != SP_MANAGE_RESULT)
        return false;
    if (!ready || !owner->hub || !persisted || transaction.stage != 1 || peer >= owner->count ||
        memcmp(owner->peers[peer].device, transaction.device, 6) ||
        m->present != ((1u << 0) | (1u << 6) | (1u << 24)) || memcmp(m->bytes[0], transaction.operation, 16))
        return true;
    if (m->value[6]) {
        phase = "failed";
        message = m->value[6] == 1 ? "Дождитесь завершения полива и проверки грунта."
                                   : "Не удалось сохранить безопасное состояние на контроллере.";
        return true;
    }
    transaction.stage = 2;
    if (!save_transaction()) {
        phase = "storage_error";
        message = "Контроллер ответил, но подтверждение не сохранено на Hub.";
        return true;
    }
    cleanup();
    return true;
}
bool sp_maintenance_node(const sp_message_t *m, sp_message_t *out) {
    if (m->value[0] != SP_MANAGE)
        return false;
    if (!ready || owner->hub || !core || m->present != ((1u << 0) | (1u << 3) | (1u << 24)) ||
        m->value[3] != 1 || (m->bytes[0][6] & 0xf0) != 0x40 || (m->bytes[0][8] & 0xc0) != 0x80)
        return false;
    uint8_t status_code = 0;
    if (node_detaching) {
        if (memcmp(node_operation, m->bytes[0], 16))
            status_code = 1;
    } else if (core->active_slot >= 0 || core->soak_pending)
        status_code = 1;
    else {
        sp_config_t next = core->saved.config;
        next.automatic = false;
        if ((core->saved.config.automatic && !sp_core_configure(core, &next, core->saved.config.revision)) ||
            !core->storage_ok || sp_radio_prepare_unpair(m->bytes[0]) != ESP_OK)
            status_code = 2;
        else {
            node_detaching = true;
            memcpy(node_operation, m->bytes[0], 16);
            restart_at = now_ms() + 1200;
        }
    }
    sp_message_init(out, SP_MANAGE_RESULT);
    sp_message_set(out, SP_K_STATUS, status_code);
    sp_message_bytes(out, SP_K_ID, m->bytes[0]);
    return true;
}
