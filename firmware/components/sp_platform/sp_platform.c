#include "sp_platform.h"
#include "sp_internal.h"
#include "sp_service.h"
#include "sp_web.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static sp_core_t core;
static sp_identity_t identity;
static nvs_handle_t journal;
static sp_registry_entry_t registry[SP_PEER_MAX];
static uint32_t observed_boot[SP_PEER_MAX];
static sp_ram_history_t *history;
static sp_history_store_t history_store;
static uint64_t event_cursor[SP_PEER_MAX];
static uint64_t hub_utc, hub_mono;
static sp_api_t *api;
static sp_api_request_t *web_request;
static sp_sensor_registry_t sensors;
typedef struct {
    bool active;
    sp_message_t request;
    unsigned attempts;
    uint64_t due;
} pending_t;
static pending_t pending[SP_PEER_MAX];
static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000;
}
static uint64_t utc_ms(uint64_t now) {
    return hub_utc ? hub_utc + now - hub_mono : 0;
}
static bool api_send(void *ctx, unsigned peer, const sp_message_t *m) {
    (void)ctx;
    bool stop = m->value[0] == SP_COMMAND && m->value[3] == SP_STOP;
    if (peer >= identity.count || (pending[peer].active && !stop))
        return false;
    pending[peer] = (pending_t){.active = true, .request = *m, .due = now_ms()};
    return true;
}
static bool api_time(void *ctx, uint64_t value) {
    (void)ctx;
    uint64_t now = now_ms(), expected = utc_ms(now);
    if (value < SP_MIN_EPOCH_MS || value > SP_MAX_EPOCH_MS ||
        (expected && (value > expected ? value - expected : expected - value) > 5000))
        return false;
    hub_utc = value < expected ? expected : value;
    hub_mono = now;
    return true;
}
static bool api_save_plant(void *ctx, const uint8_t device[6], const sp_plant_meta_t *meta) {
    (void)ctx;
    char id[17];
    sp_api_device_id(device, id);
    nvs_handle_t h;
    if (nvs_open("plants", NVS_READWRITE, &h) != ESP_OK)
        return false;
    bool ok = nvs_set_blob(h, id + 4, meta, sizeof(*meta)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
static bool save_core(void *ctx, const sp_persist_t *p) {
    (void)ctx;
    return nvs_set_blob(journal, "state_v1", p, sizeof(*p)) == ESP_OK && nvs_commit(journal) == ESP_OK;
}
static void print_message(unsigned peer, const sp_message_t *m) {
    /* USB diagnostics are not /api/v1 JSON. No secret material is printed. */
    uint8_t buf[SP_CBOR_MAX];
    size_t len;
    if (!sp_cbor_encode(m, buf, sizeof(buf), &len))
        return;
    printf("SPP peer=%u cbor=", peer);
    for (size_t i = 0; i < len; i++)
        printf("%02x", buf[i]);
    puts("");
}
static void hello(unsigned peer) {
    sp_message_t m;
    sp_message_init(&m, SP_HELLO);
    sp_message_bytes(&m, SP_K_DEVICE, identity.self);
    sp_message_bytes(&m, SP_K_PLANT, identity.plant);
    sp_message_set(&m, SP_K_FLAGS, identity.hub ? 0 : 7); /* soil, tank, pump */
    (void)sp_radio_send(peer, &m);
}
static bool hex_id(const char *text, uint8_t out[16]) {
    if (strlen(text) != 32)
        return false;
    for (size_t i = 0; i < 16; i++) {
        unsigned v = 0;
        for (unsigned j = 0; j < 2; j++) {
            char ch = text[i * 2 + j];
            unsigned n = ch >= '0' && ch <= '9'   ? (unsigned)(ch - '0')
                         : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                                                  : 16;
            if (n > 15)
                return false;
            v = (v << 4) | n;
        }
        out[i] = (uint8_t)v;
    }
    return (out[6] & 0xf0) == 0x40 && (out[8] & 0xc0) == 0x80;
}
static bool number(const char *s, uint64_t *v) {
    if (!s || !*s || strlen(s) > 19)
        return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    char *end;
    *v = strtoull(s, &end, 10);
    return !*end;
}
static void submit(unsigned peer, const sp_message_t *m) {
    if (!identity.hub) {
        sp_message_t out;
        if (sp_node_dispatch(&core, m, &out))
            print_message(0, &out);
        else
            puts("ERR invalid_message");
        return;
    }
    if (peer >= identity.count || !sp_registry_online(&registry[peer], now_ms())) {
        puts("ERR node_offline");
        return;
    }
    if (pending[peer].active) {
        puts("ERR request_pending; query its ID before issuing another water command");
        return;
    }
    pending[peer] = (pending_t){.active = true, .request = *m, .due = now_ms()};
    puts("QUEUED locally; success requires Node response");
}
static void command(char *line) {
    char *args[16], *save = NULL;
    unsigned argc = 0;
    for (char *p = strtok_r(line, " \r\t", &save); p; p = strtok_r(NULL, " \r\t", &save)) {
        if (argc == 16) {
            puts("ERR too_many_arguments");
            return;
        }
        args[argc++] = p;
    }
    if (!argc)
        return;
    if (argc == 1 && !strcmp(args[0], "status")) {
        if (identity.hub)
            for (unsigned i = 0; i < identity.count; i++) {
                printf("peer=%u online=%u age_ms=%" PRIu64 "\n", i,
                       sp_registry_online(&registry[i], now_ms()), now_ms() - registry[i].received_ms);
                if (registry[i].have_state)
                    print_message(i, &registry[i].state);
            }
        else {
            sp_message_t m;
            sp_node_state(&core, &m);
            print_message(0, &m);
            sp_config_message(&core.saved.config, &m);
            print_message(0, &m);
        }
        return;
    }
    uint64_t value;
    if (argc == 2 && !strcmp(args[0], "time") && number(args[1], &value)) {
        bool ok = value >= SP_MIN_EPOCH_MS && value <= SP_MAX_EPOCH_MS;
        if (identity.hub && ok) {
            uint64_t now = now_ms(), expected = utc_ms(now);
            if (expected && (value > expected ? value - expected : expected - value) > 5000)
                ok = false;
            if (ok) {
                hub_utc = value < expected ? expected : value;
                hub_mono = now;
            }
        } else if (ok)
            ok = sp_core_set_time(&core, value, now_ms());
        puts(ok ? "OK time" : "ERR time");
        return;
    }
    if (argc == 1 && !strcmp(args[0], "ack-fault") && !identity.hub) {
        puts(sp_core_acknowledge_fault(&core) ? "OK fault_acknowledged; limits retained"
                                              : "ERR fault_acknowledgement");
        return;
    }
    uint64_t peer;
    if (argc < 3 || !number(args[1], &peer) || peer >= identity.count)
        goto usage;
    sp_message_t m;
    if ((argc == 4 && !strcmp(args[0], "water")) || (argc == 4 && !strcmp(args[0], "stop")) ||
        (argc == 3 && !strcmp(args[0], "query"))) {
        bool water = !strcmp(args[0], "water"), query = !strcmp(args[0], "query");
        uint8_t id[16];
        sp_message_init(&m, query ? SP_QUERY : SP_COMMAND);
        if (!hex_id(args[2], id))
            goto usage;
        sp_message_bytes(&m, SP_K_ID, id);
        if (!query)
            sp_message_set(&m, SP_K_ACTION, water ? SP_WATER : SP_STOP);
        if (water) {
            if (!number(args[3], &value) || value < 10 || value > 300)
                goto usage;
            uint64_t utc = identity.hub ? utc_ms(now_ms()) : sp_core_utc(&core, now_ms());
            if (!utc) {
                puts("ERR clock_unset");
                return;
            }
            sp_message_set(&m, SP_K_ML, value);
            sp_message_set(&m, SP_K_DEADLINE, utc + 10000);
        } else if (!query) {
            if (!hex_id(args[3], id))
                goto usage;
            sp_message_bytes(&m, SP_K_TARGET, id);
        }
        /* A stop is allowed to replace an unanswered request; it names the exact water ID. */
        if (!water && !query)
            pending[peer].active = false;
        submit((unsigned)peer, &m);
        return;
    }
    if (argc == 13 && !strcmp(args[0], "config")) {
        /* expected revision, dry, wet, ul/s, auto, threshold, portion, daily, maxrun, interval, maxage */
        uint64_t v[11];
        for (unsigned i = 0; i < 11; i++)
            if (!number(args[i + 2], &v[i]))
                goto usage;
        sp_message_init(&m, SP_CONFIG);
        uint8_t id[16];
        esp_fill_random(id, 16);
        id[6] = (id[6] & 15) | 0x40;
        id[8] = (id[8] & 63) | 0x80;
        sp_message_bytes(&m, SP_K_ID, id);
        const unsigned keys[] = {1, 8, 9, 19, 3, 10, 4, 11, 12, 13, 14};
        for (unsigned i = 0; i < 11; i++)
            sp_message_set(&m, keys[i], v[i]);
        submit((unsigned)peer, &m);
        return;
    }
usage:
    puts("status | time <UTC-ms> | water <peer> <UUIDv4-32hex> <ml> | stop <peer> <new-UUID> <water-UUID> | "
         "query <peer> <UUID>");
    puts("config <peer> <expected-rev> <dry-ADC> <wet-ADC> <flow-ul/s> <auto-0/1> <threshold> <portion-ml> "
         "<daily-ml> <maxrun-s> <interval-min> <maxage-s>");
    puts("Node USB only: ack-fault (after physical inspection; never resets water budget)");
}
static void poll_console(void) {
    static char line[320];
    static size_t len;
    static bool overflow;
    static uint64_t allowed_at;
    char ch;
    for (unsigned i = 0; i < 64 && read(STDIN_FILENO, &ch, 1) == 1; i++) {
        if (ch == '\n' || ch == '\r') {
            if (len || overflow) {
                line[len] = 0;
                if (overflow)
                    puts("ERR line_too_long");
                else if (now_ms() < allowed_at)
                    puts("ERR rate_limit");
                else {
                    command(line);
                    allowed_at = now_ms() + 100;
                }
            }
            len = 0;
            overflow = false;
        } else if (!overflow) {
            if (len + 1 < sizeof(line))
                line[len++] = ch;
            else
                overflow = true;
        }
    }
}
static void hub_receive(const sp_received_t *r, uint64_t now) {
    const sp_message_t *m = &r->message;
    unsigned i = r->peer;
    if (api)
        sp_api_receive(api, i, identity.peers[i].boot, m);
    if (m->value[0] == SP_CONFIG && pending[i].active && pending[i].request.value[0] == SP_CONFIG_GET)
        pending[i].active = false;
    if (observed_boot[i] != identity.peers[i].boot) {
        registry[i].have_state = false; /* A known reboot invalidates the previous snapshot immediately. */
        observed_boot[i] = identity.peers[i].boot;
    }
    if (m->value[0] == SP_STATE) {
        (void)sp_registry_accept(&registry[i], m, now);
        return;
    }
    if (m->value[0] == SP_RESULT || m->value[0] == SP_CONFIG_RESULT) {
        print_message(i, m);
        pending_t *p = &pending[i];
        if (p->active && ((m->value[0] == SP_RESULT && sp_message_has(m, SP_K_ID) &&
                           !memcmp(m->bytes[0], p->request.bytes[0], 16)) ||
                          (m->value[0] == SP_CONFIG_RESULT && p->request.value[0] == SP_CONFIG &&
                           sp_message_has(m, SP_K_ID) && !memcmp(m->bytes[0], p->request.bytes[0], 16))))
            p->active = false;
    }
    if (m->value[0] == SP_EVENT && sp_message_has(m, SP_K_FLAGS) && sp_message_has(m, SP_K_SEQUENCE)) {
        uint64_t seq = m->value[SP_K_SEQUENCE];
        if (m->value[SP_K_FLAGS] == 1) {
            printf("history_gap peer=%u after=%" PRIu64 " available_through=%" PRIu64 "\n", i,
                   event_cursor[i], seq);
            event_cursor[i] = seq >= SP_EVENT_SLOTS ? seq - SP_EVENT_SLOTS : 0;
        } else if (!m->value[SP_K_FLAGS] && seq == event_cursor[i] + 1 && sp_message_has(m, 24) &&
                   sp_message_has(m, 2) && sp_message_has(m, 3) && sp_message_has(m, 6) &&
                   sp_message_has(m, 7) && sp_message_has(m, 14) && sp_message_has(m, 20) &&
                   m->value[20] <= 2 && m->value[3] >= 1 && m->value[3] <= 5 && m->value[6] <= SP_REJECTED &&
                   m->value[7] < SP_REASON_COUNT) {
            sp_history_record_t record = {.event = {.seq = seq,
                                                    .utc_ms = m->value[2],
                                                    .uptime_ms = m->value[14],
                                                    .source = m->value[20],
                                                    .kind = m->value[3],
                                                    .status = m->value[6],
                                                    .reason = m->value[7]}};
            memcpy(record.device, identity.peers[i].device, 6);
            memcpy(record.event.command_id, m->bytes[0], 16);
            if (history_store.append && history_store.append(history_store.ctx, &record))
                event_cursor[i] = seq;
        }
    }
}
void sp_platform_run(bool hub) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    printf("Smart Plant %s 0.1.0 engineering; STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n", hub ? "Hub" : "Node",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    /* Never erase NVS automatically: doing so would reset dedupe, limits and AEAD epochs. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK || !sp_identity_load(&identity, hub)) {
        puts("LOCKED: physical provisioning required or NVS invalid; no pump/no radio");
        return;
    }
    if (!hub) {
        nvs_handle_t settings;
        bool hardware = nvs_open("provision", NVS_READONLY, &settings) == ESP_OK;
        if (hardware) {
            hardware = sp_board_start(settings);
            nvs_close(settings);
        }
        if (nvs_open("safety", NVS_READWRITE, &journal) != ESP_OK) {
            puts("LOCKED: safety storage");
            return;
        }
        sp_persist_t *loaded = malloc(sizeof(*loaded));
        if (!loaded) {
            puts("LOCKED: boot memory");
            return;
        }
        size_t len = sizeof(*loaded);
        err = nvs_get_blob(journal, "state_v1", loaded, &len);
        if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || (err == ESP_OK && len != sizeof(*loaded))) {
            free(loaded);
            puts("LOCKED: journal format; do not erase limits");
            return;
        }
        bool restored =
            sp_core_init(&core, err == ESP_OK ? loaded : NULL, save_core, sp_board_pump, NULL, now_ms());
        free(loaded);
        if (!restored) {
            puts("LOCKED: journal integrity");
            return;
        }
        core.hardware_ready = hardware;
        sp_board_register_sensors(&sensors, &core);
    }
    if (hub) {
        history = calloc(1, sizeof(*history));
        if (history)
            history_store = sp_ram_history_store(history);
        else
            puts("history_unavailable; commands and registry remain available");
    }
    err = sp_radio_start(&identity);
    if (err != ESP_OK)
        printf("radio_unavailable error=%d; local safety remains active\n", err);
    if (hub && err == ESP_OK) {
        api = calloc(1, sizeof(*api));
        web_request = malloc(sizeof(*web_request));
        if (api) {
            api->count = identity.count;
            api->history = history_store;
            api->io = (sp_api_io_t){
                .send = api_send, .reply = sp_web_reply, .save_plant = api_save_plant, .set_time = api_time};
            for (unsigned i = 0; i < identity.count; i++) {
                memcpy(api->nodes[i].device, identity.peers[i].device, 6);
                char id[17];
                sp_api_device_id(identity.peers[i].device, id);
                nvs_handle_t h;
                if (nvs_open("plants", NVS_READONLY, &h) == ESP_OK) {
                    sp_plant_meta_t meta;
                    size_t size = sizeof(meta);
                    if (nvs_get_blob(h, id + 4, &meta, &size) == ESP_OK && size == sizeof(meta) &&
                        meta.version == 1 && memchr(meta.name, 0, sizeof(meta.name)) &&
                        memchr(meta.species, 0, sizeof(meta.species)))
                        api->nodes[i].plant = meta;
                    nvs_close(h);
                }
            }
            puts(sp_web_start() ? "Web: https://192.168.4.1 (physical credentials required)"
                                : "Web unavailable: provision web credentials/certificate first");
        }
    }
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint64_t periodic_at = 0, last_command_at = 0, sensor_at = 0;
    unsigned sensor_slot = 0;
    unsigned periodic_peer = 0;
    for (;;) {
        uint64_t now = now_ms();
        if (api) {
            sp_api_tick(api, now, utc_ms(now));
            if (web_request && sp_web_take(web_request))
                sp_api_handle(api, web_request);
        }
        if (!hub) {
            sp_sensors_poll(&sensors, now);
            core.hardware_ready = sp_board_healthy();
            sp_core_tick(&core, now);
            if (sp_core_auto_due(&core)) {
                sp_request_t req = {.action = SP_WATER,
                                    .requested_ml = core.saved.config.portion_ml,
                                    .expires_ms = sp_core_utc(&core, now) + 10000};
                esp_fill_random(req.id, 16);
                req.id[6] = (req.id[6] & 15) | 0x40;
                req.id[8] = (req.id[8] & 63) | 0x80;
                sp_result_t result;
                (void)sp_core_command(&core, &req, true, &result);
            }
        }
        sp_received_t r;
        if (sp_radio_receive(&r)) {
            if (hub)
                hub_receive(&r, now);
            else if (r.message.value[0] != SP_HELLO) {
                sp_message_t response;
                bool mutating = r.message.value[0] == SP_COMMAND || r.message.value[0] == SP_CONFIG;
                bool stop = r.message.value[0] == SP_COMMAND && r.message.value[3] == SP_STOP;
                if (!mutating || stop || now >= last_command_at + 250) {
                    if (mutating)
                        last_command_at = now;
                    if (sp_node_dispatch(&core, &r.message, &response))
                        (void)sp_radio_send(0, &response);
                }
            }
        }
        poll_console();
        if (!hub && sensors.count && now >= sensor_at) {
            sp_message_t m;
            sp_sensor_message(&sensors.drivers[sensor_slot], now, &m);
            (void)sp_radio_send(0, &m);
            sensor_slot = (sensor_slot + 1) % sensors.count;
            sensor_at = now + 500;
        }
        if (now >= periodic_at) {
            unsigned i = periodic_peer;
            hello(i);
            sp_message_t m;
            if (hub) {
                if (hub_utc) {
                    sp_message_init(&m, SP_TIME);
                    sp_message_set(&m, SP_K_TIME, utc_ms(now_ms()));
                    (void)sp_radio_send(i, &m);
                }
                if (history_store.append && sp_registry_online(&registry[i], now)) {
                    sp_message_init(&m, SP_EVENT_QUERY);
                    sp_message_set(&m, SP_K_SEQUENCE, event_cursor[i]);
                    (void)sp_radio_send(i, &m);
                }
            } else {
                sp_node_state(&core, &m);
                (void)sp_radio_send(0, &m);
            }
            periodic_peer = (i + 1) % identity.count;
            periodic_at = now + SP_TELEMETRY_MS / identity.count;
        }
        if (hub)
            for (unsigned i = 0; i < identity.count; i++) {
                pending_t *p = &pending[i];
                if (!p->active || now < p->due)
                    continue;
                if (!sp_registry_online(&registry[i], now) || p->attempts >= SP_RPC_ATTEMPTS) {
                    printf("request_outcome_unknown peer=%u; query the SAME command ID; no new water ID\n",
                           i);
                    p->active = false;
                    if (api)
                        sp_api_timeout(api, i);
                    continue;
                }
                (void)sp_radio_send(i, &p->request);
                p->attempts++;
                p->due = now + SP_RPC_TIMEOUT_MS;
                break; /* Bound time spent on radio per control iteration. */
            }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
