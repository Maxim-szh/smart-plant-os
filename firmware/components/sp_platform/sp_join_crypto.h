#ifndef SP_JOIN_CRYPTO_H
#define SP_JOIN_CRYPTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SP_JOIN_BEACON_SIZE 98u
#define SP_JOIN_OFFER_SIZE 152u
#define SP_JOIN_OFFER_PREFIX 120u
#define SP_JOIN_ACK_SIZE 81u
/* SPJ1/type/node-id/node-MAC/node-nonce/node-public-key */
#define SP_JOIN_NODE_ID 5u
#define SP_JOIN_NODE_MAC 11u
#define SP_JOIN_NODE_NONCE 17u
#define SP_JOIN_NODE_PUBLIC 33u
/* SPJ1/type/node-id/hub-id/hub-MAC/node-nonce/hub-nonce/hub-public-key/MAC */
#define SP_JOIN_HUB_ID 11u
#define SP_JOIN_HUB_MAC 17u
#define SP_JOIN_OFFER_NONCE 23u
#define SP_JOIN_HUB_NONCE 39u
#define SP_JOIN_HUB_PUBLIC 55u
typedef int (*sp_join_rng_t)(void *, unsigned char *, size_t);
bool sp_join_generate(uint8_t secret[32], uint8_t public_key[65], sp_join_rng_t, void *);
bool sp_join_public(const uint8_t secret[32], uint8_t public_key[65], sp_join_rng_t, void *);
bool sp_join_derive(const uint8_t secret[32], const uint8_t remote[65],
                    const uint8_t beacon[SP_JOIN_BEACON_SIZE], const uint8_t offer[SP_JOIN_OFFER_PREFIX],
                    uint8_t keys[32], uint8_t transcript[32], sp_join_rng_t, void *);
bool sp_join_proof(const uint8_t key[16], const uint8_t transcript[32], bool ack, uint8_t proof[32]);
bool sp_join_equal(const uint8_t *, const uint8_t *, size_t);
#define SP_JOIN_PMK_SIZE 61u
bool sp_join_pmk_seal(const uint8_t app_key[16], const uint8_t node[6], const uint8_t hub[6],
                      const uint8_t pmk[16], uint8_t out[SP_JOIN_PMK_SIZE], sp_join_rng_t, void *);
bool sp_join_pmk_open(const uint8_t app_key[16], const uint8_t node[6], const uint8_t hub[6], const uint8_t *,
                      size_t, uint8_t pmk[16]);
#endif
