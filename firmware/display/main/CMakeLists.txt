# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT DEFINED TINY_BOARD)
    set(TINY_BOARD trmnl_og)
endif()
idf_component_register(SRCS main.c bt.c
                       INCLUDE_DIRS ../../boards/${TINY_BOARD}
                       REQUIRES hal_esp usbdisp
                       PRIV_REQUIRES esp_system)
