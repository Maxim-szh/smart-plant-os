#include "sp_internal.h"
#include "sp_update.h"
#include "sp_web.h"
#include "sp_setup.h"
#include "sp_ap_policy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "sp_crypto.h"
#include "sp_pairing.h"
#include "sp_epochs.h"
#include "sp_text.h"
#include "esp_random.h"

typedef struct {
    uint8_t mac[6], bytes[SP_WIRE_MAX];
    size_t len;
    int8_t rssi;
    bool have_rssi;
} rx_packet_t;
static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_done;
static EventGroupHandle_t wifi_events;
static sp_identity_t *identity;
static bool radio_ok;
static unsigned rx_packets, rx_unknown, rx_header, rx_auth, rx_valid, tx_attempts, tx_ok;
static uint64_t diagnostic_at;
static unsigned tx_timeouts;
static volatile uint8_t last_disconnect;
#define WIFI_SCAN_DONE_BIT BIT2
static bool scan_active, scan_available;
static uint64_t scan_deadline, scan_next_at;
static const char *scan_phase = "idle";
static unsigned scan_count;
static struct {
    char ssid[33];
    int rssi;
    unsigned channel;
    bool supported;
} scans[20];
static void scan_tick(void) {
    if (!scan_active)
        return;
    if (xEventGroupGetBits(wifi_events) & WIFI_SCAN_DONE_BIT) {
        xEventGroupClearBits(wifi_events, WIFI_SCAN_DONE_BIT);
        uint16_t count = 20;
        wifi_ap_record_t *records = calloc(count, sizeof(*records));
        scan_count = 0;
        if (!records || esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
            scan_phase = "failed";
            (void)esp_wifi_clear_ap_list();
        } else {
            for (unsigned i = 0; i < count; i++) {
                char ssid[33] = {0};
                memcpy(ssid, records[i].ssid, 32);
                if (!sp_text_valid(ssid, 1, 32))
                    continue;
                bool duplicate = false;
                for (unsigned j = 0; j < scan_count; j++)
                    if (!strcmp(scans[j].ssid, ssid))
                        duplicate = true;
                if (duplicate)
                    continue;
                snprintf(scans[scan_count].ssid, 33, "%s", ssid);
                scans[scan_count].rssi = records[i].rssi;
                scans[scan_count].channel = records[i].primary;
                scans[scan_count].supported = records[i].primary >= 1 && records[i].primary <= 11 &&
                                              (records[i].authmode == WIFI_AUTH_WPA2_PSK ||
                                               records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK ||
                                               records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK);
                scan_count++;
            }
            scan_phase = "completed";
            scan_available = true;
        }
        free(records);
        scan_active = false;
    } else if ((uint64_t)esp_timer_get_time() / 1000 >= scan_deadline) {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
        scan_active = false;
        scan_phase = "failed";
    }
}

