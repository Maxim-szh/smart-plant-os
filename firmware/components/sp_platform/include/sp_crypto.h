#ifndef SP_CRYPTO_H
#define SP_CRYPTO_H
#include "sp_wire.h"
/* Allocation-free frame protection; no ESP-IDF or radio API dependency. */
bool sp_frame_seal(const uint8_t key[16], const sp_header_t *, const sp_message_t *, uint8_t out[SP_WIRE_MAX],
                   size_t *len);
bool sp_frame_open(const uint8_t key[16], const uint8_t *, size_t len, sp_header_t *, sp_message_t *);
#endif
