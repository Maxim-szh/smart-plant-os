#ifndef SP_SETUP_H
#define SP_SETUP_H
#include "sp_internal.h"
#include "sp_api.h"
bool sp_setup_handle(sp_identity_t *, sp_api_t *, const sp_api_request_t *);
esp_err_t sp_radio_network_begin(const char *, const char *);
void sp_radio_network_json(cJSON *);
uint8_t sp_radio_channel(void);
bool sp_radio_setup_window(void);
bool sp_radio_network_busy(void);
esp_err_t sp_radio_scan_begin(void);
void sp_radio_scan_json(cJSON *);
void sp_radio_diagnostics_json(cJSON *);
void sp_radio_health_message(sp_message_t *);
esp_err_t sp_radio_add_peer(const sp_peer_t *);
#endif
