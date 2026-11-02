#ifndef SP_UPDATE_WIRE_H
#define SP_UPDATE_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SP_UPDATE_DATA 192u
#define SP_UPDATE_HEADER 40u
#define SP_UPDATE_MAX 248u
enum { SP_U_BEGIN = 1, SP_U_DATA = 2, SP_U_FINISH = 3, SP_U_ACK = 4, SP_U_ABORT = 5 };
typedef struct {
    uint8_t role, type, session[16];
    uint32_t boot, sequence, target_boot, offset;
} sp_update_header_t;
bool sp_update_seal(const uint8_t key[16], const sp_update_header_t *, const uint8_t *, size_t, uint8_t *,
                    size_t *);
bool sp_update_open(const uint8_t key[16], const uint8_t *, size_t, sp_update_header_t *, uint8_t *,
                    size_t *);
#endif
