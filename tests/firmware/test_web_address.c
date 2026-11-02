#include "sp_web_address.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const uint8_t home[4] = {192, 168, 10, 152}, ap[4] = {192, 168, 4, 1}, zero[4] = {0};
    uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 10, 152};
    assert(sp_web_home_address(home, 4, home));
    assert(!sp_web_home_address(ap, 4, home));
    assert(sp_web_home_address(mapped, 16, home));
    assert(!sp_web_home_address(mapped, 16, ap));
    mapped[10] = 0;
    assert(!sp_web_home_address(mapped, 16, home));
    assert(!sp_web_home_address(home, 4, zero));
    assert(!sp_web_home_address(home, 3, home));
    assert(!sp_web_home_address(NULL, 4, home));
    puts("PASS: IPv4 and mapped IPv6 home routing; AP and unknown addresses rejected");
}
