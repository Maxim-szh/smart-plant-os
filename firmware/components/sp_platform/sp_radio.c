#include "sp_internal.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sp_crypto.h"

typedef struct {
    uint8_t mac[6], bytes[SP_WIRE_MAX];
    size_t len;
} rx_packet_t;
static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_done;
static sp_identity_t *identity;
static bool radio_ok;

static bool blob(nvs_handle_t nvs, const char *key, void *data, size_t expected) {
    size_t len = expected;
    return nvs_get_blob(nvs, key, data, &len) == ESP_OK && len == expected;
}
static bool nonzero(const uint8_t *p, size_t n) {
    uint8_t sum = 0;
    while (n--)
        sum |= *p++;
    return sum != 0;
}
bool sp_identity_load(sp_identity_t *id, bool hub) {
    memset(id, 0, sizeof(*id));
    id->hub = hub;
    nvs_handle_t nvs;
    if (nvs_open("provision", NVS_READONLY, &nvs) != ESP_OK)
        return false;
    uint8_t role = 255;
    size_t country_len = sizeof(id->country);
    bool ok = nvs_get_u8(nvs, "role", &role) == ESP_OK && role == (hub ? 1 : 0) &&
              nvs_get_u8(nvs, "count", &id->count) == ESP_OK && id->count >= 1 && id->count <= SP_PEER_MAX &&
              (hub || id->count == 1) && nvs_get_u8(nvs, "channel", &id->channel) == ESP_OK &&
              id->channel >= 1 && id->channel <= 11 &&
              nvs_get_str(nvs, "country", id->country, &country_len) == ESP_OK && country_len == 3 &&
              blob(nvs, "device", id->self, 6) && nonzero(id->self, 6) && blob(nvs, "plant", id->plant, 16) &&
              blob(nvs, "pmk", id->pmk, 16) && nonzero(id->pmk, 16);
    for (unsigned i = 0; ok && i < id->count; i++) {
        sp_peer_t *p = &id->peers[i];
        char key[16];
        snprintf(key, sizeof(key), "p%umac", i);
        ok = blob(nvs, key, p->mac, 6) && !(p->mac[0] & 1) && nonzero(p->mac, 6);
        snprintf(key, sizeof(key), "p%uid", i);
        ok = ok && blob(nvs, key, p->device, 6) && nonzero(p->device, 6);
        snprintf(key, sizeof(key), "p%ulmk", i);
        ok = ok && blob(nvs, key, p->lmk, 16) && nonzero(p->lmk, 16);
        snprintf(key, sizeof(key), "p%uapp", i);
        ok = ok && blob(nvs, key, p->app_key, 16) && nonzero(p->app_key, 16);
        for (unsigned j = 0; j < i; j++)
            if (!memcmp(p->mac, id->peers[j].mac, 6) || !memcmp(p->device, id->peers[j].device, 6))
                ok = false;
    }
    nvs_close(nvs);
    if (!ok || nvs_open("runtime", NVS_READWRITE, &id->runtime) != ESP_OK)
        return false;
    esp_err_t err = nvs_get_u32(id->runtime, "boot", &id->boot);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || id->boot == UINT32_MAX)
        return false;
    id->boot++;
    if (nvs_set_u32(id->runtime, "boot", id->boot) != ESP_OK || nvs_commit(id->runtime) != ESP_OK)
        return false;
    for (unsigned i = 0; i < id->count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "p%uboot", i);
        err = nvs_get_u32(id->runtime, key, &id->peers[i].boot);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            return false;
    }
    return true;
}
static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !data || len < SP_WIRE_HEADER + SP_WIRE_TAG || len > SP_WIRE_MAX)
        return;
    rx_packet_t p = {.len = (size_t)len};
    memcpy(p.mac, info->src_addr, 6);
    memcpy(p.bytes, data, (size_t)len);
    (void)xQueueSend(rx_queue, &p, 0); /* Bounded; no crypto, storage or GPIO in Wi-Fi callbacks. */
}
static void on_sent(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac;
    (void)status;
    xSemaphoreGive(tx_done); /* MAC ACK is deliberately not application success. */
}
esp_err_t sp_radio_start(sp_identity_t *id) {
    identity = id;
    rx_queue = xQueueCreate(16, sizeof(rx_packet_t));
    tx_done = xSemaphoreCreateBinary();
    if (!rx_queue || !tx_done)
        return ESP_ERR_NO_MEM;
    esp_err_t err;
#define TRY(call)                                                                                            \
    do {                                                                                                     \
        err = (call);                                                                                        \
        if (err != ESP_OK)                                                                                   \
            return err;                                                                                      \
    } while (0)
    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta())
        return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&init));
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* Engineering firmware uses STA radio without joining an access point. */
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_country_t country = {.schan = 1, .nchan = 11, .policy = WIFI_COUNTRY_POLICY_MANUAL};
    memcpy(country.cc, id->country, 3);
    TRY(esp_wifi_set_country(&country));
    TRY(esp_wifi_start());
    TRY(esp_wifi_set_ps(WIFI_PS_NONE));
    TRY(esp_wifi_set_channel(id->channel, WIFI_SECOND_CHAN_NONE));
    TRY(esp_now_init());
    TRY(esp_now_set_pmk(id->pmk));
    for (unsigned i = 0; i < id->count; i++) {
        esp_now_peer_info_t peer = {.channel = id->channel, .ifidx = WIFI_IF_STA, .encrypt = true};
        memcpy(peer.peer_addr, id->peers[i].mac, 6);
        memcpy(peer.lmk, id->peers[i].lmk, 16);
        TRY(esp_now_add_peer(&peer));
    }
    TRY(esp_now_register_recv_cb(on_receive));
    TRY(esp_now_register_send_cb(on_sent));
    radio_ok = true;
    return ESP_OK;
