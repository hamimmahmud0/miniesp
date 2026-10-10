#!/bin/bash
# Build a firmware variant:
#   tools/build.sh [std|psram]
#   std   (default)  single core, byte-accessible IRAM pool, no PSRAM  -> build/esp32_unix.bin         (partitions.csv)
#   psram            dual core + PSRAM (WROVER-class boards)           -> build-psram/esp32_unix.bin  (partitions.psram.csv, sdkconfig.psram)
# The two variants have different partition tables: switch between them with a USB flash (tools/flash.sh), never with OTA.
set -euo pipefail
cd "$(dirname "$0")/.."
. ~/esp/env.sh >/dev/null 2>&1
case "${1:-std}" in
    std)   idf.py build ;;
    psram) B=build-psram
           mkdir -p $B
           if [ ! -f $B/sdkconfig ] || [ sdkconfig.defaults -nt $B/sdkconfig ] || [ sdkconfig.psram -nt $B/sdkconfig ]; then rm -f $B/sdkconfig; fi
           idf.py -B $B -DSDKCONFIG=$B/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.psram" build ;;
    *) echo "usage: tools/build.sh [std|psram]"; exit 2 ;;
esac
