#ifndef SP_SERVICE_H
#define SP_SERVICE_H
#include "sp_core.h"
#include "sp_wire.h"

/* Pure application boundary, shared by ESP-NOW and in-process fault simulations. */
bool sp_node_dispatch(sp_core_t *, const sp_message_t *, sp_message_t *response);
void sp_node_state(const sp_core_t *, sp_message_t *);
void sp_config_message(const sp_config_t *, sp_message_t *);
void sp_message_bytes(sp_message_t *, unsigned key, const uint8_t *);

typedef struct {
    bool have_state;
    uint64_t received_ms;
    sp_message_t state;
} sp_registry_entry_t;
bool sp_registry_accept(sp_registry_entry_t *, const sp_message_t *, uint64_t now);
bool sp_registry_online(const sp_registry_entry_t *, uint64_t now);

/* Hub history is replaceable; its failure cannot enter the Node safety path. */
typedef struct {
    uint8_t device[6];
    sp_event_t event;
} sp_history_record_t;
typedef struct {
    void *ctx;
    bool (*append)(void *, const sp_history_record_t *);
    bool (*read_after)(void *, const uint8_t device[6], uint64_t after, sp_history_record_t *, bool *gap);
    bool (*bounds)(void *, const uint8_t device[6], uint64_t *first, uint64_t *last);
} sp_history_store_t;
#define SP_HISTORY_SLOTS 128u
typedef struct {
    sp_history_record_t records[SP_HISTORY_SLOTS];
    size_t head, count;
} sp_ram_history_t;
sp_history_store_t sp_ram_history_store(sp_ram_history_t *);
#endif
