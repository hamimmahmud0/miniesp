#!/bin/bash
# Build the firmware (needs ~/esp/env.sh = ESP-IDF 5.3). The first build downloads wolfSSH/wolfSSL/mDNS/LittleFS into managed_components/.
set -euo pipefail
cd "$(dirname "$0")/.."
. ~/esp/env.sh >/dev/null 2>&1
[ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig ] && rm -f sdkconfig
idf.py build
