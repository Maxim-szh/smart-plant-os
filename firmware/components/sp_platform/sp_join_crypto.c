#include "sp_join_crypto.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include <string.h>
bool sp_join_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
bool sp_join_public(const uint8_t secret[32], uint8_t public_key[65], sp_join_rng_t rng, void *ctx) {
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    int e = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    size_t n = 0;
    if (!e)
        e = mbedtls_mpi_read_binary(&d, secret, 32);
    if (!e)
        e = mbedtls_ecp_check_privkey(&group, &d);
    if (!e)
        e = mbedtls_ecp_mul(&group, &q, &d, &group.G, rng, ctx);
    if (!e)
        e = mbedtls_ecp_point_write_binary(&group, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, public_key, 65);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
    return !e && n == 65;
}
bool sp_join_generate(uint8_t secret[32], uint8_t public_key[65], sp_join_rng_t rng, void *ctx) {
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    int e = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    size_t n = 0;
    if (!e)
        e = mbedtls_ecp_gen_keypair(&group, &d, &q, rng, ctx);
    if (!e)
        e = mbedtls_mpi_write_binary(&d, secret, 32);
    if (!e)
        e = mbedtls_ecp_point_write_binary(&group, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, public_key, 65);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
    if (e)
        mbedtls_platform_zeroize(secret, 32);
    return !e && n == 65;
}
bool sp_join_derive(const uint8_t secret[32], const uint8_t remote[65],
                    const uint8_t beacon[SP_JOIN_BEACON_SIZE], const uint8_t offer[SP_JOIN_OFFER_PREFIX],
                    uint8_t keys[32], uint8_t transcript[32], sp_join_rng_t rng, void *ctx) {
    uint8_t shared[32] = {0}, input[SP_JOIN_BEACON_SIZE + SP_JOIN_OFFER_PREFIX];
    memcpy(input, beacon, SP_JOIN_BEACON_SIZE);
    memcpy(input + SP_JOIN_BEACON_SIZE, offer, SP_JOIN_OFFER_PREFIX);
    int e = mbedtls_sha256(input, sizeof(input), transcript, 0);
    mbedtls_ecp_group group;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    if (!e)
        e = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (!e)
        e = mbedtls_mpi_read_binary(&d, secret, 32);
    if (!e)
        e = mbedtls_ecp_check_privkey(&group, &d);
    if (!e)
        e = mbedtls_ecp_point_read_binary(&group, &q, remote, 65);
    if (!e)
        e = mbedtls_ecp_check_pubkey(&group, &q);
    if (!e)
        e = mbedtls_ecdh_compute_shared(&group, &z, &q, &d, rng, ctx);
    if (!e)
        e = mbedtls_mpi_write_binary(&z, shared, 32);
    static const uint8_t info[] = "SmartPlantOS/SPJ1/LMK+APP";
    if (!e)
        e = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), transcript, 32, shared, 32, info,
                         sizeof(info) - 1, keys, 32);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&group);
    if (e)
        mbedtls_platform_zeroize(keys, 32);
    return !e;
}
bool sp_join_proof(const uint8_t key[16], const uint8_t transcript[32], bool ack, uint8_t proof[32]) {
    uint8_t input[33];
    memcpy(input, transcript, 32);
    input[32] = ack ? 2 : 1;
    return !mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 16, input, sizeof(input),
                            proof);
}

static int pmk_wrap_key(const uint8_t app[16], const uint8_t header[17], uint8_t key[16]) {
    static const uint8_t info[] = "SmartPlantOS/SPJ1/PMK";
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), header, 17, app, 16, info,
                        sizeof(info) - 1, key, 16);
}
bool sp_join_pmk_seal(const uint8_t app_key[16], const uint8_t node[6], const uint8_t hub[6],
                      const uint8_t pmk[16], uint8_t out[SP_JOIN_PMK_SIZE], sp_join_rng_t rng, void *ctx) {
    memcpy(out, "SPJ1", 4);
    out[4] = 4;
    memcpy(out + 5, node, 6);
    memcpy(out + 11, hub, 6);
    uint8_t key[16];
    int e = rng(ctx, out + 17, 12);
    if (!e)
        e = pmk_wrap_key(app_key, out, key);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    if (!e)
        e = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!e)
        e = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 16, out + 17, 12, out, 17, pmk, out + 29, 16,
                                      out + 45);
    mbedtls_gcm_free(&g);
    mbedtls_platform_zeroize(key, sizeof(key));
    return !e;
}
bool sp_join_pmk_open(const uint8_t app_key[16], const uint8_t node[6], const uint8_t hub[6],
                      const uint8_t *in, size_t size, uint8_t pmk[16]) {
    if (size != SP_JOIN_PMK_SIZE || memcmp(in, "SPJ1", 4) || in[4] != 4 || memcmp(in + 5, node, 6) ||
        memcmp(in + 11, hub, 6))
        return false;
    uint8_t key[16];
    int e = pmk_wrap_key(app_key, in, key);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    if (!e)
        e = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!e)
        e = mbedtls_gcm_auth_decrypt(&g, 16, in + 17, 12, in, 17, in + 45, 16, in + 29, pmk);
    mbedtls_gcm_free(&g);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (e)
        mbedtls_platform_zeroize(pmk, 16);
    return !e;
}