#undef TRY
}
bool sp_radio_send(unsigned peer, const sp_message_t *m) {
    if (!radio_ok || peer >= identity->count || identity->tx_sequence == UINT32_MAX)
        return false;
    sp_peer_t *p = &identity->peers[peer];
    if (m->value[SP_K_KIND] != SP_HELLO && !p->boot)
        return false;
    uint8_t frame[SP_WIRE_MAX];
    size_t len;
    sp_header_t h = {.role = identity->hub,
                     .boot = identity->boot,
                     .sequence = ++identity->tx_sequence,
                     .target_boot = m->value[SP_K_KIND] == SP_HELLO ? 0 : p->boot};
    if (!sp_frame_seal(p->app_key, &h, m, frame, &len) || esp_now_send(p->mac, frame, len) != ESP_OK)
        return false;
    /* Never send the next frame before completion. A missing callback disables transport,
       rather than misattributing a late callback to the next physical command. */
    if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(100)) != pdTRUE)
        radio_ok = false;
    return radio_ok;
}
bool sp_radio_receive(sp_received_t *out) {
    rx_packet_t packet;
    if (!radio_ok || xQueueReceive(rx_queue, &packet, 0) != pdTRUE)
        return false;
    unsigned i;
    for (i = 0; i < identity->count; i++)
        if (!memcmp(identity->peers[i].mac, packet.mac, 6))
            break;
    if (i == identity->count)
        return false;
    sp_peer_t *p = &identity->peers[i];
    sp_header_t h;
    if (!sp_header_decode(&h, packet.bytes, packet.len) || h.role == identity->hub || h.boot < p->boot ||
        (h.boot == p->boot && h.sequence <= p->rx_sequence))
        return false;
    if (!sp_frame_open(p->app_key, packet.bytes, packet.len, &h, &out->message) ||
        !sp_header_fresh(&h, identity->hub, identity->boot, p->boot, p->rx_sequence,
                         out->message.value[0] == SP_HELLO))
        return false;
    if (out->message.value[SP_K_KIND] == SP_HELLO) {
        if (h.target_boot || !sp_message_has(&out->message, SP_K_DEVICE) ||
            memcmp(out->message.bytes[SP_K_DEVICE - 24], p->device, 6))
            return false;
    } else if (h.target_boot != identity->boot)
        return false;
    if (h.boot > p->boot) {
        char key[16];
        snprintf(key, sizeof(key), "p%uboot", i);
        if (nvs_set_u32(identity->runtime, key, h.boot) != ESP_OK ||
            nvs_commit(identity->runtime) != ESP_OK) {
            radio_ok = false;
            return false;
        }
        p->boot = h.boot;
    }
    p->rx_sequence = h.sequence;
    out->peer = i;
    return true;
}