static volatile bool home_wifi_enabled;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CHANNEL_UNSUPPORTED_BIT BIT1
#define NODE_CHANNEL_SCAN_MS 600u
#define NODE_HUB_TIMEOUT_MS 5000u
static uint64_t node_last_hub_rx_ms, node_scan_due_ms;
static uint8_t node_scan_channel;
typedef struct {
    uint8_t mac[6], device[6], lmk[16], app_key[16];
} link_v1_t;
typedef struct {
    uint32_t version, count;
    uint8_t owner[6];
    link_v1_t peers[SP_PEER_MAX];
} links_v1_t;
typedef struct {
    uint32_t version, count;
    uint8_t owner[6], operation[16];
    link_v1_t peer;
} node_link_v2_t;
typedef struct {
    char ssid[33], password[64];
} network_v1_t;
static network_v1_t trial_network;
static wifi_config_t previous_network;
static bool network_trial, previous_enabled;
static uint64_t network_deadline;
static const char *network_phase = "idle";
static bool setup_ap_available;
static sp_ap_policy_t ap_policy;
static uint64_t ap_check_at, ap_forced_until;
static void update_setup_ap(void) {
    if (!setup_ap_available || !wifi_events)
        return;
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    if (now < ap_check_at)
        return;
    ap_check_at = now + 200;
    bool connected = (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
    bool enable = sp_ap_policy_step(&ap_policy, now, connected, network_trial || now < ap_forced_until);
    wifi_mode_t current;
    if (esp_wifi_get_mode(&current) != ESP_OK)
        return;
    wifi_mode_t wanted = enable ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if (current != wanted) {
        esp_err_t err = esp_wifi_set_mode(wanted);
        if (err == ESP_OK)
            printf("Setup AP %s; home Wi-Fi connected=%u\n", enable ? "enabled" : "disabled", connected);
        else
            printf("Setup AP mode change failed error=%s\n", esp_err_to_name(err));
    }
}

static const char *wifi_disconnect_reason(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "AP_NOT_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRED";
    default:
        return "OTHER";
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(wifi_events, WIFI_SCAN_DONE_BIT);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        wifi_ap_record_t ap = {0};
        uint8_t primary = 0;
        wifi_second_chan_t secondary;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && esp_wifi_get_channel(&primary, &secondary) == ESP_OK) {
            if (primary < 1 || primary > 11) {
                printf("Home Wi-Fi channel %u is outside this profile's Node scan range 1-11; using "
                       "standalone AP\n",
                       primary);
                home_wifi_enabled = false;
                xEventGroupSetBits(wifi_events, WIFI_CHANNEL_UNSUPPORTED_BIT);
                return;
            }
            identity->channel = primary;
            printf("Home Wi-Fi connected ip=" IPSTR " channel=%u ssid=%s\n", IP2STR(&event->ip_info.ip),
                   primary, (const char *)ap.ssid);
            xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && home_wifi_enabled) {
        wifi_event_sta_disconnected_t *event = data;
        if (event)
            last_disconnect = event->reason;
        if (event)
            printf("Home Wi-Fi disconnected reason=%u (%s) ssid=%s rssi=%d; retrying\n", event->reason,
                   wifi_disconnect_reason(event->reason), event->ssid, event->rssi);
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
            printf("Home Wi-Fi reconnect request failed error=%s\n", esp_err_to_name(err));
    }
}

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
    esp_err_t provision_status = nvs_open("provision", NVS_READONLY, &nvs);
    if (!hub && provision_status == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh empty Node only: never repair, erase or replace an existing identity. */
        uint8_t device[6], plant[16], pmk[16];
        esp_fill_random(device, sizeof(device));
        esp_fill_random(plant, sizeof(plant));
        esp_fill_random(pmk, sizeof(pmk));
        plant[6] = (plant[6] & 15) | 0x40;
        plant[8] = (plant[8] & 63) | 0x80;
        if (nvs_open("provision", NVS_READWRITE, &nvs) != ESP_OK)
            return false;
        bool created = nvs_set_u8(nvs, "role", 0) == ESP_OK && nvs_set_u8(nvs, "count", 0) == ESP_OK &&
                       nvs_set_u8(nvs, "channel", 6) == ESP_OK &&
                       nvs_set_str(nvs, "country", "01") == ESP_OK &&
                       nvs_set_blob(nvs, "device", device, sizeof(device)) == ESP_OK &&
                       nvs_set_blob(nvs, "plant", plant, sizeof(plant)) == ESP_OK &&
                       nvs_set_blob(nvs, "pmk", pmk, sizeof(pmk)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
        nvs_close(nvs);
        if (!created || nvs_open("provision", NVS_READONLY, &nvs) != ESP_OK)
            return false;
    } else if (provision_status != ESP_OK)
        return false;
    uint8_t role = 255;
    size_t country_len = sizeof(id->country);
    bool ok = nvs_get_u8(nvs, "role", &role) == ESP_OK && role == (hub ? 1 : 0) &&
              nvs_get_u8(nvs, "count", &id->count) == ESP_OK && id->count <= SP_PEER_MAX &&
              (hub || id->count <= 1) && nvs_get_u8(nvs, "channel", &id->channel) == ESP_OK &&
              id->channel >= 1 && id->channel <= 11 &&
              nvs_get_str(nvs, "country", id->country, &country_len) == ESP_OK && country_len == 3 &&
              blob(nvs, "device", id->self, 6) && nonzero(id->self, 6) && blob(nvs, "plant", id->plant, 16) &&
              blob(nvs, "pmk", id->pmk, 16) && nonzero(id->pmk, 16);
    links_v1_t links = {0};
    nvs_handle_t link_store;
    bool overlay = false;
    if (nvs_open("links", NVS_READONLY, &link_store) == ESP_OK) {
        size_t length = sizeof(links);
        esp_err_t e = nvs_get_blob(link_store, "registry_v1", &links, &length);
        if (e == ESP_OK) {
            overlay = length == sizeof(links) && links.version == 1 && links.count <= SP_PEER_MAX &&
                      (hub || links.count <= 1) && !memcmp(links.owner, id->self, 6);
            if (!overlay)
                ok = false;
            else
                id->count = links.count;
        } else if (e != ESP_ERR_NVS_NOT_FOUND)
            ok = false;
        if (!hub) {
            node_link_v2_t node = {0};
            length = sizeof(node);
            e = nvs_get_blob(link_store, "node_v2", &node, &length);
            if (e == ESP_OK) {
                if (length != sizeof(node) || node.version != 2 || node.count > 1 ||
                    memcmp(node.owner, id->self, 6))
                    ok = false;
                else {
                    overlay = true;
                    links.count = id->count = node.count;
                    if (node.count)
                        links.peers[0] = node.peer;
                }
            } else if (e != ESP_ERR_NVS_NOT_FOUND)
                ok = false;
        }
        nvs_close(link_store);
    }
    for (unsigned i = 0; ok && i < id->count; i++) {
        sp_peer_t *p = &id->peers[i];
        char key[16];
        if (overlay) {
            memcpy(p->mac, links.peers[i].mac, 6);
            memcpy(p->device, links.peers[i].device, 6);
            memcpy(p->lmk, links.peers[i].lmk, 16);
            memcpy(p->app_key, links.peers[i].app_key, 16);
            ok = !(p->mac[0] & 1) && nonzero(p->mac, 6) && nonzero(p->device, 6) && nonzero(p->lmk, 16) &&
                 nonzero(p->app_key, 16);
        } else {
            snprintf(key, sizeof(key), "p%umac", i);
            ok = blob(nvs, key, p->mac, 6) && !(p->mac[0] & 1) && nonzero(p->mac, 6);
            snprintf(key, sizeof(key), "p%uid", i);
            ok = ok && blob(nvs, key, p->device, 6) && nonzero(p->device, 6);
            snprintf(key, sizeof(key), "p%ulmk", i);
            ok = ok && blob(nvs, key, p->lmk, 16) && nonzero(p->lmk, 16);
            snprintf(key, sizeof(key), "p%uapp", i);
            ok = ok && blob(nvs, key, p->app_key, 16) && nonzero(p->app_key, 16);
        }
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
    if (!sp_peer_epochs_load(id))
        return false;
    return true;
}
static void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !data || len < 5 || len > SP_WIRE_MAX)
        return;
    rx_packet_t p = {.len = (size_t)len,
                     .have_rssi = info->rx_ctrl != NULL,
                     .rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0};
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
        if (err != ESP_OK) {                                                                                 \
            printf("radio_start_failed step=%s error=%d\n", #call, (int)err);                                \
            return err;                                                                                      \
        }                                                                                                    \
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
    setup_ap_available = id->hub && sp_web_ap(id->channel);
    sp_ap_policy_init(&ap_policy);
    /* Follow the connected router's regulatory domain; without it, use the provisioned safe fallback. */
    wifi_country_t country = {.schan = 1, .nchan = 11, .policy = WIFI_COUNTRY_POLICY_AUTO};
    memcpy(country.cc, id->country, 3);
    TRY(esp_wifi_set_country(&country));

    char ssid[33] = {0}, password[64] = {0};
    nvs_handle_t provision;
    esp_err_t provision_err =
        id->hub ? nvs_open("provision", NVS_READONLY, &provision) : ESP_ERR_INVALID_STATE;
    if (id->hub && provision_err == ESP_OK) {
        size_t ssid_size = sizeof(ssid), password_size = sizeof(password);
        esp_err_t ssid_err = nvs_get_str(provision, "home_ssid", ssid, &ssid_size);
        esp_err_t pass_err = nvs_get_str(provision, "home_pass", password, &password_size);
        printf("Wi-Fi NVS read: namespace=provision open=%s ssid=%s pass=%s\n",
               esp_err_to_name(provision_err), esp_err_to_name(ssid_err), esp_err_to_name(pass_err));
        nvs_handle_t network_store;
        if (nvs_open("network", NVS_READONLY, &network_store) == ESP_OK) {
            network_v1_t saved = {0};
            size_t length = sizeof(saved);
            esp_err_t saved_err = nvs_get_blob(network_store, "home_v1", &saved, &length);
            nvs_close(network_store);
            if (saved_err == ESP_OK && length == sizeof(saved) && memchr(saved.ssid, 0, sizeof(saved.ssid)) &&
                memchr(saved.password, 0, sizeof(saved.password))) {
                memcpy(ssid, saved.ssid, sizeof(ssid));
                memcpy(password, saved.password, sizeof(password));
                ssid_err = pass_err = ESP_OK;
            }
        }
        bool has_home_wifi = ssid_err == ESP_OK && ssid[0];
        if (has_home_wifi && pass_err == ESP_OK && password[0]) {
            printf("Connecting to home Wi-Fi ssid=%s\n", ssid);
            wifi_config_t sta = {0};
            memcpy(sta.sta.ssid, ssid, strlen(ssid));
            memcpy(sta.sta.password, password, strlen(password));
            TRY(esp_wifi_set_config(WIFI_IF_STA, &sta));
            home_wifi_enabled = true;
        } else {
            printf("Home Wi-Fi credentials unavailable; SSID status=%s password status=%s\n",
                   esp_err_to_name(ssid_err), esp_err_to_name(pass_err));
        }
        nvs_close(provision);
    } else if (id->hub) {
        printf("Wi-Fi NVS open failed namespace=provision error=%s\n", esp_err_to_name(provision_err));
    }
    wifi_events = xEventGroupCreate();
    if (!wifi_events)
        return ESP_ERR_NO_MEM;
    TRY(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL));
    TRY(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    TRY(esp_wifi_start());
    TRY(esp_wifi_set_ps(WIFI_PS_NONE));
    if (home_wifi_enabled) {
        TRY(esp_wifi_connect());
        EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_CHANNEL_UNSUPPORTED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            printf("Home Wi-Fi unavailable or unsupported; continuing in setup mode on fallback channel=%u\n",
                   id->channel);
            home_wifi_enabled = false;
            xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_CHANNEL_UNSUPPORTED_BIT);
            (void)esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            TRY(esp_wifi_set_mode(setup_ap_available ? WIFI_MODE_APSTA : WIFI_MODE_STA));
            TRY(esp_wifi_set_channel(id->channel, WIFI_SECOND_CHAN_NONE));
        }
    } else {
        TRY(esp_wifi_set_channel(id->channel, WIFI_SECOND_CHAN_NONE));
    }
    TRY(esp_now_init());
    TRY(esp_now_set_pmk(id->pmk));
    for (unsigned i = 0; i < id->count; i++) {
        /* Channel 0 means the peer uses the current radio channel; this allows discovery after router
         * changes. */
        esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true};
        memcpy(peer.peer_addr, id->peers[i].mac, 6);
        memcpy(peer.lmk, id->peers[i].lmk, 16);
        TRY(esp_now_add_peer(&peer));
    }
    esp_now_peer_info_t broadcast = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memset(broadcast.peer_addr, 0xff, 6);
    TRY(esp_now_add_peer(&broadcast));
    TRY(esp_now_register_recv_cb(on_receive));
    TRY(esp_now_register_send_cb(on_sent));
    if (!id->hub) {
        node_scan_channel = id->channel;
        node_last_hub_rx_ms = 0;
        node_scan_due_ms = 0;
    }
    radio_ok = true;
    return ESP_OK;
