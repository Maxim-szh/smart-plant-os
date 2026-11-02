#include "sp_ap_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    sp_ap_policy_t p;
    sp_ap_policy_init(&p);
    assert(sp_ap_policy_step(&p, 0, false, false));
    assert(sp_ap_policy_step(&p, 5000, true, false));
    assert(sp_ap_policy_step(&p, 34999, true, false));
    assert(!sp_ap_policy_step(&p, 35000, true, false));
    assert(!sp_ap_policy_step(&p, 40000, false, false));
    assert(!sp_ap_policy_step(&p, 99999, false, false));
    assert(sp_ap_policy_step(&p, 100000, false, false));
    assert(sp_ap_policy_step(&p, 100001, true, false));
    assert(!sp_ap_policy_step(&p, 130001, true, false));
    // Brief Wi-Fi loss must not expose the recovery AP.
    assert(!sp_ap_policy_step(&p, 140000, false, false));
    assert(!sp_ap_policy_step(&p, 141000, true, false));
    // Explicit setup/trial opens the AP even with a working home network.
    assert(sp_ap_policy_step(&p, 142000, true, true));
    assert(sp_ap_policy_step(&p, 160000, true, true));
    assert(sp_ap_policy_step(&p, 189999, true, false));
    assert(!sp_ap_policy_step(&p, 190000, true, false));
    puts("PASS: initial AP, 30s connected grace, 60s recovery, transient loss, forced setup window");
}
