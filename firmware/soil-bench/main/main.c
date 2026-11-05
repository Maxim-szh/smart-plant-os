#include <stdio.h>
#include <inttypes.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Isolated bench image. GPIO3 input only; no radio, NVS writes or actuators. */
void app_main(void) {
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_oneshot_chan_cfg_t cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &cfg));
    uint32_t seq = 0;
    for (;;) {
        int raw[9];
        for (unsigned i = 0; i < 9; i++) {
            int value;
            raw[i] = adc_oneshot_read(adc, ADC_CHANNEL_3, &value) == ESP_OK ? value : -1;
        }
        char line[320];
        int n = snprintf(line, sizeof line,
            "{\"kind\":\"soil_bench\",\"v\":1,\"firmware\":\"bench-1\",\"gpio\":3,\"seq\":%" PRIu32 ",\"device_ms\":%" PRId64 ",\"raw\":[",
            seq++, esp_timer_get_time() / 1000);
        for (unsigned i = 0; i < 9; i++) {
            if (raw[i] < 0) n += snprintf(line + n, sizeof line - n, "%snull", i ? "," : "");
            else n += snprintf(line + n, sizeof line - n, "%s%d", i ? "," : "", raw[i]);
        }
        snprintf(line + n, sizeof line - n, "]}");
        puts(line);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