#undef TRY
}
void sp_radio_process(void) {
    if (identity && identity->hub && wifi_events)
        scan_tick();
    uint64_t diagnostic_now = (uint64_t)esp_timer_get_time() / 1000;
    if (identity && identity->count && diagnostic_now < 120000 && diagnostic_now >= diagnostic_at) {
        diagnostic_at = diagnostic_now + 5000;
        printf("radio_diag role=%u count=%u enabled=%u channel=%u rx=%u unknown=%u header=%u auth=%u "
               "valid=%u tx=%u ok=%u\n",
               identity->hub, identity->count, radio_ok, sp_radio_channel(), rx_packets, rx_unknown,
               rx_header, rx_auth, rx_valid, tx_attempts, tx_ok);
    }
    if (!radio_ok || !identity)
        return;
    if (identity->hub) {
        if (network_trial) {
            uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
            EventBits_t bits = xEventGroupGetBits(wifi_events);
            wifi_ap_record_t current_ap = {0};
            bool trial_connected = (bits & WIFI_CONNECTED_BIT) &&
                                   esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK &&
                                   strlen(trial_network.ssid) == strnlen((const char *)current_ap.ssid, 32) &&
                                   !memcmp(trial_network.ssid, current_ap.ssid, strlen(trial_network.ssid));
            if (trial_connected) {
                nvs_handle_t store;
                bool saved = nvs_open("network", NVS_READWRITE, &store) == ESP_OK;
                if (saved) {
                    saved = nvs_set_blob(store, "home_v1", &trial_network, sizeof(trial_network)) == ESP_OK &&
                            nvs_commit(store) == ESP_OK;
                    nvs_close(store);
                }
                if (saved) {
                    network_trial = false;
                    network_phase = "connected";
                    memset(&trial_network, 0, sizeof(trial_network));
                    memset(&previous_network, 0, sizeof(previous_network));
                } else {
                    network_deadline = 0;
                    network_phase = "storage_error";
                }
            }
            if (network_trial && (now >= network_deadline || (bits & WIFI_CHANNEL_UNSUPPORTED_BIT))) {
                network_trial = false;
                if (strcmp(network_phase, "storage_error"))
                    network_phase = "failed";
                home_wifi_enabled = false;
                (void)esp_wifi_disconnect();
                xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_CHANNEL_UNSUPPORTED_BIT);
                vTaskDelay(pdMS_TO_TICKS(100));
                (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
                (void)esp_wifi_set_config(WIFI_IF_STA, &previous_network);
                home_wifi_enabled = previous_enabled;
                if (home_wifi_enabled)
                    (void)esp_wifi_connect();
                else
                    (void)esp_wifi_set_channel(identity->channel, WIFI_SECOND_CHAN_NONE);
                memset(&trial_network, 0, sizeof(trial_network));
                memset(&previous_network, 0, sizeof(previous_network));
            }
        }
        if (wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_CHANNEL_UNSUPPORTED_BIT)) {
            xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_CHANNEL_UNSUPPORTED_BIT);
            (void)esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            if (esp_wifi_set_mode(setup_ap_available ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK ||
                esp_wifi_set_channel(identity->channel, WIFI_SECOND_CHAN_NONE) != ESP_OK)
                puts("Could not restore standalone channel; reboot required");
        }
        update_setup_ap();
        return;
    }
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    if (node_last_hub_rx_ms && now - node_last_hub_rx_ms < NODE_HUB_TIMEOUT_MS)
        return;
    if (now < node_scan_due_ms)
        return;
    node_scan_channel = node_scan_channel >= 11 ? 1 : (uint8_t)(node_scan_channel + 1);
    if (esp_wifi_set_channel(node_scan_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        node_scan_due_ms = now + NODE_CHANNEL_SCAN_MS;
        return;
    }
    node_scan_due_ms = now + NODE_CHANNEL_SCAN_MS;
}
bool sp_radio_send(unsigned peer, const sp_message_t *m) {
    if (!radio_ok || peer >= identity->count || identity->tx_sequence == UINT32_MAX)
        return false;
    tx_attempts++;
    sp_peer_t *p = &identity->peers[peer];
    if (m->value[SP_K_KIND] != SP_HELLO && !p->boot)
        return false;
    uint8_t frame[SP_WIRE_MAX];
    size_t len;
    sp_header_t h = {.role = identity->hub,
                     .boot = identity->boot,
                     .sequence = ++identity->tx_sequence,
                     .target_boot = m->value[SP_K_KIND] == SP_HELLO ? 0 : p->boot};
    if (!sp_frame_seal(p->app_key, &h, m, frame, &len)) {
        printf("radio_tx_encode_failed role=%u kind=%u\n", identity->hub, (unsigned)m->value[0]);
        return false;
    }
    esp_err_t sent = esp_now_send(p->mac, frame, len);
    if (sent != ESP_OK) {
        printf("radio_tx_failed role=%u kind=%u error=%s\n", identity->hub, (unsigned)m->value[0],
               esp_err_to_name(sent));
        return false;
    }
    /* Never send the next frame before completion. A missing callback disables transport,
       rather than misattributing a late callback to the next physical command. */
    if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        printf("radio_tx_callback_timeout role=%u; transport disabled\n", identity->hub);
        tx_timeouts++;
        radio_ok = false;
    }
    if (radio_ok)
        tx_ok++;
    return radio_ok;
}
bool sp_radio_update_send(unsigned peer, const uint8_t *data, size_t len) {
    if (!radio_ok || peer >= identity->count || len > SP_WIRE_MAX)
        return false;
    if (esp_now_send(identity->peers[peer].mac, data, len) != ESP_OK)
        return false;
    if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        tx_timeouts++;
        radio_ok = false;
    }
    return radio_ok;
}
bool sp_radio_receive(sp_received_t *out) {
    rx_packet_t packet;
    if (!radio_ok || xQueueReceive(rx_queue, &packet, 0) != pdTRUE)
        return false;
    rx_packets++;
    if (sp_pairing_receive(packet.mac, packet.bytes, packet.len))
        return false;
    unsigned i;
    for (i = 0; i < identity->count; i++)
        if (!memcmp(identity->peers[i].mac, packet.mac, 6))
            break;
    if (i == identity->count) {
        rx_unknown++;
        return false;
    }
    if (sp_update_receive(i, packet.bytes, packet.len))
        return false;
    sp_peer_t *p = &identity->peers[i];
    sp_header_t h;
    if (!sp_header_decode(&h, packet.bytes, packet.len) || h.role == identity->hub || h.boot < p->boot ||
        (h.boot == p->boot && h.sequence <= p->rx_sequence)) {
        rx_header++;
        return false;
    }
    if (!sp_frame_open(p->app_key, packet.bytes, packet.len, &h, &out->message) ||
        !sp_header_fresh(&h, identity->hub, identity->boot, p->boot, p->rx_sequence,
                         out->message.value[0] == SP_HELLO)) {
        rx_auth++;
        return false;
    }
    if (out->message.value[SP_K_KIND] == SP_HELLO) {
        if (h.target_boot || !sp_message_has(&out->message, SP_K_DEVICE) ||
            memcmp(out->message.bytes[SP_K_DEVICE - 24], p->device, 6))
            return false;
    } else if (h.target_boot != identity->boot)
        return false;
    if (h.boot > p->boot) {
        if (!sp_peer_epoch_store(identity->runtime, p->device, h.boot)) {
            radio_ok = false;
            return false;
        }
        p->boot = h.boot;
    }
    p->rx_sequence = h.sequence;
    out->have_rssi = packet.have_rssi;
    out->rssi = packet.rssi;
    if (!identity->hub) {
        uint8_t current = 0;
        wifi_second_chan_t secondary;
        if (esp_wifi_get_channel(&current, &secondary) == ESP_OK) {
            identity->channel = current;
            node_scan_channel = current;
            node_last_hub_rx_ms = (uint64_t)esp_timer_get_time() / 1000;
            node_scan_due_ms = node_last_hub_rx_ms + NODE_HUB_TIMEOUT_MS;
        }
    }
    rx_valid++;
    sp_pairing_link_seen(i);
    sp_update_link_seen(i);
    out->peer = i;
    return true;
}

