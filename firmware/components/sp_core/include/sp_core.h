#ifndef SP_CORE_H
#define SP_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hardware-free domain model. All durations use monotonic milliseconds. */
#define SP_SAMPLE_COUNT 9
#define SP_COMMAND_SLOTS 128
#define SP_DAY_MS UINT64_C(86400000)
#define SP_MIN_EPOCH_MS UINT64_C(1704067200000)
#define SP_MAX_EPOCH_MS UINT64_C(4102444800000)
#define SP_HARD_RUN_MS 30000u
#define SP_CONFIRM_SAMPLES 6u
#define SP_SOAK_MS 120000u
#define SP_CORE_VERSION 1u
#define SP_EVENT_SLOTS 32u

typedef enum {
    SP_OK,
    SP_SOIL_NOT_DRY,
    SP_SENSOR_STALE,
    SP_SENSOR_FAULT,
    SP_SENSOR_UNCALIBRATED,
    SP_TANK_EMPTY,
    SP_TANK_UNKNOWN,
    SP_PUMP_UNCALIBRATED,
    SP_COOLDOWN,
    SP_DAILY_LIMIT,
    SP_SINGLE_LIMIT,
    SP_BUSY,
    SP_CLOCK_UNSET,
    SP_HARDWARE_FAULT,
    SP_CONFIG_INVALID,
    SP_EXPIRED,
    SP_ID_CONFLICT,
    SP_STORAGE_FAULT,
    SP_NO_RESPONSE,
    SP_REASON_COUNT
} sp_reason_t;
typedef enum { SP_Q_MISSING, SP_Q_UNCALIBRATED, SP_Q_OK, SP_Q_FAULT, SP_Q_STALE } sp_quality_t;
typedef enum { SP_ACCEPTED, SP_RUNNING, SP_COMPLETED, SP_STOPPED, SP_FAILED, SP_REJECTED } sp_status_t;
typedef enum { SP_WATER = 1, SP_STOP = 2 } sp_action_t;

typedef struct {
    uint64_t seq, utc_ms, uptime_ms;
    uint8_t command_id[16];
    uint8_t kind;   /* 1 boot, 2 config, 3 command, 4 latched fault, 5 physical acknowledgement */
    uint8_t source; /* 0 device, 1 user, 2 automatic */
    sp_status_t status;
    sp_reason_t reason;
} sp_event_t;
typedef struct {
    uint32_t revision;
    bool automatic;
    uint16_t dry_raw, wet_raw; /* Both 0 => not calibrated; 12-bit ADC. */
    uint16_t threshold_pct, portion_ml, daily_ml, max_run_s, interval_min, max_age_s;
    uint32_t flow_ul_s; /* Integer microlitres/second; 0 => not calibrated. */
} sp_config_t;
typedef struct {
    sp_quality_t quality;
    uint16_t raw_adc, spread_adc;
    uint8_t index_pct, stable_samples, dry_samples;
    int32_t filtered_q8;
    uint64_t sampled_ms;
} sp_soil_t;
typedef struct {
    uint8_t id[16], target[16];
    sp_action_t action;
    uint16_t requested_ml;
    uint64_t expires_ms; /* UTC; transport deadline, excluded from identity. */
} sp_request_t;
typedef struct {
    bool used;
    sp_request_t request;
    sp_status_t status;
    sp_reason_t reason;
    uint32_t estimated_ul;
    bool automatic;
    uint64_t created_ms, started_ms, finished_ms;
    uint64_t generation, created_monotonic_ms;
} sp_result_t;
typedef struct {
    uint32_t version;
    sp_config_t config;
    sp_result_t commands[SP_COMMAND_SLOTS];
    uint64_t cooldown_until_utc, event_seq;
    uint64_t generation;
    bool response_fault;
    sp_event_t events[SP_EVENT_SLOTS];
} sp_persist_t;
typedef bool (*sp_save_fn)(void *, const sp_persist_t *);
typedef void (*sp_pump_fn)(void *, bool, uint32_t); /* true => arm independent cutoff */
typedef struct {
    sp_persist_t saved;
    sp_soil_t soil;
    sp_save_fn save;
    sp_pump_fn pump;
    void *io;
    bool hardware_ready, storage_ok, clock_set, dry_armed, soak_pending;
    int8_t tank; /* -1 unknown, 0 empty, 1 present */
    uint8_t tank_good_samples, pre_water_index;
    int active_slot;
    uint64_t boot_ms, now_ms, utc_anchor, mono_anchor, tank_sampled_ms;
    uint64_t run_started_ms, run_deadline_ms, cooldown_until_ms, soak_at_ms;
    sp_reason_t last_reason;
} sp_core_t;

sp_config_t sp_config_default(void);
bool sp_config_valid(const sp_config_t *);
bool sp_core_init(sp_core_t *, const sp_persist_t *, sp_save_fn, sp_pump_fn, void *, uint64_t);
bool sp_core_configure(sp_core_t *, const sp_config_t *, uint32_t expected_revision);
bool sp_core_set_time(sp_core_t *, uint64_t utc_ms, uint64_t mono_ms);
uint64_t sp_core_utc(const sp_core_t *, uint64_t mono_ms);
void sp_core_sample(sp_core_t *, const uint16_t samples[SP_SAMPLE_COUNT], bool valid, uint64_t mono_ms);
/* Normalized primary soil binding. Drivers cannot bypass freshness, confirmation or hysteresis. */
void sp_core_observe_soil(sp_core_t *,sp_quality_t,int index_pct,uint16_t raw,uint16_t spread,uint64_t mono_ms);
void sp_core_tank(sp_core_t *, int value, uint64_t mono_ms);
void sp_core_tick(sp_core_t *, uint64_t mono_ms);
sp_reason_t sp_core_can_water(const sp_core_t *, uint16_t ml, bool automatic);
bool sp_core_auto_due(const sp_core_t *);
sp_reason_t sp_core_command(sp_core_t *, const sp_request_t *, bool automatic, sp_result_t *);
const sp_result_t *sp_core_result(const sp_core_t *, const uint8_t id[16]);
uint32_t sp_core_reserved_ml(const sp_core_t *);
/* Physical inspection only; never resets command IDs, water budget or cooldown. */
bool sp_core_acknowledge_fault(sp_core_t *);
const sp_event_t *sp_core_event_after(const sp_core_t *, uint64_t after, bool *cursor_expired);
const char *sp_reason_name(sp_reason_t);
const char *sp_quality_name(sp_quality_t);
const char *sp_status_name(sp_status_t);
#endif
