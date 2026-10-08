# ESP-IDF component. Hardware sources live in esp/ (not src/) because the host build globs components/*/src;
# src/epd_lut.c, src/epd_gray.c, src/epd_rects.c, src/epd_pins.c, src/epd_parwin.c, src/epd_parwave.c, src/epd_gridcell.c, src/epd_rowtime.c, src/epd_bwpart.c and src/epd_x4p.c are hardware-free and
# also host-tested (test/).
# epd_uc8179.c: TRMNL OG (epd.h). epd_bwry.c: 4-color BWRY 400x300, PicPak (epd_bwry.h).
# esp32s3 (TRMNL X): parallel panel through FastEPD (idf_component.yml pulls it, pinned), epd_par.h.
# esp32s3 (Xteink X4 Pro): esp/epd_x4p.c, SSD1677 / UC8179 / UC8279 over SPI (epd_x4p.h; src/epd_x4p.c host-tested).
set(EPD_SRCS esp/epd_uc8179.c esp/epd_bwry.c src/epd_lut.c src/epd_gray.c src/epd_rects.c src/epd_pins.c src/epd_parwin.c
             src/epd_parwave.c src/epd_parjob.c src/epd_rails.c src/epd_gridcell.c src/epd_bwpart.c src/epd_rowtime.c src/epd_scan.c src/epd_split.c src/epd_graytab.c src/epd_bwspeed.c src/epd_bwrymap.c src/epd_bwseq.c src/epd_bwcard.c src/epd_bwfast.c
              src/epd_rev.c)
set(EPD_PRIV)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND EPD_SRCS esp/epd_fastepd.cpp )
    set(EPD_PRIV bitbank2__fastepd esp_driver_i2c esp_driver_rmt esp_driver_pcnt)
endif()
idf_component_register(SRCS ${EPD_SRCS}
                       INCLUDE_DIRS include ../../include
                       REQUIRES esp_driver_spi esp_driver_gpio esp_timer
                       PRIV_REQUIRES ${EPD_PRIV})

# FastEPD compile options, set here so every project using this component gets them. Warnings: two known
# ones, silenced to keep the build warning-free. -ftrivial-auto-var-init=zero: FastEPD 8dc8c74
# src/arduino_io.inl (ESP-IDF path of bbepI2CInit/Write/Read) hands stack-garbage i2c_master_bus_config_t
# and i2c_device_config_t to the IDF driver (scl_wait_us, flags.disable_ack_check, flags.allow_pd never
# set). A garbage scl_wait_us gives a near-zero SCL timeout: "I2C transaction timeout detected", or an I2C
# interrupt storm ("Interrupt wdt timeout on CPU0" in i2c_master_isr_handler_default), depending on what
# the stack held. Stock trmnl-firmware is Arduino and goes through Wire, so it never hits this.
# Zeroed locals select the IDF defaults (2000 us, ACK check on, no power down).
if(IDF_TARGET STREQUAL "esp32s3")
    idf_component_get_property(fastepd_lib bitbank2__fastepd COMPONENT_LIB)
    if(NOT TARGET ${fastepd_lib})
        message(FATAL_ERROR "epd: FastEPD target missing, cannot apply -ftrivial-auto-var-init=zero")
    endif()
    target_compile_options(${fastepd_lib} PRIVATE -Wno-missing-field-initializers -Wno-unused-function
                           -ftrivial-auto-var-init=zero)
endif()
