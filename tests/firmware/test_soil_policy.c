#include "sp_soil_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static sp_persist_t disk;
static bool save(void *ctx, const sp_persist_t *p) {
    (void)ctx;
    disk = *p;
    return true;
}
static void pump(void *ctx, bool on, uint32_t ms) {
    (void)ctx;
    (void)on;
    (void)ms;
}
static void start(sp_core_t *c) {
    assert(sp_core_init(c, NULL, save, pump, NULL, 0));
    sp_config_t cfg = sp_config_default();
    cfg.dry_raw = 2350;
    cfg.wet_raw = 792;
    cfg.flow_ul_s = 3000;
    assert(sp_core_configure(c, &cfg, 0));
    c->hardware_ready = true;
    assert(sp_core_set_time(c, SP_MIN_EPOCH_MS, 0));
}
static void feed(sp_core_t *c, unsigned raw, uint64_t now) {
    uint16_t a[9];
    for (unsigned i = 0; i < 9; i++)
        a[i] = raw;
    sp_core_sample(c, a, true, now);
    for (unsigned i = 0; i < 3; i++)
        sp_core_tank(c, 1, now);
    sp_core_tick(c, now);
}
static void dry(sp_core_t *c) {
    for (uint64_t t = c->now_ms + 500; t <= 40000; t += 500)
        feed(c, 2300, t);
}
static void water(sp_core_t *c) {
    sp_request_t req = {
        .id = {1}, .action = SP_WATER, .requested_ml = 30, .expires_ms = SP_MIN_EPOCH_MS + 50000};
    sp_result_t out;
    assert(sp_core_command(c, &req, true, &out) == SP_OK);
    for (uint64_t t = 40500; t <= 50000; t += 500)
        feed(c, 2300, t);
    assert(c->soak_pending && disk.response_fault);
}
int main(void) {
    sp_core_t c;
    start(&c);
    for (uint64_t t = 500; t <= 10000; t += 500)
        feed(&c, 2300, t);
    assert(sp_core_can_water(&c, 30, true) == SP_SOIL_NOT_DRY);
    dry(&c);
    assert(sp_core_can_water(&c, 30, true) == SP_OK);
    assert(sp_soil_poll_interval(&c, 40000) == SP_SOIL_WATCH_MS);
    feed(&c, 1391, 40500);
    assert(c.soil.quality == SP_Q_STALE);
    assert(sp_soil_poll_interval(&c, 40500) == SP_SOIL_FAST_MS);
    feed(&c, 1391, 41000);
    assert(c.soil.quality == SP_Q_STALE);
    for (uint64_t t = 41500; t <= 50000; t += 500)
        feed(&c, 1391, t);
    assert(c.soil.quality == SP_Q_OK && c.soil.index_pct == 61);
    assert(sp_soil_poll_interval(&c, 90000) == SP_SOIL_IDLE_MS);
    feed(&c, 0, 50500);
    assert(c.soil.quality == SP_Q_FAULT);
    start(&c);
    dry(&c);
    water(&c);
    assert(sp_core_can_water(&c, 30, false) == SP_BUSY);
    assert(!sp_core_acknowledge_fault(&c));
    sp_core_t reboot;
    assert(sp_core_init(&reboot, &disk, save, pump, NULL, 0));
    assert(reboot.saved.response_fault); /* no silent loss of pending observation */
    /* Response must persist for 10s after minimum 2min redistribution. */
    for (uint64_t t = 50500; t <= 185000; t += 500)
        feed(&c, 1391, t);
    assert(!c.soak_pending && !c.saved.response_fault && !disk.response_fault);
    start(&c);
    dry(&c);
    water(&c);
    for (uint64_t t = 50500; t <= 651000; t += 500)
        feed(&c, 2300, t);
    assert(!c.soak_pending && c.saved.response_fault);
    assert(sp_core_can_water(&c, 30, false) == SP_NO_RESPONSE);
    /* Repeated tick on one wet sample is not repeated evidence. */
    start(&c);
    dry(&c);
    water(&c);
    for (uint64_t t = 50500; t <= 169500; t += 500)
        feed(&c, 2300, t);
    for (uint64_t t = 170000; t <= 175000; t += 500)
        feed(&c, 1391, t);
    for (uint64_t t = 175001; t <= 185000; t += 100)
        sp_core_tick(&c, t);
    assert(c.soak_pending);
    puts("soil policy: confirmation, anomaly, adaptive cadence, durable response and no stale evidence "
         "passed");
}
