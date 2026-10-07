# ESP-IDF component. The host build (firmware/host) globs src/ instead.
idf_component_register(SRCS  src/wakecal.c
                       INCLUDE_DIRS include ../../include
                       REQUIRES )

# ESP32-C3 boards (TRMNL OG: 4 MB flash, the app slot is full): control code is built for size. The render path
# (gfx, scene, plugin_host) stays at -O2.
if(CONFIG_IDF_TARGET_ESP32C3)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-format-truncation) # -Os inlines otherwise: truncation warnings -O2 has not
endif()
