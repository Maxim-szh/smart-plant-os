#ifndef SP_PLATFORM_H
#define SP_PLATFORM_H
#include <stdbool.h>
/* Board entrypoint; Node and Hub use the same wire contract and distinct roles. */
void sp_platform_run(bool hub);
#endif
