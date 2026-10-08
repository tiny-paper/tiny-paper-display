# ESP-IDF component: tiny/hal.h for ESP32-C3 (TRMNL OG, PicPak) and ESP32-S3 (TRMNL X, Xteink X4 Pro); board.h per TINY_BOARD. Sources in esp/ (not src/): the host
# build globs components/*/src (src/ holds hardware-free, host-tested logic). LittleFS comes from joltwallet/littlefs (idf_component.yml).
# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT DEFINED TINY_BOARD)
  set(TINY_BOARD trmnl_og)
endif()
# TRMNL X: the ESP32-C5 modem as a second WiFi radio (esp/net_c5.c, docs/design/x-5ghz.md)
set(c5_srcs)
if(TINY_BOARD STREQUAL "trmnl_x")
  set(c5_srcs     )
endif()
idf_component_register(SRCS ${c5_srcs} esp/sys.c esp/link.c esp/io.c esp/kv.c esp/fs.c esp/net.c   esp/panel.c esp/panel_bwry.c esp/panel_bwfast.c esp/board.c  esp/ota.c esp/ble_link.c esp/panel_par.c esp/panel_par_box.c esp/panel_par_log.c esp/x_board.c
                              esp/diag.c  esp/part_stats.c  esp/stacks.c  src/usb_detect.c src/touch_btn.c src/btn_queue.c   src/rtclock.c  src/reset_class.c src/btn_level.c
                               src/busy_sleep.c src/reset_report.c 
                       INCLUDE_DIRS include ../../include
                       PRIV_INCLUDE_DIRS esp src ../../boards/${TINY_BOARD} ../../third_party/picpak
                       PRIV_REQUIRES epd timekeeper esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_uart esp_driver_usb_serial_jtag esp_adc
                                     nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls tcp_transport
                                     lwip esp_timer esp_hw_support vfs esp_partition app_update esp_app_format bt spi_flash
                       LDFRAGMENTS esp/linker.lf
                       WHOLE_ARCHIVE)
# ESP32-C3 boards (the TRMNL OG's app slot is full): network, OTA, NVS, diagnostics and the small helpers are built for
# size (2026-09-28: 2880 bytes less on the OG); the panel, I/O and link paths stay at -O2
if(CONFIG_IDF_TARGET_ESP32C3)
  set_source_files_properties(esp/net.c  esp/ota.c esp/kv.c  esp/diag.c esp/board.c src/usb_detect.c 
                               src/rtclock.c  src/reset_class.c src/btn_level.c 
                              PROPERTIES COMPILE_OPTIONS "-Os;-Wno-format-truncation")
endif()
# TLS session resumption across deep sleep (esp/net.c): the handshake of esp-tls goes through our wrapper
if(NOT CONFIG_TINY_NO_WIFI)
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mbedtls_ssl_handshake")
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_read")
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_connect,--wrap=esp_transport_write")
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_tls_conn_read")
  # the TLS RX buffers (esp/tls_rx.c, src/tls_rx.h): the static one only with a block for it and no positive error
  # to esp-tls; every record's buffer read within its size, from the handshake's first step (issues #2, #128)
  if(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_mbedtls_dynamic_set_rx_buf_static")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_mbedtls_add_rx_buffer,--wrap=esp_mbedtls_free_rx_buffer")
  endif()
endif()
# printf, vprintf, fprintf (ESP-IDF's only) to the integer-only formatter (esp/link.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf")
# link diag: the panic record (esp/diag.c wraps esp_panic_handler, called from another object of esp_system)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
# flash operation counters (esp/part_stats.c): what a file write costs per block erase and per program (issues #114 #122)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_partition_read,--wrap=esp_partition_write,--wrap=esp_partition_erase_range")
# panel driver calls: HAL_ESP_ST_PANEL around them (esp/sys.c), a reset there is not counted toward the crash loop
target_link_libraries(${COMPONENT_LIB} INTERFACE
  "-Wl,--wrap=hal_panel_begin,--wrap=hal_panel_write,--wrap=hal_panel_refresh,--wrap=hal_panel_sleep")
