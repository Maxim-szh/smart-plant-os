#ifndef SP_SESSIONS_H
#define SP_SESSIONS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SP_TRUSTED_MAX 8u
#define SP_TEMP_MAX 8u
#define SP_BROWSER_COOKIE_SECONDS 31536000u
/* Persistent grants contain only a digest; browser secrets are never stored here. */
typedef struct {
    char id[17], name[49];
    uint8_t digest[32];
} sp_browser_grant_t;
typedef struct {
    uint32_t version;
    uint8_t owner[32];
    sp_browser_grant_t grants[SP_TRUSTED_MAX];
} sp_browser_store_t;
typedef struct {
    int (*load)(void *, void *, size_t); /* 0 missing, 1 loaded, -1 invalid/unavailable */
    bool (*save)(void *, const void *, size_t);
    void (*random)(void *, void *, size_t);
    void *ctx;
} sp_sessions_io_t;
typedef struct {
    sp_browser_store_t store;
    sp_browser_grant_t temporary[SP_TEMP_MAX];
    uint64_t until[SP_TEMP_MAX];
    sp_sessions_io_t io;
    bool persistent_ready;
} sp_sessions_t;
void sp_sessions_init(sp_sessions_t *, const char *owner, sp_sessions_io_t);
int sp_sessions_check(sp_sessions_t *, const char *secret, uint64_t now);
bool sp_sessions_issue(sp_sessions_t *, bool remember, const char *name, int replace, uint64_t now,
                       char secret[65], int *slot);
bool sp_sessions_revoke(sp_sessions_t *, unsigned slot);
bool sp_sessions_clear(sp_sessions_t *);
bool sp_sessions_rename(sp_sessions_t *, unsigned slot, const char *name);
const sp_browser_grant_t *sp_sessions_grant(const sp_sessions_t *, unsigned slot);
#endif
