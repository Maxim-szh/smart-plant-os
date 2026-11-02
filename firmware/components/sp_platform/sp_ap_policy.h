#ifndef SP_AP_POLICY_H
#define SP_AP_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#define SP_AP_CONNECTED_GRACE_MS 30000u
#define SP_AP_RECOVERY_MS 60000u
typedef struct {
    bool enabled, connected;
    uint64_t changed_at;
} sp_ap_policy_t;
void sp_ap_policy_init(sp_ap_policy_t *);
bool sp_ap_policy_step(sp_ap_policy_t *, uint64_t now, bool connected, bool trial);
#endif
