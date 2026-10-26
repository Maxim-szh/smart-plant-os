#ifndef SP_WIRE_H
#define SP_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_WIRE_MAX 249
#define SP_WIRE_HEADER 16
#define SP_WIRE_TAG 16
#define SP_CBOR_MAX (SP_WIRE_MAX - SP_WIRE_HEADER - SP_WIRE_TAG)
#define SP_PEER_MAX 10
#define SP_OFFLINE_MS 15000u
#define SP_TELEMETRY_MS 3000u
#define SP_RPC_TIMEOUT_MS 1500u
#define SP_RPC_ATTEMPTS 3
/* SPP is deliberately unrelated to Bluetooth Serial Port Profile. */
enum {
    SP_HELLO = 1,
    SP_STATE = 2,
    SP_TIME = 3,
    SP_COMMAND = 4,
    SP_RESULT = 5,
    SP_QUERY = 6,
    SP_CONFIG = 7,
    SP_CONFIG_RESULT = 8,
    SP_INFO = 9,
    SP_EVENT_QUERY = 10,
    SP_EVENT = 11,
    SP_CONFIG_GET = 12,
    SP_SENSOR = 13
};
/* Keys 0..23 are unsigned integers. 24..27 are byte strings. */
enum {
    SP_K_KIND = 0,
    SP_K_RPC = 1,
    SP_K_TIME = 2,
    SP_K_ACTION = 3,
    SP_K_ML = 4,
    SP_K_DEADLINE = 5,
    SP_K_STATUS = 6,
    SP_K_REASON = 7,
    SP_K_RAW = 8,
    SP_K_INDEX = 9,
    SP_K_QUALITY = 10,
    SP_K_TANK = 11,
    SP_K_PUMP = 12,
    SP_K_REVISION = 13,
    SP_K_UPTIME = 14,
    SP_K_AGE = 15,
    SP_K_BUDGET = 16,
    SP_K_STARTED = 17,
    SP_K_FINISHED = 18,
    SP_K_VOLUME_UL = 19,
    SP_K_MODE = 20,
    SP_K_SEQUENCE = 21,
    SP_K_SPREAD = 22,
    SP_K_FLAGS = 23,
    SP_K_ID = 24,
    SP_K_TARGET = 25,
    SP_K_DEVICE = 26,
    SP_K_PLANT = 27
};
typedef struct {
    uint32_t present;
    uint64_t value[24];
    uint8_t bytes[4][16];
} sp_message_t;
typedef struct {
    uint32_t boot, sequence, target_boot;
    uint8_t role;
} sp_header_t;
void sp_message_init(sp_message_t *, unsigned kind);
void sp_message_set(sp_message_t *, unsigned key, uint64_t);
bool sp_message_has(const sp_message_t *, unsigned key);
bool sp_cbor_encode(const sp_message_t *, uint8_t *, size_t capacity, size_t *length);
bool sp_cbor_decode(sp_message_t *, const uint8_t *, size_t);
void sp_header_encode(const sp_header_t *, uint8_t out[SP_WIRE_HEADER]);
bool sp_header_decode(sp_header_t *, const uint8_t *, size_t);
void sp_nonce(const sp_header_t *, uint8_t out[12]);
bool sp_header_fresh(const sp_header_t *, uint8_t local_role, uint32_t local_boot, uint32_t peer_boot,
                     uint32_t peer_sequence, bool hello);
#endif
