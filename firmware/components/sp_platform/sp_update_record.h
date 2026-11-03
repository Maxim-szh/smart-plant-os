#ifndef SP_UPDATE_RECORD_H
#define SP_UPDATE_RECORD_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Legacy v1 is promoted only after an exact full-image SHA match; no NVS rewrite on read. */
typedef struct {
    uint32_t version, address;
    uint8_t session[16], hash[32], elf[32];
} sp_update_record_t;
enum {
    SP_UPDATE_EVIDENCE_NONE,
    SP_UPDATE_EVIDENCE_VERIFYING,
    SP_UPDATE_EVIDENCE_VALIDATED,
    SP_UPDATE_EVIDENCE_ROLLED_BACK
};
bool sp_update_record_promote_legacy(sp_update_record_t *, size_t, const uint8_t image_hash[32],
                                     const uint8_t image_elf[32]);
bool sp_update_record_valid(const sp_update_record_t *);
unsigned sp_update_record_evidence(const sp_update_record_t *, uint32_t running_address,
                                   const uint8_t *running_elf, bool running_pending, bool running_valid,
                                   const uint8_t *candidate_elf, bool candidate_failed);
#endif
