#ifndef SP_API_H
#define SP_API_H
#include "sp_service.h"
#include "sp_sensors.h"
#include "cJSON.h"

#define SP_API_BODY_MAX 2048u
#define SP_API_REPLY_MAX 16384u
typedef struct {
    uint32_t id;
    char method[8], path[180], if_match[16], body[SP_API_BODY_MAX + 1];
} sp_api_request_t;
typedef struct {
    uint32_t version, revision;
    uint8_t plant[16];
    char name[81], species[41];
} sp_plant_meta_t;
typedef struct {
    uint8_t device[6];
    sp_plant_meta_t plant;
    bool have_plant, have_config;
    uint32_t boot;
    sp_registry_entry_t registry;
    sp_config_t config;
    sp_sensor_driver_t sensors[SP_SENSOR_MAX];
    bool have_sensor[SP_SENSOR_MAX];
    uint32_t request_id;
    unsigned request_kind;
    sp_message_t request;
    sp_config_t proposed;
} sp_api_node_t;
typedef struct {
    void *ctx;
    bool (*send)(void *, unsigned peer, const sp_message_t *);
    void (*reply)(void *, uint32_t request, int status, cJSON *owned_json);
    bool (*save_plant)(void *, const uint8_t device[6], const sp_plant_meta_t *);
    bool (*set_time)(void *, uint64_t utc_ms);
} sp_api_io_t;
typedef struct {
    unsigned count;
    sp_api_node_t nodes[SP_PEER_MAX];
    sp_api_io_t io;
    sp_history_store_t history;
    uint64_t now, utc;
} sp_api_t;
void sp_api_handle(sp_api_t *, const sp_api_request_t *);
void sp_api_receive(sp_api_t *, unsigned peer, uint32_t boot, const sp_message_t *);
void sp_api_timeout(sp_api_t *, unsigned peer);
void sp_api_tick(sp_api_t *, uint64_t now, uint64_t utc);
cJSON *sp_api_error(const char *code, const char *message);
void sp_api_device_id(const uint8_t id[6], char text[17]);
void sp_api_uuid(const uint8_t id[16], char text[37]);
bool sp_api_parse_uuid(const char *, uint8_t out[16]);
#endif
