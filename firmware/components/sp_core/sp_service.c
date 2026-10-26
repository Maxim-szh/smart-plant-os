#include "sp_service.h"
#include <string.h>
#define KEY(k) (UINT32_C(1) << (k))
static bool shape(const sp_message_t *m, uint32_t required) {
    return m->present == required;
}
void sp_message_bytes(sp_message_t *m, unsigned k, const uint8_t *bytes) {
    if (k < 24 || k > 27)
        return;
    m->present |= KEY(k);
    memcpy(m->bytes[k - 24], bytes, k == SP_K_DEVICE ? 6 : 16);
}
static bool uuid(const uint8_t *id) {
    /* UUID v4, RFC 4122 variant; readable forms are validated by the USB/API adapter. */
    return (id[6] & 0xf0) == 0x40 && (id[8] & 0xc0) == 0x80;
}
void sp_node_state(const sp_core_t *c, sp_message_t *out) {
    sp_message_init(out, SP_STATE);
    sp_message_set(out, SP_K_RAW, c->soil.raw_adc);
    sp_message_set(out, SP_K_INDEX, c->soil.index_pct);
    sp_message_set(out, SP_K_QUALITY, c->soil.quality);
    sp_message_set(out, SP_K_TANK,
                   c->now_ms - c->tank_sampled_ms > 1500
                       ? 0
                       : (uint64_t)(c->tank + 1)); /* 0 unknown, 1 empty, 2 present */
    sp_message_set(out, SP_K_PUMP, c->active_slot >= 0);
    sp_message_set(out, SP_K_REVISION, c->saved.config.revision);
    sp_message_set(out, SP_K_UPTIME, c->now_ms - c->boot_ms);
    sp_message_set(out, SP_K_AGE, c->now_ms - c->soil.sampled_ms);
    sp_message_set(out, SP_K_BUDGET, sp_core_reserved_ml(c));
    sp_message_set(out, SP_K_MODE, c->saved.config.automatic);
    sp_message_set(out, SP_K_REASON, c->last_reason);
    sp_message_set(out, SP_K_SEQUENCE, c->saved.event_seq);
    sp_message_set(out, SP_K_SPREAD, c->soil.spread_adc);
    sp_message_set(out, SP_K_TIME, sp_core_utc(c, c->now_ms));
    uint64_t utc = sp_core_utc(c, c->now_ms), last = 0;
    uint32_t daily = 0;
    bool known = utc != 0;
    for (unsigned i = 0; i < SP_COMMAND_SLOTS; i++) {
        const sp_result_t *r = &c->saved.commands[i];
        if (!r->used || r->request.action != SP_WATER || r->status == SP_REJECTED)
            continue;
        if (r->status == SP_FAILED && !r->finished_ms)
            known = false;
        if (r->status == SP_COMPLETED && r->finished_ms > last)
            last = r->finished_ms;
        if (r->finished_ms && r->finished_ms / SP_DAY_MS == utc / SP_DAY_MS)
            daily += r->estimated_ul;
    }
    sp_message_set(out, SP_K_FINISHED, last);
    if (known)
        sp_message_set(out, SP_K_VOLUME_UL, daily);
    if (c->active_slot >= 0)
        sp_message_bytes(out, SP_K_ID, c->saved.commands[c->active_slot].request.id);
}
static void result_message(const sp_result_t *r, sp_reason_t reason, sp_message_t *out) {
    sp_message_init(out, SP_RESULT);
    sp_message_bytes(out, SP_K_ID, r->request.id);
    sp_message_set(out, SP_K_STATUS, reason == SP_ID_CONFLICT ? SP_REJECTED : r->status);
    sp_message_set(out, SP_K_REASON, reason);
    sp_message_set(out, SP_K_STARTED, r->started_ms);
    sp_message_set(out, SP_K_FINISHED, r->finished_ms);
    sp_message_set(out, SP_K_VOLUME_UL, r->estimated_ul);
    sp_message_set(out, SP_K_ACTION, r->request.action);
    sp_message_set(out, SP_K_ML, r->request.requested_ml);
}
void sp_config_message(const sp_config_t *c, sp_message_t *m) {
    sp_message_init(m, SP_CONFIG);
    sp_message_set(m, 1, c->revision);
    sp_message_set(m, 3, c->automatic);
    sp_message_set(m, 4, c->portion_ml);
    sp_message_set(m, 8, c->dry_raw);
    sp_message_set(m, 9, c->wet_raw);
    sp_message_set(m, 10, c->threshold_pct);
    sp_message_set(m, 11, c->daily_ml);
    sp_message_set(m, 12, c->max_run_s);
    sp_message_set(m, 13, c->interval_min);
    sp_message_set(m, 14, c->max_age_s);
    sp_message_set(m, 19, c->flow_ul_s);
}
bool sp_node_dispatch(sp_core_t *c, const sp_message_t *m, sp_message_t *out) {
    unsigned kind = (unsigned)m->value[SP_K_KIND];
    if (!sp_message_has(m, 0))
        return false;
    if (kind == SP_CONFIG_GET && shape(m, KEY(0))) {
        sp_config_message(&c->saved.config, out);
        return true;
    }
    if (kind == SP_TIME && shape(m, KEY(0) | KEY(2))) {
        bool ok = sp_core_set_time(c, m->value[2], c->now_ms);
        sp_message_init(out, SP_INFO);
        sp_message_set(out, SP_K_FLAGS, ok ? 0 : 1);
        return true;
    }
    if (kind == SP_COMMAND) {
        uint64_t action = m->value[SP_K_ACTION];
        uint32_t mask = KEY(0) | KEY(3) | KEY(24);
        if (action == SP_WATER)
            mask |= KEY(4) | KEY(5);
        else if (action == SP_STOP)
            mask |= KEY(25);
        else
            return false;
        if (!shape(m, mask) || !uuid(m->bytes[0]) || (action == SP_STOP && !uuid(m->bytes[1])) ||
            (action == SP_WATER && m->value[4] > UINT16_MAX))
            return false;
        sp_request_t req = {
            .action = (sp_action_t)action, .requested_ml = (uint16_t)m->value[4], .expires_ms = m->value[5]};
        memcpy(req.id, m->bytes[0], 16);
        if (action == SP_STOP)
            memcpy(req.target, m->bytes[1], 16);
        sp_result_t r;
        sp_reason_t reason = sp_core_command(c, &req, false, &r);
        result_message(&r, reason, out);
        return true;
    }
    if (kind == SP_QUERY && shape(m, KEY(0) | KEY(24))) {
        const sp_result_t *r = sp_core_result(c, m->bytes[0]);
        if (r)
            result_message(r, r->reason, out);
        else {
            sp_message_init(out, SP_RESULT);
            sp_message_bytes(out, SP_K_ID, m->bytes[0]);
            sp_message_set(out, SP_K_FLAGS, 1);
        }
        return true;
    }
    if (kind == SP_CONFIG) {
        uint32_t mask = KEY(0) | KEY(1) | KEY(3) | KEY(4) | KEY(8) | KEY(9) | KEY(10) | KEY(11) | KEY(12) |
                        KEY(13) | KEY(14) | KEY(19) | KEY(24);
        if (!shape(m, mask) || !uuid(m->bytes[0]) || m->value[1] > UINT32_MAX || m->value[19] > UINT32_MAX ||
            m->value[3] > 1)
            return false;
        const unsigned short_keys[] = {4, 8, 9, 10, 11, 12, 13, 14};
        for (unsigned i = 0; i < sizeof(short_keys) / sizeof(short_keys[0]); i++)
            if (m->value[short_keys[i]] > UINT16_MAX)
                return false;
        sp_config_t cfg = {.automatic = m->value[3],
                           .portion_ml = m->value[4],
                           .dry_raw = m->value[8],
                           .wet_raw = m->value[9],
                           .threshold_pct = m->value[10],
                           .daily_ml = m->value[11],
                           .max_run_s = m->value[12],
                           .interval_min = m->value[13],
                           .max_age_s = m->value[14],
                           .flow_ul_s = m->value[19]};
        sp_message_t current;
        sp_config_message(&c->saved.config, &current);
        bool identical = true;
        for (unsigned key = 0; key < 24; key++)
            if (key != 1 && sp_message_has(&current, key) && current.value[key] != m->value[key])
                identical = false;
        bool repeated = c->storage_ok && m->value[1] < UINT32_MAX &&
                        c->saved.config.revision == m->value[1] + 1 && identical;
        bool ok = repeated || sp_core_configure(c, &cfg, (uint32_t)m->value[1]);
        sp_message_init(out, SP_CONFIG_RESULT);
        sp_message_bytes(out, SP_K_ID, m->bytes[0]);
        sp_message_set(out, SP_K_FLAGS, ok ? 0 : 1);
        sp_message_set(out, SP_K_REVISION, c->saved.config.revision);
        return true;
    }
    if (kind == SP_EVENT_QUERY && shape(m, KEY(0) | KEY(21))) {
        bool gap;
        const sp_event_t *e = sp_core_event_after(c, m->value[21], &gap);
        sp_message_init(out, SP_EVENT);
        sp_message_set(out, SP_K_FLAGS, gap ? 1 : !e ? 2 : 0);
        if (e) {
            sp_message_set(out, SP_K_SEQUENCE, e->seq);
            sp_message_set(out, SP_K_TIME, e->utc_ms);
            sp_message_set(out, SP_K_UPTIME, e->uptime_ms);
            sp_message_set(out, SP_K_MODE, e->source);
            sp_message_set(out, SP_K_ACTION, e->kind);
            sp_message_set(out, SP_K_STATUS, e->status);
            sp_message_set(out, SP_K_REASON, e->reason);
            sp_message_bytes(out, SP_K_ID, e->command_id);
        } else
            sp_message_set(out, SP_K_SEQUENCE, c->saved.event_seq);
        return true;
    }
    return false;
}
bool sp_registry_accept(sp_registry_entry_t *entry, const sp_message_t *m, uint64_t now) {
    if (m->value[SP_K_PUMP] && !sp_message_has(m, SP_K_ID))
        return false;
    const unsigned keys[] = {0, 2, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 21, 22};
    uint32_t required = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        required |= KEY(keys[i]);
    if (m->value[0] != SP_STATE || (m->present & ~(KEY(24) | KEY(18) | KEY(19))) != required ||
        m->value[7] >= SP_REASON_COUNT || m->value[8] > 4095 || m->value[9] > 100 ||
        m->value[10] > SP_Q_STALE || m->value[11] > 2 || m->value[12] > 1 || m->value[13] > UINT32_MAX ||
        m->value[20] > 1 || m->value[22] > 4095)
        return false;
    entry->state = *m;
    entry->have_state = true;
    entry->received_ms = now;
    return true;
}
bool sp_registry_online(const sp_registry_entry_t *e, uint64_t now) {
    return e->have_state && now >= e->received_ms && now - e->received_ms <= SP_OFFLINE_MS;
}
static bool ram_append(void *ctx, const sp_history_record_t *r) {
    sp_ram_history_t *h = ctx;
    for (size_t i = 0; i < h->count; i++) {
        const sp_history_record_t *old =
            &h->records[(h->head + SP_HISTORY_SLOTS - h->count + i) % SP_HISTORY_SLOTS];
        if (!memcmp(old->device, r->device, 6) && old->event.seq == r->event.seq)
            return true;
    }
    h->records[h->head] = *r;
    h->head = (h->head + 1) % SP_HISTORY_SLOTS;
    if (h->count < SP_HISTORY_SLOTS)
        h->count++;
    return true;
}
static bool ram_bounds(void *ctx, const uint8_t device[6], uint64_t *first, uint64_t *last) {
    sp_ram_history_t *h = ctx;
    *first = UINT64_MAX;
    *last = 0;
    for (size_t i = 0; i < h->count; i++) {
        const sp_history_record_t *r =
            &h->records[(h->head + SP_HISTORY_SLOTS - h->count + i) % SP_HISTORY_SLOTS];
        if (memcmp(r->device, device, 6))
            continue;
        if (r->event.seq < *first)
            *first = r->event.seq;
        if (r->event.seq > *last)
            *last = r->event.seq;
    }
    return *last != 0;
}
static bool ram_read(void *ctx, const uint8_t device[6], uint64_t after, sp_history_record_t *out,
                     bool *gap) {
    sp_ram_history_t *h = ctx;
    uint64_t first, last;
    *gap = false;
    if (!ram_bounds(ctx, device, &first, &last))
        return false;
    *gap = after < first - 1;
    if (*gap || after >= last)
        return false;
    for (size_t i = 0; i < h->count; i++) {
        const sp_history_record_t *r =
            &h->records[(h->head + SP_HISTORY_SLOTS - h->count + i) % SP_HISTORY_SLOTS];
        if (!memcmp(r->device, device, 6) && r->event.seq == after + 1) {
            *out = *r;
            return true;
        }
    }
    *gap = true;
    return false;
}
sp_history_store_t sp_ram_history_store(sp_ram_history_t *h) {
    return (sp_history_store_t){.ctx = h, .append = ram_append, .read_after = ram_read, .bounds = ram_bounds};
}
