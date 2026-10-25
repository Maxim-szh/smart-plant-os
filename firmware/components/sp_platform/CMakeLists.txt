idf_component_register(SRCS "sp_platform.c" "sp_radio.c" "sp_board.c" "sp_crypto.c"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
  REQUIRES sp_core nvs_flash esp_wifi esp_event esp_netif esp_timer esp_adc driver mbedtls esp_system)
