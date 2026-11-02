#ifndef SP_EPOCHS_H
#define SP_EPOCHS_H
#include "sp_internal.h"
bool sp_peer_epochs_load(sp_identity_t *);
bool sp_peer_epoch_store(nvs_handle_t, const uint8_t device[6], uint32_t boot);
#endif