esp_err_t sp_radio_network_begin(const char *ssid, const char *password) {
    if (!identity || !identity->hub || !radio_ok || network_trial || scan_active)
        return ESP_ERR_INVALID_STATE;
    if (!sp_text_valid(ssid, 1, 32) || !password || strlen(password) < 8 || strlen(password) > 63)
        return ESP_ERR_INVALID_ARG;
    if (esp_wifi_get_config(WIFI_IF_STA, &previous_network) != ESP_OK)
        return ESP_FAIL;
    previous_enabled = home_wifi_enabled;
    memset(&trial_network, 0, sizeof(trial_network));
    snprintf(trial_network.ssid, sizeof(trial_network.ssid), "%s", ssid);
    snprintf(trial_network.password, sizeof(trial_network.password), "%s", password);
    home_wifi_enabled = false;
    (void)esp_wifi_disconnect();
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_CHANNEL_UNSUPPORTED_BIT);
    vTaskDelay(pdMS_TO_TICKS(100));
    wifi_config_t sta = {0};
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    memcpy(sta.sta.ssid, ssid, strlen(ssid));
    memcpy(sta.sta.password, password, strlen(password));
    network_trial = true;
    network_phase = "connecting";
    network_deadline = (uint64_t)esp_timer_get_time() / 1000 + 30000;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) {
        network_deadline = 0;
        return ESP_FAIL;
    }
    home_wifi_enabled = true;
    if (esp_wifi_connect() != ESP_OK) {
        network_deadline = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}
