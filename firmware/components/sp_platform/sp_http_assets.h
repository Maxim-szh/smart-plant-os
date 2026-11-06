#ifndef SP_HTTP_ASSETS_H
#define SP_HTTP_ASSETS_H
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* Conservative token parser. Explicit gzip;q=0 overrides wildcard. */
static inline bool sp_http_accepts_gzip(const char *value) {
    bool wildcard = false;
    while (value && *value) {
        while (*value == ' ' || *value == '\t' || *value == ',')
            value++;
        const char *end = strchr(value, ',');
        if (!end)
            end = value + strlen(value);
        const char *token = value;
        while (value < end && *value != ';' && *value != ' ' && *value != '\t')
            value++;
        bool gzip = value - token == 4 && !strncasecmp(token, "gzip", 4);
        bool star = value - token == 1 && *token == '*';
        bool allowed = true;
        while (value < end) {
            if (*value++ != ';')
                continue;
            while (value < end && (*value == ' ' || *value == '\t'))
                value++;
            if (value < end && (*value == 'q' || *value == 'Q')) {
                value++;
                if (value == end || *value++ != '=') {
                    allowed = false;
                    break;
                }
                const char *tail = value;
                bool positive = false, valid = tail < end && (*tail == '0' || *tail == '1');
                bool one = valid && *tail == '1';
                if (valid) {
                    positive = one;
                    tail++;
                }
                if (valid && tail < end && *tail == '.') {
                    tail++;
                    unsigned digits = 0;
                    while (tail < end && *tail >= '0' && *tail <= '9') {
                        if (++digits > 3 || (one && *tail != '0'))
                            valid = false;
                        positive |= *tail != '0';
                        tail++;
                    }
                }
                allowed = valid && positive && (tail == end || *tail == ';' || *tail == ' ' || *tail == '\t');
                value = tail;
            }
        }
        if (gzip)
            return allowed;
        if (star)
            wildcard = allowed;
        value = *end ? end + 1 : end;
    }
    return wildcard;
}
static inline bool sp_http_etag_matches(const char *value, const char *tag) {
    while (value && *value) {
        while (*value == ' ' || *value == '\t' || *value == ',')
            value++;
        if (*value == '*' && (!value[1] || value[1] == ',' || value[1] == ' '))
            return true;
        if (!strncmp(value, "W/", 2))
            value += 2;
        const char *end = strchr(value, ',');
        if (!end)
            end = value + strlen(value);
        const char *trim = end;
        while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t'))
            trim--;
        if ((size_t)(trim - value) == strlen(tag) && !memcmp(value, tag, trim - value))
            return true;
        value = *end ? end + 1 : end;
    }
    return false;
}
#endif
