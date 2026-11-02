#ifndef SP_MAINTENANCE_H
#define SP_MAINTENANCE_H
#include "sp_internal.h"
#include "sp_api.h"
typedef struct {
    void *ctx;
    bool (*idle)(void *);
    bool (*remove)(void *, unsigned slot);
} sp_maintenance_io_t;
bool sp_maintenance_init(sp_identity_t *, sp_api_t *, sp_core_t *, sp_maintenance_io_t);
bool sp_maintenance_handle(const sp_api_request_t *);
bool sp_maintenance_receive(unsigned peer, const sp_message_t *);
bool sp_maintenance_node(const sp_message_t *, sp_message_t *);
void sp_maintenance_tick(void);
bool sp_maintenance_busy(void);
#endif
