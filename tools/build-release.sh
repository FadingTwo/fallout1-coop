#!/usr/bin/env bash
# Builds release packages into <out dir> (default ../dist):
#
#   fallout-coop-<version>-linux.tar.gz    (needs SDL2 on the system)
#   fallout-coop-<version>-windows.zip     (self-contained)
#   latest.txt                             (for tools/coop_server.py)
#
#   tools/build-release.sh [out dir] [server url]
#
# The server URL is built in (-DCOOP_SERVER) and used in latest.txt.
# Both are built with zig (pip install ziglang). Linux also needs SDL2
# development files (or pass SDL2_INCLUDE_DIRS and SDL2_LIBRARIES in the
# environment). Game data is never packaged.

set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
out=$(realpath -m "${1:-$repo/../dist}")
server=${2:-}
version=$(sed -n 's/^#define COOP_VERSION "\(.*\)"/\1/p' "$repo/src/game/coop.h")
work="$out/work"

mkdir -p "$out" "$work"

sdl_args=()
[ -n "${SDL2_INCLUDE_DIRS:-}" ] && sdl_args+=("-DSDL2_INCLUDE_DIRS=$SDL2_INCLUDE_DIRS")
[ -n "${SDL2_LIBRARIES:-}" ] && sdl_args+=("-DSDL2_LIBRARIES=$SDL2_LIBRARIES")

# Source paths in the binaries (__FILE__, debug info) relative to the
# repository and the build directory, so they don't show where it was built.
prefix_map="-ffile-prefix-map=$repo/= -ffile-prefix-map=$work/="

echo "== Linux"
# zig against glibc 2.31, so the build runs on older distributions too.
cmake -S "$repo" -B "$work/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOOP_SERVER="$server" \
    -DCMAKE_TOOLCHAIN_FILE="$repo/tools/zig-linux/toolchain.cmake" -DCMAKE_C_FLAGS="-g $prefix_map" -DCMAKE_CXX_FLAGS="-g $prefix_map" "${sdl_args[@]}" >/dev/null
cmake --build "$work/linux"

echo "== Windows"
cmake -S "$repo" -B "$work/windows" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOOP_SERVER="$server" \
    -DCMAKE_TOOLCHAIN_FILE="$repo/tools/zig-windows/toolchain.cmake" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_C_FLAGS="-gdwarf-4 $prefix_map" -DCMAKE_CXX_FLAGS="-gdwarf-4 $prefix_map" >/dev/null
cmake --build "$work/windows"

package() {
    local dir=$1 binary=$2
    rm -rf "$dir"
    mkdir -p "$dir"
    cp "$binary" "$dir/"
    cp "$repo/COOP.md" "$repo/README.md" "$repo/LICENSE.md" "$repo/NOTICE.txt" "$dir/"
    cp "$repo/third_party/fonts/OFL.txt" "$dir/OFL-NotoSansMono.txt"
}

name="fallout-coop-$version"

package "$work/$name-linux" "$work/linux/fallout-ce"
strip "$work/$name-linux/fallout-ce"

# Symbols for reading crash reports (tools/crash-symbols.sh); not shipped.
mkdir -p "$out/symbols/$version"
cp "$work/linux/fallout-ce" "$out/symbols/$version/fallout-ce"
cp "$work/windows/fallout-ce.exe" "$out/symbols/$version/"
tar -C "$work" -czf "$out/$name-linux.tar.gz" "$name-linux"

package "$work/$name-windows" "$work/windows/fallout-ce.exe"
# DWARF debug info (for crash reports) stays in the symbols copy only; the
# code is the same.
objcopy --strip-debug "$work/$name-windows/fallout-ce.exe"
(cd "$work" && python3 -c "import shutil; shutil.make_archive('$out/$name-windows', 'zip', '.', '$name-windows')")

{
    echo "version=$version"
    echo "notes=Fallout co-op $version"
    if [ -n "$server" ]; then
        echo "url_linux=$server/fallout-coop/$name-linux.tar.gz"
        echo "url_windows=$server/fallout-coop/$name-windows.zip"
    fi
    echo "sha256_linux=$(sha256sum "$out/$name-linux.tar.gz" | cut -d' ' -f1)"
    echo "sha256_windows=$(sha256sum "$out/$name-windows.zip" | cut -d' ' -f1)"
} > "$out/latest.txt"

echo "== Done"
ls -la "$out"/*.tar.gz "$out"/*.zip "$out/latest.txt"
