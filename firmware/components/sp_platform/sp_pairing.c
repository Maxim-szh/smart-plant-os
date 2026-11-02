#include "sp_pairing.h"
#include "sp_join_crypto.h"
#include "sp_setup.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/platform_util.h"
#include <stdio.h>
#include <string.h>
#define SP_PAIR_WINDOW_MS 120000u
#define SP_PAIR_CANDIDATES 10u
static const uint8_t magic[4] = {'S', 'P', 'J', '1'};
static sp_identity_t *owner;
static uint8_t self_mac[6], beacon[SP_JOIN_BEACON_SIZE], ack_frame[SP_JOIN_ACK_SIZE];
static bool ready, have_ack;
static bool proven[SP_PEER_MAX], wrapped[SP_PEER_MAX];
static uint8_t pmk_frames[SP_PEER_MAX][SP_JOIN_PMK_SIZE];
static unsigned pmk_slot;
static uint64_t next_pmk;
static uint64_t window_until, next_send, next_crypto, ack_until;
static const char *phase = "idle";
static char selected[17];
typedef struct {
    uint32_t version;
    uint8_t secret[32], nonce[16];
} node_seed_t;
static node_seed_t seed;
typedef struct {
    bool used;
    uint8_t mac[6], beacon[SP_JOIN_BEACON_SIZE];
    uint64_t seen;
} candidate_t;
static candidate_t candidates[SP_PAIR_CANDIDATES];
typedef struct {
    uint32_t version;
    uint8_t owner[6];
    sp_peer_t peer;
    uint8_t offer[SP_JOIN_OFFER_SIZE], transcript[32];
} pending_t;
static pending_t pending;
static bool have_pending;
static int random_bytes(void *ctx, unsigned char *out, size_t n) {
    (void)ctx;
    esp_fill_random(out, n);
    return 0;
}
static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000;
}
static esp_err_t store(const char *key, const void *data, size_t n) {
    nvs_handle_t h;
    esp_err_t e = nvs_open("pairing", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, key, data, n);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}
