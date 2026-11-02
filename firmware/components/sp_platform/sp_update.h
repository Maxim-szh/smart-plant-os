#ifndef SP_UPDATE_H
#define SP_UPDATE_H
#include "sp_internal.h"
#include "sp_api.h"
void sp_update_boot_guard(void);
void sp_update_init(sp_identity_t *, sp_api_t *, sp_core_t *, bool (*idle)(void *), bool startup_ready);
void sp_update_link_seen(unsigned peer);
void sp_update_tick(void);
bool sp_update_handle(const sp_api_request_t *);
bool sp_update_receive(unsigned peer, const uint8_t *, size_t);
bool sp_update_busy(void);
bool sp_update_verifying(void);
bool sp_radio_update_send(unsigned, const uint8_t *, size_t);
#endif
