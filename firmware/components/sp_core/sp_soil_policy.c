#include "sp_soil_policy.h"
uint32_t sp_soil_poll_interval(const sp_core_t *c, uint64_t now) {
    const sp_soil_t *s = &c->soil;
    if (c->active_slot >= 0 || c->soak_pending || now < c->boot_ms + 30000 || s->quality != SP_Q_OK ||
        now < s->watch_until_ms)
        return SP_SOIL_FAST_MS;
    if (s->index_pct <= c->saved.config.threshold_pct + 10)
        return SP_SOIL_WATCH_MS;
    return SP_SOIL_IDLE_MS;
}
