#include "sp_text.h"
#include <stdint.h>
#include <string.h>
bool sp_text_valid(const char *text, size_t minimum, size_t maximum) {
    if (!text)
        return false;
    size_t size = strlen(text);
    if (size < minimum || size > maximum)
        return false;
    const unsigned char *p = (const unsigned char *)text, *end = p + size;
    while (p < end) {
        uint32_t value = *p++;
        unsigned more = 0;
        uint32_t min = 0;
        if (value < 0x80) {
            if (value < 32 || value == 127)
                return false;
            continue;
        }
        if (value >= 0xc2 && value <= 0xdf) {
            value &= 31;
            more = 1;
            min = 0x80;
        } else if (value >= 0xe0 && value <= 0xef) {
            value &= 15;
            more = 2;
            min = 0x800;
        } else if (value >= 0xf0 && value <= 0xf4) {
            value &= 7;
            more = 3;
            min = 0x10000;
        } else
            return false;
        if ((size_t)(end - p) < more)
            return false;
        while (more--) {
            if ((*p & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (*p++ & 63);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f))
            return false;
    }
    return true;
}
