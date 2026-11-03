#include "sp_diary.h"
#include "sp_text.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
/* I/O reply is supplied by the Hub's existing owner-authenticated request queue. */
extern void sp_web_reply(void *, uint32_t, int, cJSON *);
typedef struct {
    uint8_t id[16], plant[16];
    uint32_t seq, revision;
    uint64_t at_ms, due_ms;
    char note[161];
    uint8_t kind, done;
} entry_t;
typedef struct {
    uint32_t version, count, next_seq;
    entry_t rows[SP_DIARY_MAX];
} store_t;
static store_t *allocate_store(void) {
#ifdef ESP_PLATFORM
    store_t *external = heap_caps_calloc(1, sizeof(store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (external)
        return external;
#endif
    return calloc(1, sizeof(store_t));
}
static store_t *store;
static sp_diary_io_t io;
static bool ready;
static const char *const kinds[] = {"repot", "fertilize", "prune", "move", "stage", "note", "reminder"};
static bool uuid(const uint8_t *p) {
    unsigned sum = 0;
    for (unsigned i = 0; i < 16; i++)
        sum |= p[i];
    return sum && (p[6] & 0xf0) == 0x40 && (p[8] & 0xc0) == 0x80;
}
static bool timestamp(uint64_t n) {
    return n >= SP_MIN_EPOCH_MS && n <= SP_MAX_EPOCH_MS;
}
static bool note_valid(const char *text, unsigned minimum) {
    if (!text || strlen(text) > 160)
        return false;
    char clean[161];
    snprintf(clean, sizeof(clean), "%s", text);
    for (char *p = clean; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    return sp_text_valid(clean, minimum, 160);
}
static bool valid(const store_t *s) {
    if (s->version != 1 || s->count > SP_DIARY_MAX || !s->next_seq)
        return false;
    for (unsigned i = 0; i < s->count; i++) {
        const entry_t *e = &s->rows[i];
        if ((i && e->seq <= s->rows[i - 1].seq) || (!e->due_ms && e->done) || !uuid(e->id) ||
            !uuid(e->plant) || e->kind >= 7 || e->done > 1 || !timestamp(e->at_ms) ||
            !memchr(e->note, 0, sizeof(e->note)) || !note_valid(e->note, e->kind >= 5 ? 1 : 0) || !e->seq ||
            e->seq >= s->next_seq || (e->kind == 6 ? !timestamp(e->due_ms) : e->due_ms != 0))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(e->id, s->rows[j].id, 16) || e->seq == s->rows[j].seq)
                return false;
    }
    return true;
}
bool sp_diary_start(sp_diary_io_t callbacks) {
    free(store);
    store = allocate_store();
    io = callbacks;
    ready = false;
    if (!store)
        return false;
    int result = io.load ? io.load(io.ctx, store, sizeof(*store)) : 0;
    if (result == 0) {
        store->version = 1;
        store->next_seq = 1;
    }
    ready = result >= 0 && valid(store);
    return ready;
}
static bool commit(store_t *next) {
    if (!valid(next) || !io.save || !io.save(io.ctx, next, sizeof(*next)))
        return false;
    *store = *next;
    return true;
}
static void error(const sp_api_request_t *q, int status, const char *why) {
    sp_web_reply(NULL, q->id, status, sp_api_error(why, why));
}
static cJSON *json(const entry_t *e) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    char id[37], plant[37];
    sp_api_uuid(e->id, id);
    sp_api_uuid(e->plant, plant);
    cJSON_AddStringToObject(o, "entry_id", id);
    cJSON_AddStringToObject(o, "plant_id", plant);
    cJSON_AddNumberToObject(o, "seq", e->seq);
    cJSON_AddNumberToObject(o, "revision", e->revision);
    cJSON_AddStringToObject(o, "kind", kinds[e->kind]);
    cJSON_AddStringToObject(o, "note", e->note);
    cJSON_AddNumberToObject(o, "at_ms", (double)e->at_ms);
    if (e->due_ms)
        cJSON_AddNumberToObject(o, "remind_at_ms", (double)e->due_ms);
    else
        cJSON_AddNullToObject(o, "remind_at_ms");
    cJSON_AddBoolToObject(o, "done", e->done);
    return o;
}
static bool number(const cJSON *o, const char *name, uint64_t max, uint64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    double n = cJSON_GetNumberValue(v);
    if (!cJSON_IsNumber(v) || !isfinite(n) || n < 0 || floor(n) != n || n > (double)max)
        return false;
    *out = (uint64_t)n;
    return true;
}
static bool body_entry(const cJSON *j, const uint8_t plant[16], entry_t *e) {
    if (!cJSON_IsObject(j) || cJSON_GetArraySize(j) != 6)
        return false;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "entry_id")),
               *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "kind")),
               *note = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "note"));
    if (!sp_api_parse_uuid(id, e->id) || !kind || !note_valid(note, 0) ||
        !number(j, "schema_version", 1, &e->at_ms) || e->at_ms != 1 ||
        !number(j, "at_ms", SP_MAX_EPOCH_MS, &e->at_ms) || !timestamp(e->at_ms))
        return false;
    e->kind = 7;
    for (unsigned i = 0; i < 7; i++)
        if (!strcmp(kind, kinds[i]))
            e->kind = (uint8_t)i;
    if (e->kind >= 7 || (e->kind >= 5 && strspn(note, " ") == strlen(note)))
        return false;
    const cJSON *due = cJSON_GetObjectItemCaseSensitive(j, "remind_at_ms");
    if (cJSON_IsNull(due))
        e->due_ms = 0;
    else if (!number(j, "remind_at_ms", SP_MAX_EPOCH_MS, &e->due_ms))
        return false;
    if (e->kind == 6 ? !timestamp(e->due_ms) : e->due_ms != 0)
        return false;
    memcpy(e->plant, plant, 16);
    snprintf(e->note, sizeof(e->note), "%s", note);
    return true;
}
bool sp_diary_handle(const sp_api_request_t *q) {
    const char *prefix = "/api/v1/plants/";
    const char *tail = NULL;
    uint8_t plant[16] = {0};
    bool reminders = !strncmp(q->path, "/api/v1/reminders", 17) && (q->path[17] == 0 || q->path[17] == '?');
    if (reminders)
        tail = q->path + 17;
    else if (!strncmp(q->path, prefix, strlen(prefix)) && strlen(q->path) >= strlen(prefix) + 42) {
        char id[37];
        memcpy(id, q->path + strlen(prefix), 36);
        id[36] = 0;
        const char *rest = q->path + strlen(prefix) + 36;
        if (strncmp(rest, "/diary", 6) || !sp_api_parse_uuid(id, plant))
            return false;
        tail = rest + 6;
        if (*tail && *tail != '?' && *tail != '/')
            return false;
    } else
        return false;
    if (!ready) {
        error(q, 503, "care_unavailable");
        return true;
    }
    if (!reminders && (!io.plant_exists || !io.plant_exists(io.ctx, plant))) {
        error(q, 404, "plant_not_found");
        return true;
    }
    if (!strcmp(q->method, "GET") && (*tail == 0 || *tail == '?')) {
        unsigned after = 0, limit = 32;
        if (*tail) {
            char *end = NULL;
            const char *value = tail + 7;
            errno = 0;
            unsigned long long cursor = 0, requested = 0;
            bool ok = !strncmp(tail, "?after=", 7) && *value >= '0' && *value <= '9';
            if (ok) {
                cursor = strtoull(value, &end, 10);
                ok = !errno && cursor <= UINT32_MAX && !strncmp(end, "&limit=", 7);
            }
            if (ok) {
                value = end + 7;
                errno = 0;
                ok = *value >= '0' && *value <= '9';
                if (ok) {
                    requested = strtoull(value, &end, 10);
                    ok = !errno && !*end && requested >= 1 && requested <= 32;
                }
            }
            if (!ok) {
                error(q, 400, "invalid_cursor");
                return true;
            }
            after = (unsigned)cursor;
            limit = (unsigned)requested;
        }
        cJSON *out = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(out, "entries");
        cJSON_AddNumberToObject(out, "schema_version", 1);
        unsigned next = after, count = 0;
        bool more = false;
        for (unsigned i = 0; i < store->count; i++) {
            entry_t *e = &store->rows[i];
            if (e->seq <= after || (reminders ? (e->kind != 6 || e->done) : memcmp(e->plant, plant, 16) != 0))
                continue;
            if (count == limit) {
                more = true;
                break;
            }
            cJSON_AddItemToArray(list, json(e));
            next = e->seq;
            count++;
        }
        cJSON_AddNumberToObject(out, "next_cursor", next);
        cJSON_AddBoolToObject(out, "has_more", more);
        sp_web_reply(NULL, q->id, 200, out);
        return true;
    }
    if (reminders) {
        error(q, 405, "method_not_allowed");
        return true;
    }
    cJSON *body = cJSON_ParseWithOpts(q->body, NULL, true);
    if (!body || strstr(q->body, "\\u0000")) {
        cJSON_Delete(body);
        error(q, 400, "invalid_body");
        return true;
    }
    store_t *next = allocate_store();
    if (!next) {
        cJSON_Delete(body);
        error(q, 503, "busy");
        return true;
    }
    *next = *store;
    int status = 200;
    const char *why = NULL;
    cJSON *out = NULL;
    if (!*tail && !strcmp(q->method, "POST")) {
        entry_t fresh = {0};
        if (!body_entry(body, plant, &fresh)) {
            status = 422;
            why = "care_invalid";
        } else {
            int existing = -1;
            for (unsigned i = 0; i < store->count; i++)
                if (!memcmp(store->rows[i].id, fresh.id, 16))
                    existing = (int)i;
            if (existing >= 0) {
                entry_t *old = &store->rows[existing];
                if (memcmp(old->plant, fresh.plant, 16) || old->kind != fresh.kind ||
                    old->at_ms != fresh.at_ms || old->due_ms != fresh.due_ms ||
                    strcmp(old->note, fresh.note)) {
                    status = 409;
                    why = "entry_conflict";
                } else
                    out = json(old);
            } else if (store->count == SP_DIARY_MAX || store->next_seq == UINT32_MAX) {
                status = 507;
                why = "care_full";
            } else {
                fresh.seq = next->next_seq++;
                next->rows[next->count++] = fresh;
                if (!commit(next)) {
                    status = 503;
                    why = "storage_fault";
                } else
                    out = json(&fresh);
            }
        }
    } else if (*tail == '/' && strlen(tail) == 37 &&
               (!strcmp(q->method, "PUT") || !strcmp(q->method, "DELETE"))) {
        uint8_t id[16];
        uint64_t revision = 0;
        if (!sp_api_parse_uuid(tail + 1, id) || !number(body, "revision", UINT32_MAX - 1, &revision)) {
            status = 422;
            why = "care_invalid";
        } else {
            unsigned index = next->count;
            for (unsigned i = 0; i < next->count; i++)
                if (!memcmp(next->rows[i].id, id, 16) && !memcmp(next->rows[i].plant, plant, 16))
                    index = i;
            if (index == next->count) {
                status = 404;
                why = "entry_not_found";
            } else if (next->rows[index].revision != revision) {
                status = 409;
                why = "revision_conflict";
            } else if (!strcmp(q->method, "DELETE")) {
                if (cJSON_GetArraySize(body) != 1) {
                    status = 422;
                    why = "care_invalid";
                } else {
                    memmove(&next->rows[index], &next->rows[index + 1],
                            (next->count - index - 1) * sizeof(entry_t));
                    memset(&next->rows[--next->count], 0, sizeof(entry_t));
                    if (!commit(next)) {
                        status = 503;
                        why = "storage_fault";
                    }
                }
            } else {
                const cJSON *done = cJSON_GetObjectItemCaseSensitive(body, "done");
                if (cJSON_GetArraySize(body) != 2 || !cJSON_IsBool(done) || next->rows[index].kind != 6) {
                    status = 422;
                    why = "care_invalid";
                } else {
                    next->rows[index].done = cJSON_IsTrue(done);
                    next->rows[index].revision++;
                    if (!commit(next)) {
                        status = 503;
                        why = "storage_fault";
                    } else
                        out = json(&next->rows[index]);
                }
            }
        }
    } else {
        status = 405;
        why = "method_not_allowed";
    }
    free(next);
    cJSON_Delete(body);
    if (why) {
        cJSON_Delete(out);
        error(q, status, why);
    } else {
        if (!out) {
            out = cJSON_CreateObject();
            cJSON_AddNumberToObject(out, "schema_version", 1);
        }
        sp_web_reply(NULL, q->id, status, out);
    }
    return true;
}
