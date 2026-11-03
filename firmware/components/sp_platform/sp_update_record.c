#include "sp_update_record.h"
#include <string.h>
static bool nonzero(const uint8_t *data, unsigned size) {
    uint8_t sum = 0;
    while (size--)
        sum |= *data++;
    return sum != 0;
}
bool sp_update_record_valid(const sp_update_record_t *r) {
    return r && r->version == 2 &&
           (r->address == 0x30000 || r->address == 0x210000 || r->address == 0x230000) &&
           (r->session[6] & 0xf0) == 0x40 && (r->session[8] & 0xc0) == 0x80 && nonzero(r->hash, 32) &&
           nonzero(r->elf, 32);
}
unsigned sp_update_record_evidence(const sp_update_record_t *r, uint32_t running, const uint8_t *elf,
                                   bool pending, bool valid, const uint8_t *candidate, bool failed) {
    if (!sp_update_record_valid(r))
        return SP_UPDATE_EVIDENCE_NONE;
    if (running == r->address) {
        if (!elf || memcmp(elf, r->elf, 32))
            return SP_UPDATE_EVIDENCE_NONE;
        if (pending)
            return SP_UPDATE_EVIDENCE_VERIFYING;
        return valid ? SP_UPDATE_EVIDENCE_VALIDATED : SP_UPDATE_EVIDENCE_NONE;
    }
    if (failed && candidate && !memcmp(candidate, r->elf, 32))
        return SP_UPDATE_EVIDENCE_ROLLED_BACK;
    return SP_UPDATE_EVIDENCE_NONE;
}

bool sp_update_record_promote_legacy(sp_update_record_t *record, size_t size, const uint8_t hash[32],
                                     const uint8_t elf[32]) {
    if (!record || size != 56 || record->version != 1 || !hash || !elf || memcmp(record->hash, hash, 32))
        return false;
    sp_update_record_t next = *record;
    next.version = 2;
    memcpy(next.elf, elf, 32);
    if (!sp_update_record_valid(&next))
        return false;
    *record = next;
    return true;
}
