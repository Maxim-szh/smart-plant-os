#include "sp_ap_policy.h"
void sp_ap_policy_init(sp_ap_policy_t *p) {
    *p = (sp_ap_policy_t){.enabled = true};
}
bool sp_ap_policy_step(sp_ap_policy_t *p, uint64_t now, bool connected, bool trial) {
    if (connected != p->connected) {
        p->connected = connected;
        p->changed_at = now;
    }
    if (trial) {
        p->enabled = true;
        p->changed_at = now;
    } else if (connected && now - p->changed_at >= SP_AP_CONNECTED_GRACE_MS)
        p->enabled = false;
    else if (!connected && now - p->changed_at >= SP_AP_RECOVERY_MS)
        p->enabled = true;
    return p->enabled;
}
