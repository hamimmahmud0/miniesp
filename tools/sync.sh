#!/bin/bash
# Bring the board's files in line with fs_image/ after a firmware update (an OTA only replaces the firmware, so
# programs and files on the board's LittleFS keep their old versions, or are gone if the filesystem was reformatted).
#   tools/sync.sh HOST
# - system files (/bin/*.aot, the sysinfo CGI) are copied when missing or different (compared by SHA-256);
# - config/user files (motd, pkg sources, service units, ~/.profile, ~/.shrc, the homepage) are copied only when missing,
#   so your edits are never overwritten;
# - then "pkg fix" reinstalls recorded packages that are missing.
set -uo pipefail
HOST=${1:?usage: sync.sh HOST}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FS=$ROOT/fs_image
USER_=${ESP_USER:-esp}
PW=${ESP_PASSWORD:-$(cat "$ROOT/.creds/esp32_ssh_password" 2>/dev/null || true)}
ASK=$(mktemp); trap 'rm -f "$ASK"' EXIT
printf '#!/bin/sh\nprintf %%s "%s"\n' "$PW" > "$ASK"; chmod 700 "$ASK"
SSH=(env SSH_ASKPASS="$ASK" SSH_ASKPASS_REQUIRE=force DISPLAY=none ssh -o StrictHostKeyChecking=accept-new -o PreferredAuthentications=password -o PubkeyAuthentication=no -o ConnectTimeout=15 "$USER_@$HOST")
q() { grep -v "post-quantum\|store now\|server may need\|^\*\*\|^esp32-unix\|^miniesp$"; }
run() { "${SSH[@]}" "$1" </dev/null 2>&1 | q; }

for d in /bin /etc /etc/pkg /etc/services /var /var/pkg /www /www/cgi-bin /esp/.local /esp/.local/bin; do run "mkdir $d" >/dev/null; done

put() {      # put LOCAL REMOTE
    "${SSH[@]}" "put $2" < "$1" 2>&1 | q
}
sync_if_different() {
    local local_=$1 remote=$2 want have
    want=$(sha256sum "$local_" | cut -d' ' -f1)
    have=$(run "sha256sum $remote" | awk 'NR==1{print $1}')
    if [ "$want" != "$have" ]; then echo "update  $remote"; put "$local_" "$remote"; fi
}
put_if_missing() {
    # sha256sum prints a hash only for an existing file
    if ! run "sha256sum $2" | grep -qE '^[0-9a-f]{64} '; then echo "restore $2"; put "$1" "$2"; fi
}

for f in "$FS"/bin/*.aot; do sync_if_different "$f" "/bin/$(basename "$f")"; done
for f in "$FS"/www/cgi-bin/*.aot; do [ -e "$f" ] && sync_if_different "$f" "/www/cgi-bin/$(basename "$f")"; done
put_if_missing "$FS/etc/motd" /etc/motd
put_if_missing "$FS/etc/pkg/sources.list" /etc/pkg/sources.list
for f in "$FS"/etc/services/*.service; do [ -e "$f" ] && put_if_missing "$f" "/etc/services/$(basename "$f")"; done
put_if_missing "$FS/www/index.html" /www/index.html
for f in .profile .shrc; do [ -e "$FS/esp/$f" ] && put_if_missing "$FS/esp/$f" "/esp/$f"; done
run "service reload" >/dev/null
echo "pkg fix:"; run "pkg fix"
echo "sync done"
