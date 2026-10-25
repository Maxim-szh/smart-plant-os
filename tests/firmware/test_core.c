#include "sp_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    sp_persist_t disk;
    bool fail, on;
    unsigned starts, saves;
} fixture_t;
static bool save(void *io, const sp_persist_t *p) {
    fixture_t *f = io;
    f->saves++;
    if (f->fail)
        return false;
    f->disk = *p;
    return true;
}
static void pump(void *io, bool on, uint32_t ms) {
    fixture_t *f = io;
    f->on = on;
    if (on) {
        assert(ms > 0 && ms <= SP_HARD_RUN_MS);
        f->starts++;
        bool persisted = false;
        for (unsigned i = 0; i < SP_COMMAND_SLOTS; i++)
            if (f->disk.commands[i].status == SP_RUNNING && f->disk.commands[i].used)
                persisted = true;
        assert(persisted); /* Write-ahead invariant, not just final outcome. */
    }
}
static void tank(sp_core_t *c, uint64_t t) {
    for (int i = 0; i < 3; i++)
        sp_core_tank(c, 1, t);
}
static void setup(sp_core_t *c, fixture_t *f) {
    memset(f, 0, sizeof(*f));
    assert(sp_core_init(c, NULL, save, pump, f, 0));
    sp_config_t cfg = sp_config_default();
    cfg.dry_raw = 3000;
    cfg.wet_raw = 1000;
    cfg.flow_ul_s = 3000;
    cfg.interval_min = 60;
    assert(sp_core_configure(c, &cfg, 0));
    c->hardware_ready = true;
    assert(sp_core_set_time(c, SP_MIN_EPOCH_MS, 0));
    tank(c, 0);
}
static sp_request_t request(unsigned id, sp_core_t *c) {
    sp_request_t r = {
        .id = {0}, .action = SP_WATER, .requested_ml = 30, .expires_ms = sp_core_utc(c, c->now_ms) + 10000};
    r.id[0] = (uint8_t)id;
    r.id[6] = 0x40;
    r.id[8] = 0x80;
    return r;
}
static void sample(sp_core_t *c, uint16_t raw, unsigned n, uint64_t *t) {
    uint16_t a[SP_SAMPLE_COUNT];
    for (unsigned j = 0; j < SP_SAMPLE_COUNT; j++)
        a[j] = raw;
    for (unsigned i = 0; i < n; i++) {
        *t += 500;
        sp_core_sample(c, a, true, *t);
        tank(c, *t);
        sp_core_tick(c, *t);
    }
}
static void test_sensor(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    uint64_t t = 0;
    sample(&c, 2900, 11, &t);
    assert(c.soil.quality == SP_Q_OK && c.soil.index_pct == 5);
    assert(sp_core_can_water(&c, 30, true) == SP_OK);
    uint16_t spikes[] = {0, 2900, 2900, 2900, 2900, 2900, 2900, 2900, 4095};
    sp_core_sample(&c, spikes, true, t);
    assert(c.soil.quality == SP_Q_OK && c.soil.raw_adc == 2900);
    uint16_t noise[] = {1000, 1000, 1000, 1000, 2900, 3900, 3900, 3900, 3900};
    sp_core_sample(&c, noise, true, t);
    assert(c.soil.quality == SP_Q_FAULT);
    assert(sp_core_can_water(&c, 30, true) == SP_SENSOR_FAULT);
    sample(&c, 2900, 11, &t);
    tank(&c, t + 31000);
    sp_core_tick(&c, t + 31000);
    assert(sp_core_can_water(&c, 30, true) == SP_SENSOR_STALE);
    sample(&c, 0, 1, &t);
    assert(c.soil.quality == SP_Q_FAULT);
    sample(&c, 4095, 1, &t);
    assert(c.soil.quality == SP_Q_FAULT);
    c.saved.config.dry_raw = 1000;
    c.saved.config.wet_raw = 3000;
    sample(&c, 1100, 20, &t);
    assert(c.soil.index_pct < 10);
    c.saved.config.dry_raw = c.saved.config.wet_raw = 0;
    sample(&c, 2000, 1, &t);
    assert(c.soil.quality == SP_Q_UNCALIBRATED);
}
static void test_commands(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    sp_result_t out;
    sp_request_t r = request(1, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK && f.on && f.starts == 1);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK && f.starts == 1);
    r.requested_ml = 31;
    assert(sp_core_command(&c, &r, false, &out) == SP_ID_CONFLICT);
    r.requested_ml = 30;
    sp_request_t other = request(2, &c);
    assert(sp_core_command(&c, &other, false, &out) == SP_BUSY);
    tank(&c, 10000);
    sp_core_tick(&c, 10000);
    assert(!f.on);
    const sp_result_t *done = sp_core_result(&c, r.id);
    assert(done && done->status == SP_COMPLETED && done->estimated_ul == 30000);
    assert(sp_core_reserved_ml(&c) == 30 && sp_core_can_water(&c, 30, false) == SP_COOLDOWN);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK && out.status == SP_COMPLETED && f.starts == 1);
    sp_core_t reboot;
    assert(sp_core_init(&reboot, &f.disk, save, pump, &f, 0));
    reboot.hardware_ready = true;
    tank(&reboot, 0);
    assert(sp_core_can_water(&reboot, 30, false) == SP_CLOCK_UNSET);
    assert(sp_core_set_time(&reboot, SP_MIN_EPOCH_MS + SP_DAY_MS * 100, 0));
    assert(sp_core_reserved_ml(&reboot) == 30); /* Reboot + forward clock cannot release budget. */
    assert(!sp_core_set_time(&reboot, SP_MIN_EPOCH_MS, 1));
    assert(sp_core_command(&reboot, &r, false, &out) == SP_OK && f.starts == 1);
}
static void test_failure_and_stop(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    sp_result_t out;
    sp_request_t r = request(1, &c);
    f.fail = true;
    assert(sp_core_command(&c, &r, false, &out) == SP_STORAGE_FAULT && !f.on && !f.starts);
    setup(&c, &f);
    r = request(1, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK);
    sp_core_t reboot;
    assert(sp_core_init(&reboot, &f.disk, save, pump, &f, 0));
    assert(!f.on && reboot.saved.response_fault && sp_core_reserved_ml(&reboot) == 30);
    assert(sp_core_acknowledge_fault(&reboot) && sp_core_reserved_ml(&reboot) == 30);
    setup(&c, &f);
    r = request(1, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK);
    sp_core_tank(&c, 0, 100);
    sp_core_tick(&c, 100);
    assert(!f.on && sp_core_result(&c, r.id)->reason == SP_TANK_EMPTY);
    setup(&c, &f);
    r = request(1, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK);
    sp_request_t stop = request(2, &c);
    stop.action = SP_STOP;
    stop.requested_ml = 0;
    memcpy(stop.target, r.id, 16);
    f.fail = true;
    assert(sp_core_command(&c, &stop, false, &out) == SP_OK && !f.on);
    setup(&c, &f);
    r = request(1, &c);
    stop = request(2, &c);
    stop.action = SP_STOP;
    stop.requested_ml = 0;
    memcpy(stop.target, r.id, 16);
    assert(sp_core_command(&c, &stop, false, &out) == SP_OK);
    assert(sp_core_command(&c, &r, false, &out) == SP_EXPIRED && !f.starts); /* Reordered packets. */
}
static void test_limits(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    sp_result_t out;
    sp_request_t r = request(1, &c);
    r.expires_ms = SP_MIN_EPOCH_MS - 1;
    assert(sp_core_command(&c, &r, false, &out) == SP_EXPIRED && !f.starts);
    setup(&c, &f);
    assert(sp_core_can_water(&c, 301, false) == SP_SINGLE_LIMIT);
    sp_core_tick(&c, 2000);
    assert(sp_core_can_water(&c, 30, false) == SP_TANK_UNKNOWN);
    setup(&c, &f);
    c.saved.config.daily_ml = 30;
    r = request(1, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_OK);
    tank(&c, 10000);
    sp_core_tick(&c, 10000);
    tank(&c, 4000000);
    sp_core_tick(&c, 4000000);
    assert(sp_core_can_water(&c, 30, false) == SP_DAILY_LIMIT);
    tank(&c, SP_DAY_MS + 1);
    sp_core_tick(&c, SP_DAY_MS + 1);
    assert(sp_core_can_water(&c, 30, false) == SP_OK);
    setup(&c, &f);
    for (unsigned i = 0; i < SP_COMMAND_SLOTS; i++) {
        r = request(i, &c);
        r.requested_ml = 1;
        assert(sp_core_command(&c, &r, false, &out) == SP_SINGLE_LIMIT);
    }
    r = request(200, &c);
    assert(sp_core_command(&c, &r, false, &out) == SP_STORAGE_FAULT && !f.starts);
}
static void test_soak_and_config(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    uint64_t t = 0;
    sample(&c, 2900, 11, &t);
    sp_result_t out;
    sp_request_t r = request(1, &c);
    assert(sp_core_command(&c, &r, true, &out) == SP_OK);
    sp_config_t cfg = c.saved.config;
    assert(!sp_core_configure(&c, &cfg, cfg.revision));
    sample(&c, 2900, 261, &t);
    assert(c.saved.response_fault && !f.on);
    assert(sp_core_can_water(&c, 30, false) == SP_NO_RESPONSE);
    assert(sp_core_acknowledge_fault(&c));
    assert(sp_core_can_water(&c, 30, false) == SP_COOLDOWN);
    cfg = c.saved.config;
    assert(!sp_core_configure(&c, &cfg, cfg.revision + 1));
    cfg.dry_raw = 1000;
    cfg.wet_raw = 1100;
    assert(!sp_config_valid(&cfg));
    cfg = c.saved.config;
    cfg.flow_ul_s = 1;
    assert(!sp_config_valid(&cfg));
    cfg = c.saved.config;
    cfg.threshold_pct = 96;
    assert(!sp_config_valid(&cfg));
}
static void test_protocol(void) {
    sp_message_t m, d;
    sp_message_init(&m, SP_TIME);
    sp_message_set(&m, SP_K_TIME, SP_MIN_EPOCH_MS);
    const uint8_t golden[] = {0xa2, 0x00, 0x03, 0x02, 0x1b, 0x00, 0x00, 0x01, 0x8c, 0xc2, 0x51, 0xf4, 0x00};
    uint8_t bytes[SP_CBOR_MAX];
    size_t n;
    assert(sp_cbor_encode(&m, bytes, sizeof(bytes), &n));
    assert(n == sizeof(golden) && !memcmp(bytes, golden, n));
    assert(sp_cbor_decode(&d, bytes, n));
    for (size_t i = 0; i < n; i++)
        assert(!sp_cbor_decode(&d, bytes, i));
    const uint8_t dup[] = {0xa2, 0, 1, 0, 2}, noncanonical[] = {0xa1, 0, 0x18, 1},
                  indefinite[] = {0xbf, 0, 1, 0xff};
    assert(!sp_cbor_decode(&d, dup, sizeof(dup)));
    assert(!sp_cbor_decode(&d, noncanonical, sizeof(noncanonical)));
    assert(!sp_cbor_decode(&d, indefinite, sizeof(indefinite)));
    sp_header_t h = {.role = 1, .boot = 2, .sequence = 3, .target_boot = 4}, back;
    uint8_t frame[32] = {0}, nonce[12];
    sp_header_encode(&h, frame);
    assert(sp_header_decode(&back, frame, sizeof(frame)));
    sp_nonce(&h, nonce);
    assert(nonce[3] == 1 && nonce[7] == 2 && nonce[11] == 3);
    frame[2] = 2;
    assert(!sp_header_decode(&back, frame, sizeof(frame)));
    /* Bounded malformed-packet corpus under ASan/UBSan. */
    uint32_t random = 1234567;
    for (unsigned i = 0; i < 20000; i++) {
        n = i % sizeof(bytes);
        for (size_t j = 0; j < n; j++) {
            random = random * 1664525 + 1013904223;
            bytes[j] = (uint8_t)(random >> 24);
        }
        if (sp_cbor_decode(&d, bytes, n)) {
            uint8_t roundtrip[SP_CBOR_MAX];
            size_t size;
            assert(sp_cbor_encode(&d, roundtrip, sizeof(roundtrip), &size));
            assert(size == n && !memcmp(bytes, roundtrip, n));
        }
    }
}
static void test_services_history(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    sp_message_t m, out, decoded;
    sp_request_t req = request(1, &c);
    sp_message_init(&m, SP_COMMAND);
    sp_message_set(&m, 3, SP_WATER);
    sp_message_set(&m, 4, 30);
    sp_message_set(&m, 5, req.expires_ms);
    sp_message_bytes(&m, 24, req.id);
    uint8_t wire[SP_CBOR_MAX];
    size_t len;
    assert(sp_cbor_encode(&m, wire, sizeof(wire), &len));
    assert(sp_cbor_decode(&decoded, wire, len));
    assert(sp_node_dispatch(&c, &decoded, &out));
    assert(f.starts == 1);
    /* Lose the response and retransmit the request: exactly one physical start. */
    assert(sp_node_dispatch(&c, &decoded, &out) && f.starts == 1);
    sp_message_set(&decoded, 4, UINT64_MAX);
    assert(!sp_node_dispatch(&c, &decoded, &out));
    sp_node_state(&c, &m);
    assert(sp_cbor_encode(&m, wire, sizeof(wire), &len));
    assert(len + 32 <= 200);
    sp_registry_entry_t e = {0};
    assert(!sp_registry_online(&e, 0));
    assert(sp_registry_accept(&e, &m, 100));
    assert(sp_registry_online(&e, 100) && !sp_registry_online(&e, 100 + SP_OFFLINE_MS + 1));
    sp_message_set(&m, 10, 99);
    assert(!sp_registry_accept(&e, &m, 200));
    bool gap;
    const sp_event_t *event = sp_core_event_after(&c, 0, &gap);
    assert(event && !gap && event->seq == 1);
    sp_ram_history_t history = {0};
    sp_history_store_t store = sp_ram_history_store(&history);
    sp_history_record_t record = {.device = {1}};
    for (unsigned i = 1; i <= SP_HISTORY_SLOTS + 2; i++) {
        record.event.seq = i;
        assert(store.append(store.ctx, &record));
    }
    assert(store.append(store.ctx, &record) && history.count == SP_HISTORY_SLOTS);
    assert(!store.read_after(store.ctx, record.device, 0, &record, &gap) && gap);
    assert(store.read_after(store.ctx, record.device, 2, &record, &gap) && !gap && record.event.seq == 3);
    assert(!memcmp(record.device, (uint8_t[]){1, 0, 0, 0, 0, 0}, 6));
}
static void test_clock_and_config_retries(void) {
    sp_core_t c;
    fixture_t f;
    setup(&c, &f);
    /* Lost config acknowledgement: same values/revision are idempotent, changed values conflict. */
    sp_config_t cfg = c.saved.config;
    cfg.portion_ml = 20;
    sp_message_t request_msg, response;
    sp_config_message(&cfg, &request_msg);
    sp_request_t r = request(1, &c);
    sp_message_bytes(&request_msg, SP_K_ID, r.id);
    assert(sp_node_dispatch(&c, &request_msg, &response) && response.value[SP_K_FLAGS] == 0);
    unsigned saves = f.saves;
    assert(sp_node_dispatch(&c, &request_msg, &response) && response.value[SP_K_FLAGS] == 0 &&
           f.saves == saves);
    sp_message_set(&request_msg, SP_K_ML, 21);
    assert(sp_node_dispatch(&c, &request_msg, &response) && response.value[SP_K_FLAGS] == 1);
    uint32_t revision = c.saved.config.revision;
    uint64_t seq = c.saved.event_seq;
    cfg = c.saved.config;
    cfg.portion_ml = 22;
    f.fail = true;
    assert(!sp_core_configure(&c, &cfg, revision));
    assert(c.saved.config.portion_ml == 20 && c.saved.config.revision == revision &&
           c.saved.event_seq == seq);
    setup(&c, &f);
    uint64_t t = SP_DAY_MS + 1000;
    tank(&c, t);
    sp_core_tick(&c, t);
    r = request(1, &c);
    sp_result_t result;
    assert(sp_core_command(&c, &r, false, &result) == SP_OK);
    tank(&c, t + 10000);
    sp_core_tick(&c, t + 10000);
    /* Many individually permitted clock corrections must not release a recent reservation. */
    for (unsigned i = 0; i < 20000; i++)
        assert(sp_core_set_time(&c, sp_core_utc(&c, c.now_ms) + 5000, c.now_ms));
    assert(sp_core_reserved_ml(&c) == 30);
    sp_core_t reboot;
    assert(sp_core_init(&reboot, &f.disk, save, pump, &f, 0));
    reboot.hardware_ready = true;
    tank(&reboot, 0);
    assert(sp_core_set_time(&reboot, SP_MIN_EPOCH_MS + SP_DAY_MS * 100, 0));
    assert(sp_core_can_water(&reboot, 30, false) == SP_COOLDOWN);
    sp_persist_t bad = f.disk;
    bad.version = 99;
    assert(!sp_core_init(&reboot, &bad, save, pump, &f, 0) && !f.on);
}
int main(void) {
    test_sensor();
    test_commands();
    test_failure_and_stop();
    test_limits();
    test_soak_and_config();
    test_protocol();
    test_services_history();
    test_clock_and_config_retries();
    printf("PASS: 8 scenario groups + 20000 malformed packets; persist=%zu bytes, core=%zu bytes\n",
           sizeof(sp_persist_t), sizeof(sp_core_t));
    return 0;
}
