#include "sp_update_record.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    sp_update_record_t record = {.version = 2,
                                 .address = 0x230000,
                                 .session = {1, 2, 3, 4, 5, 6, 0x40, 1, 0x80, 1, 2, 3, 4, 5, 6, 7},
                                 .hash = {1},
                                 .elf = {2}};
    uint8_t same[32] = {2}, other[32] = {3};
    assert(sp_update_record_valid(&record));
    assert(sp_update_record_evidence(&record, 0x230000, same, true, false, same, false) ==
           SP_UPDATE_EVIDENCE_VERIFYING);
    assert(sp_update_record_evidence(&record, 0x230000, same, false, true, same, false) ==
           SP_UPDATE_EVIDENCE_VALIDATED);
    assert(sp_update_record_evidence(&record, 0x30000, other, false, true, same, true) ==
           SP_UPDATE_EVIDENCE_ROLLED_BACK);
    /* USB switches to another VALID slot: old web operation is not a rollback. */
    assert(sp_update_record_evidence(&record, 0x30000, other, false, true, same, false) ==
           SP_UPDATE_EVIDENCE_NONE);
    /* USB overwrites the old address with another ELF: address equality proves nothing. */
    assert(sp_update_record_evidence(&record, 0x230000, other, false, true, other, false) ==
           SP_UPDATE_EVIDENCE_NONE);
    assert(sp_update_record_evidence(&record, 0x30000, other, false, true, other, true) ==
           SP_UPDATE_EVIDENCE_NONE);
    assert(sp_update_record_evidence(&record, 0x230000, NULL, false, true, same, false) ==
           SP_UPDATE_EVIDENCE_NONE);
    record.version = 1;
    assert(!sp_update_record_valid(&record));
    assert(sp_update_record_evidence(&record, 0x230000, same, false, true, same, false) ==
           SP_UPDATE_EVIDENCE_NONE);
    uint8_t expected_hash[32] = {1}, wrong_hash[32] = {4};
    memset(record.elf, 0, 32);
    assert(!sp_update_record_promote_legacy(&record, 56, wrong_hash, same));
    assert(record.version == 1);
    assert(sp_update_record_promote_legacy(&record, 56, expected_hash, same));
    assert(sp_update_record_evidence(&record, 0x230000, same, false, true, same, false) ==
           SP_UPDATE_EVIDENCE_VALIDATED);
    record.version = 2;
    record.session[6] = 0;
    assert(!sp_update_record_valid(&record));
    puts("PASS: exact ELF/slot proof, pending validation, actual rollback, unrelated USB update and legacy "
         "history");
}
