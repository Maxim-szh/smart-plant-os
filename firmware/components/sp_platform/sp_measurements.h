#ifndef SP_MEASUREMENTS_H
#define SP_MEASUREMENTS_H
#include "sp_api.h"
typedef struct {
    int (*load)(void *, const char *, void *, size_t);
    bool (*save)(void *, const char *, const void *, size_t);
    bool (*plant_exists)(void *, const uint8_t[16]);
    void *ctx;
} sp_measurements_io_t;
void sp_measurements_start(sp_measurements_io_t);
void sp_measurements_tick(const sp_api_t *);
bool sp_measurements_handle(const sp_api_t *, const sp_api_request_t *);
#endif
