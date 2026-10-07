# ESP-IDF component. The host build (firmware/host) globs src/ instead.
idf_component_register(SRCS src/busy.c src/cbor.c src/util.c    
                       INCLUDE_DIRS ../../include)
