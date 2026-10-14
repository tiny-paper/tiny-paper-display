# ESP-IDF component. The host build (firmware/host) globs src/ instead.
# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS src/usbdisp.c src/usbdisp_area.c src/usbdisp_panel.c src/usbdisp_png.c src/usbdisp_hal.c
                            src/usbdisp_bt.c
                       INCLUDE_DIRS include ../../include
                       REQUIRES link common imgdec btseal )
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  include(${CMAKE_CURRENT_LIST_DIR}/../../boards/${TINY_BOARD}/caps.cmake)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE USBDISP_BANDS=${TINY_CAP_APP_BANDS})
endif()