void sp_radio_network_json(cJSON *o) {
    wifi_ap_record_t ap = {0};
    esp_netif_ip_info_t info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) &&
                     esp_wifi_sta_get_ap_info(&ap) == ESP_OK && netif &&
                     esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr;
    char ssid[33] = {0}, ip[16] = {0};
    if (connected) {
        memcpy(ssid, ap.ssid, 32);
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    uint8_t channel = 0;
    wifi_second_chan_t secondary;
    (void)esp_wifi_get_channel(&channel, &secondary);
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    cJSON_AddBoolToObject(o, "setup_ap_enabled",
                          setup_ap_available && (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP));
    cJSON_AddBoolToObject(o, "wifi_connected", connected);
    cJSON_AddStringToObject(o, "wifi_phase",
                            network_trial                                 ? "connecting"
                            : !strcmp(network_phase, "idle") && connected ? "connected"
                                                                          : network_phase);
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddNumberToObject(o, "channel", channel);
    if (connected)
        cJSON_AddNumberToObject(o, "rssi_dbm", ap.rssi);
    else
        cJSON_AddNullToObject(o, "rssi_dbm");
    cJSON_AddStringToObject(o, "last_disconnect_reason",
                            last_disconnect ? wifi_disconnect_reason(last_disconnect) : "NONE");
}
esp_err_t sp_radio_add_peer(const sp_peer_t *peer) {
    if (!identity || !radio_ok || identity->count >= SP_PEER_MAX || (!identity->hub && identity->count) ||
        network_trial)
        return ESP_ERR_INVALID_STATE;
    uint8_t self_mac[6];
    esp_read_mac(self_mac, ESP_MAC_WIFI_STA);
    if (!memcmp(peer->mac, self_mac, 6) || !memcmp(peer->device, identity->self, 6))
        return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < identity->count; i++)
        if (!memcmp(peer->mac, identity->peers[i].mac, 6) ||
            !memcmp(peer->device, identity->peers[i].device, 6))
            return ESP_ERR_INVALID_STATE;
    if (!nonzero(peer->lmk, 16) || !nonzero(peer->app_key, 16) || !memcmp(peer->lmk, peer->app_key, 16))
        return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < identity->count; i++) {
        const sp_peer_t *p = &identity->peers[i];
        if (!memcmp(peer->lmk, p->lmk, 16) || !memcmp(peer->app_key, p->app_key, 16) ||
            !memcmp(peer->lmk, p->app_key, 16) || !memcmp(peer->app_key, p->lmk, 16))
            return ESP_ERR_INVALID_ARG;
    }
    esp_now_peer_info_t radio = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true};
    memcpy(radio.peer_addr, peer->mac, 6);
    memcpy(radio.lmk, peer->lmk, 16);
    esp_err_t err = esp_now_add_peer(&radio);
    if (err != ESP_OK)
        return err;
    links_v1_t links = {.version = 1, .count = identity->count + 1};
    memcpy(links.owner, identity->self, 6);
    for (unsigned i = 0; i < links.count; i++) {
        const sp_peer_t *p = i == identity->count ? peer : &identity->peers[i];
        memcpy(links.peers[i].mac, p->mac, 6);
        memcpy(links.peers[i].device, p->device, 6);
        memcpy(links.peers[i].lmk, p->lmk, 16);
        memcpy(links.peers[i].app_key, p->app_key, 16);
    }
    nvs_handle_t store;
    err = nvs_open("links", NVS_READWRITE, &store);
    if (err == ESP_OK) {
        if (identity->hub)
            err = nvs_set_blob(store, "registry_v1", &links, sizeof(links));
        else {
            node_link_v2_t node = {.version = 2, .count = 1, .peer = links.peers[0]};
            memcpy(node.owner, identity->self, 6);
            err = nvs_set_blob(store, "node_v2", &node, sizeof(node));
        }
        if (err == ESP_OK)
            err = nvs_commit(store);
        nvs_close(store);
    }
    if (err != ESP_OK) {
        (void)esp_now_del_peer(peer->mac);
        return err;
    }
    identity->peers[identity->count++] = *peer;
    if (!identity->hub) {
        node_last_hub_rx_ms = (uint64_t)esp_timer_get_time() / 1000;
        node_scan_due_ms = node_last_hub_rx_ms + NODE_HUB_TIMEOUT_MS;
    }
    return ESP_OK;
}

