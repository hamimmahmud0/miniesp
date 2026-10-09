#!/bin/bash
# Print "pkg" journal lines for every .aot in DIR:  tools/mkjournal.sh DIR BASE_URL [VERSION]
#   tools/mkjournal.sh pkgs https://raw.githubusercontent.com/hamimmahmud0/miniesp/main/pkgs 1.0 >> journals/main.journal
set -euo pipefail
dir=${1:?usage: mkjournal.sh DIR BASE_URL [VERSION]}; base=${2:?base url}; ver=${3:-1.0}; abi=${ABI:-3}
for f in "$dir"/*.aot; do
    [ -e "$f" ] || continue
    n=$(basename "$f" .aot)
    printf 'pkg %s %s %s/%s.aot sha256=%s size=%s abi=%s\n' "$n" "$ver" "${base%/}" "$n" "$(sha256sum "$f" | cut -d' ' -f1)" "$(stat -c%s "$f")" "$abi"
done
