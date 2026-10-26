#include "sp_crypto.h"
#include "sp_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t key[16] = {0}, frame[SP_WIRE_MAX], wrong[16] = {1};
    size_t len;
    sp_header_t h = {.role = 1, .boot = 2, .sequence = 3, .target_boot = 4}, back;
    sp_message_t m, decoded;
    sp_message_init(&m, SP_TIME);
    sp_message_set(&m, SP_K_TIME, SP_MIN_EPOCH_MS);
    assert(sp_frame_seal(key, &h, &m, frame, &len));
    /* Independently computed with Python cryptography AESGCM; all-zero key is test-only. */
    const uint8_t golden[] = {0x53, 0x50, 0x01, 0x01, 0,    0,    0,    2,    0,    0,    0,    3,
                              0,    0,    0,    4,    0x5a, 0x68, 0x44, 0x59, 0x58, 0xfa, 0xb1, 0x9f,
                              0x9d, 0x20, 0x37, 0x20, 0x21, 0x75, 0x25, 0x07, 0x16, 0x09, 0x93, 0x64,
                              0xca, 0xcc, 0xa0, 0xb4, 0xe0, 0xf9, 0xce, 0x41, 0x4e};
    assert(len == sizeof(golden) && !memcmp(frame, golden, len));
    assert(sp_frame_open(key, frame, len, &back, &decoded));
    assert(decoded.value[2] == SP_MIN_EPOCH_MS);
    assert(!sp_frame_open(wrong, frame, len, &back, &decoded));
    for (size_t i = 0; i < len; i++) {
        frame[i] ^= 1;
        assert(!sp_frame_open(key, frame, len, &back, &decoded));
        frame[i] ^= 1;
    }
    assert(sp_header_fresh(&h, 0, 4, 2, 2, false));
    assert(!sp_header_fresh(&h, 0, 4, 2, 3, false)); /* duplicate */
    assert(!sp_header_fresh(&h, 0, 5, 2, 0, false)); /* receiver reboot */
    assert(!sp_header_fresh(&h, 0, 4, 3, 0, false)); /* older sender epoch */
    assert(!sp_header_fresh(&h, 1, 4, 2, 0, false)); /* reflected direction */
    assert(!sp_header_fresh(&h, 0, 4, 2, 0, true));  /* HELLO cannot carry target */
    h.target_boot = 0;
    assert(sp_header_fresh(&h, 0, 4, 2, 0, true));
    h.sequence = 0;
    assert(!sp_frame_seal(key, &h, &m, frame, &len));
    puts("PASS: AES-GCM roundtrip, every-byte tampering, wrong key, replay epochs and direction");
    return 0;
}
