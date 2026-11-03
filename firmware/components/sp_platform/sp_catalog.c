#include "sp_catalog.h"
#include "sp_web.h"
#include "sp_text.h"
#include "mbedtls/sha256.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    sp_plant_meta_t meta;
    uint8_t device[6];
} row_t;
typedef struct {
    uint8_t device[6], plant[16];
} binding_t;
typedef struct {
    uint32_t version, count, binding_count;
    row_t rows[SP_PLANT_MAX];
    binding_t bindings[40];
} catalog_t;
static catalog_t *catalog;
static bool nonzero(const uint8_t *p, size_t n) {
    unsigned v = 0;
    while (n--)
        v |= *p++;
    return v != 0;
}
static bool valid_meta(const sp_plant_meta_t *m) {
    return m->version == 2 && nonzero(m->plant, 16) && memchr(m->name, 0, sizeof(m->name)) && m->name[0] &&
           memchr(m->species, 0, sizeof(m->species)) && memchr(m->room, 0, sizeof(m->room)) &&
           m->setup_stage <= 3;
}
static bool valid(const catalog_t *c) {
    if (c->version != 1 || c->count > SP_PLANT_MAX || c->binding_count > 40)
        return false;
    for (unsigned i = 0; i < c->count; i++) {
        if (!valid_meta(&c->rows[i].meta))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(c->rows[i].meta.plant, c->rows[j].meta.plant, 16) ||
                (nonzero(c->rows[i].device, 6) && !memcmp(c->rows[i].device, c->rows[j].device, 6)))
                return false;
    }
    for (unsigned i = 0; i < c->binding_count; i++) {
        bool found = false;
        if (!nonzero(c->bindings[i].device, 6))
            return false;
        for (unsigned j = 0; j < c->count; j++)
            if (!memcmp(c->bindings[i].plant, c->rows[j].meta.plant, 16))
                found = true;
        if (!found)
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(c->bindings[i].device, c->bindings[j].device, 6))
                return false;
    }
    return true;
}
static bool bind_device(catalog_t *c, const uint8_t device[6], const uint8_t plant[16], bool replacing) {
    for (unsigned i = 0; i < c->binding_count; i++)
        if (!memcmp(c->bindings[i].device, device, 6)) {
            if (!replacing && memcmp(c->bindings[i].plant, plant, 16))
                return false;
            memcpy(c->bindings[i].plant, plant, 16);
            return true;
        }
    if (c->binding_count == 40)
        return false;
    binding_t *b = &c->bindings[c->binding_count++];
    memcpy(b->device, device, 6);
    memcpy(b->plant, plant, 16);
    return true;
}
static bool commit(catalog_t *next) {
    if (!valid(next))
        return false;
    nvs_handle_t h;
    if (nvs_open("catalog", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, "state_v1", next, sizeof(*next)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok)
        *catalog = *next;
    return ok;
}
bool sp_catalog_load(sp_identity_t *id, sp_api_t *api) {
    if (!id->hub)
        goto fail;
    free(catalog);
    catalog = NULL;
    catalog = calloc(1, sizeof(*catalog));
    if (!catalog)
        goto fail;
    catalog->version = 1;
    nvs_handle_t h;
    esp_err_t e = nvs_open("catalog", NVS_READONLY, &h);
    if (e == ESP_OK) {
        size_t n = sizeof(*catalog);
        e = nvs_get_blob(h, "state_v1", catalog, &n);
        nvs_close(h);
        if (e != ESP_ERR_NVS_NOT_FOUND && (e != ESP_OK || n != sizeof(*catalog) || !valid(catalog)))
            goto fail;
    } else if (e != ESP_ERR_NVS_NOT_FOUND)
        goto fail;
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        memset(catalog, 0, sizeof(*catalog));
        catalog->version = 1;
        for (unsigned i = 0; i < id->count; i++) {
            char key[17];
            sp_api_device_id(id->peers[i].device, key);
            if (nvs_open("plants", NVS_READONLY, &h) != ESP_OK)
                continue;
            sp_plant_meta_v1_t old = {0};
            size_t n = sizeof(old);
            e = nvs_get_blob(h, key + 4, &old, &n);
            nvs_close(h);
            if (e == ESP_ERR_NVS_NOT_FOUND)
                continue;
            if (e != ESP_OK || n != sizeof(old) || old.version != 1 ||
                !memchr(old.name, 0, sizeof(old.name)) || !memchr(old.species, 0, sizeof(old.species)))
                goto fail;
            row_t *r = &catalog->rows[catalog->count++];
            r->meta.version = 2;
            r->meta.revision = old.revision;
            memcpy(r->meta.plant, old.plant, 16);
            memcpy(r->meta.name, old.name, sizeof(old.name));
            memcpy(r->meta.species, old.species, sizeof(old.species));
            memcpy(r->device, id->peers[i].device, 6);
            if (!bind_device(catalog, r->device, r->meta.plant, false))
                goto fail;
        }
        if (!valid(catalog))
            goto fail;
        /* Migration leaves old plants blobs intact for USB/OTA fallback firmware. */
        if (catalog->count && !commit(catalog))
            goto fail;
    }
    for (unsigned i = 0; i < api->count; i++)
        for (unsigned j = 0; j < catalog->count; j++)
            if (!memcmp(api->nodes[i].device, catalog->rows[j].device, 6)) {
                api->nodes[i].plant = catalog->rows[j].meta;
                api->nodes[i].have_plant = true;
            }
    return true;
fail:
    free(catalog);
    catalog = NULL;
    return false;
}
bool sp_catalog_save(const uint8_t device[6], const sp_plant_meta_t *meta) {
    if (!catalog || !valid_meta(meta) || !nonzero(device, 6))
        return false;
    catalog_t *next = malloc(sizeof(*next));
    if (!next)
        return false;
    *next = *catalog;
    unsigned slot = next->count;
    for (unsigned i = 0; i < next->count; i++) {
        if (!memcmp(next->rows[i].meta.plant, meta->plant, 16)) {
            slot = i;
            break;
        }
        if (!memcmp(next->rows[i].device, device, 6)) {
            free(next);
            return false;
        }
    }
    if (slot == SP_PLANT_MAX || (slot < next->count && nonzero(next->rows[slot].device, 6) &&
                                 memcmp(next->rows[slot].device, device, 6))) {
        free(next);
        return false;
    }
    if (slot == next->count)
        next->count++;
    next->rows[slot].meta = *meta;
    memcpy(next->rows[slot].device, device, 6);
    bool ok = bind_device(next, device, meta->plant, false) && commit(next);
    free(next);
    return ok;
}
bool sp_catalog_archive(const uint8_t device[6]) {
    if (!catalog)
        return false;
    catalog_t *next = malloc(sizeof(*next));
    if (!next)
        return false;
    *next = *catalog;
    bool found = false;
    for (unsigned i = 0; i < next->count; i++)
        if (!memcmp(next->rows[i].device, device, 6)) {
            memset(next->rows[i].device, 0, 6);
            found = true;
            break;
        }
    bool ok = !found || commit(next);
    free(next);
    return ok;
}
bool sp_catalog_claim(sp_api_node_t *node, const uint8_t plant[16]) {
    if (!catalog || node->have_plant)
        return false;
    for (unsigned i = 0; i < catalog->count; i++)
        if (!memcmp(catalog->rows[i].meta.plant, plant, 16) && !nonzero(catalog->rows[i].device, 6)) {
            sp_plant_meta_t meta = catalog->rows[i].meta;
            meta.setup_stage = 2;
            if (!sp_catalog_save(node->device, &meta))
                return false;
            node->plant = meta;
            node->have_plant = true;
            return true;
        }
    return false;
}
static bool replace_binding(sp_api_t *api, const uint8_t plant[16], const char *device, uint32_t revision) {
    if (!catalog)
        return false;
    unsigned target = catalog->count, source = catalog->count, node = api->count;
    for (unsigned i = 0; i < catalog->count; i++) {
        if (!memcmp(catalog->rows[i].meta.plant, plant, 16))
            target = i;
        char id[17];
        sp_api_device_id(catalog->rows[i].device, id);
        if (!strcmp(id, device) && nonzero(catalog->rows[i].device, 6))
            source = i;
    }
    for (unsigned i = 0; i < api->count; i++) {
        char id[17];
        sp_api_device_id(api->nodes[i].device, id);
        if (!strcmp(id, device))
            node = i;
    }
    if (target == catalog->count || node == api->count || source == target ||
        nonzero(catalog->rows[target].device, 6) || catalog->rows[target].meta.revision != revision ||
        revision == UINT32_MAX)
        return false;
    bool represented = source < catalog->count;
    if (represented) {
        if (catalog->rows[source].meta.setup_stage != 0 || catalog->rows[source].meta.revision != 0)
            return false;
    } else {
        if (api->nodes[node].have_plant)
            return false;
        for (unsigned i = 0; i < catalog->binding_count; i++)
            if (!memcmp(catalog->bindings[i].device, api->nodes[node].device, 6))
                return false;
    }
    catalog_t *next = malloc(sizeof(*next));
    if (!next)
        return false;
    *next = *catalog;
    memcpy(next->rows[target].device, api->nodes[node].device, 6);
    next->rows[target].meta.revision++;
    next->rows[target].meta.setup_stage = 2;
    sp_plant_meta_t meta = next->rows[target].meta;
    bool ok = bind_device(next, next->rows[target].device, meta.plant, true);
    if (represented) {
        memmove(&next->rows[source], &next->rows[source + 1], (next->count - source - 1) * sizeof(row_t));
        memset(&next->rows[--next->count], 0, sizeof(row_t));
    }
    ok = ok && commit(next);
    free(next);
    if (ok) {
        api->nodes[node].plant = meta;
        api->nodes[node].have_plant = true;
    }
    return ok;
}
static bool snapshot(char out[65]) {
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)catalog, sizeof(*catalog), hash, 0))
        return false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        out[2 * i] = hex[hash[i] >> 4];
        out[2 * i + 1] = hex[hash[i] & 15];
    }
    out[64] = 0;
    return true;
}
static void backup_error(const sp_api_request_t *q, int status, const char *reason) {
    sp_web_reply(NULL, q->id, status, sp_api_error(reason, reason));
}
static bool import_row(const cJSON *o, sp_plant_meta_t *meta) {
    if (!cJSON_IsObject(o) || cJSON_GetArraySize(o) != 4)
        return false;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "plant_id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name"));
    const char *room = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "room"));
    const cJSON *species = cJSON_GetObjectItemCaseSensitive(o, "species_id");
    const char *type = cJSON_GetStringValue(species);
    if (!sp_api_parse_uuid(id, meta->plant) || !sp_text_valid(name, 1, 80) || !sp_text_valid(room, 0, 80) ||
        (!cJSON_IsNull(species) && !sp_text_valid(type, 1, 40)))
        return false;
    bool named = false;
    for (const char *p = name; *p; p++)
        if (*p != ' ')
            named = true;
    if (!named)
        return false;
    if (type)
        for (const char *p = type; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return false;
    meta->version = 2;
    meta->setup_stage = 2;
    snprintf(meta->name, sizeof(meta->name), "%s", name);
    snprintf(meta->room, sizeof(meta->room), "%s", room);
    if (type)
        snprintf(meta->species, sizeof(meta->species), "%s", type);
    return true;
}
static bool backup_handle(sp_api_t *api, const sp_api_request_t *q) {
    if (strncmp(q->path, "/api/v1/backup", 14))
        return false;
    if (!catalog) {
        backup_error(q, 503, "catalog_unavailable");
        return true;
    }
    if (!strcmp(q->path, "/api/v1/backup") && !strcmp(q->method, "GET")) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "format", "smartplantos.settings");
        cJSON_AddNumberToObject(out, "format_version", 1);
        cJSON *rows = cJSON_AddArrayToObject(out, "plants");
        for (unsigned i = 0; i < catalog->count; i++) {
            const sp_plant_meta_t *m = &catalog->rows[i].meta;
            char id[37];
            sp_api_uuid(m->plant, id);
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "plant_id", id);
            cJSON_AddStringToObject(row, "name", m->name);
            cJSON_AddStringToObject(row, "room", m->room);
            if (m->species[0])
                cJSON_AddStringToObject(row, "species_id", m->species);
            else
                cJSON_AddNullToObject(row, "species_id");
            cJSON_AddItemToArray(rows, row);
        }
        sp_web_reply(NULL, q->id, 200, out);
        return true;
    }
    bool preview = !strcmp(q->path, "/api/v1/backup/preview"),
         restore = !strcmp(q->path, "/api/v1/backup/restore");
    if ((!preview && !restore) || strcmp(q->method, "POST")) {
        backup_error(q, 404, "not_found");
        return true;
    }
    cJSON *body = cJSON_ParseWithOpts(q->body, NULL, true),
          *file = cJSON_GetObjectItemCaseSensitive(body, "backup"),
          *plants = cJSON_GetObjectItemCaseSensitive(file, "plants");
    const char *format = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(file, "format"));
    bool ok = cJSON_IsObject(body) && cJSON_GetArraySize(body) == (preview ? 1 : 3) &&
              !strstr(q->body, "\\u0000") && cJSON_IsObject(file) && cJSON_GetArraySize(file) == 3 &&
              format && !strcmp(format, "smartplantos.settings") &&
              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(file, "format_version")) == 1 &&
              cJSON_IsArray(plants) && cJSON_GetArraySize(plants) <= (int)SP_PLANT_MAX;
    char expected[65];
    if (!snapshot(expected)) {
        cJSON_Delete(body);
        backup_error(q, 503, "snapshot_unavailable");
        return true;
    }
    if (ok && restore) {
        const char *provided = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "snapshot"));
        if (!provided || strcmp(expected, provided) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "confirm"))) {
            cJSON_Delete(body);
            backup_error(q, 409, "backup_preview_expired");
            return true;
        }
    }
    catalog_t *next = ok ? malloc(sizeof(*next)) : NULL;
    if (ok && !next) {
        cJSON_Delete(body);
        backup_error(q, 503, "busy");
        return true;
    }
    if (next)
        *next = *catalog;
    cJSON *out = cJSON_CreateObject(), *changes = cJSON_AddArrayToObject(out, "changes");
    unsigned created = 0, updated = 0, unchanged = 0;
    uint8_t ids[SP_PLANT_MAX][16];
    unsigned seen = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, plants) {
        if (!ok)
            break;
        sp_plant_meta_t meta = {0};
        ok = import_row(item, &meta);
        if (!ok)
            break;
        for (unsigned i = 0; i < seen; i++)
            if (!memcmp(ids[i], meta.plant, 16))
                ok = false;
        if (!ok)
            break;
        memcpy(ids[seen++], meta.plant, 16);
        unsigned slot = next->count;
        for (unsigned i = 0; i < next->count; i++)
            if (!memcmp(next->rows[i].meta.plant, meta.plant, 16))
                slot = i;
        bool exists = slot < next->count, changed = !exists ||
                                                    strcmp(next->rows[slot].meta.name, meta.name) ||
                                                    strcmp(next->rows[slot].meta.room, meta.room) ||
                                                    strcmp(next->rows[slot].meta.species, meta.species);
        if ((!exists && next->count == SP_PLANT_MAX) ||
            (exists && changed && next->rows[slot].meta.revision == UINT32_MAX)) {
            ok = false;
            break;
        }
        cJSON *change = cJSON_CreateObject();
        char id[37];
        sp_api_uuid(meta.plant, id);
        cJSON_AddStringToObject(change, "plant_id", id);
        cJSON_AddStringToObject(change, "name", meta.name);
        cJSON_AddStringToObject(change, "room", meta.room);
        cJSON_AddStringToObject(change, "action", !exists ? "create" : changed ? "update" : "unchanged");
        cJSON_AddBoolToObject(change, "has_controller", exists && nonzero(next->rows[slot].device, 6));
        if (exists) {
            cJSON_AddStringToObject(change, "previous_name", next->rows[slot].meta.name);
            cJSON_AddStringToObject(change, "previous_room", next->rows[slot].meta.room);
        } else {
            cJSON_AddNullToObject(change, "previous_name");
            cJSON_AddNullToObject(change, "previous_room");
        }
        cJSON_AddItemToArray(changes, change);
        if (!exists) {
            next->rows[next->count++].meta = meta;
            created++;
        } else if (changed) {
            sp_plant_meta_t *m = &next->rows[slot].meta;
            snprintf(m->name, sizeof(m->name), "%s", meta.name);
            snprintf(m->room, sizeof(m->room), "%s", meta.room);
            snprintf(m->species, sizeof(m->species), "%s", meta.species);
            m->revision++;
            updated++;
        } else
            unchanged++;
    }
    if (ok && restore && (created || updated) && !commit(next)) {
        free(next);
        cJSON_Delete(out);
        cJSON_Delete(body);
        backup_error(q, 503, "storage_fault");
        return true;
    }
    if (ok && restore)
        for (unsigned i = 0; i < api->count; i++)
            for (unsigned j = 0; j < catalog->count; j++)
                if (nonzero(catalog->rows[j].device, 6) &&
                    !memcmp(api->nodes[i].device, catalog->rows[j].device, 6)) {
                    api->nodes[i].plant = catalog->rows[j].meta;
                    api->nodes[i].have_plant = true;
                }
    free(next);
    cJSON_Delete(body);
    if (!ok) {
        cJSON_Delete(out);
        backup_error(q, 400, "backup_invalid");
        return true;
    }
    cJSON_AddNumberToObject(out, "schema_version", 1);
    cJSON_AddStringToObject(out, "snapshot", expected);
    cJSON *summary = cJSON_AddObjectToObject(out, "summary");
    cJSON_AddNumberToObject(summary, "created", created);
    cJSON_AddNumberToObject(summary, "updated", updated);
    cJSON_AddNumberToObject(summary, "unchanged", unchanged);
    sp_web_reply(NULL, q->id, 200, out);
    return true;
}
bool sp_catalog_handle(sp_api_t *api, const sp_api_request_t *q) {
    if (backup_handle(api, q))
        return true;
    const char *prefix = "/api/v1/plants/";
    if (!strncmp(q->path, prefix, strlen(prefix)) && !strcmp(q->method, "POST")) {
        const char *tail = q->path + strlen(prefix);
        char id[37];
        uint8_t plant[16];
        bool valid = strlen(tail) == 47 && !strcmp(tail + 36, "/controller");
        if (valid) {
            memcpy(id, tail, 36);
            id[36] = 0;
            valid = sp_api_parse_uuid(id, plant);
        }
        cJSON *o = cJSON_ParseWithOpts(q->body, NULL, true);
        cJSON *rev = cJSON_GetObjectItemCaseSensitive(o, "revision");
        const char *device = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "device_id"));
        valid = valid && cJSON_IsObject(o) && cJSON_GetArraySize(o) == 3 && !strstr(q->body, "\\u0000") &&
                device && cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "schema_version")) == 1 &&
                cJSON_IsNumber(rev) && rev->valuedouble >= 0 && rev->valuedouble < UINT32_MAX &&
                (uint32_t)rev->valuedouble == rev->valuedouble;
        bool ok = valid && replace_binding(api, plant, device, (uint32_t)rev->valuedouble);
        cJSON_Delete(o);
        if (!ok)
            sp_web_reply(NULL, q->id, valid ? 409 : 400,
                         sp_api_error(valid ? "binding_conflict" : "invalid_body",
                                      valid ? "binding_conflict" : "invalid_body"));
        else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "schema_version", 1);
            sp_web_reply(NULL, q->id, 200, r);
        }
        return true;
    }
    if (strcmp(q->path, "/api/v1/plants"))
        return false;
    if (strcmp(q->method, "GET") || !catalog) {
        sp_web_reply(NULL, q->id, 503, sp_api_error("catalog_unavailable", "catalog_unavailable"));
        return true;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON *a = cJSON_AddArrayToObject(o, "plants");
    for (unsigned i = 0; i < catalog->count; i++) {
        row_t *r = &catalog->rows[i];
        char plant[37], device[17];
        sp_api_uuid(r->meta.plant, plant);
        sp_api_device_id(r->device, device);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "plant_id", plant);
        if (nonzero(r->device, 6))
            cJSON_AddStringToObject(p, "device_id", device);
        else
            cJSON_AddNullToObject(p, "device_id");
        cJSON_AddStringToObject(p, "name", r->meta.name);
        cJSON_AddStringToObject(p, "room", r->meta.room);
        if (r->meta.species[0])
            cJSON_AddStringToObject(p, "species_id", r->meta.species);
        else
            cJSON_AddNullToObject(p, "species_id");
        cJSON_AddNumberToObject(p, "revision", r->meta.revision);
        cJSON_AddNumberToObject(p, "setup_stage", r->meta.setup_stage);
        cJSON *history = cJSON_AddArrayToObject(p, "controller_history");
        for (unsigned j = 0; j < catalog->binding_count; j++)
            if (!memcmp(catalog->bindings[j].plant, r->meta.plant, 16)) {
                char historical[17];
                sp_api_device_id(catalog->bindings[j].device, historical);
                cJSON_AddItemToArray(history, cJSON_CreateString(historical));
            }
        cJSON_AddItemToArray(a, p);
    }
    sp_web_reply(NULL, q->id, 200, o);
    return true;
}
