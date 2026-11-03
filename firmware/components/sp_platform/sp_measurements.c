#include "sp_measurements.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
extern void sp_web_reply(void *, uint32_t, int, cJSON *);
#define HOUR_MS UINT64_C(3600000)
#define DAY_MS UINT64_C(86400000)
#define ROWS 10u
#define BAD 1u
#define CHANGED 2u
typedef struct {
    float sum, min, max;
    uint16_t count;
    uint8_t flags, source_slot;
} cell_t;
typedef struct {
    uint8_t plant[16], device[6], used, pad;
    cell_t cells[4];
} row_t;
typedef struct {
    uint32_t version, epoch, count, last_minute;
    row_t rows[ROWS];
} bucket_t;
typedef struct {
    sp_measurements_io_t io;
    bucket_t current;
    uint64_t last_minute, last_flush, retry_after;
    bool dirty, storage_ok, have_time, scanned;
} state_t;
static state_t *state;
static const char *const metrics[] = {"soil_index", "air_temperature", "air_humidity", "illuminance"};
static const char *const units[] = {"percent", "degC", "percent", "lux"};
static bool valid(const bucket_t *b) {
    if (b->version != 1 || b->count > ROWS)
        return false;
    for (unsigned i = 0; i < b->count; i++) {
        const row_t *r = &b->rows[i];
        if ((r->plant[6] & 0xf0) != 0x40 || (r->plant[8] & 0xc0) != 0x80 || r->used > 15)
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(r->plant, b->rows[j].plant, 16))
                return false;
        for (unsigned k = 0; k < 4; k++)
            if (r->used & (1u << k)) {
                const cell_t *c = &r->cells[k];
                if (c->count > 1440 || c->flags > 3 || !isfinite(c->sum) || !isfinite(c->min) ||
                    !isfinite(c->max) || (c->count && c->min > c->max))
                    return false;
            }
    }
    return true;
}
static int load(bool daily, uint32_t epoch, bucket_t *out) {
    memset(out, 0, sizeof(*out));
    char key[12];
    snprintf(key, sizeof(key), "%c%02u", daily ? 'd' : 'h', (unsigned)(epoch % (daily ? 14u : 48u)));
    int result = state->io.load ? state->io.load(state->io.ctx, key, out, sizeof(*out)) : 0;
    if (result == 1 && (!valid(out) || out->epoch != epoch)) {
        if (valid(out))
            return 0;
        state->storage_ok = false;
        return -1;
    }
    if (result < 0)
        state->storage_ok = false;
    return result;
}
static bool save(bool daily, const bucket_t *b) {
    char key[12];
    snprintf(key, sizeof(key), "%c%02u", daily ? 'd' : 'h', (unsigned)(b->epoch % (daily ? 14u : 48u)));
    bool ok = state->io.save && state->io.save(state->io.ctx, key, b, sizeof(*b));
    if (!ok) {
        state->storage_ok = false;
        state->retry_after = state->last_minute + 15;
    }
    return ok;
}
static row_t *row(bucket_t *b, const uint8_t *plant, const uint8_t *device) {
    for (unsigned i = 0; i < b->count; i++)
        if (!memcmp(b->rows[i].plant, plant, 16))
            return &b->rows[i];
    if (b->count == ROWS)
        return NULL;
    row_t *r = &b->rows[b->count++];
    memcpy(r->plant, plant, 16);
    memcpy(r->device, device, 6);
    return r;
}
static void combine(cell_t *to, const cell_t *from) {
    if (from->count) {
        if (!to->count) {
            to->min = from->min;
            to->max = from->max;
        } else {
            to->min = fminf(to->min, from->min);
            to->max = fmaxf(to->max, from->max);
        }
        to->sum += from->sum;
        to->count += from->count;
    }
    to->flags |= from->flags;
    to->source_slot = from->source_slot;
}
static void aggregate_day(uint32_t day, bucket_t *out) {
    memset(out, 0, sizeof(*out));
    out->version = 1;
    out->epoch = day;
    bucket_t *part = malloc(sizeof(*part));
    if (!part) {
        state->storage_ok = false;
        return;
    }
    for (unsigned h = 0; h < 24; h++) {
        uint32_t hour = day * 24 + h;
        bool have = false;
        if (state->current.version && state->current.epoch == hour) {
            *part = state->current;
            have = true;
        } else
            have = load(false, hour, part) == 1;
        if (!have)
            continue;
        for (unsigned i = 0; i < part->count; i++) {
            row_t *to = row(out, part->rows[i].plant, part->rows[i].device);
            if (!to) {
                state->storage_ok = false;
                continue;
            }
            bool changed = memcmp(to->device, part->rows[i].device, 6) != 0;
            for (unsigned k = 0; k < 4; k++)
                if (part->rows[i].used & (1u << k)) {
                    if (changed || (to->used & (1u << k) &&
                                    to->cells[k].source_slot != part->rows[i].cells[k].source_slot))
                        to->cells[k].flags |= CHANGED;
                    combine(&to->cells[k], &part->rows[i].cells[k]);
                }
            to->used |= part->rows[i].used;
            memcpy(to->device, part->rows[i].device, 6);
        }
    }
    free(part);
}
void sp_measurements_start(sp_measurements_io_t io) {
    free(state);
    state = calloc(1, sizeof(*state));
    if (state) {
        state->io = io;
        state->storage_ok = true;
    }
}
static void observe(const sp_api_node_t *n, unsigned metric, unsigned slot, double value, bool good) {
    bucket_t *b = &state->current;
    row_t *r = row(b, n->plant.plant, n->device);
    if (!r) {
        state->storage_ok = false;
        return;
    }
    cell_t *c = &r->cells[metric];
    if (memcmp(r->device, n->device, 6) || (r->used & (1u << metric) && c->source_slot != slot))
        c->flags |= CHANGED;
    memcpy(r->device, n->device, 6);
    r->used |= (uint8_t)(1u << metric);
    c->source_slot = (uint8_t)slot;
    if (good && isfinite(value) && c->count < 60) {
        if (!c->count)
            c->min = c->max = (float)value;
        else {
            c->min = fminf(c->min, (float)value);
            c->max = fmaxf(c->max, (float)value);
        }
        c->sum += (float)value;
        c->count++;
        state->dirty = true;
    } else if (!(c->flags & BAD)) {
        c->flags |= BAD;
        state->dirty = true;
    }
}
void sp_measurements_tick(const sp_api_t *a) {
    if (!state || a->utc < SP_MIN_EPOCH_MS || a->utc > SP_MAX_EPOCH_MS)
        return;
    uint64_t minute = a->utc / 60000;
    uint32_t hour = (uint32_t)(a->utc / HOUR_MS);
    state->have_time = true;
    if (!state->scanned) {
        state->scanned = true;
        bucket_t *old = malloc(sizeof(*old));
        uint32_t latest = 0;
        if (old) {
            for (unsigned i = 0; i < 48; i++) {
                char key[12];
                snprintf(key, sizeof(key), "h%02u", i);
                int result = state->io.load ? state->io.load(state->io.ctx, key, old, sizeof(*old)) : 0;
                if (result == 1 && valid(old) && old->epoch % 48 == i) {
                    if (old->last_minute > latest)
                        latest = old->last_minute;
                } else if (result < 0 || result == 1)
                    state->storage_ok = false;
            }
            state->last_minute = latest;
            uint32_t last_day = latest / 1440, today = hour / 24;
            for (unsigned n = 0; latest && n < 3 && last_day >= n; n++) {
                uint32_t day = last_day - n;
                if (day >= today)
                    continue;
                if (load(true, day, old) == 0) {
                    aggregate_day(day, old);
                    if (old->count)
                        (void)save(true, old);
                }
            }
            free(old);
        } else
            state->storage_ok = false;
    }
    if (state->last_minute && minute <= state->last_minute)
        return;
    if (state->current.version && state->current.epoch != hour) {
        if (state->dirty && minute < state->retry_after) {
            state->last_minute = minute;
            return;
        }
        state->last_minute = minute;
        if (state->dirty && !save(false, &state->current))
            return;
        uint32_t previous_day = state->current.epoch / 24;
        state->dirty = false;
        if (previous_day != hour / 24) {
            bucket_t *day = malloc(sizeof(*day));
            if (day) {
                aggregate_day(previous_day, day);
                if (day->count)
                    (void)save(true, day);
                free(day);
            } else
                state->storage_ok = false;
        }
        memset(&state->current, 0, sizeof(state->current));
    }
    if (!state->current.version) {
        int result = load(false, hour, &state->current);
        if (result < 0) {
            memset(&state->current, 0, sizeof(state->current));
            state->last_minute = minute;
            return;
        }
        if (result != 1) {
            memset(&state->current, 0, sizeof(state->current));
            state->current.version = 1;
            state->current.epoch = hour;
        }
        if (state->current.last_minute >= minute) {
            state->last_minute = minute;
            return;
        }
    }
    if (minute <= state->current.last_minute) {
        state->last_minute = state->current.last_minute;
        return;
    }
    state->last_minute = minute;
    for (unsigned i = 0; i < a->count; i++) {
        const sp_api_node_t *n = &a->nodes[i];
        if (!n->have_plant)
            continue;
        bool online = sp_registry_online(&n->registry, a->now);
        if (n->hardware_known && (n->hardware_flags & 1) && n->registry.have_state) {
            const sp_message_t *m = &n->registry.state;
            uint64_t age = m->value[15] + (a->now - n->registry.received_ms);
            bool good = online && m->value[10] == SP_Q_OK &&
                        age <= (uint64_t)(n->have_config ? n->config.max_age_s : 30) * 1000;
            observe(n, 0, 255, (double)m->value[9], good);
        }
        bool seen[4] = {0};
        for (unsigned j = 0; j < SP_SENSOR_MAX; j++)
            if (n->have_sensor[j]) {
                const sp_sensor_driver_t *d = &n->sensors[j];
                unsigned k = d->descriptor.kind == SP_SENSOR_TEMPERATURE ? 1
                             : d->descriptor.kind == SP_SENSOR_HUMIDITY  ? 2
                             : d->descriptor.kind == SP_SENSOR_LIGHT     ? 3
                                                                         : 4;
                if (k == 4 || seen[k])
                    continue;
                seen[k] = true;
                bool good = online && d->sample.quality == SP_Q_OK &&
                            sp_sensor_age(d, a->now) <= d->descriptor.max_age_ms;
                observe(n, k, j, d->sample.milli_value / 1000.0, good);
            }
    }
    state->current.last_minute = (uint32_t)minute;
    if (state->dirty && minute >= state->retry_after && minute - state->last_flush >= 15 &&
        save(false, &state->current)) {
        state->dirty = false;
        state->last_flush = minute;
    }
}
static const row_t *find(const bucket_t *b, const uint8_t *plant) {
    for (unsigned i = 0; i < b->count; i++)
        if (!memcmp(b->rows[i].plant, plant, 16))
            return &b->rows[i];
    return NULL;
}
static cJSON *period(uint32_t first, unsigned count, bool daily, const uint8_t *plant, unsigned metric,
                     uint64_t end_ms) {
    cJSON *out = cJSON_CreateObject(), *points = cJSON_AddArrayToObject(out, "points");
    uint64_t width = daily ? DAY_MS : HOUR_MS;
    cJSON_AddNumberToObject(out, "start_ms", (double)((uint64_t)first * width));
    cJSON_AddNumberToObject(out, "end_ms", (double)end_ms);
    double sum = 0;
    unsigned samples = 0;
    bucket_t *b = malloc(sizeof(*b));
    if (!b) {
        cJSON_Delete(out);
        return NULL;
    }
    for (unsigned i = 0; i < count; i++) {
        uint32_t epoch = first + i;
        bool have = false;
        if (!daily && state->current.version && state->current.epoch == epoch) {
            *b = state->current;
            have = true;
        } else if (daily && (epoch == state->current.epoch / 24 || epoch + 1 == state->current.epoch / 24)) {
            aggregate_day(epoch, b);
            have = b->count > 0;
        } else
            have = load(daily, epoch, b) == 1;
        const row_t *r = have ? find(b, plant) : NULL;
        const cell_t *c = r && (r->used & (1u << metric)) ? &r->cells[metric] : NULL;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "at_ms", (double)((uint64_t)epoch * width));
        cJSON_AddNumberToObject(p, "samples", c ? c->count : 0);
        cJSON_AddStringToObject(p, "quality", !c || !c->count ? "missing" : c->flags ? "mixed" : "ok");
        cJSON_AddBoolToObject(p, "controller_changed", c && (c->flags & CHANGED));
        if (c && c->count) {
            cJSON_AddNumberToObject(p, "value", c->sum / c->count);
            cJSON_AddNumberToObject(p, "min", c->min);
            cJSON_AddNumberToObject(p, "max", c->max);
            sum += c->sum;
            samples += c->count;
        } else {
            cJSON_AddNullToObject(p, "value");
            cJSON_AddNullToObject(p, "min");
            cJSON_AddNullToObject(p, "max");
        }
        cJSON_AddItemToArray(points, p);
    }
    free(b);
    cJSON_AddNumberToObject(out, "samples", samples);
    if (samples)
        cJSON_AddNumberToObject(out, "mean", sum / samples);
    else
        cJSON_AddNullToObject(out, "mean");
    return out;
}
bool sp_measurements_handle(const sp_api_t *a, const sp_api_request_t *q) {
    const char *prefix = "/api/v1/plants/";
    size_t p = strlen(prefix);
    if (strncmp(q->path, prefix, p) || strlen(q->path) < p + 49)
        return false;
    const char *tail = q->path + p + 36;
    if (strncmp(tail, "/measurements", 13))
        return false;
    if (!state) {
        sp_web_reply(NULL, q->id, 503, sp_api_error("history_unavailable", "history_unavailable"));
        return true;
    }
    uint8_t plant[16];
    char id[37];
    memcpy(id, q->path + p, 36);
    id[36] = 0;
    if (!sp_api_parse_uuid(id, plant) || !state->io.plant_exists ||
        !state->io.plant_exists(state->io.ctx, plant)) {
        sp_web_reply(NULL, q->id, 404, sp_api_error("plant_not_found", "plant_not_found"));
        return true;
    }
    char window[8] = {0}, metric_name[24] = {0};
    int used = 0;
    const char *query = tail + 13;
    unsigned metric = 4;
    if (strcmp(q->method, "GET") ||
        sscanf(query, "?period=%7[^&]&metric=%23[a-z_]%n", window, metric_name, &used) != 2 || query[used] ||
        (strcmp(window, "day") && strcmp(window, "week"))) {
        sp_web_reply(NULL, q->id, 400, sp_api_error("invalid_period", "invalid_period"));
        return true;
    }
    for (unsigned i = 0; i < 4; i++)
        if (!strcmp(metric_name, metrics[i]))
            metric = i;
    if (metric == 4) {
        sp_web_reply(NULL, q->id, 422, sp_api_error("invalid_metric", "invalid_metric"));
        return true;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "schema_version", 1);
    cJSON_AddStringToObject(out, "plant_id", id);
    cJSON_AddStringToObject(out, "metric", metrics[metric]);
    cJSON_AddStringToObject(out, "unit", units[metric]);
    cJSON_AddStringToObject(out, "period", window);
    cJSON_AddBoolToObject(out, "storage_available", state->storage_ok);
    cJSON_AddNumberToObject(out, "checkpoint_minutes", 15);
    cJSON_AddStringToObject(out, "clock_state",
                            a->utc >= SP_MIN_EPOCH_MS && a->utc <= SP_MAX_EPOCH_MS
                                ? (a->utc / 60000 < state->last_minute ? "rollback" : "synced")
                                : "unset");
    if (a->utc >= SP_MIN_EPOCH_MS && a->utc <= SP_MAX_EPOCH_MS) {
        bool daily = !strcmp(window, "week");
        unsigned count = daily ? 7 : 24;
        uint64_t width = daily ? DAY_MS : HOUR_MS;
        uint32_t end = (uint32_t)(a->utc / width) + 1;
        cJSON *current = period(end - count, count, daily, plant, metric, a->utc);
        if (!current) {
            cJSON_Delete(out);
            sp_web_reply(NULL, q->id, 503, sp_api_error("history_unavailable", "history_unavailable"));
            return true;
        }
        cJSON_AddItemToObject(out, "current", current);
        cJSON *previous =
            period(end - 2 * count, count, daily, plant, metric, (uint64_t)(end - count) * width);
        if (!previous) {
            cJSON_Delete(out);
            sp_web_reply(NULL, q->id, 503, sp_api_error("history_unavailable", "history_unavailable"));
            return true;
        }
        cJSON_AddItemToObject(out, "previous", previous);
    } else {
        cJSON_AddNullToObject(out, "current");
        cJSON_AddNullToObject(out, "previous");
    }
    sp_web_reply(NULL, q->id, 200, out);
    return true;
}