uint8_t sp_radio_channel(void) {
    uint8_t channel = 0;
    wifi_second_chan_t secondary;
    return esp_wifi_get_channel(&channel, &secondary) == ESP_OK && channel >= 1 && channel <= 11 ? channel
           : identity ? identity->channel
                      : 6;
}

bool sp_radio_setup_window(void) {
    if (!radio_ok || !identity || !identity->hub || !setup_ap_available)
        return false;
    ap_forced_until = (uint64_t)esp_timer_get_time() / 1000 + 300000;
    return true;
}

bool sp_radio_pair_send(const uint8_t *data, size_t size) {
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    if (!radio_ok || !data || size > 250)
        return false;
    if (esp_now_send(broadcast, data, size) != ESP_OK)
        return false;
    if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        printf("radio_tx_callback_timeout role=%u; transport disabled\n", identity->hub);
        tx_timeouts++;
        radio_ok = false;
    }
    return radio_ok;
}

esp_err_t sp_radio_pair_pmk(const uint8_t key[16]) {
    if (!identity || identity->hub || identity->count != 1 || !radio_ok)
        return ESP_ERR_INVALID_STATE;
    if (!memcmp(identity->pmk, key, 16))
        return ESP_OK;
    nvs_handle_t h;
    esp_err_t e = nvs_open("provision", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, "pmk", key, 16);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK)
        return e;
    sp_peer_t *p = &identity->peers[0];
    esp_now_peer_info_t radio = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = true};
    memcpy(radio.peer_addr, p->mac, 6);
    memcpy(radio.lmk, p->lmk, 16);
    e = esp_now_del_peer(p->mac);
    if (e == ESP_OK)
        e = esp_now_set_pmk(key);
    if (e == ESP_OK)
        e = esp_now_add_peer(&radio);
    if (e != ESP_OK) {
        radio_ok = false;
        return e;
    }
    memcpy(identity->pmk, key, 16);
    node_last_hub_rx_ms = (uint64_t)esp_timer_get_time() / 1000;
    node_scan_due_ms = node_last_hub_rx_ms + NODE_HUB_TIMEOUT_MS;
    puts("Radio PMK synchronized over protected pairing exchange");
    return ESP_OK;
}

