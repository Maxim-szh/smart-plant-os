#include "sp_sessions.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static sp_browser_store_t disk;
static bool have, fail;
static unsigned rng;
static int load(void *ctx, void *out, size_t n) {
    (void)ctx;
    assert(n == sizeof(disk));
    if (!have)
        return 0;
    memcpy(out, &disk, n);
    return 1;
}
static bool save(void *ctx, const void *p, size_t n) {
    (void)ctx;
    assert(n == sizeof(disk));
    if (fail)
        return false;
    memcpy(&disk, p, n);
    have = true;
    return true;
}
static void random_bytes(void *ctx, void *p, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++)
        ((unsigned char *)p)[i] = (unsigned char)++rng;
}
int main(void) {
    sp_sessions_io_t io = {.load = load, .save = save, .random = random_bytes};
    sp_sessions_t s, reloaded;
    char a[65], b[65], c[65];
    int ai, bi, ci;
    sp_sessions_init(&s, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", io);
    assert(s.persistent_ready);
    assert(sp_sessions_issue(&s, true, "Телефон", -1, 0, a, &ai));
    assert(sp_sessions_issue(&s, true, "Компьютер", -1, 0, b, &bi));
    assert(ai != bi && sp_sessions_check(&s, a, 10000000) == ai && sp_sessions_check(&s, b, 10000000) == bi);
    assert(sp_sessions_issue(&s, false, "Гостевой браузер", -1, 10, c, &ci));
    assert(sp_sessions_check(&s, c, 900009) == ci && sp_sessions_check(&s, c, 900010) < 0);
    sp_sessions_init(&reloaded, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", io);
    assert(sp_sessions_check(&reloaded, a, 0) == ai && sp_sessions_check(&reloaded, b, 0) == bi &&
           sp_sessions_check(&reloaded, c, 0) < 0);
    fail = true;
    assert(!sp_sessions_revoke(&reloaded, ai));
    assert(sp_sessions_check(&reloaded, a, 0) == ai);
    fail = false;
    assert(sp_sessions_revoke(&reloaded, ai));
    assert(sp_sessions_check(&reloaded, a, 0) < 0 && sp_sessions_check(&reloaded, b, 0) == bi);
    assert(sp_sessions_rename(&reloaded, bi, "Домашний ПК"));
    assert(!strcmp(sp_sessions_grant(&reloaded, bi)->name, "Домашний ПК"));
    sp_sessions_init(&s, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", io);
    assert(sp_sessions_check(&s, a, 0) < 0 && sp_sessions_check(&s, b, 0) == bi);
    sp_sessions_init(&s, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", io);
    assert(sp_sessions_check(&s, b, 0) < 0);
    sp_sessions_init(&s, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", io);
    disk.grants[bi].name[48] = 'x';
    memset(disk.grants[bi].name, 'x', 48);
    sp_sessions_init(&s, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", io);
    assert(!s.persistent_ready && sp_sessions_check(&s, b, 0) < 0);
    assert(sp_sessions_issue(&s, false, "Recovery", -1, 0, c, &ci));
    assert(sp_sessions_clear(&s));
    assert(s.persistent_ready && sp_sessions_check(&s, c, 0) < 0);
    puts("PASS: independent trusted browsers, digest-only persistence, reboot, expiry, revocation failure, "
         "owner change and corrupt-store recovery");
}
