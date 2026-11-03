#include "sp_sessions.h"
#include "sp_text.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
static bool digest(const char *text, uint8_t out[32]) {
    char input[96];
    int n = snprintf(input, sizeof(input), "smartplant-browser-v1:%s", text);
    return n > 0 && (size_t)n < sizeof(input) &&
           mbedtls_sha256((const unsigned char *)input, (size_t)n, out, 0) == 0;
}
static bool same(const uint8_t *a, const uint8_t *b) {
    unsigned diff = 0;
    for (unsigned i = 0; i < 32; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
static bool hex(const char *s, unsigned size) {
    if (!s || strlen(s) != size)
        return false;
    for (unsigned i = 0; i < size; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
static void random_hex(sp_sessions_t *s, char *out, unsigned bytes) {
    uint8_t raw[32];
    s->io.random(s->io.ctx, raw, bytes);
    for (unsigned i = 0; i < bytes; i++)
        snprintf(out + 2 * i, 3, "%02x", raw[i]);
    memset(raw, 0, sizeof(raw));
}
void sp_sessions_init(sp_sessions_t *s, const char *owner, sp_sessions_io_t io) {
    memset(s, 0, sizeof(*s));
    s->io = io;
    uint8_t owner_digest[32];
    if (!digest(owner, owner_digest))
        return;
    int result = io.load ? io.load(io.ctx, &s->store, sizeof(s->store)) : 0;
    if (result == 1) {
        bool valid = s->store.version == 1;
        for (unsigned i = 0; i < SP_TRUSTED_MAX; i++) {
            sp_browser_grant_t *g = &s->store.grants[i];
            if (!g->id[0])
                continue;
            valid = valid && memchr(g->id, 0, sizeof(g->id)) && hex(g->id, 16) &&
                    memchr(g->name, 0, sizeof(g->name)) && sp_text_valid(g->name, 1, 48);
            if (!valid)
                break;
            for (unsigned j = 0; j < i; j++)
                if (!strcmp(g->id, s->store.grants[j].id))
                    valid = false;
        }
        if (!valid)
            result = -1;
        else if (!same(s->store.owner, owner_digest))
            result = 0;
    }
    if (result != 1) {
        memset(&s->store, 0, sizeof(s->store));
        s->store.version = 1;
        memcpy(s->store.owner, owner_digest, 32);
    }
    s->persistent_ready = result >= 0;
}
const sp_browser_grant_t *sp_sessions_grant(const sp_sessions_t *s, unsigned slot) {
    return slot < SP_TRUSTED_MAX                 ? &s->store.grants[slot]
           : slot < SP_TRUSTED_MAX + SP_TEMP_MAX ? &s->temporary[slot - SP_TRUSTED_MAX]
                                                 : NULL;
}
int sp_sessions_check(sp_sessions_t *s, const char *secret, uint64_t now) {
    if (!hex(secret, 64))
        return -1;
    uint8_t hash[32];
    if (!digest(secret, hash))
        return -1;
    int found = -1;
    for (unsigned i = 0; i < SP_TRUSTED_MAX + SP_TEMP_MAX; i++) {
        const sp_browser_grant_t *g = sp_sessions_grant(s, i);
        bool valid =
            g->id[0] && (i < SP_TRUSTED_MAX ? s->persistent_ready : now < s->until[i - SP_TRUSTED_MAX]);
        if (same(g->digest, hash) && valid)
            found = (int)i;
    }
    memset(hash, 0, sizeof(hash));
    return found;
}
bool sp_sessions_issue(sp_sessions_t *s, bool remember, const char *name, int replace, uint64_t now,
                       char secret[65], int *slot) {
    if (!sp_text_valid(name, 1, 48) || !s->io.random || (remember && !s->persistent_ready))
        return false;
    unsigned start = remember ? 0 : SP_TRUSTED_MAX,
             end = remember ? SP_TRUSTED_MAX : SP_TRUSTED_MAX + SP_TEMP_MAX;
    int index = -1;
    if (replace >= (int)start && replace < (int)end)
        index = replace;
    for (unsigned i = start; index < 0 && i < end; i++) {
        const sp_browser_grant_t *g = sp_sessions_grant(s, i);
        if (!g->id[0] || (!remember && now >= s->until[i - SP_TRUSTED_MAX]))
            index = (int)i;
    }
    if (index < 0)
        return false;
    sp_browser_grant_t next = {0};
    random_hex(s, secret, 32);
    if (!digest(secret, next.digest)) {
        memset(secret, 0, 65);
        return false;
    }
    random_hex(s, next.id, 8);
    snprintf(next.name, sizeof(next.name), "%s", name);
    for (unsigned i = 0; i < SP_TRUSTED_MAX + SP_TEMP_MAX; i++)
        if ((int)i != index && !strcmp(next.id, sp_sessions_grant(s, i)->id)) {
            memset(secret, 0, 65);
            return false;
        }
    if (!remember && replace >= 0 && replace < (int)SP_TRUSTED_MAX) {
        sp_browser_store_t changed = s->store;
        memset(&changed.grants[replace], 0, sizeof(changed.grants[replace]));
        if (!s->io.save || !s->io.save(s->io.ctx, &changed, sizeof(changed))) {
            memset(secret, 0, 65);
            return false;
        }
        s->store = changed;
    }
    if (remember) {
        sp_browser_store_t changed = s->store;
        changed.grants[index] = next;
        if (!s->io.save || !s->io.save(s->io.ctx, &changed, sizeof(changed))) {
            memset(secret, 0, 65);
            return false;
        }
        s->store = changed;
    } else {
        s->temporary[index - SP_TRUSTED_MAX] = next;
        s->until[index - SP_TRUSTED_MAX] = now + 900000;
    }
    if (remember && replace >= (int)SP_TRUSTED_MAX && replace < (int)(SP_TRUSTED_MAX + SP_TEMP_MAX)) {
        memset(&s->temporary[replace - SP_TRUSTED_MAX], 0, sizeof(s->temporary[0]));
        s->until[replace - SP_TRUSTED_MAX] = 0;
    }
    *slot = index;
    return true;
}
bool sp_sessions_revoke(sp_sessions_t *s, unsigned slot) {
    if (slot >= SP_TRUSTED_MAX + SP_TEMP_MAX)
        return false;
    if (slot < SP_TRUSTED_MAX) {
        sp_browser_store_t changed = s->store;
        memset(&changed.grants[slot], 0, sizeof(changed.grants[slot]));
        if (!s->io.save || !s->io.save(s->io.ctx, &changed, sizeof(changed)))
            return false;
        s->store = changed;
    } else {
        memset(&s->temporary[slot - SP_TRUSTED_MAX], 0, sizeof(s->temporary[0]));
        s->until[slot - SP_TRUSTED_MAX] = 0;
    }
    return true;
}
bool sp_sessions_clear(sp_sessions_t *s) {
    sp_browser_store_t changed = s->store;
    memset(changed.grants, 0, sizeof(changed.grants));
    if (!s->io.save || !s->io.save(s->io.ctx, &changed, sizeof(changed)))
        return false;
    s->store = changed;
    s->persistent_ready = true;
    memset(s->temporary, 0, sizeof(s->temporary));
    memset(s->until, 0, sizeof(s->until));
    return true;
}
bool sp_sessions_rename(sp_sessions_t *s, unsigned slot, const char *name) {
    const sp_browser_grant_t *g = sp_sessions_grant(s, slot);
    if (!g || !g->id[0] || !sp_text_valid(name, 1, 48))
        return false;
    if (slot < SP_TRUSTED_MAX) {
        sp_browser_store_t next = s->store;
        snprintf(next.grants[slot].name, 49, "%s", name);
        if (!s->io.save || !s->io.save(s->io.ctx, &next, sizeof(next)))
            return false;
        s->store = next;
    } else
        snprintf(s->temporary[slot - SP_TRUSTED_MAX].name, 49, "%s", name);
    return true;
}