esp_err_t sp_radio_prepare_unpair(const uint8_t operation[16]) {
    if (!identity || identity->hub || identity->count != 1 || !radio_ok)
        return ESP_ERR_INVALID_STATE;
    node_link_v2_t node = {.version = 2, .count = 0};
    memcpy(node.owner, identity->self, 6);
    memcpy(node.operation, operation, 16);
    nvs_handle_t h;
    esp_err_t e = nvs_open("links", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, "node_v2", &node, sizeof(node));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e;
}
esp_err_t sp_radio_remove_peer(unsigned slot) {
    if (!identity || !identity->hub || !radio_ok || slot >= identity->count || network_trial)
        return ESP_ERR_INVALID_STATE;
    sp_peer_t old = identity->peers[slot];
    links_v1_t next = {.version = 1, .count = identity->count - 1};
    memcpy(next.owner, identity->self, 6);
    unsigned k = 0;
    for (unsigned i = 0; i < identity->count; i++)
        if (i != slot) {
            sp_peer_t *p = &identity->peers[i];
            link_v1_t *d = &next.peers[k++];
            memcpy(d->mac, p->mac, 6);
            memcpy(d->device, p->device, 6);
            memcpy(d->lmk, p->lmk, 16);
            memcpy(d->app_key, p->app_key, 16);
        }
    nvs_handle_t h;
    esp_err_t e = nvs_open("links", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_blob(h, "registry_v1", &next, sizeof(next));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK)
        return e;
    e = esp_now_del_peer(old.mac);
    if (e != ESP_OK) {
        radio_ok = false;
        return e;
    }
    sp_pairing_peer_removed(slot);
    memmove(&identity->peers[slot], &identity->peers[slot + 1],
            (identity->count - slot - 1) * sizeof(sp_peer_t));
    memset(&identity->peers[--identity->count], 0, sizeof(sp_peer_t));
    return ESP_OK;
}

