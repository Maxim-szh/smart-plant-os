#ifndef SP_SENSORS_H
#define SP_SENSORS_H
#include "sp_core.h"
#include "sp_wire.h"

#define SP_SENSOR_MAX 8u
/* Stable protocol identifiers, independent of vendor, GPIO and transport. */
typedef enum {
    SP_SENSOR_SOIL,
    SP_SENSOR_TANK,
    SP_SENSOR_TEMPERATURE,
    SP_SENSOR_HUMIDITY,
    SP_SENSOR_LIGHT,
    SP_SENSOR_BATTERY,
    SP_SENSOR_FLOW,
    SP_SENSOR_KIND_COUNT
} sp_sensor_kind_t;
typedef enum {
    SP_UNIT_PERCENT,
    SP_UNIT_BOOLEAN,
    SP_UNIT_CELSIUS,
    SP_UNIT_LUX,
    SP_UNIT_VOLT,
    SP_UNIT_ML_SECOND,
    SP_UNIT_COUNT
} sp_sensor_unit_t;
typedef struct {
    uint8_t slot, kind, unit;
    uint32_t interval_ms, max_age_ms;
} sp_sensor_descriptor_t;
typedef struct {
    sp_quality_t quality;
    int32_t milli_value; /* Fixed-point canonical unit: -12345 = -12.345 °C. */
    uint64_t sampled_ms;
    bool has_raw;
    uint16_t raw;
} sp_measurement_t;
typedef bool (*sp_sensor_read_fn)(void *, uint64_t now, sp_measurement_t *);
typedef struct {
    sp_sensor_descriptor_t descriptor;
    sp_sensor_read_fn read;
    void *ctx;
    sp_measurement_t sample;
    uint64_t next_ms;
    uint64_t received_ms, age_at_receive_ms;
    bool remote;
} sp_sensor_driver_t;
typedef struct {
    sp_sensor_driver_t drivers[SP_SENSOR_MAX];
    unsigned count;
} sp_sensor_registry_t;
bool sp_sensors_register(sp_sensor_registry_t *, const sp_sensor_descriptor_t *, sp_sensor_read_fn,
                         void *ctx);
void sp_sensors_poll(sp_sensor_registry_t *, uint64_t now);
const sp_sensor_driver_t *sp_sensors_find(const sp_sensor_registry_t *, unsigned slot);
bool sp_sensor_descriptor_valid(const sp_sensor_descriptor_t *);
bool sp_measurement_valid(const sp_sensor_descriptor_t *, const sp_measurement_t *);
void sp_sensor_message(const sp_sensor_driver_t *, uint64_t now, sp_message_t *);
bool sp_sensor_from_message(const sp_message_t *, uint64_t now, sp_sensor_driver_t *);
const char *sp_sensor_kind_name(unsigned);
const char *sp_sensor_unit_name(unsigned);
uint64_t sp_sensor_age(const sp_sensor_driver_t *, uint64_t now);
#endif
