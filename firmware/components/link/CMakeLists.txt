# ESP-IDF component. The host build (firmware/host) globs src/ instead.
idf_component_register(SRCS  src/link_frame.c   
                       INCLUDE_DIRS include
                       REQUIRES common )
# Control code, not the drawing (gfx, scene, plugin_host stay -O2): optimized for size, the TRMNL OG app slot is nearly full
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-format-truncation)
if(NOT CONFIG_TINY_LANG_PACKS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE TINY_LANG_PACKS=0)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../boards/${TINY_BOARD}/caps.cmake)
if(NOT TINY_CAP_WIFI_SEVERAL)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE TINY_WIFI_SEVERAL=0)
endif()
