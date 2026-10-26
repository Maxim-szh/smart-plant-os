#include "sp_api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static sp_api_t api;
static sp_core_t core;
static sp_message_t sent;
static cJSON *response;
static unsigned calls;
static int status;
static const uint8_t command[16] = {1, 2, 3, 4, 5, 6, 0x40, 8, 0x80, 10, 11, 12, 13, 14, 15, 16};
static bool send_message(void *ctx, unsigned peer, const sp_message_t *m) {
    (void)ctx;
    assert(peer == 0);
    sent = *m;
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
    return false;
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
    api.io = (sp_api_io_t){
        .send = send_message, .reply = reply_message, .save_plant = save_plant, .set_time = time_set};
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
    request("PUT", "plant", "{\"schema_version\":1,\"revision\":0,\"name\":\"test\",\"species_id\":null}");
    assert(status == 503);
    assert(strcmp(api.nodes[0].plant.name, "test"));
    sp_api_tick(&api, 25000, SP_MIN_EPOCH_MS + 25000);
    request("GET", "state", NULL);
    assert(status == 503);
    cJSON_Delete(response);
    return 0;
}
