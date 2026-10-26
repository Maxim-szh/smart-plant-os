#include "sp_sensors.h"
#include <limits.h>
#include <string.h>
uint64_t sp_sensor_age(const sp_sensor_driver_t *d, uint64_t now) {
    uint64_t anchor = d->remote ? d->received_ms : d->sample.sampled_ms;
    uint64_t base = d->remote ? d->age_at_receive_ms : 0;
    uint64_t elapsed = now >= anchor ? now - anchor : 0;
    return elapsed > UINT64_MAX - base ? UINT64_MAX : base + elapsed;
}

const char *sp_sensor_kind_name(unsigned k) {
    static const char *const names[] = {"soil_index",  "water_present",   "air_temperature", "air_humidity",
                                        "illuminance", "battery_voltage", "water_flow"};
    return k < SP_SENSOR_KIND_COUNT ? names[k] : "unknown";
}
const char *sp_sensor_unit_name(unsigned u) {
    static const char *const names[] = {"percent", "boolean", "degC", "lux", "V", "mL/s"};
    return u < SP_UNIT_COUNT ? names[u] : "unknown";
}
bool sp_sensor_descriptor_valid(const sp_sensor_descriptor_t *d) {
    static const uint8_t units[] = {SP_UNIT_PERCENT, SP_UNIT_BOOLEAN, SP_UNIT_CELSIUS,  SP_UNIT_PERCENT,
                                    SP_UNIT_LUX,     SP_UNIT_VOLT,    SP_UNIT_ML_SECOND};
    return d && d->slot < SP_SENSOR_MAX && d->kind < SP_SENSOR_KIND_COUNT && d->unit == units[d->kind] &&
           d->interval_ms >= 20 && d->interval_ms <= 3600000 && d->max_age_ms >= d->interval_ms &&
           d->max_age_ms <= 86400000;
}
bool sp_measurement_valid(const sp_sensor_descriptor_t *d, const sp_measurement_t *m) {
    if (!sp_sensor_descriptor_valid(d) || m->quality > SP_Q_STALE || (m->has_raw && m->raw > 4095))
        return false;
    if (m->quality != SP_Q_OK)
        return true;
    if (d->unit == SP_UNIT_PERCENT)
        return m->milli_value >= 0 && m->milli_value <= 100000;
    if (d->unit == SP_UNIT_BOOLEAN)
        return m->milli_value == 0 || m->milli_value == 1000;
    if (d->unit == SP_UNIT_CELSIUS)
        return m->milli_value >= -100000 && m->milli_value <= 200000;
    return m->milli_value >= 0;
}
bool sp_sensors_register(sp_sensor_registry_t *r, const sp_sensor_descriptor_t *d, sp_sensor_read_fn read,
                         void *ctx) {
    if (!r || !read || !sp_sensor_descriptor_valid(d) || r->count >= SP_SENSOR_MAX ||
        sp_sensors_find(r, d->slot))
        return false;
    r->drivers[r->count++] =
        (sp_sensor_driver_t){.descriptor = *d, .read = read, .ctx = ctx, .sample = {.quality = SP_Q_MISSING}};
    return true;
}
const sp_sensor_driver_t *sp_sensors_find(const sp_sensor_registry_t *r, unsigned slot) {
    for (unsigned i = 0; i < r->count; i++)
        if (r->drivers[i].descriptor.slot == slot)
            return &r->drivers[i];
    return NULL;
}
void sp_sensors_poll(sp_sensor_registry_t *r, uint64_t now) {
    for (unsigned i = 0; i < r->count; i++) {
        sp_sensor_driver_t *d = &r->drivers[i];
        if (now >= d->next_ms) {
            sp_measurement_t m = {.quality = SP_Q_MISSING};
            bool ok = d->read(d->ctx, now, &m);
            if (!ok || !sp_measurement_valid(&d->descriptor, &m) || m.sampled_ms > now)
                m = (sp_measurement_t){.quality = SP_Q_FAULT, .sampled_ms = now};
            d->sample = m;
            d->next_ms = now + d->descriptor.interval_ms;
        }
        if (d->sample.quality == SP_Q_OK && now - d->sample.sampled_ms > d->descriptor.max_age_ms)
            d->sample.quality = SP_Q_STALE;
    }
}
void sp_sensor_message(const sp_sensor_driver_t *d, uint64_t now, sp_message_t *m) {
    sp_message_init(m, SP_SENSOR);
    sp_message_set(m, 1, d->descriptor.slot);
    sp_message_set(m, 3, d->descriptor.kind);
    sp_message_set(m, 4, d->descriptor.unit);
    int64_t v = d->sample.milli_value;
    sp_message_set(m, 8, v >= 0 ? (uint64_t)v * 2 : (uint64_t)(-v) * 2 - 1); /* Zigzag int32 in CBOR uint. */
    sp_message_set(m, 10, d->sample.quality);
    sp_message_set(m, 14, now - d->sample.sampled_ms);
    sp_message_set(m, 15, d->descriptor.max_age_ms);
    sp_message_set(m, 16, d->descriptor.interval_ms);
    if (d->sample.has_raw)
        sp_message_set(m, 19, d->sample.raw);
}
bool sp_sensor_from_message(const sp_message_t *m, uint64_t now, sp_sensor_driver_t *d) {
    uint32_t mask = 0;
    const unsigned keys[] = {0, 1, 3, 4, 8, 10, 14, 15, 16};
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        mask |= UINT32_C(1) << keys[i];
    if (m->value[0] != SP_SENSOR || (m->present & ~(UINT32_C(1) << 19)) != mask ||
        m->value[1] >= SP_SENSOR_MAX || m->value[3] >= SP_SENSOR_KIND_COUNT || m->value[4] >= SP_UNIT_COUNT ||
        m->value[8] > UINT32_MAX || m->value[10] > SP_Q_STALE || m->value[15] > UINT32_MAX ||
        m->value[16] > UINT32_MAX || (sp_message_has(m, 19) && m->value[19] > 4095))
        return false;
    memset(d, 0, sizeof(*d));
    d->remote = true;
    d->received_ms = now;
    d->age_at_receive_ms = m->value[14];
    d->descriptor = (sp_sensor_descriptor_t){.slot = m->value[1],
                                             .kind = m->value[3],
                                             .unit = m->value[4],
                                             .max_age_ms = m->value[15],
                                             .interval_ms = m->value[16]};
    uint64_t v = m->value[8];
    d->sample = (sp_measurement_t){.quality = m->value[10],
                                   .milli_value = (int32_t)(v & 1 ? -(int64_t)(v / 2) - 1 : (int64_t)(v / 2)),
                                   .sampled_ms = now >= m->value[14] ? now - m->value[14] : 0,
                                   .has_raw = sp_message_has(m, 19),
                                   .raw = (uint16_t)m->value[19]};
    return sp_measurement_valid(&d->descriptor, &d->sample);
}
