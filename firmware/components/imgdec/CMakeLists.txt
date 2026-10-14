# ESP-IDF component. The host build (firmware/host) globs src/ instead.
set(imgdec_srcs src/imgdec.c src/imgwebp_probe.c src/imginflate.c src/imgpng.c src/imgbmp.c src/imggif.c  src/imgjpg.c )
# IDF's early requirements scan has no project variables yet.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Also used outside app/ (e.g. bringup), so load the selected board here.
    include("${CMAKE_CURRENT_LIST_DIR}/../../boards/${TINY_BOARD}/caps.cmake")
    if(TINY_CAP_WEBP)
        list(APPEND imgdec_srcs src/imgwebp.c)
    endif()
endif()
idf_component_register(SRCS ${imgdec_srcs}
                       INCLUDE_DIRS include
                       REQUIRES common)
target_compile_definitions(${COMPONENT_LIB} PUBLIC IMGDEC_WEBP=${TINY_CAP_WEBP})
# Size over speed on the C3 boards: an image is decoded once per new TRMNL screen, not per frame
# Progressive JPEG only where the RAM for its coefficient store is (Kconfig IMGDEC_PROGRESSIVE: the PSRAM boards)
if(NOT CONFIG_IMGDEC_PROGRESSIVE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IMGDEC_NO_PROGRESSIVE)
endif()
if(CONFIG_IDF_TARGET_ESP32C3)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
endif()
# The inflate loop is on every picture shown over USB (usb-display.md "steps": inflate): speed over size off the C3
if(NOT CONFIG_IDF_TARGET_ESP32C3)
    set_source_files_properties(src/imginflate.c PROPERTIES COMPILE_OPTIONS -O2)
endif()
