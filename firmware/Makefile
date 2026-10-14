# Host unit tests need cmake, ninja, a C11 compiler and network access on first configure (Unity is fetched).
# The device builds run in Docker (tools/idf/idf.sh: a small image and the ESP-IDF cache, about 4.5 GB).
BUILD ?= build/host
IDF := tools/idf/idf.sh
BOARD ?= trmnl_og
# Firmware version: VERSION + UTC time and short hash of the HEAD commit, computed here since the build container may
# not see the repository's metadata. Without git: VERSION alone.
FW_STAMP := $(shell TZ=UTC git log -1 --format=%cd --date=format-local:%Y%m%d.%H%M 2>/dev/null)
FW_SHA := $(shell git rev-parse --short=7 HEAD 2>/dev/null)
FW_SEMVER := $(shell cat VERSION)
FW_VERSION ?= $(if $(FW_STAMP),$(FW_SEMVER)+$(FW_STAMP).$(FW_SHA))

.PHONY: test esp-setup esp clean

test:
	python3 tools/check_fastepd_source.py --pin components/epd/idf_component.yml components/epd/esp/epd_fastepd.cpp ../PLAN.md
	cmake -S host -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD)
	ctest --test-dir $(BUILD) --output-on-failure

# One-time: build the image and fetch ESP-IDF and its tools into the cache.
esp-setup:
	$(IDF) setup

# The display firmware for boards/$(BOARD) (trmnl_og, picpak, trmnl_x); binaries and flash.txt in
# build/esp/display[-$(BOARD)]/dist (no suffix for trmnl_og).
esp:
	$(IDF) TINY_FW_VERSION=$(FW_VERSION) tools/idf/build.sh display board=$(BOARD)

clean:
	rm -rf build
