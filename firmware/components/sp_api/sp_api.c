#include "sp_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void num(cJSON *o, const char *k, double v) {
    cJSON_AddNumberToObject(o, k, v);
}
static void str(cJSON *o, const char *k, const char *v) {
    if (v)
        cJSON_AddStringToObject(o, k, v);
    else
        cJSON_AddNullToObject(o, k);
}
static cJSON *obj(void) {
    cJSON *o = cJSON_CreateObject();
    num(o, "schema_version", 1);
    return o;
}
void sp_api_device_id(const uint8_t id[6], char out[17]) {
    snprintf(out, 17, "spo-%02x%02x%02x%02x%02x%02x", id[0], id[1], id[2], id[3], id[4], id[5]);
}
void sp_api_uuid(const uint8_t id[16], char out[37]) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", id[0], id[1],
             id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12], id[13], id[14],
             id[15]);
}
bool sp_api_parse_uuid(const char *s, uint8_t out[16]) {
    if (!s || strlen(s) != 36)
        return false;
    unsigned n = 0;
    for (unsigned i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i++] != '-')
                return false;
            continue;
        }
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = s[i++];
            unsigned x = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                                                : 16;
            if (x > 15)
                return false;
            v = v * 16 + x;
        }
        out[n++] = (uint8_t)v;
    }
    return (out[6] & 0xf0) == 0x40 && (out[8] & 0xc0) == 0x80;
}
static void stamp(cJSON *o, const char *key, uint64_t ms) {
    if (!ms) {
        cJSON_AddNullToObject(o, key);
        return;
    }
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    char text[32];
    if (!gmtime_r(&t, &tm) || !strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        cJSON_AddNullToObject(o, key);
        return;
    }
    str(o, key, text);
}
static const char *text(const cJSON *o, const char *k) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
}
static bool integer(const cJSON *o, const char *k, uint64_t min, uint64_t max, uint64_t *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < (double)min ||
        v->valuedouble > (double)max || floor(v->valuedouble) != v->valuedouble)
        return false;
    *out = (uint64_t)v->valuedouble;
    return true;
}
static bool keys(const cJSON *o, const char *const *allowed, unsigned count) {
    if (!cJSON_IsObject(o) || cJSON_GetArraySize(o) != (int)count)
        return false;
    for (unsigned i = 0; i < count; i++) {
        unsigned found = 0;
        for (cJSON *p = o->child; p; p = p->next)
            if (p->string && !strcmp(p->string, allowed[i]))
                found++;
        if (found != 1)
            return false;
    }
    return true;
}
cJSON *sp_api_error(const char *code, const char *message) {
    cJSON *o = obj(), *e = cJSON_AddObjectToObject(o, "error");
    str(e, "code", code);
    str(e, "message", message);
    return o;
}
static void reply(sp_api_t *a, uint32_t id, int code, cJSON *o) {
    a->io.reply(a->io.ctx, id, code, o);
}
static void error(sp_api_t *a, uint32_t id, int code, const char *why) {
    reply(a, id, code, sp_api_error(why, why));
}
void sp_api_tick(sp_api_t *a, uint64_t now, uint64_t utc) {
    a->now = now;
    a->utc = utc;
}
static void identity(cJSON *o, const sp_api_node_t *n) {
    char id[17];
    sp_api_device_id(n->device, id);
    str(o, "device_id", id);
}
static void plant_identity(cJSON *o, const sp_api_node_t *n, const char *name_key) {
    char id[37];
    sp_api_uuid(n->plant.plant, id);
    str(o, "plant_id", id);
    str(o, name_key, n->plant.name);
    str(o, "species_id", n->plant.species[0] ? n->plant.species : NULL);
}
static cJSON *config_json(const sp_api_node_t *n) {
    cJSON *o = obj();
    identity(o, n);
    plant_identity(o, n, "plant_name");
    const sp_config_t *c = &n->config;
    num(o, "revision", c->revision);
    str(o, "mode", c->automatic ? "automatic_threshold" : "manual");
    num(o, "dry_threshold_pct", c->threshold_pct);
    num(o, "portion_ml", c->portion_ml);
    num(o, "max_daily_ml", c->daily_ml);
    num(o, "max_run_seconds", c->max_run_s);
    num(o, "min_interval_minutes", c->interval_min);
    num(o, "sensor_max_age_seconds", c->max_age_s);
    if (c->dry_raw != c->wet_raw) {
        cJSON *cal = cJSON_AddObjectToObject(o, "soil_calibration");
        num(cal, "dry_raw", c->dry_raw);
        num(cal, "wet_raw", c->wet_raw);
    } else
        cJSON_AddNullToObject(o, "soil_calibration");
    if (c->flow_ul_s)
        num(o, "pump_calibration_ml_per_second", c->flow_ul_s / 1000.0);
    else
        cJSON_AddNullToObject(o, "pump_calibration_ml_per_second");
    return o;
}
static cJSON *sensor_array(const sp_api_t *a, const sp_api_node_t *n, bool samples) {
    cJSON *arr = cJSON_CreateArray();
    for (unsigned i = 0; i < SP_SENSOR_MAX; i++)
        if (n->have_sensor[i]) {
            const sp_sensor_driver_t *d = &n->sensors[i];
            cJSON *o = cJSON_CreateObject();
            char id[16];
            snprintf(id, sizeof(id), "sensor-%u", i);
            str(o, "sensor_id", id);
            str(o, "kind", sp_sensor_kind_name(d->descriptor.kind));
            str(o, "unit", sp_sensor_unit_name(d->descriptor.unit));
            num(o, "max_age_ms", d->descriptor.max_age_ms);
            if (samples) {
                uint64_t age = sp_sensor_age(d, a->now);
                sp_quality_t q = d->sample.quality;
                if (q == SP_Q_OK && age > d->descriptor.max_age_ms)
                    q = SP_Q_STALE;
                str(o, "quality", sp_quality_name(q));
                num(o, "sample_age_ms", age);
                if (q == SP_Q_OK)
                    num(o, "value", d->sample.milli_value / 1000.0);
                else
                    cJSON_AddNullToObject(o, "value");
            }
            cJSON_AddItemToArray(arr, o);
        }
    return arr;
}
static cJSON *device_json(const sp_api_t *a, const sp_api_node_t *n) {
    cJSON *o = obj();
    identity(o, n);
    str(o, "api_version", "1.0");
    str(o, "firmware_version", "0.1.0");
    str(o, "board", "esp32-c3-super-mini");
    cJSON *cap = cJSON_AddObjectToObject(o, "capabilities");
    str(cap, "soil_sensor", "analog");
    cJSON_AddBoolToObject(cap, "tank_sensor", true);
    cJSON_AddBoolToObject(cap, "pump", true);
    str(cap, "water_volume", "estimated");
    str(cap, "event_history", "local_bounded");
    cJSON_AddItemToObject(cap, "sensors", sensor_array(a, n, false));
    return o;
}
static cJSON *state_json(const sp_api_t *a, const sp_api_node_t *n) {
    const sp_message_t *m = &n->registry.state;
    uint64_t elapsed = a->now - n->registry.received_ms;
    uint64_t utc = m->value[2] ? m->value[2] + elapsed : 0, age = m->value[15] + elapsed;
    cJSON *o = obj();
    identity(o, n);
    num(o, "revision", m->value[13]);
    str(o, "clock_state", utc ? "synced" : "unset");
    stamp(o, "observed_at", utc);
    num(o, "uptime_ms", m->value[14] + elapsed);
    cJSON *plant = cJSON_AddObjectToObject(o, "plant");
    plant_identity(plant, n, "name");
    cJSON *soil = cJSON_AddObjectToObject(o, "soil");
    sp_quality_t q = (sp_quality_t)m->value[10];
    if (q == SP_Q_OK && age > (uint64_t)(n->have_config ? n->config.max_age_s : 30) * 1000)
        q = SP_Q_STALE;
    str(soil, "quality", sp_quality_name(q));
    if (q == SP_Q_MISSING)
        cJSON_AddNullToObject(soil, "raw_adc");
    else
        num(soil, "raw_adc", m->value[8]);
    if (q == SP_Q_OK)
        num(soil, "soil_index_pct", m->value[9]);
    else
        cJSON_AddNullToObject(soil, "soil_index_pct");
    stamp(soil, "sampled_at", utc > age ? utc - age : 0);
    num(soil, "sample_age_ms", age);
    cJSON *tank = cJSON_AddObjectToObject(o, "tank");
    str(tank, "quality", m->value[11] ? "ok" : "missing");
    if (m->value[11])
        cJSON_AddBoolToObject(tank, "water_present", m->value[11] == 2);
    else
        cJSON_AddNullToObject(tank, "water_present");
    cJSON *pump = cJSON_AddObjectToObject(o, "pump");
    str(pump, "state", m->value[12] ? "running" : "off");
    cJSON *w = cJSON_AddObjectToObject(o, "watering");
    str(w, "mode", m->value[20] ? "automatic_threshold" : "manual");
    char uuid[37];
    if (sp_message_has(m, 24)) {
        sp_api_uuid(m->bytes[0], uuid);
        str(w, "active_command_id", uuid);
    } else
        cJSON_AddNullToObject(w, "active_command_id");
    stamp(w, "last_completed_at", m->value[18]);
    num(w, "safety_reserved_ml", m->value[16]);
    if (utc && sp_message_has(m, 19))
        num(w, "daily_estimated_ml", m->value[19] / 1000.0);
    else
        cJSON_AddNullToObject(w, "daily_estimated_ml");
    cJSON *d = cJSON_AddObjectToObject(o, "last_decision");
    unsigned reason = m->value[7];
    str(d, "status", reason == SP_OK ? "ready" : reason == SP_SOIL_NOT_DRY ? "not_needed" : "blocked");
    str(d, "reason_code", reason ? sp_reason_name(reason) : NULL);
    stamp(d, "evaluated_at", m->value[2]);
    cJSON_AddItemToObject(o, "sensors", sensor_array(a, n, true));
    return o;
}
static cJSON *result_json(const sp_api_node_t *n, const sp_message_t *m) {
    cJSON *o = obj();
    identity(o, n);
    char uuid[37];
    sp_api_uuid(m->bytes[0], uuid);
    str(o, "command_id", uuid);
    str(o, "kind", m->value[3] == SP_STOP ? "stop" : "water");
    str(o, "status", sp_status_name(m->value[6]));
    str(o, "reason_code", m->value[7] ? sp_reason_name(m->value[7]) : NULL);
    if (m->value[3] == SP_WATER)
        num(o, "requested_ml", m->value[4]);
    else
        cJSON_AddNullToObject(o, "requested_ml");
    if (m->value[6] == SP_REJECTED)
        cJSON_AddNullToObject(o, "estimated_ml");
    else
        num(o, "estimated_ml", m->value[19] / 1000.0);
    stamp(o, "started_at", m->value[17]);
    stamp(o, "finished_at", m->value[18]);
    str(o, "clock_state", m->value[18] || m->value[17] ? "synced" : "unset");
    return o;
}
static bool decode_config(const sp_message_t *m, sp_config_t *out) {
    const unsigned keys[] = {0, 1, 3, 4, 8, 9, 10, 11, 12, 13, 14, 19};
    uint32_t mask = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        mask |= UINT32_C(1) << keys[i];
    if (m->present != mask || m->value[0] != SP_CONFIG || m->value[1] > UINT32_MAX ||
        m->value[19] > UINT32_MAX || m->value[3] > 1)
        return false;
    for (unsigned k = 4; k <= 14; k++)
        if (sp_message_has(m, k) && m->value[k] > UINT16_MAX)
            return false;
    *out = (sp_config_t){.revision = m->value[1],
                         .automatic = m->value[3],
                         .portion_ml = m->value[4],
                         .dry_raw = m->value[8],
                         .wet_raw = m->value[9],
                         .threshold_pct = m->value[10],
                         .daily_ml = m->value[11],
                         .max_run_s = m->value[12],
                         .interval_min = m->value[13],
                         .max_age_s = m->value[14],
                         .flow_ul_s = m->value[19]};
    return sp_config_valid(out);
}
void sp_api_timeout(sp_api_t *a, unsigned peer) {
    if (peer < a->count && a->nodes[peer].request_id) {
        error(a, a->nodes[peer].request_id, 503, "request_outcome_unknown");
        a->nodes[peer].request_id = 0;
    }
}
void sp_api_receive(sp_api_t *a, unsigned peer, uint32_t boot, const sp_message_t *m) {
    if (peer >= a->count)
        return;
    sp_api_node_t *n = &a->nodes[peer];
    if (n->boot != boot) {
        sp_api_timeout(a, peer);
        n->registry.have_state = false;
        n->have_config = false;
        memset(n->have_sensor, 0, sizeof(n->have_sensor));
        n->boot = boot;
    }
    unsigned kind = m->value[0];
    if (kind == SP_HELLO && sp_message_has(m, 27)) {
        if (!n->have_plant) {
            memcpy(n->plant.plant, m->bytes[3], 16);
            n->plant.version = 1;
            if (!n->plant.name[0])
                snprintf(n->plant.name, sizeof(n->plant.name), "Plant %02x%02x", n->device[4], n->device[5]);
            n->have_plant = true;
            if (a->io.save_plant)
                (void)a->io.save_plant(a->io.ctx, n->device, &n->plant);
        }
        return;
    }
    if (kind == SP_STATE) {
        (void)sp_registry_accept(&n->registry, m, a->now);
        return;
    }
    if (kind == SP_SENSOR) {
        sp_sensor_driver_t d;
        if (sp_sensor_from_message(m, a->now, &d)) {
            n->sensors[d.descriptor.slot] = d;
            n->have_sensor[d.descriptor.slot] = true;
        }
        return;
    }
    if (kind == SP_CONFIG && decode_config(m, &n->config)) {
        n->have_config = true;
        if (n->request_id && n->request_kind == SP_CONFIG_GET) {
            reply(a, n->request_id, 200, config_json(n));
            n->request_id = 0;
        }
        return;
    }
    if (!n->request_id || !sp_message_has(m, 24) || memcmp(m->bytes[0], n->request.bytes[0], 16))
        return;
    if (kind == SP_RESULT && (n->request_kind == SP_COMMAND || n->request_kind == SP_QUERY)) {
        if (sp_message_has(m, 23) && m->value[23] == 1)
            error(a, n->request_id, 404, "command_not_found");
        else if (m->present == ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 17) |
                                (1u << 18) | (1u << 19) | (1u << 24)) &&
                 (m->value[3] == SP_WATER || m->value[3] == SP_STOP) && m->value[6] <= SP_REJECTED &&
                 m->value[7] < SP_REASON_COUNT) {
            if (m->value[7] == SP_ID_CONFLICT)
                error(a, n->request_id, 409, "command_conflict");
            else
                reply(a, n->request_id,
                      n->request_kind == SP_QUERY  ? 200
                      : m->value[6] == SP_REJECTED ? 422
                                                   : 202,
                      result_json(n, m));
        } else
            error(a, n->request_id, 503, "invalid_node_response");
        n->request_id = 0;
    } else if (kind == SP_CONFIG_RESULT && n->request_kind == SP_CONFIG) {
        if (m->present != ((1u << 0) | (1u << 13) | (1u << 23) | (1u << 24)) || m->value[13] > UINT32_MAX ||
            m->value[23] > 1) {
            error(a, n->request_id, 503, "invalid_node_response");
            n->request_id = 0;
            return;
        }
        if (!m->value[23]) {
            n->config = n->proposed;
            n->config.revision = m->value[13];
            n->have_config = true;
            reply(a, n->request_id, 200, config_json(n));
        } else
            error(a, n->request_id, m->value[13] != n->proposed.revision ? 409 : 422, "config_rejected");
        n->request_id = 0;
    }
}
static bool submit(sp_api_t *a, unsigned peer, const sp_api_request_t *r, const sp_message_t *m) {
    sp_api_node_t *n = &a->nodes[peer];
    if (!sp_registry_online(&n->registry, a->now)) {
        error(a, r->id, 503, "node_offline");
        return false;
    }
    bool stop = m->value[0] == SP_COMMAND && m->value[3] == SP_STOP;
    if (n->request_id && !stop) {
        error(a, r->id, 503, "request_pending");
        return false;
    }
    if (stop)
        sp_api_timeout(a, peer);
    if (!a->io.send(a->io.ctx, peer, m)) {
        error(a, r->id, 503, "request_pending");
        return false;
    }
    n->request = *m;
    n->request_id = r->id;
    n->request_kind = m->value[0];
    return true;
}
static bool parse_config(sp_api_node_t *n, const cJSON *o, const char *if_match, sp_config_t *c) {
    static const char *const fields[] = {"schema_version",
                                         "device_id",
                                         "revision",
                                         "plant_id",
                                         "plant_name",
                                         "species_id",
                                         "mode",
                                         "dry_threshold_pct",
                                         "portion_ml",
                                         "max_daily_ml",
                                         "max_run_seconds",
                                         "min_interval_minutes",
                                         "sensor_max_age_seconds",
                                         "soil_calibration",
                                         "pump_calibration_ml_per_second"};
    if (!keys(o, fields, sizeof(fields) / sizeof(fields[0])))
        return false;
    char id[17], plant[37], rev[16];
    sp_api_device_id(n->device, id);
    sp_api_uuid(n->plant.plant, plant);
    if (!text(o, "device_id") || strcmp(text(o, "device_id"), id) || !text(o, "plant_id") ||
        strcmp(text(o, "plant_id"), plant) || !text(o, "plant_name") ||
        strcmp(text(o, "plant_name"), n->plant.name))
        return false;
    cJSON *species = cJSON_GetObjectItemCaseSensitive(o, "species_id");
    if (n->plant.species[0] ? (!cJSON_IsString(species) || strcmp(species->valuestring, n->plant.species))
                            : !cJSON_IsNull(species))
        return false;
    uint64_t v;
    if (!integer(o, "schema_version", 1, 1, &v) || !integer(o, "revision", 0, UINT32_MAX, &v))
        return false;
    memset(c, 0, sizeof(*c));
    c->revision = (uint32_t)v;
    snprintf(rev, sizeof(rev), "%lu", (unsigned long)c->revision);
    if (strcmp(rev, if_match))
        return false;
    const char *mode = text(o, "mode");
    if (!mode || (strcmp(mode, "manual") && strcmp(mode, "automatic_threshold")))
        return false;
    c->automatic = !strcmp(mode, "automatic_threshold");
#define CFG(field, key, lo, hi)                                                                              \
    do {                                                                                                     \
        if (!integer(o, key, lo, hi, &v))                                                                    \
            return false;                                                                                    \
        c->field = (uint16_t)v;                                                                              \
    } while (0)
    CFG(threshold_pct, "dry_threshold_pct", 1, 95);
    CFG(portion_ml, "portion_ml", 10, 300);
    CFG(daily_ml, "max_daily_ml", 10, 1000);
    CFG(max_run_s, "max_run_seconds", 1, 30);
    CFG(interval_min, "min_interval_minutes", 60, 10080);
    CFG(max_age_s, "sensor_max_age_seconds", 30, 900);
#undef CFG
    cJSON *cal = cJSON_GetObjectItemCaseSensitive(o, "soil_calibration");
    if (!cJSON_IsNull(cal)) {
        static const char *const ks[] = {"dry_raw", "wet_raw"};
        if (!keys(cal, ks, 2) || !integer(cal, "dry_raw", 0, 4095, &v))
            return false;
        c->dry_raw = v;
        if (!integer(cal, "wet_raw", 0, 4095, &v))
            return false;
        c->wet_raw = v;
    }
    cJSON *flow = cJSON_GetObjectItemCaseSensitive(o, "pump_calibration_ml_per_second");
    if (!cJSON_IsNull(flow)) {
        if (!cJSON_IsNumber(flow) || !isfinite(flow->valuedouble) || flow->valuedouble <= 0 ||
            flow->valuedouble > 1000)
            return false;
        double scaled = flow->valuedouble * 1000;
        if (fabs(scaled - round(scaled)) > 1e-6)
            return false;
        c->flow_ul_s = (uint32_t)llround(scaled);
    }
    return sp_config_valid(c);
}
static void events(sp_api_t *a, const sp_api_request_t *r, sp_api_node_t *n, const char *query) {
    uint64_t after = 0;
    unsigned limit = 50;
    char tail;
    if (query && *query) {
        unsigned long long value;
        int count = sscanf(query, "after=%llu&limit=%u%c", &value, &limit, &tail);
        if (count != 2) {
            error(a, r->id, 400, "invalid_cursor");
            return;
        }
        after = value;
    }
    if (limit < 1 || limit > 100) {
        error(a, r->id, 400, "invalid_limit");
        return;
    }
    if (!a->history.bounds) {
        error(a, r->id, 503, "history_unavailable");
        return;
    }
    uint64_t first, last;
    bool have = a->history.bounds(a->history.ctx, n->device, &first, &last);
    if (have && after < first - 1) {
        cJSON *o = sp_api_error("cursor_expired", "History no longer contains this cursor");
        num(cJSON_GetObjectItem(o, "error"), "oldest_seq", first);
        reply(a, r->id, 410, o);
        return;
    }
    cJSON *o = obj();
    identity(o, n);
    cJSON *arr = cJSON_AddArrayToObject(o, "events");
    uint64_t cursor = after;
    /* A page may be shorter than requested to stay within the HTTP reply budget. */
    for (unsigned i = 0; have && i < limit && i < 32; i++) {
        sp_history_record_t record;
        bool gap;
        if (!a->history.read_after(a->history.ctx, n->device, cursor, &record, &gap))
            break;
        const sp_event_t *e = &record.event;
        cJSON *item = cJSON_CreateObject();
        char did[17], eid[48], uuid[37];
        sp_api_device_id(n->device, did);
        snprintf(eid, sizeof(eid), "%s:%llu", did, (unsigned long long)e->seq);
        str(item, "event_id", eid);
        num(item, "seq", e->seq);
        str(item, "type", "device.event");
        str(item, "source", e->source == 1 ? "user" : e->source == 2 ? "automatic" : "device");
        stamp(item, "at", e->utc_ms);
        num(item, "uptime_ms", e->uptime_ms);
        cJSON *p = cJSON_AddObjectToObject(item, "payload");
        num(p, "kind", e->kind);
        str(p, "status", sp_status_name(e->status));
        str(p, "reason_code", e->reason ? sp_reason_name(e->reason) : NULL);
        bool command = false;
        for (unsigned j = 0; j < 16; j++)
            command |= e->command_id[j] != 0;
        if (command) {
            sp_api_uuid(e->command_id, uuid);
            str(p, "command_id", uuid);
        } else
            cJSON_AddNullToObject(p, "command_id");
        cJSON_AddItemToArray(arr, item);
        cursor = e->seq;
    }
    num(o, "next_cursor", cursor);
    num(o, "oldest_seq", have ? first : 1);
    reply(a, r->id, 200, o);
}
void sp_api_handle(sp_api_t *a, const sp_api_request_t *r) {
    bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST"), put = !strcmp(r->method, "PUT");
    if (get && !strcmp(r->path, "/api/v1/nodes")) {
        cJSON *o = obj(), *arr = cJSON_AddArrayToObject(o, "nodes");
        for (unsigned i = 0; i < a->count; i++) {
            sp_api_node_t *n = &a->nodes[i];
            cJSON *row = cJSON_CreateObject();
            identity(row, n);
            char did[17];
            sp_api_device_id(n->device, did);
            str(row, "display_name", n->plant.name[0] ? n->plant.name : did);
            cJSON_AddBoolToObject(row, "online", sp_registry_online(&n->registry, a->now));
            uint64_t age = a->now - n->registry.received_ms;
            stamp(row, "last_seen_at", n->registry.have_state && a->utc > age ? a->utc - age : 0);
            cJSON_AddItemToArray(arr, row);
        }
        reply(a, r->id, 200, o);
        return;
    }
    cJSON *body = NULL;
    if (post || put) {
        body = cJSON_ParseWithOpts(r->body, NULL, true);
        if (!body) {
            error(a, r->id, 400, "invalid_json");
            return;
        }
    }
    if (post && !strcmp(r->path, "/api/v1/time")) {
        static const char *const allowed[] = {"schema_version", "utc_ms"};
        uint64_t utc, version;
        if (!keys(body, allowed, 2) || !integer(body, "schema_version", 1, 1, &version) ||
            !integer(body, "utc_ms", SP_MIN_EPOCH_MS, SP_MAX_EPOCH_MS, &utc))
            error(a, r->id, 400, "invalid_time");
        else if (!a->io.set_time(a->io.ctx, utc))
            error(a, r->id, 409, "clock_jump");
        else {
            cJSON *o = obj();
            str(o, "clock_state", "synced");
            reply(a, r->id, 200, o);
        }
        cJSON_Delete(body);
        return;
    }
    const char *prefix = "/api/v1/nodes/";
    unsigned peer = a->count;
    const char *route = NULL;
    if (!strncmp(r->path, prefix, strlen(prefix))) {
        const char *id = r->path + strlen(prefix);
        for (unsigned i = 0; i < a->count; i++) {
            char expected[17];
            sp_api_device_id(a->nodes[i].device, expected);
            if (!strncmp(id, expected, 16) && id[16] == '/') {
                peer = i;
                route = id + 17;
                break;
            }
        }
    }
    if (peer == a->count) {
        error(a, r->id, 404, "not_found");
        goto done;
    }
    sp_api_node_t *n = &a->nodes[peer];
    if (get && !strcmp(route, "device")) {
        reply(a, r->id, 200, device_json(a, n));
        goto done;
    }
    if (get && !strncmp(route, "events", 6) && (route[6] == 0 || route[6] == '?')) {
        events(a, r, n, route[6] ? route + 7 : NULL);
        goto done;
    }
    if (!sp_registry_online(&n->registry, a->now)) {
        error(a, r->id, 503, "node_offline");
        goto done;
    }
    if (!n->have_plant) {
        error(a, r->id, 503, "identity_pending");
        goto done;
    }
    if (get && !strcmp(route, "state")) {
        reply(a, r->id, 200, state_json(a, n));
        goto done;
    }
    sp_message_t m;
    if (get && !strcmp(route, "config")) {
        sp_message_init(&m, SP_CONFIG_GET);
        submit(a, peer, r, &m);
        goto done;
    }
    if (put && !strcmp(route, "config")) {
        sp_config_t proposed;
        if (!parse_config(n, body, r->if_match, &proposed)) {
            error(a, r->id, 422, "config_invalid");
            goto done;
        }
        sp_config_message(&proposed, &m);
        uint8_t id[16] = {0};
        memcpy(id, n->device, 6);
        id[6] = 0x40;
        id[8] = 0x80;
        for (unsigned i = 0; i < 4; i++)
            id[12 + i] = (uint8_t)(r->id >> (24 - 8 * i));
        sp_message_bytes(&m, 24, id);
        if (submit(a, peer, r, &m))
            n->proposed = proposed;
        goto done;
    }
    if (put && !strcmp(route, "plant")) {
        static const char *const ks[] = {"schema_version", "revision", "name", "species_id"};
        uint64_t rev, v;
        const char *name = text(body, "name"), *species = text(body, "species_id");
        if (!keys(body, ks, 4) || !integer(body, "schema_version", 1, 1, &v) ||
            !integer(body, "revision", 0, UINT32_MAX - 1, &rev) || !name || !name[0] || strlen(name) > 80 ||
            (!species && !cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(body, "species_id"))) ||
            (species && (!species[0] || strlen(species) > 40))) {
            error(a, r->id, 422, "plant_invalid");
            goto done;
        }
        if (species)
            for (const char *s = species; *s; s++)
                if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) {
                    error(a, r->id, 422, "plant_invalid");
                    goto done;
                }
        if (rev != n->plant.revision) {
            error(a, r->id, 409, "revision_conflict");
            goto done;
        }
        sp_plant_meta_t next = n->plant;
        next.revision++;
        snprintf(next.name, sizeof(next.name), "%s", name);
        snprintf(next.species, sizeof(next.species), "%s", species ? species : "");
        if (!a->io.save_plant || !a->io.save_plant(a->io.ctx, n->device, &next)) {
            error(a, r->id, 503, "storage_fault");
            goto done;
        }
        n->plant = next;
        cJSON *o = obj();
        num(o, "revision", next.revision);
        str(o, "name", next.name);
        str(o, "species_id", next.species[0] ? next.species : NULL);
        reply(a, r->id, 200, o);
        goto done;
    }
    if (get && !strcmp(route, "plant")) {
        cJSON *o = obj();
        num(o, "revision", n->plant.revision);
        str(o, "name", n->plant.name);
        str(o, "species_id", n->plant.species[0] ? n->plant.species : NULL);
        reply(a, r->id, 200, o);
        goto done;
    }
    if (post && (!strcmp(route, "commands/water") || !strcmp(route, "commands/stop"))) {
        bool water = !strcmp(route, "commands/water");
        const char *ks[] = {"schema_version", "command_id", water ? "requested_ml" : "target_command_id"};
        uint64_t v;
        uint8_t id[16];
        if (!keys(body, ks, 3) || !integer(body, "schema_version", 1, 1, &v) ||
            !sp_api_parse_uuid(text(body, "command_id"), id)) {
            error(a, r->id, 400, "invalid_command");
            goto done;
        }
        sp_message_init(&m, SP_COMMAND);
        sp_message_set(&m, 3, water ? SP_WATER : SP_STOP);
        sp_message_bytes(&m, 24, id);
        if (water) {
            if (!integer(body, "requested_ml", 10, 300, &v)) {
                error(a, r->id, 422, "single_limit");
                goto done;
            }
            if (!a->utc) {
                error(a, r->id, 503, "clock_unset");
                goto done;
            }
            sp_message_set(&m, 4, v);
            sp_message_set(&m, 5, a->utc + 10000);
        } else {
            if (!sp_api_parse_uuid(text(body, "target_command_id"), id)) {
                error(a, r->id, 400, "invalid_command");
                goto done;
            }
            sp_message_bytes(&m, 25, id);
        }
        submit(a, peer, r, &m);
        goto done;
    }
    if (get && !strncmp(route, "commands/", 9)) {
        uint8_t id[16];
        if (!sp_api_parse_uuid(route + 9, id)) {
            error(a, r->id, 400, "invalid_command");
            goto done;
        }
        sp_message_init(&m, SP_QUERY);
        sp_message_bytes(&m, 24, id);
        submit(a, peer, r, &m);
        goto done;
    }
    error(a, r->id, 404, "not_found");
done:
    cJSON_Delete(body);
}
