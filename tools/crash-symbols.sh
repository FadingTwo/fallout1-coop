#!/usr/bin/env bash
# Turns the addresses in a crash report into functions and source lines.
#
#   tools/crash-symbols.sh <crash report> [symbols dir]
#
# The symbols dir defaults to ../dist/symbols/<version from the report>,
# where tools/build-release.sh keeps the unstripped binaries of each release.
set -euo pipefail

report=$1
version=$(sed -n 's/^Version: //p' "$report" | head -1)
symbols=${2:-$(dirname "$0")/../../dist/symbols/$version}

if grep -q "^Platform: Windows" "$report"; then
    exe="$symbols/fallout-ce.exe"
    address=$(sed -n 's/^Address: //p' "$report")
    base=$(sed -n 's/^Module base: //p' "$report")
    # The exe's preferred base, where its symbols are.
    image=$(objdump -p "$exe" | awk '/^ImageBase/ {print "0x"$2}')
    printf '0x%x\n' $((address - base + image)) | addr2line -f -C -e "$exe" | paste - -
else
    binary="$symbols/fallout-ce"
    base=$(sed -n 's/^Program base: //p' "$report")
    sed -n 's/^  \(0x[0-9a-f]*\)$/\1/p' "$report" | while read -r frame; do
        offset=$((frame - base))
        if [ $offset -ge 0 ] && [ $offset -lt $((64 * 1024 * 1024)) ]; then
            printf '0x%x\n' $offset | addr2line -f -C -e "$binary" | paste - -
        else
            echo "$frame (system library)"
        fi
    done
fi