esp_err_t sp_radio_scan_begin(void) {
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
    if (!identity || !identity->hub || !radio_ok || network_trial || scan_active || now < scan_next_at)
        return ESP_ERR_INVALID_STATE;
    wifi_scan_config_t config = {
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = {.min = 30, .max = 60}};
    xEventGroupClearBits(wifi_events, WIFI_SCAN_DONE_BIT);
    esp_err_t e = esp_wifi_scan_start(&config, false);
    if (e != ESP_OK)
        return e;
    scan_active = true;
    scan_available = false;
    scan_count = 0;
    scan_phase = "scanning";
    scan_deadline = now + 20000;
    scan_next_at = now + 30000;
    return ESP_OK;
}
void sp_radio_scan_json(cJSON *o) {
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddStringToObject(o, "phase", scan_phase);
    cJSON_AddBoolToObject(o, "available", scan_available);
    cJSON *rows = cJSON_AddArrayToObject(o, "networks");
    for (unsigned i = 0; i < scan_count; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "ssid", scans[i].ssid);
        cJSON_AddNumberToObject(r, "rssi_dbm", scans[i].rssi);
        cJSON_AddNumberToObject(r, "channel", scans[i].channel);
        cJSON_AddBoolToObject(r, "supported", scans[i].supported);
        cJSON_AddItemToArray(rows, r);
    }
}
void sp_radio_diagnostics_json(cJSON *o) {
    cJSON_AddBoolToObject(o, "enabled", radio_ok);
    cJSON_AddNumberToObject(o, "channel", sp_radio_channel());
    cJSON_AddNumberToObject(o, "received_packets", rx_packets);
    cJSON_AddNumberToObject(o, "unknown_packets", rx_unknown);
    cJSON_AddNumberToObject(o, "header_rejections", rx_header);
    cJSON_AddNumberToObject(o, "authentication_rejections", rx_auth);
    cJSON_AddNumberToObject(o, "accepted_messages", rx_valid);
    cJSON_AddNumberToObject(o, "tx_attempts", tx_attempts);
    cJSON_AddNumberToObject(o, "tx_completed", tx_ok);
    cJSON_AddNumberToObject(o, "tx_callback_timeouts", tx_timeouts);
}
void sp_radio_health_message(sp_message_t *m) {
    sp_message_init(m, SP_HEALTH);
    sp_message_set(m, 1, 1);
    sp_message_set(m, 10, rx_valid);
    sp_message_set(m, 11, rx_auth);
    sp_message_set(m, 12, tx_timeouts);
    sp_message_set(m, 23, radio_ok);
}

bool sp_radio_network_busy(void) {
    return scan_active || network_trial;
}
