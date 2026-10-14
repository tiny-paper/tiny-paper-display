# ESP-IDF component. The host build (firmware/host) globs src/ instead and links mbedtls fetched there.
# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS src/btseal.c
                       INCLUDE_DIRS include
                       REQUIRES mbedtls )
