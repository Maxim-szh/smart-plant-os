idf_component_register(SRCS "sp_platform.c" "sp_radio.c" "sp_board.c" "sp_crypto.c" "sp_web.c"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
  REQUIRES sp_core sp_api nvs_flash esp_wifi esp_event esp_netif esp_timer esp_adc driver mbedtls esp_system esp_https_server)
if(IDF_TARGET STREQUAL "esp32s3")
  target_add_binary_data(${COMPONENT_LIB} "../../../web/live.html" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/live.js" BINARY)
endif()
