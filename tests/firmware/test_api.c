#include "sp_api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static sp_api_t api;
static sp_core_t core;
static sp_message_t sent;
static cJSON *response;
static unsigned calls, radio_sends;
static int status;
static bool metadata_writable = true;
static bool pause_requested, pause_writable = true, pause_readable = true;
static int load_pause(void *ctx, const uint8_t device[6], const uint8_t plant[16]) {
    (void)ctx;
    (void)device;
    (void)plant;
    return pause_readable ? pause_requested : -1;
}
static bool save_pause(void *ctx, const uint8_t device[6], const uint8_t plant[16], bool requested) {
    (void)ctx;
    (void)device;
    (void)plant;
    if (!pause_writable)
        return false;
    pause_requested = requested;
    return true;
}
static const uint8_t command[16] = {1, 2, 3, 4, 5, 6, 0x40, 8, 0x80, 10, 11, 12, 13, 14, 15, 16};
static bool send_message(void *ctx, unsigned peer, const sp_message_t *m) {
    (void)ctx;
    assert(peer == 0);
    sent = *m;
    radio_sends++;
    calls++;
    return true;
}
static void reply_message(void *ctx, uint32_t id, int code, cJSON *o) {
    (void)ctx;
    assert(id);
    cJSON_Delete(response);
    response = o;
    status = code;
}
static bool save(void *ctx, const sp_persist_t *p) {
    (void)ctx;
    (void)p;
    return true;
}
static bool save_plant(void *ctx, const uint8_t *id, const sp_plant_meta_t *p) {
    (void)ctx;
    (void)id;
    (void)p;
    return metadata_writable;
}
static bool time_set(void *ctx, uint64_t utc) {
    (void)ctx;
    return sp_core_set_time(&core, utc, api.now);
}
static void fixture(const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", name);
    cJSON_AddItemToObject(o, "data", cJSON_Duplicate(response, true));
    char *s = cJSON_PrintUnformatted(o);
    puts(s);
    free(s);
    cJSON_Delete(o);
}
static void request(const char *method, const char *resource, const char *body) {
    sp_api_request_t r = {.id = ++calls};
    snprintf(r.method, sizeof(r.method), "%s", method);
    snprintf(r.path, sizeof(r.path), "/api/v1/nodes/spo-000000000001/%s", resource);
    if (body)
        snprintf(r.body, sizeof(r.body), "%s", body);
    status = 0;
    sp_api_handle(&api, &r);
}
static bool temperature(void *ctx, uint64_t now, sp_measurement_t *out) {
    (void)ctx;
    *out = (sp_measurement_t){.quality = SP_Q_OK, .milli_value = -12500, .sampled_ms = now};
    return true;
}
int main(void) {
    sp_sensor_registry_t sensors = {0};
    sp_sensor_descriptor_t desc = {.slot = 2,
                                   .kind = SP_SENSOR_TEMPERATURE,
                                   .unit = SP_UNIT_CELSIUS,
                                   .interval_ms = 1000,
                                   .max_age_ms = 3000};
    assert(sp_sensors_register(&sensors, &desc, temperature, NULL));
    assert(!sp_sensors_register(&sensors, &desc, temperature, NULL));
    desc.slot = 3;
    desc.unit = SP_UNIT_PERCENT;
    assert(!sp_sensors_register(&sensors, &desc, temperature, NULL));
    sp_sensors_poll(&sensors, 5000);
    assert(sensors.drivers[0].sample.milli_value == -12500);
    sp_message_t m;
    sp_sensor_message(&sensors.drivers[0], 15000, &m);
    sp_sensor_driver_t remote;
    assert(sp_sensor_from_message(&m, 10, &remote));
    assert(sp_sensor_age(&remote, 1010) == 11000);
    sp_message_set(&m, 8, UINT64_MAX);
    assert(!sp_sensor_from_message(&m, 10, &remote));
    assert(sp_core_init(&core, NULL, save, NULL, NULL, 0));
    api.count = 1;
    api.nodes[0].device[5] = 1;
    api.io = (sp_api_io_t){.send = send_message,
                           .reply = reply_message,
                           .save_plant = save_plant,
                           .set_time = time_set,
                           .load_care_pause = load_pause,
                           .save_care_pause = save_pause};
    sp_api_tick(&api, 5000, SP_MIN_EPOCH_MS + 5000);
    request("GET", "state", NULL);
    assert(status == 503);
    fixture("Error");
    sp_message_init(&m, SP_HELLO);
    sp_message_bytes(&m, SP_K_PLANT, command);
    sp_api_receive(&api, 0, 1, &m);
    sp_core_tick(&core, 5000);
    sp_node_state(&core, &m);
    sp_api_receive(&api, 0, 1, &m);
    sp_sensor_message(&sensors.drivers[0], 5000, &m);
    sp_api_receive(&api, 0, 1, &m);
    request("GET", "state", NULL);
    assert(status == 200);
    fixture("State");
    request("GET", "device", NULL);
    assert(status == 200);
    fixture("Device");
    request("GET", "config", NULL);
    assert(!status && sent.value[0] == SP_CONFIG_GET);
    assert(sp_node_dispatch(&core, &sent, &m));
    sp_api_receive(&api, 0, 1, &m);
    assert(status == 200);
    fixture("Config");
    request("POST", "commands/water",
            "{\"schema_version\":1,\"schema_version\":1,\"command_id\":\"01020304-0506-4008-800a-"
            "0b0c0d0e0f10\",\"requested_ml\":10}");
    assert(status == 400);
    request("POST", "commands/water",
            "{\"schema_version\":1,\"command_id\":\"01020304-0506-4008-800a-0b0c0d0e0f10\",\"requested_ml\":"
            "301}");
    assert(status == 422);
    request("POST", "commands/stop",
            "{\"schema_version\":1,\"command_id\":\"02020304-0506-4008-800a-0b0c0d0e0f10\",\"target_command_"
            "id\":\"01020304-0506-4008-800a-0b0c0d0e0f10\"}");
    assert(!status);
    assert(sp_node_dispatch(&core, &sent, &m));
    sp_api_receive(&api, 0, 1, &m);
    assert(status == 202);
    fixture("CommandResult");
    request("GET", "commands/01020304-0506-4008-800a-0b0c0d0e0f10", NULL);
    assert(!status);
    sp_api_timeout(&api, 0);
    assert(status == 503);
    metadata_writable = false;
    request("PUT", "plant", "{\"schema_version\":1,\"revision\":0,\"name\":\"test\",\"species_id\":null}");
    assert(status == 503);
    assert(strcmp(api.nodes[0].plant.name, "test"));
    sp_api_tick(&api, 25000, SP_MIN_EPOCH_MS + 25000);
    request("GET", "state", NULL);
    assert(status == 200);
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "availability"), "online")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "soil"), "soil_index_pct")));
    sp_message_init(&m, SP_HEALTH);
    const unsigned health_keys[] = {1, 7, 8, 9, 10, 11, 12, 14, 23};
    const uint64_t health_values[] = {1, 9, 123456, 100000, 24, 0, 0, 15000, 1};
    for (unsigned i = 0; i < 9; i++)
        sp_message_set(&m, health_keys[i], health_values[i]);
    sp_api_receive(&api, 0, 1, &m);
    assert(api.nodes[0].health.known && api.nodes[0].health.reset_reason == 9 &&
           api.nodes[0].health.uptime_ms == 15000);
    sp_message_set(&m, 7, 255);
    sp_api_receive(&api, 0, 1, &m);
    assert(api.nodes[0].health.reset_reason == 9);
    sp_message_init(&m, SP_HELLO);
    sp_api_receive(&api, 0, 2, &m);
    assert(!api.nodes[0].health.known);
    metadata_writable = true;
    const unsigned radio_calls = radio_sends;
    const sp_persist_t safety_before = core.saved;
    char profile_body[512];
    snprintf(profile_body, sizeof(profile_body),
             "{\"schema_version\":1,\"revision\":%u,\"name\":\"My "
             "tomato\",\"species_id\":\"tomato-cherry\",\"room\":\"Kitchen\",\"setup_stage\":3,\"growth_"
             "stage\":\"flower\"}",
             api.nodes[0].plant.revision);
    request("PUT", "plant", profile_body);
    assert(status == 200 && !strcmp(api.nodes[0].plant.growth_stage, "flower"));
    assert(radio_sends == radio_calls && !memcmp(&core.saved, &safety_before, sizeof(safety_before)));
    request("PUT", "plant", profile_body);
    assert(status == 409);
    snprintf(profile_body, sizeof(profile_body),
             "{\"schema_version\":1,\"revision\":%u,\"name\":\"Monstera\",\"species_id\":\"monstera\","
             "\"room\":\"Kitchen\",\"setup_stage\":3,\"growth_stage\":\"fruit\"}",
             api.nodes[0].plant.revision);
    request("PUT", "plant", profile_body);
    assert(status == 422 && !strcmp(api.nodes[0].plant.growth_stage, "flower"));
    snprintf(profile_body, sizeof(profile_body),
             "{\"schema_version\":1,\"revision\":%u,\"name\":\"Monstera\",\"species_id\":\"monstera\"}",
             api.nodes[0].plant.revision);
    request("PUT", "plant", profile_body);
    assert(status == 200 && !api.nodes[0].plant.growth_stage[0]);
    assert(radio_sends == radio_calls && !memcmp(&core.saved, &safety_before, sizeof(safety_before)));
    /* Care service is a persistent intent plus an acknowledged manual CONFIG, never a water command. */
    assert(sp_core_init(&core, NULL, save, NULL, NULL, 30000));
    sp_config_t cfg = core.saved.config;
    cfg.dry_raw = 2900;
    cfg.wet_raw = 1300;
    cfg.flow_ul_s = 8000;
    cfg.automatic = true;
    assert(sp_core_configure(&core, &cfg, core.saved.config.revision));
    sp_api_tick(&api, 30000, SP_MIN_EPOCH_MS + 30000);
    sp_node_state(&core, &m);
    sp_api_receive(&api, 0, 2, &m);
    request("GET", "config", NULL);
    assert(sp_node_dispatch(&core, &sent, &m));
    sp_api_receive(&api, 0, 2, &m);
    assert(status == 200);
    const unsigned sends_before_pause = radio_sends;
    request("GET", "care-service", NULL);
    assert(status == 200 &&
           !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(response, "phase")), "inactive"));
    request("POST", "care-service", "{\"schema_version\":1,\"action\":\"start\",\"revision\":4294967295}");
    assert(status == 409 && radio_sends == sends_before_pause);
    char pause_body[160];
    snprintf(pause_body, sizeof(pause_body), "{\"schema_version\":1,\"action\":\"start\",\"revision\":%u}",
             api.nodes[0].config.revision);
    pause_writable = false;
    request("POST", "care-service", pause_body);
    assert(status == 503 && !pause_requested && radio_sends == sends_before_pause);
    pause_writable = true;
    request("POST", "care-service", pause_body);
    assert(!status && pause_requested && sent.value[0] == SP_CONFIG && !sent.value[3]);
    assert(api.nodes[0].proposed.dry_raw == cfg.dry_raw && api.nodes[0].proposed.flow_ul_s == cfg.flow_ul_s);
    assert(sp_node_dispatch(&core, &sent, &m));
    sp_api_receive(&api, 0, 2, &m);
    assert(status == 200 && !core.saved.config.automatic);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(response, "phase")), "active"));
    request("GET", "care-service", NULL);
    assert(status == 200 &&
           !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(response, "phase")), "unverified"));
    sp_node_state(&core, &m);
    sp_api_receive(&api, 0, 2, &m);
    request("GET", "config", NULL);
    assert(sp_node_dispatch(&core, &sent, &m));
    sp_api_receive(&api, 0, 2, &m);
    cJSON *automatic_again = cJSON_Duplicate(response, true);
    cJSON_ReplaceItemInObject(automatic_again, "mode", cJSON_CreateString("automatic_threshold"));
    char *automatic_body = cJSON_PrintUnformatted(automatic_again);
    sp_api_request_t blocked_config = {
        .id = ++calls, .method = "PUT", .path = "/api/v1/nodes/spo-000000000001/config"};
    snprintf(blocked_config.body, sizeof(blocked_config.body), "%s", automatic_body);
    snprintf(blocked_config.if_match, sizeof(blocked_config.if_match), "%u", api.nodes[0].config.revision);
    unsigned sends_before_block = radio_sends;
    sp_api_handle(&api, &blocked_config);
    assert(status == 409 && radio_sends == sends_before_block && !core.saved.config.automatic);
    free(automatic_body);
    cJSON_Delete(automatic_again);
    const unsigned paused_sends = radio_sends;
    snprintf(pause_body, sizeof(pause_body), "{\"schema_version\":1,\"action\":\"finish\",\"revision\":%u}",
             api.nodes[0].config.revision);
    request("POST", "care-service", pause_body);
    assert(status == 200 && !pause_requested && !core.saved.config.automatic && radio_sends == paused_sends);
    pause_requested = true;
    pause_readable = false;
    request("GET", "care-service", NULL);
    assert(status == 503);
    pause_readable = true;
    sp_api_tick(&api, 60000, SP_MIN_EPOCH_MS + 60000);
    request("GET", "care-service", NULL);
    assert(status == 200 &&
           !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(response, "phase")), "unverified"));
    request("POST", "care-service", pause_body);
    assert(status == 503 && pause_requested && !core.saved.config.automatic);
    cJSON_Delete(response);
    return 0;
}
