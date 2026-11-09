idf_component_register(SRCS "sp_platform.c" "sp_radio.c" "sp_board.c" "sp_crypto.c" "sp_web.c" "sp_setup.c" "sp_ap_policy.c" "sp_join_crypto.c" "sp_pairing.c" "sp_catalog.c" "sp_epochs.c" "sp_maintenance.c" "sp_update_wire.c" "sp_update.c" "sp_update_record.c" "sp_image.c" "sp_system.c" "sp_sessions.c" "sp_diary.c" "sp_measurements.c"
  INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
  REQUIRES esp_driver_usb_serial_jtag sp_core sp_api nvs_flash esp_wifi esp_event esp_netif esp_timer esp_adc driver mbedtls esp_system esp_https_server app_update esp_partition spi_flash)
if(IDF_TARGET STREQUAL "esp32s3")
  target_add_binary_data(${COMPONENT_LIB} "../../../web/boot.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/index.html" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/app.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/live.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/setup.html" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/setup.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/pairing.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/api.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/updates.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/system.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/network.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/views.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/plant-data.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/plants.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/garden.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/care.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/home.js" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/fonts/faces.css" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/fonts/onest-latin.woff2" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/fonts/onest-cyrillic.woff2" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/fonts/literata-latin.woff2" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/fonts/literata-cyrillic.woff2" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/manifest.webmanifest" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/icon-192.png" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/icon-512.png" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/apple-touch-icon.png" BINARY)

  target_add_binary_data(${COMPONENT_LIB} "../../../web/style.css" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/demo.html" BINARY)
  target_add_binary_data(${COMPONENT_LIB} "../../../web/demo.js" BINARY)
endif()

if(IDF_TARGET STREQUAL "esp32s3")
  set(SP_ASSET_HEADER "${CMAKE_CURRENT_BINARY_DIR}/web_assets/sp_web_assets.h")
  file(GLOB_RECURSE SP_WEB_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../../web/*")
  add_custom_command(OUTPUT "${SP_ASSET_HEADER}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/build_web_assets.py"
      --web "${CMAKE_CURRENT_SOURCE_DIR}/../../../web"
      --cmake "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
      --source "${CMAKE_CURRENT_SOURCE_DIR}/sp_web.c" --output "${SP_ASSET_HEADER}"
    DEPENDS ${SP_WEB_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/build_web_assets.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_SOURCE_DIR}/sp_web.c"
    VERBATIM)
  add_custom_target(sp_web_assets DEPENDS "${SP_ASSET_HEADER}")
  add_dependencies(${COMPONENT_LIB} sp_web_assets)
  target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/web_assets")
endif()
