#include "sp_core.h"
#include "sp_soil_policy.h"
#include <string.h>

/* Analog adapter only. Digital/normalized drivers call sp_core_observe_soil directly. */
void sp_core_sample(sp_core_t *c, const uint16_t samples[SP_SAMPLE_COUNT], bool valid, uint64_t now) {
    sp_soil_t *s = &c->soil;
    if (!valid || !samples) {
        sp_core_observe_soil(c, SP_Q_FAULT, 0, 0, 0, now);
        return;
    }
    uint16_t sorted[SP_SAMPLE_COUNT];
    memcpy(sorted, samples, sizeof(sorted));
    for (unsigned i = 0; i < SP_SAMPLE_COUNT; i++)
        if (sorted[i] > 4095) {
            sp_core_observe_soil(c, SP_Q_FAULT, 0, 0, 0, now);
            return;
        }
    for (unsigned i = 1; i < SP_SAMPLE_COUNT; i++) {
        uint16_t v = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    uint16_t raw = sorted[SP_SAMPLE_COUNT / 2], spread = sorted[SP_SAMPLE_COUNT - 2] - sorted[1];
    if (raw <= 15 || raw >= 4080 || spread > 300) {
        sp_core_observe_soil(c, SP_Q_FAULT, 0, raw, spread, now);
        return;
    }
    int span = (int)c->saved.config.dry_raw - c->saved.config.wet_raw;
    if (span < 0)
        span = -span;
    int jump_limit = span >= 200 ? span * 15 / 100 : 300;
    if (jump_limit < 100)
        jump_limit = 100;
    int distance = (int)raw - s->analog_reference;
    if (distance < 0)
        distance = -distance;
    if (s->analog_seen && distance > jump_limit) {
        int candidate_distance = (int)raw - s->candidate_raw;
        if (candidate_distance < 0)
            candidate_distance = -candidate_distance;
        int tolerance = span / 30;
        if (tolerance < 30)
            tolerance = 30;
        if (!s->candidate_samples || candidate_distance > tolerance) {
            s->candidate_raw = raw;
            s->candidate_samples = 1;
        } else if (s->candidate_samples < 3)
            s->candidate_samples++;
        s->watch_until_ms = now + SP_SOIL_ANOMALY_HOLD_MS;
        if (s->candidate_samples < 3) {
            sp_core_observe_soil(c, SP_Q_STALE, 0, raw, spread, now);
            return;
        }
        /* Confirmed new electrical state, not proof of correct soil contact. */
        s->stable_samples = 0;
    }
    s->candidate_samples = 0;
    s->analog_reference = raw;
    s->analog_seen = true;
    s->analog_at_ms = now;
    if (!s->stable_samples)
        s->filtered_q8 = (int32_t)raw * 256;
    else
        s->filtered_q8 += ((int32_t)raw * 256 - s->filtered_q8) / 4;
    int dry = c->saved.config.dry_raw, wet = c->saved.config.wet_raw;
    if (dry == wet) {
        sp_core_observe_soil(c, SP_Q_UNCALIBRATED, 0, raw, spread, now);
        return;
    }
    int pct = (s->filtered_q8 / 256 - dry) * 100 / (wet - dry);
    sp_core_observe_soil(c, SP_Q_OK, pct < 0 ? 0 : pct > 100 ? 100 : pct, raw, spread, now);
}
