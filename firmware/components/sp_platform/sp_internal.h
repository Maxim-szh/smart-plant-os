#ifndef SP_INTERNAL_H
#define SP_INTERNAL_H
#include "sp_core.h"
#include "sp_wire.h"
#include "sp_sensors.h"
#include "nvs.h"
#include "esp_err.h"

typedef struct {
    uint8_t mac[6], device[6], lmk[16], app_key[16];
    uint32_t boot, rx_sequence;
} sp_peer_t;
typedef struct {
    bool hub;
    uint8_t self[6], plant[16], pmk[16], channel, count;
    char country[3];
    sp_peer_t peers[SP_PEER_MAX];
    uint32_t boot, tx_sequence;
    nvs_handle_t runtime;
} sp_identity_t;
typedef struct {
    unsigned peer;
    sp_message_t message;
} sp_received_t;
/* Single application task owns identity, transport send/receive and the core. */
esp_err_t sp_radio_start(sp_identity_t *);
void sp_radio_process(void);
bool sp_radio_send(unsigned peer, const sp_message_t *);
bool sp_radio_receive(sp_received_t *);
esp_err_t sp_radio_pair_pmk(const uint8_t key[16]);
esp_err_t sp_radio_prepare_unpair(const uint8_t operation[16]);
esp_err_t sp_radio_remove_peer(unsigned slot);
bool sp_radio_pair_send(const uint8_t *, size_t);
esp_err_t sp_radio_add_peer(const sp_peer_t *);
bool sp_identity_load(sp_identity_t *, bool hub);
bool sp_board_start(nvs_handle_t settings);
bool sp_board_sample(uint16_t out[SP_SAMPLE_COUNT]);
int sp_board_tank(void);
void sp_board_pump(void *, bool on, uint32_t duration_ms);
bool sp_board_healthy(void);
uint32_t sp_board_capabilities(void);
void sp_board_register_sensors(sp_sensor_registry_t *, sp_core_t *);
#endif
