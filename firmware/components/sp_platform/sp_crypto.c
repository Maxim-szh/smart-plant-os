#include "sp_crypto.h"
#include "mbedtls/gcm.h"
bool sp_frame_seal(const uint8_t key[16], const sp_header_t *h, const sp_message_t *m,
                   uint8_t out[SP_WIRE_MAX], size_t *len) {
    uint8_t plain[SP_CBOR_MAX], nonce[12];
    size_t size;
    if (h->role > 1 || !h->boot || !h->sequence || !sp_cbor_encode(m, plain, sizeof(plain), &size))
        return false;
    sp_header_encode(h, out);
    sp_nonce(h, nonce);
    mbedtls_gcm_context crypto;
    mbedtls_gcm_init(&crypto);
    int err = mbedtls_gcm_setkey(&crypto, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!err)
        err = mbedtls_gcm_crypt_and_tag(&crypto, MBEDTLS_GCM_ENCRYPT, size, nonce, sizeof(nonce), out,
                                        SP_WIRE_HEADER, plain, out + SP_WIRE_HEADER, SP_WIRE_TAG,
                                        out + SP_WIRE_HEADER + size);
    mbedtls_gcm_free(&crypto);
    *len = SP_WIRE_HEADER + size + SP_WIRE_TAG;
    return err == 0;
}
bool sp_frame_open(const uint8_t key[16], const uint8_t *frame, size_t len, sp_header_t *h, sp_message_t *m) {
    if (!sp_header_decode(h, frame, len))
        return false;
    uint8_t plain[SP_CBOR_MAX], nonce[12];
    size_t size = len - SP_WIRE_HEADER - SP_WIRE_TAG;
    sp_nonce(h, nonce);
    mbedtls_gcm_context crypto;
    mbedtls_gcm_init(&crypto);
    int err = mbedtls_gcm_setkey(&crypto, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!err)
        err = mbedtls_gcm_auth_decrypt(&crypto, size, nonce, sizeof(nonce), frame, SP_WIRE_HEADER,
                                       frame + SP_WIRE_HEADER + size, SP_WIRE_TAG, frame + SP_WIRE_HEADER,
                                       plain);
    mbedtls_gcm_free(&crypto);
    return !err && sp_cbor_decode(m, plain, size);
}
