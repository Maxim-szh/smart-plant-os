#include "sp_core.h"
#include <limits.h>
#include <string.h>

static const char *const reasons[] = {
    "ready",           "soil_not_dry",     "sensor_stale",      "sensor_fault",        "sensor_uncalibrated",
    "tank_empty",      "tank_unknown",     "pump_uncalibrated", "cooldown_active",     "daily_limit",
    "single_limit",    "pump_busy",        "clock_unset",       "hardware_fault",      "config_invalid",
    "command_expired", "command_conflict", "storage_fault",     "watering_no_response"};
const char *sp_reason_name(sp_reason_t r) {
    return (unsigned)r < SP_REASON_COUNT ? reasons[r] : "hardware_fault";
}
const char *sp_quality_name(sp_quality_t q) {
    static const char *const names[] = {"missing", "uncalibrated", "ok", "fault", "stale"};
    return (unsigned)q < 5 ? names[q] : "fault";
}
const char *sp_status_name(sp_status_t s) {
    static const char *const names[] = {"accepted", "running", "completed", "stopped", "failed", "rejected"};
    return (unsigned)s < 6 ? names[s] : "failed";
}
sp_config_t sp_config_default(void) {
    return (sp_config_t){.threshold_pct = 40,
                         .portion_ml = 30,
                         .daily_ml = 120,
                         .max_run_s = 15,
                         .interval_min = 360,
                         .max_age_s = 30};
}
bool sp_config_valid(const sp_config_t *c) {
    int span = (int)c->dry_raw - c->wet_raw;
    bool soil = c->dry_raw <= 4095 && c->wet_raw <= 4095 && (span >= 200 || span <= -200);
    if (!(soil || (c->dry_raw == 0 && c->wet_raw == 0)))
        return false;
    if (c->threshold_pct < 1 || c->threshold_pct > 95 || c->portion_ml < 10 || c->portion_ml > 300 ||
        c->daily_ml < c->portion_ml || c->daily_ml > 1000 || c->max_run_s < 1 || c->max_run_s > 30 ||
        c->interval_min < 60 || c->interval_min > 10080 || c->max_age_s < 30 || c->max_age_s > 900 ||
        c->flow_ul_s > 1000000)
        return false;
    if (c->flow_ul_s && (uint64_t)c->portion_ml * 1000 > (uint64_t)c->max_run_s * c->flow_ul_s)
        return false;
    return !c->automatic || (soil && c->flow_ul_s);
}
static bool persist(sp_core_t *c) {
    if (!c->save || !c->save(c->io, &c->saved)) {
        c->storage_ok = false;
        c->last_reason = SP_STORAGE_FAULT;
        return false;
    }
    return true;
}
static void event(sp_core_t *c, uint8_t kind, sp_status_t status, sp_reason_t reason, const uint8_t *id) {
    if (c->saved.event_seq == UINT64_MAX) {
        c->storage_ok = false;
        return;
    }
    uint64_t seq = ++c->saved.event_seq;
    sp_event_t *e = &c->saved.events[(seq - 1) % SP_EVENT_SLOTS];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->utc_ms = sp_core_utc(c, c->now_ms);
    e->uptime_ms = c->now_ms - c->boot_ms;
    e->kind = kind;
    e->status = status;
    e->reason = reason;
    if (id) {
        memcpy(e->command_id, id, 16);
        const sp_result_t *r = sp_core_result(c, id);
        e->source = r && r->automatic ? 2 : 1;
    } else
        e->source = kind == 2 || kind == 5 ? 1 : 0;
}
const sp_event_t *sp_core_event_after(const sp_core_t *c, uint64_t after, bool *expired) {
    uint64_t last = c->saved.event_seq, first = last >= SP_EVENT_SLOTS ? last - SP_EVENT_SLOTS + 1 : 1;
    *expired = after < first - 1;
    if (*expired || after >= last)
        return NULL;
    return &c->saved.events[after % SP_EVENT_SLOTS];
}
bool sp_core_acknowledge_fault(sp_core_t *c) {
    if (!c->storage_ok || c->active_slot >= 0)
        return false;
    c->saved.response_fault = false;
    c->soak_pending = false;
    event(c, 5, SP_COMPLETED, SP_OK, NULL);
    return persist(c);
}
bool sp_core_init(sp_core_t *c, const sp_persist_t *p, sp_save_fn save, sp_pump_fn pump, void *io,
                  uint64_t now) {
    memset(c, 0, sizeof(*c));
    c->save = save;
    c->pump = pump;
    c->io = io;
    c->active_slot = -1;
    c->tank = -1;
    c->boot_ms = c->now_ms = now;
    c->dry_armed = true;
    c->storage_ok = true;
    if (pump)
        pump(io, false, 0);
    if (p) {
        if (p->version != SP_CORE_VERSION || !sp_config_valid(&p->config)) {
            c->storage_ok = false;
            return false;
        }
        c->saved = *p;
    } else {
        c->saved.version = SP_CORE_VERSION;
        c->saved.config = sp_config_default();
    }
    if (c->saved.generation == UINT64_MAX) {
        c->storage_ok = false;
        return false;
    }
    c->saved.generation++;
    if (c->saved.cooldown_until_utc)
        c->cooldown_until_ms = now + SP_HARD_RUN_MS + (uint64_t)c->saved.config.interval_min * 60000;
    bool recovered = false;
    for (size_t i = 0; i < SP_COMMAND_SLOTS; i++) {
        sp_result_t *r = &c->saved.commands[i];
        if (r->used && (r->status == SP_RUNNING || r->status == SP_ACCEPTED)) {
            r->status = SP_FAILED;
            r->reason = SP_HARDWARE_FAULT;
            recovered = true;
        }
    }
    /* Unknown run time after a crash: retain full reserved volume and require inspection. */
    if (recovered)
        c->saved.response_fault = true;
    c->last_reason = SP_CLOCK_UNSET;
    event(c, 1, recovered ? SP_FAILED : SP_COMPLETED, recovered ? SP_HARDWARE_FAULT : SP_OK, NULL);
    return persist(c);
}
uint64_t sp_core_utc(const sp_core_t *c, uint64_t now) {
    return c->clock_set && now >= c->mono_anchor ? c->utc_anchor + now - c->mono_anchor : 0;
}
bool sp_core_set_time(sp_core_t *c, uint64_t utc, uint64_t now) {
    if (utc < SP_MIN_EPOCH_MS || utc > SP_MAX_EPOCH_MS || now < c->now_ms)
        return false;
    if (c->clock_set) {
        uint64_t expected = sp_core_utc(c, now);
        uint64_t delta = utc > expected ? utc - expected : expected - utc;
        if (delta > 5000)
            return false;
        if (utc < expected)
            utc = expected;
    }
    c->clock_set = true;
    c->utc_anchor = utc;
    c->mono_anchor = now;
    c->now_ms = now;
    if (c->saved.cooldown_until_utc > utc) {
        uint64_t until = now + c->saved.cooldown_until_utc - utc;
        if (until > c->cooldown_until_ms)
            c->cooldown_until_ms = until;
    }
    return true;
}
bool sp_core_configure(sp_core_t *c, const sp_config_t *next, uint32_t revision) {
    if (!c->storage_ok || c->active_slot >= 0 || c->soak_pending || !sp_config_valid(next) ||
        revision != c->saved.config.revision || revision == UINT32_MAX)
        return false;
    sp_config_t old = c->saved.config;
    uint64_t old_seq = c->saved.event_seq;
    if (old_seq == UINT64_MAX)
        return false;
    sp_event_t overwritten = c->saved.events[old_seq % SP_EVENT_SLOTS];
    c->saved.config = *next;
    c->saved.config.revision = revision + 1;
    event(c, 2, SP_COMPLETED, SP_OK, NULL);
    if (!persist(c)) {
        c->saved.config = old;
        c->saved.events[old_seq % SP_EVENT_SLOTS] = overwritten;
        c->saved.event_seq = old_seq;
        return false;
    }
    memset(&c->soil, 0, sizeof(c->soil));
    c->dry_armed = true;
    return true;
}
void sp_core_sample(sp_core_t *c, const uint16_t samples[SP_SAMPLE_COUNT], bool valid, uint64_t now) {
    c->now_ms = now;
    if (!valid || !samples) {
        c->soil.quality = SP_Q_FAULT;
        c->soil.stable_samples = c->soil.dry_samples = 0;
        return;
    }
    uint16_t sorted[SP_SAMPLE_COUNT];
    memcpy(sorted, samples, sizeof(sorted));
    for (size_t i = 1; i < SP_SAMPLE_COUNT; i++) {
        uint16_t v = sorted[i];
        size_t j = i;
        while (j && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    sp_soil_t *s = &c->soil;
    s->raw_adc = sorted[SP_SAMPLE_COUNT / 2];
    s->spread_adc = sorted[SP_SAMPLE_COUNT - 2] - sorted[1];
    s->sampled_ms = now;
    /* A median rejects isolated spikes; the inner spread rejects sustained noise. */
    if (s->raw_adc <= 15 || s->raw_adc >= 4080 || s->spread_adc > 300) {
        s->quality = SP_Q_FAULT;
        s->stable_samples = s->dry_samples = 0;
        return;
    }
    if (!s->stable_samples)
        s->filtered_q8 = (int32_t)s->raw_adc * 256;
    else
        s->filtered_q8 += ((int32_t)s->raw_adc * 256 - s->filtered_q8) / 4;
    if (s->stable_samples < SP_CONFIRM_SAMPLES)
        s->stable_samples++;
    int dry = c->saved.config.dry_raw, wet = c->saved.config.wet_raw;
    if (dry == wet) {
        s->quality = SP_Q_UNCALIBRATED;
        s->dry_samples = 0;
        return;
    }
    int pct = (s->filtered_q8 / 256 - dry) * 100 / (wet - dry);
    s->index_pct = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
    s->quality = s->stable_samples >= SP_CONFIRM_SAMPLES ? SP_Q_OK : SP_Q_STALE;
    if (s->quality == SP_Q_OK && s->index_pct < c->saved.config.threshold_pct) {
        if (s->dry_samples < SP_CONFIRM_SAMPLES)
            s->dry_samples++;
    } else
        s->dry_samples = 0;
    if (s->quality == SP_Q_OK && s->index_pct >= c->saved.config.threshold_pct + 5)
        c->dry_armed = true;
}
void sp_core_tank(sp_core_t *c, int value, uint64_t now) {
    c->now_ms = now;
    c->tank_sampled_ms = now;
    /* Empty/fault immediately; water-present only after 3 consecutive readings. */
    if (value != 1) {
        c->tank = value == 0 ? 0 : -1;
        c->tank_good_samples = 0;
    } else if (++c->tank_good_samples >= 3) {
        c->tank = 1;
        c->tank_good_samples = 3;
    }
}
static bool retained(const sp_core_t *c, const sp_result_t *r) {
    uint64_t utc = sp_core_utc(c, c->now_ms);
    /* A reboot or time correction must not prematurely release reserved water. */
    uint64_t start = r->generation == c->saved.generation ? r->created_monotonic_ms : c->boot_ms;
    return !c->clock_set || c->now_ms < start || c->now_ms - start < SP_DAY_MS ||
           (r->created_ms && (utc < r->created_ms || utc - r->created_ms < SP_DAY_MS));
}
uint32_t sp_core_reserved_ml(const sp_core_t *c) {
    uint32_t sum = 0;
    for (size_t i = 0; i < SP_COMMAND_SLOTS; i++) {
        const sp_result_t *r = &c->saved.commands[i];
        if (r->used && r->request.action == SP_WATER && r->status != SP_REJECTED && retained(c, r))
            sum += r->request.requested_ml;
    }
    return sum;
}
sp_reason_t sp_core_can_water(const sp_core_t *c, uint16_t ml, bool automatic) {
    const sp_config_t *cfg = &c->saved.config;
    if (!c->storage_ok)
        return SP_STORAGE_FAULT;
    if (!c->hardware_ready || !c->pump)
        return SP_HARDWARE_FAULT;
    if (c->saved.response_fault)
        return SP_NO_RESPONSE;
    if (c->active_slot >= 0)
        return SP_BUSY;
    if (c->tank < 0 || c->now_ms - c->tank_sampled_ms > 1500)
        return SP_TANK_UNKNOWN;
    if (!c->tank)
        return SP_TANK_EMPTY;
    if (!cfg->flow_ul_s)
        return SP_PUMP_UNCALIBRATED;
    if (!c->clock_set)
        return SP_CLOCK_UNSET;
    if (c->now_ms < c->cooldown_until_ms)
        return SP_COOLDOWN;
    if (!sp_config_valid(cfg))
        return SP_CONFIG_INVALID;
    if (ml < 10 || ml > 300 || (uint64_t)ml * 1000 > (uint64_t)cfg->max_run_s * cfg->flow_ul_s)
        return SP_SINGLE_LIMIT;
    if (sp_core_reserved_ml(c) + ml > cfg->daily_ml)
        return SP_DAILY_LIMIT;
    if (automatic) {
        if (c->soil.quality == SP_Q_UNCALIBRATED)
            return SP_SENSOR_UNCALIBRATED;
        if (c->soil.quality == SP_Q_MISSING || c->soil.quality == SP_Q_FAULT)
            return SP_SENSOR_FAULT;
        if (c->soil.quality == SP_Q_STALE || c->now_ms - c->soil.sampled_ms > (uint64_t)cfg->max_age_s * 1000)
            return SP_SENSOR_STALE;
        if (!c->dry_armed || c->soil.dry_samples < SP_CONFIRM_SAMPLES)
            return SP_SOIL_NOT_DRY;
    }
    return SP_OK;
}
bool sp_core_auto_due(const sp_core_t *c) {
    return c->saved.config.automatic && sp_core_can_water(c, c->saved.config.portion_ml, true) == SP_OK;
}
const sp_result_t *sp_core_result(const sp_core_t *c, const uint8_t id[16]) {
    for (size_t i = 0; i < SP_COMMAND_SLOTS; i++)
        if (c->saved.commands[i].used && !memcmp(c->saved.commands[i].request.id, id, 16))
            return &c->saved.commands[i];
    return NULL;
}
static void finish(sp_core_t *c, sp_status_t status, sp_reason_t why) {
    if (c->pump)
        c->pump(c->io, false, 0); /* Stop FIRST, even if persistence fails. */
    if (c->active_slot < 0)
        return;
    sp_result_t *r = &c->saved.commands[c->active_slot];
    uint64_t elapsed = c->now_ms - c->run_started_ms;
    if (elapsed > c->run_deadline_ms - c->run_started_ms)
        elapsed = c->run_deadline_ms - c->run_started_ms;
    r->estimated_ul = (uint32_t)(elapsed * c->saved.config.flow_ul_s / 1000);
    r->status = status;
    r->reason = why;
    r->finished_ms = sp_core_utc(c, c->now_ms);
    c->active_slot = -1;
    c->last_reason = why;
    event(c, 3, status, why, r->request.id);
    if (status == SP_COMPLETED && c->soil.quality == SP_Q_OK && c->pre_water_index <= 95) {
        c->soak_pending = true;
        c->soak_at_ms = c->now_ms + SP_SOAK_MS;
    }
    (void)persist(c);
}
void sp_core_tick(sp_core_t *c, uint64_t now) {
    c->now_ms = now;
    if (c->soil.quality == SP_Q_OK && now - c->soil.sampled_ms > (uint64_t)c->saved.config.max_age_s * 1000) {
        c->soil.quality = SP_Q_STALE;
        c->soil.stable_samples = c->soil.dry_samples = 0;
    }
    if (c->active_slot >= 0) {
        if (!c->storage_ok)
            finish(c, SP_FAILED, SP_STORAGE_FAULT);
        else if (!c->hardware_ready)
            finish(c, SP_FAILED, SP_HARDWARE_FAULT);
        else if (c->tank < 0 || now - c->tank_sampled_ms > 1500)
            finish(c, SP_FAILED, SP_TANK_UNKNOWN);
        else if (!c->tank)
            finish(c, SP_FAILED, SP_TANK_EMPTY);
        else if (now >= c->run_deadline_ms)
            finish(c, SP_COMPLETED, SP_OK);
    }
    if (c->soak_pending && now >= c->soak_at_ms) {
        c->soak_pending = false;
        if (c->soil.quality != SP_Q_OK || c->soil.index_pct < c->pre_water_index + 3) {
            c->saved.response_fault = true;
            c->last_reason = SP_NO_RESPONSE;
            event(c, 4, SP_FAILED, SP_NO_RESPONSE, NULL);
            (void)persist(c);
        }
    }
    if (c->active_slot < 0)
        c->last_reason = sp_core_can_water(c, c->saved.config.portion_ml, c->saved.config.automatic);
}
sp_reason_t sp_core_command(sp_core_t *c, const sp_request_t *req, bool automatic, sp_result_t *out) {
    const sp_result_t *previous = sp_core_result(c, req->id);
    if (previous) {
        *out = *previous;
        if (previous->request.action != req->action || previous->request.requested_ml != req->requested_ml ||
            memcmp(previous->request.target, req->target, 16))
            return SP_ID_CONFLICT;
        return previous->reason;
    }
    memset(out, 0, sizeof(*out));
    out->used = true;
    out->request = *req;
    out->automatic = automatic;
    out->status = SP_REJECTED;
    out->created_ms = sp_core_utc(c, c->now_ms);
    out->generation = c->saved.generation;
    out->created_monotonic_ms = c->now_ms;
    sp_reason_t why = SP_CONFIG_INVALID;
    if (req->action == SP_STOP) {
        /* Stop requires neither time nor space in the journal. Never stops a different command. */
        if (c->active_slot >= 0 && !memcmp(c->saved.commands[c->active_slot].request.id, req->target, 16))
            finish(c, SP_STOPPED, SP_OK);
        out->status = SP_COMPLETED;
        out->finished_ms = out->created_ms;
        why = SP_OK;
    } else if (req->action == SP_WATER) {
        why = sp_core_can_water(c, req->requested_ml, automatic);
        /* A stop arriving before its delayed water request is a cancellation tombstone. */
        for (size_t i = 0; i < SP_COMMAND_SLOTS; i++) {
            const sp_result_t *r = &c->saved.commands[i];
            if (r->used && r->request.action == SP_STOP && !memcmp(r->request.target, req->id, 16))
                why = SP_EXPIRED;
        }
        if (why == SP_OK && (req->expires_ms < out->created_ms || req->expires_ms - out->created_ms > 30000))
            why = SP_EXPIRED;
    }
    out->reason = why;
    int slot = -1;
    for (size_t i = 0; i < SP_COMMAND_SLOTS; i++)
        if (!c->saved.commands[i].used || !retained(c, &c->saved.commands[i])) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        if (req->action == SP_WATER)
            out->reason = SP_STORAGE_FAULT;
        return out->reason;
    }
    if (req->action == SP_WATER && why == SP_OK) {
        uint32_t duration =
            (uint32_t)(((uint64_t)req->requested_ml * 1000000 + c->saved.config.flow_ul_s - 1) /
                       c->saved.config.flow_ul_s);
        out->status = SP_RUNNING;
        out->started_ms = out->created_ms;
        c->saved.cooldown_until_utc =
            out->created_ms + duration + (uint64_t)c->saved.config.interval_min * 60000;
        c->cooldown_until_ms = c->now_ms + duration + (uint64_t)c->saved.config.interval_min * 60000;
        c->saved.commands[slot] = *out;
        event(c, 3, SP_RUNNING, SP_OK, req->id);
        if (!persist(c)) {
            out->status = SP_FAILED;
            out->reason = SP_STORAGE_FAULT;
            c->saved.commands[slot] = *out;
            return out->reason;
        }
        c->active_slot = slot;
        c->run_started_ms = c->now_ms;
        c->run_deadline_ms = c->now_ms + duration;
        c->dry_armed = false;
        c->pre_water_index = c->soil.index_pct;
        if (c->pump)
            c->pump(c->io, true, duration);
    } else {
        c->saved.commands[slot] = *out;
        event(c, 3, out->status, out->reason, req->id);
        if (!persist(c) && req->action != SP_STOP)
            out->reason = SP_STORAGE_FAULT;
    }
    return out->reason;
}
