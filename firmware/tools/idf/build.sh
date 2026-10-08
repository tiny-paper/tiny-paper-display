#!/usr/bin/env bash
# Build an ESP-IDF project under firmware/ and collect flashable binaries. Runs inside the IDF
# container (idf.sh); use `make -C firmware esp` from the host.
#   build.sh <project dir, e.g. bringup> [qemu] [ble] [board=<boards/ dir, default trmnl_og>]
#   checked: testing-only build (app/sdkconfig.checked: heap poisoning, stack checks, heap check at every probe)
#   fault: with checked, plants a one-byte heap overrun (guard self-test, never released)
#   stackfault: with qemu, plants a task with too small a stack (guard self-test of the stack floor, never released)
#   heapfault: with qemu, holds 30 KB of heap from boot (guard self-test of the heap floors, never released)
#   ble: app with the optional Bluetooth config mode (app/sdkconfig.ble; off on every board by default)
# Output: firmware/build/esp/<project>[-<board> unless trmnl_og][-ble][-qemu]/dist/
#   <project>-merged.bin   write at 0x0 (bootloader + partition table + otadata + app); none for xteink_x4pro
#   bootloader.bin 0x0, partition-table.bin 0x8000, ota_data_initial.bin, <app>.bin (offsets per board)
#   flash.txt              the same list, plus esptool commands
set -euo pipefail
PROJ=${1:?project}
shift
MODE=
BLE=
CHECKED=
FAULT=
STACKFAULT=
HEAPFAULT=
BOARD=trmnl_og
for a in "$@"; do
    case $a in
        qemu) MODE=qemu ;;
        ble) BLE=1 ;;
        checked) CHECKED=1 ;;
        fault) FAULT=1 ;;
        stackfault) STACKFAULT=1 ;;
        heapfault) HEAPFAULT=1 ;;
        board=*) BOARD=${a#board=} ;;
        '') ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
FW=$(cd "$(dirname "$0")/../.." && pwd)
[ -d "$FW/boards/$BOARD" ] || { echo "no board $BOARD in $FW/boards" >&2; exit 2; }
NAME=$PROJ
[ "$BOARD" = trmnl_og ] || NAME=$PROJ-$BOARD
EXTRA=(-DTINY_BOARD="$BOARD")
# Firmware version (docs/spec/firmware.md): firmware/VERSION + UTC time and short hash of the HEAD commit,
# or TINY_FW_VERSION (firmware/Makefile passes it). TINY_FW_BASE_URL: update channel base (tests).
SEMVER=$(head -n1 "$FW/VERSION")
STAMP=$(TZ=UTC git -C "$FW" log -1 --format=%cd --date=format-local:%Y%m%d.%H%M 2>/dev/null || true)
SHA=$(git -C "$FW" rev-parse --short=7 HEAD 2>/dev/null || true)
FWVER=${TINY_FW_VERSION:-${STAMP:+$SEMVER+$STAMP.$SHA}}
if [ -n "$FWVER" ]; then EXTRA+=(-DTINY_FW_VERSION="$FWVER"); fi
if [ -n "${TINY_FW_BASE_URL:-}" ]; then EXTRA+=(-DTINY_FW_BASE_URL="$TINY_FW_BASE_URL"); fi
if [ -n "$BLE" ]; then NAME=$NAME-ble; EXTRA+=(-DTINY_BLE=1); fi
if [ -n "$CHECKED" ]; then NAME=$NAME-checked; EXTRA+=(-DTINY_CHECKED=1); fi
if [ -n "$FAULT" ]; then NAME=$NAME-fault; EXTRA+=(-DTINY_FAULT=1); fi
if [ -n "$STACKFAULT" ]; then NAME=$NAME-stackfault; EXTRA+=(-DTINY_STACKFAULT=1); fi
if [ -n "$HEAPFAULT" ]; then NAME=$NAME-heapfault; EXTRA+=(-DTINY_HEAPFAULT=1); fi
if [ "$MODE" = qemu ]; then NAME=$NAME-qemu; EXTRA+=(-DTINY_QEMU=1); fi
B=$FW/build/esp/$NAME
mkdir -p "$B"
# sdkconfig defaults only fill options missing from an existing sdkconfig: start it again when they changed
# (e.g. CONFIG_BT_ENABLED=n on the boards would otherwise keep Bluetooth in an older build directory)
SIG=$({ cat "$FW/boards/$BOARD"/sdkconfig.* "$FW/$PROJ"/sdkconfig.* 2>/dev/null; echo "$CHECKED$FAULT$STACKFAULT$HEAPFAULT"; } | sha256sum | cut -c1-16)
if [ "$(cat "$B/.defaults.sig" 2>/dev/null)" != "$SIG" ]; then rm -f "$B/sdkconfig"; fi
echo "$SIG" > "$B/.defaults.sig"
# A managed component whose files no longer match its hash (a build killed or cancelled while it was downloading, or a
# second job in the same checkout) fails the configure with "is corrupted". It is only a download: delete the managed
# components of this project and fetch them again once, instead of failing the job until someone cleans the runner.
IDFLOG=$B/idf-configure.log
rc=0
idf.py -C "$FW/$PROJ" -B "$B" -DSDKCONFIG="$B/sdkconfig" "${EXTRA[@]}" build 2>&1 | tee "$IDFLOG" || rc=$?  # pipefail: idf.py's status
if [ "$rc" != 0 ]; then
    if grep -q 'component ".*" is corrupted' "$IDFLOG" && [ "${TINY_NO_COMPONENT_RETRY:-}" != 1 ]; then
        echo "build.sh: corrupt managed component in $FW/$PROJ/managed_components; removing it and fetching again (once)" >&2
        rm -rf "$FW/$PROJ/managed_components"
        rc=0
        idf.py -C "$FW/$PROJ" -B "$B" -DSDKCONFIG="$B/sdkconfig" "${EXTRA[@]}" build 2>&1 | tee "$IDFLOG" || rc=$?
    fi
    # a hash mismatch that survives the fresh fetch is not a broken download: name the component and both hashes (#109)
    if [ "$rc" != 0 ]; then python3 "$(dirname "$0")/explain_hash_mismatch.py" "$IDFLOG" >&2 || true; exit 1; fi
