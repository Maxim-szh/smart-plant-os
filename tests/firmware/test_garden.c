#include "sp_diary.h"
#include "sp_measurements.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char diary[20000];
static size_t diary_size;
static bool fail;
static int status;
static cJSON *reply;
static struct {
    char key[12];
    unsigned char data[1200];
    size_t n;
} slots[62];
static unsigned writes;
static int dload(void *c, void *out, size_t n) {
    (void)c;
    assert(n < sizeof(diary));
    if (!diary_size)
        return 0;
    assert(n == diary_size);
    memcpy(out, diary, n);
    return 1;
}
static bool dsave(void *c, const void *in, size_t n) {
    (void)c;
    if (fail)
        return false;
    memcpy(diary, in, n);
    diary_size = n;
    return true;
}
static int hload(void *c, const char *key, void *out, size_t n) {
    (void)c;
    for (unsigned i = 0; i < 62; i++)
        if (!strcmp(slots[i].key, key)) {
            assert(slots[i].n == n);
            memcpy(out, slots[i].data, n);
            return 1;
        }
    return 0;
}
static bool hsave(void *c, const char *key, const void *in, size_t n) {
    (void)c;
    if (fail)
        return false;
    unsigned slot = 62;
    for (unsigned i = 0; i < 62; i++)
        if (!strcmp(slots[i].key, key)) {
            slot = i;
            break;
        }
    if (slot == 62)
        for (unsigned i = 0; i < 62; i++)
            if (!slots[i].key[0]) {
                slot = i;
                break;
            }
    assert(slot < 62 && n <= sizeof(slots[slot].data));
    snprintf(slots[slot].key, 12, "%s", key);
    memcpy(slots[slot].data, in, n);
    slots[slot].n = n;
    writes++;
    return true;
}
static bool exists(void *c, const uint8_t p[16]) {
    (void)c;
    return p[0] == 1;
}
void sp_web_reply(void *c, uint32_t id, int s, cJSON *o) {
    (void)c;
    (void)id;
    cJSON_Delete(reply);
    reply = o;
    status = s;
}
static void diary_request(const char *method, const char *suffix, const char *body) {
    sp_api_request_t q = {0};
    snprintf(q.method, sizeof(q.method), "%s", method);
    snprintf(q.path, sizeof(q.path), "/api/v1/plants/01020304-0506-4008-800a-0b0c0d0e0f10/diary%s", suffix);
    if (body)
        snprintf(q.body, sizeof(q.body), "%s", body);
    assert(sp_diary_handle(&q));
}
static void history_request(sp_api_t *a, const char *window) {
    sp_api_request_t q = {.method = "GET"};
    snprintf(q.path, sizeof(q.path),
             "/api/v1/plants/01020304-0506-4008-800a-0b0c0d0e0f10/measurements?period=%s&metric=soil_index",
             window);
    assert(sp_measurements_handle(a, &q));
    assert(status == 200);
}
int main(void) {
    sp_diary_io_t dio = {.load = dload, .save = dsave, .plant_exists = exists};
    assert(sp_diary_start(dio));
    const char *entry =
        "{\"schema_version\":1,\"entry_id\":\"11020304-0506-4008-800a-0b0c0d0e0f10\",\"kind\":\"reminder\","
        "\"note\":\"Check plant\",\"at_ms\":1704067200000,\"remind_at_ms\":1704153600000}";
    diary_request("POST", "", entry);
    assert(status == 200 && diary_size);
    diary_request("POST", "", entry);
    assert(status == 200);
    assert(sp_diary_start(dio));
    diary_request("GET", "?after=0&limit=32", NULL);
    assert(status == 200 && cJSON_GetArraySize(cJSON_GetObjectItem(reply, "entries")) == 1);
    sp_api_request_t all_diary = {.method = "GET", .path = "/api/v1/diary?after=0&limit=32"};
    unsigned reads_before = writes;
    assert(sp_diary_handle(&all_diary));
    assert(status == 200 && cJSON_GetArraySize(cJSON_GetObjectItem(reply, "entries")) == 1 &&
           writes == reads_before);
    snprintf(all_diary.method, sizeof(all_diary.method), "POST");
    assert(sp_diary_handle(&all_diary) && status == 405 && writes == reads_before);
    fail = true;
    diary_request("PUT", "/11020304-0506-4008-800a-0b0c0d0e0f10", "{\"revision\":0,\"done\":true}");
    assert(status == 503);
    fail = false;
    diary_request("PUT", "/11020304-0506-4008-800a-0b0c0d0e0f10", "{\"revision\":0,\"done\":true}");
    assert(status == 200 && cJSON_IsTrue(cJSON_GetObjectItem(reply, "done")));
    diary_request("PUT", "/11020304-0506-4008-800a-0b0c0d0e0f10", "{\"revision\":0,\"done\":false}");
    assert(status == 409);
    diary_request("DELETE", "/11020304-0506-4008-800a-0b0c0d0e0f10", "{\"revision\":1}");
    assert(status == 200);
    diary_request("GET", "", NULL);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply, "entries")) == 0);
    sp_measurements_io_t hio = {.load = hload, .save = hsave, .plant_exists = exists};
    sp_measurements_start(hio);
    sp_api_t *a = calloc(1, sizeof(*a));
    a->count = 1;
    sp_api_node_t *n = &a->nodes[0];
    n->have_plant = true;
    n->hardware_known = true;
    n->hardware_flags = 1;
    n->registry.have_state = true;
    assert(sp_api_parse_uuid("01020304-0506-4008-800a-0b0c0d0e0f10", n->plant.plant));
    n->device[0] = 2;
    history_request(a, "day");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "clock_state")), "unset"));
    assert(!writes);
    a->utc = SP_MIN_EPOCH_MS;
    a->now = 1000;
    n->registry.received_ms = a->now;
    sp_message_set(&n->registry.state, SP_K_QUALITY, SP_Q_OK);
    sp_message_set(&n->registry.state, SP_K_INDEX, 40);
    sp_measurements_tick(a);
    assert(writes == 1);
    sp_measurements_tick(a);
    assert(writes == 1);
    a->utc += 60000;
    a->now += 60000;
    n->registry.received_ms = a->now;
    sp_message_set(&n->registry.state, SP_K_QUALITY, SP_Q_FAULT);
    sp_measurements_tick(a);
    assert(writes == 1);
    history_request(a, "day");
    cJSON *current = cJSON_GetObjectItem(reply, "current");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "samples")) == 1 &&
           cJSON_GetNumberValue(cJSON_GetObjectItem(current, "mean")) == 40);
    a->utc += 15 * 60000;
    a->now += 15 * 60000;
    n->registry.received_ms = a->now;
    sp_message_set(&n->registry.state, SP_K_QUALITY, SP_Q_OK);
    sp_message_set(&n->registry.state, SP_K_INDEX, 60);
    sp_measurements_tick(a);
    assert(writes == 2);
    sp_measurements_start(hio);
    sp_measurements_tick(a);
    history_request(a, "day");
    current = cJSON_GetObjectItem(reply, "current");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "samples")) == 2 &&
           cJSON_GetNumberValue(cJSON_GetObjectItem(current, "mean")) == 50);
    a->utc += 86400000;
    a->now += 86400000;
    n->registry.received_ms = a->now;
    sp_measurements_tick(a);
    history_request(a, "week");
    current = cJSON_GetObjectItem(reply, "current");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "samples")) >= 3);
    a->utc = SP_MIN_EPOCH_MS;
    history_request(a, "day");
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(reply, "clock_state")), "rollback"));
    cJSON_Delete(reply);
    free(a);
    puts("PASS: durable/idempotent diary, reminder completion/conflict/failed commit, bounded minute "
         "sampling, checkpoint/reboot, missing values and day/week history");
}
