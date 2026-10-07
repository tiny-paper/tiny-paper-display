# Small ESP-IDF build image for tiny-paper (esp32c3 only).
# ESP-IDF and its tools live in a cache directory mounted at /idf (see idf.sh), so this image only
# carries OS packages (~0.5 GB) instead of the official espressif/idf image (~4 GB compressed, ~10 GB unpacked).
FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
        git wget flex bison gperf cmake ninja-build libffi-dev libssl-dev \
        dfu-util libusb-1.0-0 ca-certificates xz-utils \
        libgcrypt20 libglib2.0-0 libpixman-1-0 libsdl2-2.0-0 libslirp0 \
    && rm -rf /var/lib/apt/lists/*
ENV IDF_PATH=/idf/esp-idf IDF_TOOLS_PATH=/idf/tools HOME=/tmp