fi

# Guard: the app must leave its board's headroom in the slot (boards/<board>/caps.json slot_headroom_bytes); the app only
[ "$PROJ" != app ] || python3 "$FW/tools/check_slot.py" "$BOARD" "$B/tiny_paper.bin"
# Guards of the display firmware (docs/usb-display.md): it fits the same slot; no WiFi and no network code is linked,
# and Bluetooth is linked only through hal_esp's ble_link.c (started behind the stored setting, off by default)
if [ "$PROJ" = display ]; then
    python3 "$FW/tools/check_slot.py" "$BOARD" "$B/tiny_display.bin"
    python3 "$FW/tools/check_no_radio.py" "$B/tiny_display.map"
fi
D=$B/dist
rm -rf "$D"
mkdir -p "$D"
cd "$B"
# flash_args: first line holds the flash options, then "<offset> <file>" pairs
read -r -a OPTS < <(head -n1 flash_args)
PAIRS=()
CHIP=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' sdkconfig)
FSIZE=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\(.*\)"/\1/p' sdkconfig)
{
    echo "# $NAME, board $BOARD, version ${FWVER:-dev}, ESP-IDF $(cd "$IDF_PATH" && git describe --tags 2>/dev/null || echo ?), $CHIP, $FSIZE"
    echo "# flash options: ${OPTS[*]}"
    echo "# offset   file   sha256"
} > "$D/flash.txt"
while read -r off file; do
    cp "$file" "$D/"
    base=$(basename "$file")
    PAIRS+=("$off" "$base")
    echo "$off $base $(sha256sum "$D/$base" | cut -c1-16)" >> "$D/flash.txt"
done < <(tail -n +2 flash_args | sort -g)
cd "$D"
FILL=()
[ "$MODE" = qemu ] && FILL=(--fill-flash-size "$FSIZE")
if [ "$BOARD" = xteink_x4pro ]; then
    # No merged image: its gap fill would erase the factory NVS at 0x9000 (boards/xteink_x4pro/partitions.csv).
    {
        echo
        echo "# no merged image for this board: it would erase the factory NVS (0x9000..0xdfff)"
        echo "# separate:  python -m esptool --chip $CHIP -p COMx -b 460800 write_flash ${OPTS[*]} ${PAIRS[*]}"
    } >> "$D/flash.txt"
else
python -m esptool --chip "$CHIP" merge_bin -o "$PROJ-merged.bin" "${OPTS[@]}" "${FILL[@]}" "${PAIRS[@]}" >/dev/null
{
    echo "0x0 $PROJ-merged.bin $(sha256sum "$PROJ-merged.bin" | cut -c1-16)   (all of the above in one file)"
    echo
    echo "# one file:  python -m esptool --chip $CHIP -p COMx -b 460800 write_flash 0x0 $PROJ-merged.bin"
    echo "# separate:  python -m esptool --chip $CHIP -p COMx -b 460800 write_flash ${OPTS[*]} ${PAIRS[*]}"
} >> "$D/flash.txt"
fi
cat "$D/flash.txt"
ls -l "$D"
