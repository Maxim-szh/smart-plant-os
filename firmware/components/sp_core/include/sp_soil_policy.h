#ifndef SP_SOIL_POLICY_H
#define SP_SOIL_POLICY_H
#include "sp_core.h"
/* Provisional engineering profile, not agronomic or sensor accuracy claims. */
#define SP_SOIL_IDLE_MS 5000u
#define SP_SOIL_WATCH_MS 1000u
#define SP_SOIL_FAST_MS 500u
#define SP_SOIL_DRY_CONFIRM_MS 30000u
#define SP_SOIL_ANOMALY_HOLD_MS 30000u
#define SP_SOIL_RESPONSE_MS 600000u
#define SP_SOIL_RESPONSE_HOLD_MS 10000u
#define SP_SOIL_RESPONSE_RISE 3u
uint32_t sp_soil_poll_interval(const sp_core_t *, uint64_t now);
#endif
