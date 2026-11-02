#ifndef SP_WEB_H
#define SP_WEB_H
#include "sp_api.h"
#define SP_SETUP_AP_PASSWORD "smartplantos"
bool sp_web_ap(unsigned channel);
bool sp_web_start(void);
bool sp_web_take(sp_api_request_t *);
void sp_web_reply(void *, uint32_t, int, cJSON *);
#endif
