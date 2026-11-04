#ifndef SP_DIARY_H
#define SP_DIARY_H
#include "sp_api.h"
#define SP_DIARY_MAX 64u
/* Manual records only; no GPIO or Node configuration access. */
typedef struct {
    int (*load)(void *, void *, size_t);
    bool (*save)(void *, const void *, size_t);
    bool (*plant_exists)(void *, const uint8_t plant[16]);
    void *ctx;
} sp_diary_io_t;
bool sp_diary_start(sp_diary_io_t);
bool sp_diary_handle(const sp_api_request_t *);
#endif
