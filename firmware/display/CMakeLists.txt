# tiny-paper display firmware: a known board as a plain display driven only over the USB cable (docs/usb-display.md,
# docs/spec/usb-display.md). No WiFi, no account, no plugins, no layouts.
# Build: make -C firmware esp ESP_PROJECT=display BOARD=trmnl_og|picpak|trmnl_x
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

set(TINY_BOARD trmnl_og CACHE STRING "board directory under firmware/boards")
if(NOT TINY_BOARD MATCHES "^(trmnl_og|picpak|trmnl_x)$")
    # xteink_x4pro: left out until a device can be tested (docs/usb-display.md "Boards")
    message(FATAL_ERROR "display: no build for board ${TINY_BOARD} (trmnl_og, picpak, trmnl_x)")
endif()
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../boards/${TINY_BOARD}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# Every firmware component; COMPONENTS limits the build to what main needs.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Firmware version (docs/spec/firmware.md) in esp_app_desc_t and in hello `fw`, as for the app
if(NOT TINY_FW_VERSION)
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" TINY_FW_VERSION LIMIT_COUNT 1)
endif()
set(PROJECT_VER "${TINY_FW_VERSION}")
# One lock per chip, as the app (issue #109)
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
project(tiny_display)

# Warnings are errors for our own components and main, not for ESP-IDF or managed components (issue #23).
file(GLOB tiny_our_dirs LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_LIST_DIR}/../components" "${CMAKE_CURRENT_LIST_DIR}/../components/*")
foreach(c ${tiny_our_dirs} main)
  if(TARGET idf::${c})
    idf_component_get_property(tiny_werr_lib ${c} COMPONENT_LIB)
    get_target_property(tiny_werr_type ${tiny_werr_lib} TYPE)
    if(NOT tiny_werr_type STREQUAL "INTERFACE_LIBRARY")
      # ESP-IDF's own flags demote these with -Wno-error=<name>, which a plain -Werror does not undo
      target_compile_options(${tiny_werr_lib} PRIVATE -Werror -Werror=unused-function -Werror=unused-variable
                             -Werror=unused-but-set-variable -Werror=deprecated-declarations -Werror=extra)
    endif()
  endif()
endforeach()
idf_component_get_property(tiny_main_lib main COMPONENT_LIB)
target_compile_definitions(${tiny_main_lib} PRIVATE TINY_FW_VERSION="${TINY_FW_VERSION}")
include("${CMAKE_CURRENT_LIST_DIR}/../boards/${TINY_BOARD}/caps.cmake")
idf_build_set_property(COMPILE_DEFINITIONS "LINK_LOG_LINES=${TINY_CAP_LINK_LOG_LINES}" APPEND)
