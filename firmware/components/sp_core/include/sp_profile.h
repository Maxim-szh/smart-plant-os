#ifndef SP_PROFILE_H
#define SP_PROFILE_H
#include <stdbool.h>
/* Descriptive metadata only; never changes watering configuration. */
bool sp_growth_valid(const char *species, const char *stage);
#endif
