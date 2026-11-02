#ifndef SP_WEB_ADDRESS_H
#define SP_WEB_ADDRESS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
/* Socket addresses may be native IPv4 or IPv4-mapped IPv6 on a dual-stack listener. */
static inline bool sp_web_home_address(const uint8_t *local, size_t length, const uint8_t sta[4]) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (!local || !sta || !(sta[0] | sta[1] | sta[2] | sta[3]))
        return false;
    if (length == 16) {
        if (memcmp(local, mapped, sizeof(mapped)))
            return false;
        local += 12;
    } else if (length != 4)
        return false;
    return !memcmp(local, sta, 4);
}
#endif