static esp_err_t load(const char *key, void *data, size_t n) {
    nvs_handle_t h;
    esp_err_t e = nvs_open("pairing", NVS_READONLY, &h);
    if (e != ESP_OK)
        return e;
    size_t size = n;
    e = nvs_get_blob(h, key, data, &size);
    nvs_close(h);
    return e == ESP_OK && size != n ? ESP_ERR_INVALID_SIZE : e;
}
static void forget(const char *key) {
    nvs_handle_t h;
    if (nvs_open("pairing", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, key);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}
static int peer_slot(const uint8_t device[6]) {
    for (unsigned i = 0; i < owner->count; i++)
        if (!memcmp(owner->peers[i].device, device, 6))
            return (int)i;
    return -1;
}
static bool pending_link(void) {
    int slot = peer_slot(pending.peer.device);
    if (slot >= 0) {
        sp_peer_t *p = &owner->peers[slot];
        return !memcmp(p->mac, pending.peer.mac, 6) && sp_join_equal(p->lmk, pending.peer.lmk, 16) &&
               sp_join_equal(p->app_key, pending.peer.app_key, 16);
    }
    return sp_radio_add_peer(&pending.peer) == ESP_OK;
}
bool sp_pairing_start(sp_identity_t *id) {
    ready = false;
    have_pending = false;
    have_ack = false;
    window_until = next_send = next_crypto = ack_until = 0;
    phase = "idle";
    selected[0] = 0;
    memset(&seed, 0, sizeof(seed));
    memset(&pending, 0, sizeof(pending));
    memset(candidates, 0, sizeof(candidates));
    memset(beacon, 0, sizeof(beacon));
    memset(proven, 0, sizeof(proven));
    memset(wrapped, 0, sizeof(wrapped));
    pmk_slot = 0;
    next_pmk = 0;
    owner = id;
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    if (id->hub) {
        esp_err_t e = load("hub_v1", &pending, sizeof(pending));
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
            return false;
        if (e == ESP_OK) {
            if (pending.version != 1 || memcmp(pending.owner, id->self, 6) ||
                memcmp(pending.offer, magic, 4) || pending.offer[4] != 2)
                return false;
            have_pending = true;
            sp_api_device_id(pending.peer.device, selected);
            if (!pending_link()) {
                phase = "storage_error";
                ready = true;
                return true;
            }
            phase = "connecting";
            window_until = now_ms() + SP_PAIR_WINDOW_MS;
            sp_api_device_id(pending.peer.device, selected);
        }
    } else if (!id->count) {
        esp_err_t e = load("node_v1", &seed, sizeof(seed));
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            seed.version = 1;
            esp_fill_random(seed.nonce, 16);
            if (!sp_join_generate(seed.secret, beacon + SP_JOIN_NODE_PUBLIC, random_bytes, NULL) ||
                store("node_v1", &seed, sizeof(seed)) != ESP_OK)
                return false;
        } else if (e != ESP_OK || seed.version != 1 ||
                   !sp_join_public(seed.secret, beacon + SP_JOIN_NODE_PUBLIC, random_bytes, NULL))
            return false;
        memcpy(beacon, magic, 4);
        beacon[4] = 1;
        memcpy(beacon + SP_JOIN_NODE_ID, id->self, 6);
        memcpy(beacon + SP_JOIN_NODE_MAC, self_mac, 6);
        memcpy(beacon + SP_JOIN_NODE_NONCE, seed.nonce, 16);
        window_until = now_ms() + SP_PAIR_WINDOW_MS;
        phase = "discoverable";
    }
    ready = true;
    return true;
}
esp_err_t sp_pairing_open(void) {
    if (!ready || !owner->hub || have_pending)
        return ESP_ERR_INVALID_STATE;
    memset(candidates, 0, sizeof(candidates));
    window_until = now_ms() + SP_PAIR_WINDOW_MS;
    phase = "searching";
    selected[0] = 0;
    return ESP_OK;
}
esp_err_t sp_pairing_begin(const uint8_t device[6]) {
    uint64_t now = now_ms();
    if (!ready || !owner->hub || have_pending || now >= window_until || owner->count >= SP_PEER_MAX)
        return ESP_ERR_INVALID_STATE;
    candidate_t *c = NULL;
    for (unsigned i = 0; i < SP_PAIR_CANDIDATES; i++)
        if (candidates[i].used && !memcmp(candidates[i].beacon + SP_JOIN_NODE_ID, device, 6) &&
            now - candidates[i].seen < 12000)
            c = &candidates[i];
    if (!c || peer_slot(device) >= 0)
        return ESP_ERR_NOT_FOUND;
    memset(&pending, 0, sizeof(pending));
    pending.version = 1;
    memcpy(pending.owner, owner->self, 6);
    memcpy(pending.peer.device, device, 6);
    memcpy(pending.peer.mac, c->mac, 6);
    uint8_t *o = pending.offer;
    memcpy(o, magic, 4);
    o[4] = 2;
    memcpy(o + SP_JOIN_NODE_ID, device, 6);
    memcpy(o + SP_JOIN_HUB_ID, owner->self, 6);
    memcpy(o + SP_JOIN_HUB_MAC, self_mac, 6);
    memcpy(o + SP_JOIN_OFFER_NONCE, c->beacon + SP_JOIN_NODE_NONCE, 16);
    esp_fill_random(o + SP_JOIN_HUB_NONCE, 16);
    uint8_t secret[32], keys[32];
    bool ok = sp_join_generate(secret, o + SP_JOIN_HUB_PUBLIC, random_bytes, NULL) &&
              sp_join_derive(secret, c->beacon + SP_JOIN_NODE_PUBLIC, c->beacon, o, keys, pending.transcript,
                             random_bytes, NULL);
    mbedtls_platform_zeroize(secret, sizeof(secret));
    if (ok) {
        memcpy(pending.peer.lmk, keys, 16);
        memcpy(pending.peer.app_key, keys + 16, 16);
        ok = sp_join_proof(keys + 16, pending.transcript, false, o + SP_JOIN_OFFER_PREFIX);
    }
    mbedtls_platform_zeroize(keys, sizeof(keys));
    if (!ok)
        return ESP_FAIL;
    if (store("hub_v1", &pending, sizeof(pending)) != ESP_OK)
        return ESP_FAIL;
    have_pending = true;
    sp_api_device_id(device, selected);
    if (!pending_link()) {
        phase = "storage_error";
        return ESP_FAIL;
    }
    phase = "connecting";
    window_until = now_ms() + SP_PAIR_WINDOW_MS;
    next_send = 0;
    return ESP_OK;
}
esp_err_t sp_pairing_retry(void) {
    if (!ready || !owner->hub || !have_pending)
        return ESP_ERR_INVALID_STATE;
    if (!pending_link()) {
        phase = "storage_error";
        return ESP_FAIL;
    }
    phase = "connecting";
    window_until = now_ms() + SP_PAIR_WINDOW_MS;
    next_send = 0;
    return ESP_OK;
}
void sp_pairing_tick(void) {
    if (!ready)
        return;
    uint64_t now = now_ms();
    if (now >= window_until && (!strcmp(phase, "searching") || !strcmp(phase, "discoverable")))
        phase = "idle";
    if (have_pending && now >= window_until && !strcmp(phase, "connecting"))
        phase = "waiting_device";
    if (owner->hub && owner->count && now >= next_pmk) {
        for (unsigned checked = 0; checked < owner->count; checked++) {
            unsigned i = pmk_slot++ % owner->count;
            if (proven[i])
                continue;
            if (!wrapped[i])
                wrapped[i] = sp_join_pmk_seal(owner->peers[i].app_key, owner->peers[i].device, owner->self,
                                              owner->pmk, pmk_frames[i], random_bytes, NULL);
            if (wrapped[i])
                (void)sp_radio_pair_send(pmk_frames[i], SP_JOIN_PMK_SIZE);
            break;
        }
        next_pmk = now + 700;
    }
    if (now < next_send)
        return;
    if (owner->hub && have_pending && !strcmp(phase, "connecting")) {
        (void)sp_radio_pair_send(pending.offer, sizeof(pending.offer));
        next_send = now + 700;
    } else if (!owner->hub && !owner->count && now < window_until) {
        (void)sp_radio_pair_send(beacon, sizeof(beacon));
        next_send = now + 350;
    } else if (!owner->hub && have_ack && now < ack_until) {
        (void)sp_radio_pair_send(ack_frame, sizeof(ack_frame));
        next_send = now + 700;
    }
}
bool sp_pairing_receive(const uint8_t mac[6], const uint8_t *data, size_t n) {
    if (n < 5 || memcmp(data, magic, 4))
        return false;
    if (!ready)
        return true;
    uint64_t now = now_ms();
    if (!owner->hub && owner->count == 1 && data[4] == 4 && n == SP_JOIN_PMK_SIZE &&
        !memcmp(mac, owner->peers[0].mac, 6)) {
        uint8_t key[16];
        if (sp_join_pmk_open(owner->peers[0].app_key, owner->self, owner->peers[0].device, data, n, key))
            (void)sp_radio_pair_pmk(key);
        mbedtls_platform_zeroize(key, sizeof(key));
        return true;
    }
    if (owner->hub && data[4] == 1 && n == SP_JOIN_BEACON_SIZE && !have_pending && now < window_until &&
        !memcmp(mac, data + SP_JOIN_NODE_MAC, 6) && !(mac[0] & 1) && peer_slot(data + SP_JOIN_NODE_ID) < 0) {
        candidate_t *c = NULL;
        for (unsigned i = 0; i < SP_PAIR_CANDIDATES; i++)
            if (candidates[i].used && !memcmp(candidates[i].mac, mac, 6)) {
                c = &candidates[i];
                break;
            }
        if (!c)
            for (unsigned i = 0; i < SP_PAIR_CANDIDATES; i++)
                if (!candidates[i].used || now - candidates[i].seen >= 12000) {
                    c = &candidates[i];
                    break;
                }
        if (c) {
            c->used = true;
            memcpy(c->mac, mac, 6);
            memcpy(c->beacon, data, n);
            c->seen = now;
        }
    } else if (owner->hub && have_pending && data[4] == 3 && n == SP_JOIN_ACK_SIZE &&
               !memcmp(mac, pending.peer.mac, 6) && !memcmp(data + 5, owner->self, 6) &&
               !memcmp(data + 11, pending.peer.device, 6) &&
               !memcmp(data + 17, pending.offer + SP_JOIN_OFFER_NONCE, 16) &&
               !memcmp(data + 33, pending.offer + SP_JOIN_HUB_NONCE, 16)) {
        uint8_t proof[32];
        if (sp_join_proof(pending.peer.app_key, pending.transcript, true, proof) &&
            sp_join_equal(proof, data + 49, 32)) {
            forget("hub_v1");
            have_pending = false;
            phase = "paired";
            window_until = 0;
            mbedtls_platform_zeroize(&pending, sizeof(pending));
        }
    } else if (!owner->hub && !owner->count && data[4] == 2 && n == SP_JOIN_OFFER_SIZE &&
               now < window_until && now >= next_crypto && !memcmp(data + SP_JOIN_NODE_ID, owner->self, 6) &&
               !memcmp(data + SP_JOIN_HUB_MAC, mac, 6) &&
               !memcmp(data + SP_JOIN_OFFER_NONCE, seed.nonce, 16)) {
        next_crypto = now + 1000;
        uint8_t keys[32], transcript[32], proof[32];
        bool ok = sp_join_derive(seed.secret, data + SP_JOIN_HUB_PUBLIC, beacon, data, keys, transcript,
                                 random_bytes, NULL) &&
                  sp_join_proof(keys + 16, transcript, false, proof) &&
                  sp_join_equal(proof, data + SP_JOIN_OFFER_PREFIX, 32);
        sp_peer_t peer = {0};
        if (ok) {
            memcpy(peer.mac, mac, 6);
            memcpy(peer.device, data + SP_JOIN_HUB_ID, 6);
            memcpy(peer.lmk, keys, 16);
            memcpy(peer.app_key, keys + 16, 16);
            ok = sp_radio_add_peer(&peer) == ESP_OK;
        }
        if (ok) {
            memcpy(ack_frame, magic, 4);
            ack_frame[4] = 3;
            memcpy(ack_frame + 5, peer.device, 6);
            memcpy(ack_frame + 11, owner->self, 6);
            memcpy(ack_frame + 17, seed.nonce, 16);
            memcpy(ack_frame + 33, data + SP_JOIN_HUB_NONCE, 16);
            have_ack = sp_join_proof(keys + 16, transcript, true, ack_frame + 49);
            ack_until = now_ms() + 30000;
            next_send = 0;
            phase = "paired";
            forget("node_v1");
            mbedtls_platform_zeroize(&seed, sizeof(seed));
        }
        mbedtls_platform_zeroize(keys, sizeof(keys));
        mbedtls_platform_zeroize(&peer, sizeof(peer));
    }
    return true;
}
void sp_pairing_json(cJSON *o) {
    uint64_t now = now_ms();
    cJSON_AddStringToObject(o, "phase", ready ? phase : "unavailable");
    cJSON_AddNumberToObject(o, "remaining_seconds",
                            window_until > now ? (window_until - now + 999) / 1000 : 0);
    cJSON_AddStringToObject(o, "selected_device_id", selected);
    cJSON_AddBoolToObject(o, "pending", have_pending);
    cJSON_AddStringToObject(o, "security_mode", "first_use_unverified");
    cJSON *list = cJSON_AddArrayToObject(o, "candidates");
    if (!owner || !owner->hub || now >= window_until || have_pending)
        return;
    for (unsigned i = 0; i < SP_PAIR_CANDIDATES; i++)
        if (candidates[i].used && now - candidates[i].seen < 12000) {
            char id[17];
            sp_api_device_id(candidates[i].beacon + SP_JOIN_NODE_ID, id);
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "device_id", id);
            cJSON_AddStringToObject(c, "model", "ESP32-C3");
            cJSON_AddItemToArray(list, c);
        }
}

void sp_pairing_link_seen(unsigned slot) {
    if (ready && owner->hub && slot < owner->count)
        proven[slot] = true;
    if (ready && owner->hub && have_pending && slot < owner->count &&
        !memcmp(owner->peers[slot].device, pending.peer.device, 6)) {
        forget("hub_v1");
        have_pending = false;
        phase = "paired";
        window_until = 0;
        mbedtls_platform_zeroize(&pending, sizeof(pending));
    }
}
