#include "sp_image.h"
#include "sp_release.h"
#include <stdio.h>
#include <string.h>
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool sp_image_header(const uint8_t p[128], unsigned prof, char version[32]) {
    if (prof > 2)
        return false;
    unsigned chip = prof == 0 ? 5 : 9, flash = prof == 0 ? 2 : prof == 1 ? 3 : 4;
    if (p[0] != 0xe9 || p[12] != chip || p[13] || p[3] >> 4 != flash || u32(p + 32) != 0xabcd5432)
        return false;
    if (!memchr(p + 48, 0, 32) || !memchr(p + 80, 0, 32) ||
        strcmp((const char *)p + 80, prof == 0 ? "smart_plant_node" : "smart_plant_hub"))
        return false;
    const char *v = (const char *)p + 48;
    uint32_t number = 0;
    for (unsigned part = 0; part < 3; part++) {
        const char *first = v;
        unsigned value = 0, digits = 0;
        while (*v >= '0' && *v <= '9') {
            value = value * 10 + (unsigned)(*v++ - '0');
            if (++digits > 3 || value > 255)
                return false;
        }
        if (!digits || (digits > 1 && *first == '0'))
            return false;
        number = (number << 8) | value;
        if (part < 2 && *v++ != '.')
            return false;
    }
    if (strcmp(v, "-abi1") || number < SP_FIRMWARE_NUMBER)
        return false;
    memcpy(version, p + 48, 32);
    return true;
}
