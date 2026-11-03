#ifndef SP_SYSTEM_H
#define SP_SYSTEM_H
#include "sp_internal.h"
#include "sp_api.h"
typedef struct {
    uint64_t (*now_ms)(void);
    uint32_t (*free_heap)(void), (*min_heap)(void);
    unsigned (*reset_reason)(void);
    bool (*idle)(void *);
    void (*network_json)(cJSON *), (*radio_json)(cJSON *);
    cJSON *(*update_json)(void);
    esp_err_t (*scan_begin)(void);
    void (*scan_json)(cJSON *);
    bool (*recovery_open)(void);
} sp_system_io_t;
bool sp_system_handle(sp_identity_t *, sp_api_t *, const sp_api_request_t *, const sp_system_io_t *);
const char *sp_system_reset_name(unsigned);
#endif
