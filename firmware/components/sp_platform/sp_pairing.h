#ifndef SP_PAIRING_H
#define SP_PAIRING_H
#include "sp_internal.h"
#include "cJSON.h"
bool sp_pairing_start(sp_identity_t *);
void sp_pairing_tick(void);
bool sp_pairing_receive(const uint8_t mac[6], const uint8_t *, size_t);
esp_err_t sp_pairing_open(void);
esp_err_t sp_pairing_begin(const uint8_t device[6]);
esp_err_t sp_pairing_retry(void);
void sp_pairing_link_seen(unsigned);
void sp_pairing_json(cJSON *);
#endif
