#include "sp_wire.h"
#include <string.h>
void sp_message_init(sp_message_t *m, unsigned kind) {
    memset(m, 0, sizeof(*m));
    sp_message_set(m, 0, kind);
}
void sp_message_set(sp_message_t *m, unsigned k, uint64_t v) {
    if (k < 24) {
        m->present |= UINT32_C(1) << k;
        m->value[k] = v;
    }
}
bool sp_message_has(const sp_message_t *m, unsigned k) {
    return k < 28 && (m->present & (UINT32_C(1) << k));
}
static size_t blob_len(unsigned k) {
    return k == SP_K_DEVICE ? 6 : 16;
}
static bool put(uint8_t *out, size_t cap, size_t *p, uint8_t b) {
    if (*p >= cap)
        return false;
    out[(*p)++] = b;
    return true;
}
static bool number(uint8_t *o, size_t cap, size_t *p, unsigned major, uint64_t v) {
    if (v < 24)
        return put(o, cap, p, (uint8_t)(major * 32 + v));
    unsigned n = v <= 255 ? 1 : v <= 65535 ? 2 : v <= UINT32_MAX ? 4 : 8;
    if (!put(o, cap, p, (uint8_t)(major * 32 + (n == 1 ? 24 : n == 2 ? 25 : n == 4 ? 26 : 27))))
        return false;
    for (unsigned i = n; i; i--)
        if (!put(o, cap, p, (uint8_t)(v >> ((i - 1) * 8))))
            return false;
    return true;
}
bool sp_cbor_encode(const sp_message_t *m, uint8_t *out, size_t cap, size_t *len) {
    if (!m || !out || !len || !sp_message_has(m, 0) || m->present >> 28)
        return false;
    unsigned count = 0;
    for (unsigned k = 0; k < 28; k++)
        count += sp_message_has(m, k) ? 1 : 0;
    size_t p = 0;
    if (!number(out, cap, &p, 5, count))
        return false;
    for (unsigned k = 0; k < 28; k++)
        if (sp_message_has(m, k)) {
            if (!number(out, cap, &p, 0, k))
                return false;
            if (k < 24) {
                if (!number(out, cap, &p, 0, m->value[k]))
                    return false;
            } else {
                size_t n = blob_len(k);
                if (!number(out, cap, &p, 2, n))
                    return false;
                for (size_t j = 0; j < n; j++)
                    if (!put(out, cap, &p, m->bytes[k - 24][j]))
                        return false;
            }
        }
    *len = p;
    return p <= SP_CBOR_MAX;
}
static bool read_number(const uint8_t *s, size_t len, size_t *p, unsigned major, uint64_t *v) {
    if (*p >= len)
        return false;
    uint8_t b = s[(*p)++];
    if (b / 32 != major)
        return false;
    unsigned ai = b % 32;
    if (ai < 24) {
        *v = ai;
        return true;
    }
    if (ai > 27)
        return false;
    unsigned n = 1u << (ai - 24);
    if (len - *p < n)
        return false;
    *v = 0;
    for (unsigned i = 0; i < n; i++)
        *v = (*v << 8) | s[(*p)++];
    return !((n == 1 && *v < 24) || (n == 2 && *v <= 255) || (n == 4 && *v <= 65535) ||
             (n == 8 && *v <= UINT32_MAX));
}
bool sp_cbor_decode(sp_message_t *m, const uint8_t *s, size_t len) {
    if (!m || !s || len > SP_CBOR_MAX)
        return false;
    memset(m, 0, sizeof(*m));
    size_t p = 0;
    uint64_t count, key, last = 0;
    if (!read_number(s, len, &p, 5, &count) || count == 0 || count > 28)
        return false;
    for (uint64_t i = 0; i < count; i++) {
        if (!read_number(s, len, &p, 0, &key) || key > 27 || (i && key <= last))
            return false;
        last = key;
        m->present |= UINT32_C(1) << key;
        if (key < 24) {
            if (!read_number(s, len, &p, 0, &m->value[key]))
                return false;
        } else {
            uint64_t n;
            if (!read_number(s, len, &p, 2, &n) || n != blob_len((unsigned)key) || len - p < n)
                return false;
            memcpy(m->bytes[key - 24], s + p, (size_t)n);
            p += (size_t)n;
        }
    }
    return p == len && sp_message_has(m, 0) && m->value[0] >= SP_HELLO && m->value[0] <= SP_SENSOR;
}
static void be32(uint8_t *o, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        o[i] = (uint8_t)(v >> (24 - 8 * i));
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void sp_header_encode(const sp_header_t *h, uint8_t o[SP_WIRE_HEADER]) {
    o[0] = 'S';
    o[1] = 'P';
    o[2] = 1;
    o[3] = h->role;
    be32(o + 4, h->boot);
    be32(o + 8, h->sequence);
    be32(o + 12, h->target_boot);
}
bool sp_header_decode(sp_header_t *h, const uint8_t *s, size_t n) {
    if (n < SP_WIRE_HEADER + SP_WIRE_TAG || n > SP_WIRE_MAX || s[0] != 'S' || s[1] != 'P' || s[2] != 1 ||
        s[3] > 1)
        return false;
    h->role = s[3];
    h->boot = u32(s + 4);
    h->sequence = u32(s + 8);
    h->target_boot = u32(s + 12);
    return h->boot && h->sequence;
}
void sp_nonce(const sp_header_t *h, uint8_t o[12]) {
    be32(o, h->role);
    be32(o + 4, h->boot);
    be32(o + 8, h->sequence);
}
bool sp_header_fresh(const sp_header_t *h, uint8_t role, uint32_t boot, uint32_t peer_boot,
                     uint32_t peer_sequence, bool hello) {
    return h->role <= 1 && h->role != role && h->boot && h->sequence && h->boot >= peer_boot &&
           (h->boot > peer_boot || h->sequence > peer_sequence) &&
           (hello ? h->target_boot == 0 : h->target_boot == boot);
}
