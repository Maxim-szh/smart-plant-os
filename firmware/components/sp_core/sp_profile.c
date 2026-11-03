#include "sp_profile.h"
#include <string.h>
#include "sp_plant_profiles.inc"
bool sp_growth_valid(const char *species, const char *stage) {
    static const char *const stages[] = {"seed", "grow", "flower", "fruit", "adult", "rest"};
    if (!stage)
        return false;
    if (!*stage)
        return true;
    unsigned bit = 0;
    for (unsigned i = 0; i < 6; i++)
        if (!strcmp(stage, stages[i]))
            bit = 1u << i;
    if (!bit)
        return false;
    if (species)
        for (unsigned i = 0; i < sizeof(profile_masks) / sizeof(profile_masks[0]); i++)
            if (!strcmp(species, profile_masks[i].id))
                return (profile_masks[i].mask & bit) != 0;
    return true; /* Unknown/custom species retains a manually selected generic stage. */
}
