#!/usr/bin/env bash
# Run a command in the tiny-paper ESP-IDF container.
#   firmware/tools/idf/idf.sh setup          one-time: build image, fetch ESP-IDF + esp32c3/esp32s3 tools + QEMU into the cache
#   firmware/tools/idf/idf.sh idf.py build   any command, run in the current directory (must be inside the repo)
# Cache: $TINY_IDF_CACHE (default ~/.cache/tiny-paper-idf-<version>), about 3 GB (+ ~1.2 GB for esp32s3).
# Setup is locked (one at a time per cache) and the marker $CACHE/.setup-done is written last; re-running it repairs a half setup.
#   idf.sh ready   exit 0 when set up
set -euo pipefail
IDF_VERSION=${IDF_VERSION:-v5.5.1}
IMAGE=tiny-paper-idf:${IDF_VERSION}
CACHE=${TINY_IDF_CACHE:-$HOME/.cache/tiny-paper-idf-$IDF_VERSION}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
mkdir -p "$CACHE"
# Written only when the whole setup finished (clone, install.sh, qemu); a half setup has no marker and setup repairs it.
DONE=$CACHE/.setup-done

if [ "${1:-}" = ready ]; then
    [ -f "$DONE" ]
    exit
fi

run() {
    local tty=()
    [ -t 0 ] && [ -t 1 ] && tty=(-it)
    # Builds only read the cache: no bytecode, and the component manager cache is per container (HOME=/tmp), never shared
    # Named, and removed when this script ends or is cancelled: killing the docker client does not stop the container,
    # and an orphan keeps writing managed_components while the next job of the checkout starts (issue #108).
    NAME=tp-idf-$$-$RANDOM
    trap 'docker rm -f "$NAME" >/dev/null 2>&1 || true' EXIT
    trap 'exit 143' TERM INT HUP
    # TINY_IDF_CPUS: this job's share of the machine (tools/ci/job_cores.py); ninja in the container would take every core
    local cpus=()
    [ -n "${TINY_IDF_CPUS:-}" ] && cpus=(--cpus "$TINY_IDF_CPUS")
    local cmd=(docker run --name "$NAME" --rm "${cpus[@]}" "${tty[@]}" --user "$(id -u):$(id -g)"
        -v "$CACHE:/idf" -v "$ROOT:$ROOT" -w "$PWD"
        -e IDF_VERSION="$IDF_VERSION" -e PYTHONDONTWRITEBYTECODE=1 -e IDF_COMPONENT_CACHE_PATH=/tmp/component_cache
        "$IMAGE" bash -c "$1")
    # a foreground child delays the trap until docker returns; with a tty (-it) it must stay in the foreground
    if [ ${#tty[@]} -gt 0 ]; then "${cmd[@]}"; else "${cmd[@]}" & wait $!; fi
}

setup_heavy() {
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        docker build -t "$IMAGE" "$HERE"
    fi
    run '
        set -e
        if [ ! -f /idf/esp-idf/.cloned ] && [ ! -f /idf/esp-idf/.done ]; then  # .done: caches made by the old script (set after the clone)
            rm -rf /idf/esp-idf
            git clone --depth 1 --branch "$IDF_VERSION" --recursive --shallow-submodules \
                https://github.com/espressif/esp-idf.git /idf/esp-idf
            touch /idf/esp-idf/.cloned
        fi
        cd /idf/esp-idf
        # esp32s3 (TRMNL X) adds xtensa-esp-elf, its gdb and esp32ulp-elf (~1.2 GB); listing the target
        # is what makes export.sh put the Xtensa compiler on PATH
        ./install.sh esp32c3,esp32s3
        python3 tools/idf_tools.py install qemu-riscv32
        rm -rf /idf/tools/dist
    '
}

if [ "${1:-}" = setup ]; then
    # One setup at a time per cache (four runners share it): the second waits, then finds it done.
    # TINY_IDF_SETUP_CMD replaces the heavy part (tests, no docker or network).
    exec 9>"$CACHE.lock"
    flock 9
    if [ ! -f "$DONE" ]; then
        if [ -n "${TINY_IDF_SETUP_CMD:-}" ]; then bash -c "$TINY_IDF_SETUP_CMD"; else setup_heavy; fi
        touch "$DONE"
    fi
    exit 0
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -t "$IMAGE" "$HERE"
fi

[ -f "$DONE" ] || { echo "ESP-IDF not set up; run: $0 setup" >&2; exit 1; }
run ". /idf/esp-idf/export.sh >/dev/null && $*"
