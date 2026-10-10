#!/bin/bash
# USB flash that picks the firmware variant by looking for PSRAM:
#   tools/flash.sh [PORT] [std|psram]        (default PORT /dev/ttyUSB0; a variant argument skips the detection)
# 1. esptool chip_id: an "Embedded PSRAM" feature (ESP32-D0WDR2-V3, PICO-V3-02) selects the psram variant.
# 2. Otherwise (modules with external PSRAM, e.g. ESP32-WROVER, cannot be told apart from a WROOM by esptool) the psram variant is flashed
#    and its boot log is read: "Found ... PSRAM" keeps it; "Failed to init external RAM" / a boot loop re-flashes the standard variant.
# Needs both builds (tools/build.sh std; tools/build.sh psram). A first flash recreates the filesystem. Not for OTA: ota.sh refuses the psram image.
set -euo pipefail
cd "$(dirname "$0")/.."
PORT=${1:-/dev/ttyUSB0}; FORCE=${2:-}
. ~/esp/env.sh >/dev/null 2>&1
run() { if id -nG | grep -qw dialout; then "$@"; else sg dialout -c "$(printf '%q ' "$@")"; fi; }
flash() {        # flash VARIANT-DIR
    ( cd "$1" && run python -m esptool --chip esp32 -p "$PORT" -b 460800 --before default_reset --after hard_reset write_flash "@flash_args" )
}
bootlog() { run python ~/esp/tools/mon.py "$PORT" "${1:-10}" 2>&1 | strings; }
[ -f build/esp32_unix.bin ] || { echo "build the standard variant first: tools/build.sh std"; exit 1; }
want=$FORCE
if [ -z "$want" ]; then
    info=$(run python -m esptool --chip esp32 -p "$PORT" chip_id 2>&1 || true)
    echo "$info" | grep -E "Chip is|Features" || true
    if echo "$info" | grep -qi "Embedded PSRAM"; then want=psram; echo "embedded PSRAM found"; else want=probe; fi
fi
case "$want" in
    std)   flash build ;;
    psram) [ -f build-psram/esp32_unix.bin ] || { echo "build the psram variant first: tools/build.sh psram"; exit 1; }
           flash build-psram ;;
    probe) [ -f build-psram/esp32_unix.bin ] || { echo "no PSRAM seen by esptool; build the psram variant (tools/build.sh psram) to probe for external PSRAM, or flash std: tools/flash.sh $PORT std"; exit 1; }
           echo "probing for external PSRAM: flashing the psram variant ..."
           flash build-psram
           log=$(bootlog 12)
           if echo "$log" | grep -qiE "Found [0-9]+MB PSRAM|Adding pool of [0-9]+K of PSRAM"; then
               echo "PSRAM present: psram variant stays"
           else
               echo "no PSRAM (boot log shows none / init failed): flashing the standard variant"
               flash build
           fi ;;
    *) echo "variant must be std or psram"; exit 2 ;;
esac
echo "done. Check: ssh esp@HOST free   (a PSRAM line appears on the psram variant)"
