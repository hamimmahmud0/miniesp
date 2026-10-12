#!/bin/bash
# Update a running dedicated_wtms board over the network:   tools/ota.sh HOST [image.bin]      (default image: build/dedicated_wtms.bin)
# The image is POSTed to http://HOST/ota with its SHA-256; the key is the SSH password (../.creds/esp32_ssh_password, or ESP_PASSWORD).
# The board verifies the hash, switches slot, reboots, and rolls back by itself if the new image is not healthy within ~60 s.
set -euo pipefail
HOST=${1:?usage: ota.sh HOST [image.bin]}
IMG=${2:-$(dirname "$0")/../build/dedicated_wtms.bin}
[ -f "$IMG" ] || { echo "image not found: $IMG (idf.py build)"; exit 1; }
PW=${ESP_PASSWORD:-$(cat "$(dirname "$0")/../../.creds/esp32_ssh_password" 2>/dev/null || true)}
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
echo "image: $IMG ($(stat -c%s "$IMG") bytes, sha256 ${SHA:0:16}...)"
curl -sS --max-time 300 -H "X-Ota-Key: $PW" --data-binary @"$IMG" "http://$HOST/ota?sha=$SHA"
