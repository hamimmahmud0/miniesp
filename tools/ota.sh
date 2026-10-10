#!/bin/bash
# Update the firmware over the network (no USB cable):
#   tools/ota.sh HOST [path/to/esp32_unix.bin]
# Default image: build/esp32_unix.bin (run `idf.py build` first). Needs the SSH password: it is read from
# .creds/esp32_ssh_password (or set ESP_PASSWORD). User: ESP_USER (default esp). The board verifies the SHA-256,
# switches to the new image and reboots; the image rolls back by itself if it does not come up healthy.
# Afterwards tools/sync.sh restores missing/changed programs and files from fs_image/ and runs "pkg fix" (NO_SYNC=1 skips it).
set -euo pipefail
HOST=${1:?usage: ota.sh HOST [image.bin]}
IMG=${2:-$(dirname "$0")/../build/esp32_unix.bin}
USER_=${ESP_USER:-esp}
case "$IMG" in *build-psram*) echo "refusing: the psram variant has another partition table (USB flash: tools/flash.sh)"; exit 1;; esac
[ -f "$IMG" ] || { echo "image not found: $IMG (run idf.py build)"; exit 1; }
PW=${ESP_PASSWORD:-$(cat "$(dirname "$0")/../.creds/esp32_ssh_password" 2>/dev/null || true)}
ASK=$(mktemp); trap 'rm -f "$ASK"' EXIT
printf '#!/bin/sh\nprintf %%s "%s"\n' "$PW" > "$ASK"; chmod 700 "$ASK"
SSH=(env SSH_ASKPASS="$ASK" SSH_ASKPASS_REQUIRE=force DISPLAY=none ssh -o StrictHostKeyChecking=accept-new -o PreferredAuthentications=password -o PubkeyAuthentication=no -o ConnectTimeout=15 "$USER_@$HOST")
SHA=$(sha256sum "$IMG" | cut -d' ' -f1)
echo "image: $IMG ($(stat -c%s "$IMG") bytes, sha256 ${SHA:0:16}...)"
"${SSH[@]}" "ota status" </dev/null 2>/dev/null | grep -E "running|state" || true
echo "uploading..."
"${SSH[@]}" "ota --sha256 $SHA" < "$IMG" 2>&1 | grep -v "post-quantum\|store now\|server may need\|^\*\*" || true
echo "waiting for the board to come back..."
sleep 12
for i in $(seq 1 40); do
    if out=$("${SSH[@]}" "ota status" </dev/null 2>/dev/null | grep -E "running|state"); then
        echo "$out"; echo "update complete"
        # the filesystem is not part of the firmware: re-sync programs/files and reinstall packages (NO_SYNC=1 to skip)
        [ "${NO_SYNC:-0}" = 1 ] || "$(dirname "$0")/sync.sh" "$HOST"
        exit 0
    fi
    sleep 3
done
echo "board did not answer after the update (it will roll back by itself if the new image is unhealthy; check the serial log)"; exit 1
