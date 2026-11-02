#include "sp_update_wire.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include <string.h>
static void put(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (8 * i));
}
static uint32_t get(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool key_for(const uint8_t in[16], uint8_t out[16]) {
    static const uint8_t domain[] = "SmartPlantOS/SPU1/AES128";
    return !mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), NULL, 0, in, 16, domain,
                         sizeof(domain) - 1, out, 16);
}
static void nonce_for(const sp_update_header_t *h, uint8_t nonce[12]) {
    put(nonce, h->boot);
    put(nonce + 4, h->sequence);
    put(nonce + 8, h->role);
}
bool sp_update_seal(const uint8_t key[16], const sp_update_header_t *h, const uint8_t *data, size_t len,
                    uint8_t *out, size_t *size) {
    if (len > SP_UPDATE_DATA || h->role > 1 || h->type < 1 || h->type > 5 || !h->boot || !h->sequence ||
        !h->target_boot)
        return false;
    memcpy(out, "SPU\1", 4);
    out[4] = h->role;
    out[5] = h->type;
    out[6] = out[7] = 0;
    put(out + 8, h->boot);
    put(out + 12, h->sequence);
    put(out + 16, h->target_boot);
    memcpy(out + 20, h->session, 16);
    put(out + 36, h->offset);
    uint8_t derived[16], nonce[12];
    if (!key_for(key, derived))
        return false;
    nonce_for(h, nonce);
    mbedtls_gcm_context c;
    mbedtls_gcm_init(&c);
    int e = mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, derived, 128);
    if (!e)
        e = mbedtls_gcm_crypt_and_tag(&c, MBEDTLS_GCM_ENCRYPT, len, nonce, 12, out, 40, data, out + 40, 16,
                                      out + 40 + len);
    mbedtls_gcm_free(&c);
    memset(derived, 0, 16);
    *size = 56 + len;
    return !e;
}
bool sp_update_open(const uint8_t key[16], const uint8_t *in, size_t size, sp_update_header_t *h,
                    uint8_t *out, size_t *len) {
    if (size < 56 || size > SP_UPDATE_MAX || memcmp(in, "SPU\1", 4) || in[4] > 1 || in[5] < 1 || in[5] > 5 ||
        in[6] || in[7])
        return false;
    *h = (sp_update_header_t){.role = in[4],
                              .type = in[5],
                              .boot = get(in + 8),
                              .sequence = get(in + 12),
                              .target_boot = get(in + 16),
                              .offset = get(in + 36)};
    memcpy(h->session, in + 20, 16);
    if (!h->boot || !h->sequence || !h->target_boot)
        return false;
    uint8_t derived[16], nonce[12];
    if (!key_for(key, derived))
        return false;
    nonce_for(h, nonce);
    *len = size - 56;
    mbedtls_gcm_context c;
    mbedtls_gcm_init(&c);
    int e = mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, derived, 128);
    if (!e)
        e = mbedtls_gcm_auth_decrypt(&c, *len, nonce, 12, in, 40, in + 40 + *len, 16, in + 40, out);
    mbedtls_gcm_free(&c);
    memset(derived, 0, 16);
    return !e;
}
