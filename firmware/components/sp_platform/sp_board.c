#include "sp_internal.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int32_t pump_pin = -1, tank_pin = -1;
static adc_oneshot_unit_handle_t adc;
static adc_channel_t soil_channel;
static gptimer_handle_t cutoff;
static bool healthy;

#if CONFIG_IDF_TARGET_ESP32C3
static bool IRAM_ATTR cutoff_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event, void *arg) {
    (void)timer;
    (void)event;
    (void)arg;
    gpio_set_level(pump_pin, 0);
    return false;
}
static bool allowed(int pin) {
    /* C3: exclude flash, boot straps, USB, UART and on-board LED. Board review still required. */
    return pin == 0 || pin == 1 || (pin >= 3 && pin <= 7) || pin == 10;
}
#endif
bool sp_board_start(nvs_handle_t nvs) {
#if !CONFIG_IDF_TARGET_ESP32C3
    (void)nvs;
    return false; /* Hub never has a pump driver. */
#else
    int32_t soil = -1;
    if (nvs_get_i32(nvs, "soil_pin", &soil) != ESP_OK || nvs_get_i32(nvs, "pump_pin", &pump_pin) != ESP_OK ||
        nvs_get_i32(nvs, "tank_pin", &tank_pin) != ESP_OK || !allowed(soil) || !allowed(pump_pin) ||
        !allowed(tank_pin) || soil == pump_pin || soil == tank_pin || tank_pin == pump_pin)
        return false;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(soil, &unit, &soil_channel) != ESP_OK || unit != ADC_UNIT_1)
        return false;
    /* External pulldown is mandatory: software cannot control reset/ROM boot levels. */
    if (gpio_set_level(pump_pin, 0) != ESP_OK || gpio_set_direction(pump_pin, GPIO_MODE_OUTPUT) != ESP_OK)
        return false;
    gpio_config_t tank_cfg = {
        .pin_bit_mask = UINT64_C(1) << tank_pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    if (gpio_config(&tank_cfg) != ESP_OK)
        return false;
    adc_oneshot_unit_init_cfg_t ac = {.unit_id = ADC_UNIT_1};
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_oneshot_new_unit(&ac, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, soil_channel, &cc) != ESP_OK)
        return false;
    gptimer_config_t tc = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    gptimer_event_callbacks_t cb = {.on_alarm = cutoff_isr};
    if (gptimer_new_timer(&tc, &cutoff) != ESP_OK ||
        gptimer_register_event_callbacks(cutoff, &cb, NULL) != ESP_OK || gptimer_enable(cutoff) != ESP_OK)
        return false;
    healthy = true;
    return true;
#endif
}
bool sp_board_sample(uint16_t out[SP_SAMPLE_COUNT]) {
    if (!healthy)
        return false;
    for (unsigned i = 0; i < SP_SAMPLE_COUNT; i++) {
        int raw;
        if (adc_oneshot_read(adc, soil_channel, &raw) != ESP_OK)
            return false;
        out[i] = (uint16_t)raw;
    }
    return true;
}
int sp_board_tank(void) {
    return healthy ? !gpio_get_level(tank_pin) : -1;
}
bool sp_board_healthy(void) {
    return healthy;
}
static bool read_soil(void *ctx, uint64_t now, sp_measurement_t *m) {
    sp_core_t *c = ctx;
    if (now - c->boot_ms < 2000) {
        *m = (sp_measurement_t){.quality = SP_Q_MISSING, .sampled_ms = now};
        return true;
    }
    uint16_t raw[SP_SAMPLE_COUNT];
    bool ok = sp_board_sample(raw);
    sp_core_sample(c, raw, ok, now);
    *m = (sp_measurement_t){.quality = c->soil.quality,
                            .milli_value = c->soil.index_pct * 1000,
                            .sampled_ms = c->soil.sampled_ms,
                            .has_raw = ok,
                            .raw = c->soil.raw_adc};
    return true;
}
static bool read_tank(void *ctx, uint64_t now, sp_measurement_t *m) {
    sp_core_t *c = ctx;
    sp_core_tank(c, sp_board_tank(), now);
    *m = (sp_measurement_t){.quality = c->tank < 0 ? SP_Q_MISSING : SP_Q_OK,
                            .milli_value = c->tank == 1 ? 1000 : 0,
                            .sampled_ms = now};
    return true;
}
void sp_board_register_sensors(sp_sensor_registry_t *r, sp_core_t *c) {
    const sp_sensor_descriptor_t soil = {
        .slot = 0, .kind = SP_SENSOR_SOIL, .unit = SP_UNIT_PERCENT, .interval_ms = 500, .max_age_ms = 30000};
    const sp_sensor_descriptor_t tank = {
        .slot = 1, .kind = SP_SENSOR_TANK, .unit = SP_UNIT_BOOLEAN, .interval_ms = 100, .max_age_ms = 1500};
    (void)sp_sensors_register(r, &soil, read_soil, c);
    (void)sp_sensors_register(r, &tank, read_tank, c);
}
void sp_board_pump(void *ctx, bool on, uint32_t ms) {
    (void)ctx;
    if (pump_pin < 0 || !healthy)
        return;
    gpio_set_level(pump_pin, 0);
    (void)gptimer_stop(cutoff);
    if (!on)
        return;
    if (!ms || ms > SP_HARD_RUN_MS) {
        healthy = false;
        return;
    }
    gptimer_alarm_config_t alarm = {.alarm_count = (uint64_t)ms * 1000};
    if (gptimer_set_raw_count(cutoff, 0) != ESP_OK || gptimer_set_alarm_action(cutoff, &alarm) != ESP_OK) {
        healthy = false;
        return;
    }
    /* The application cannot extend this deadline while a command is running. */
    gpio_set_level(pump_pin, 1);
    if (gptimer_start(cutoff) != ESP_OK) {
        gpio_set_level(pump_pin, 0);
        healthy = false;
    }
}
