#!/usr/bin/env bash
# build-mac.sh [out dir]: builds core/, the core tests and xbrt-inspect with clang++ on the Mac (or the macos CI
# runner) and runs the tests. No Windows SDK on the include path, so any <windows.h> creeping into core/ breaks here.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$repo/gen/mac}
mkdir -p "$out"
ext=$repo/external
third=(-DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I"$ext/zydis/include" -I"$ext/zydis/dependencies/zycore/include"
       -I"$ext/xboxrecomp/include"
       -I"$ext/XbSymbolDatabase/include")
if [ ! -f "$out/libthird.a" ] || [ "$ext/zydis/src/Decoder.c" -nt "$out/libthird.a" ]; then
    # Zydis and Zycore each have a String.c, so every object is named by its library too.
    mkdir -p "$out/third"
    for c in "$ext"/zydis/src/*.c "$ext"/zydis/dependencies/zycore/src/*.c "$ext"/XbSymbolDatabase/src/lib/*.c \
             "$ext"/XbSymbolDatabase/src/OOVPADatabase/*.c; do
        clang -O2 -w "${third[@]}" -DZYDIS_DISABLE_ENCODER -I"$ext/zydis/src" -I"$ext/XbSymbolDatabase/src" -I"$ext/XbSymbolDatabase/src/OOVPADatabase" -I"$ext/XbSymbolDatabase/src/lib" \
            -c "$c" -o "$out/third/$(basename "$(dirname "$(dirname "$c")")")-$(basename "${c%.c}").o"
    done
    rm -f "$out/libthird.a"
    ar qc "$out/libthird.a" "$out"/third/*.o
fi
flags=(-std=c++2b -O1 -g -Wall -Wextra -Werror=switch -I"$repo" "${third[@]}")
clang++ "${flags[@]}" "$repo"/core/*.cpp "$repo"/tests/core/*.cpp "$out/libthird.a" -o "$out/xbrt_tests"
clang++ "${flags[@]}" "$repo"/core/*.cpp "$repo"/tools/xbrt-inspect/main.cpp "$out/libthird.a" -o "$out/xbrt-inspect"
"$out/xbrt_tests"
echo "built $out/xbrt-inspect"
python3 -I "$repo/scripts/gen-bridge-status.py" "$ext/xboxrecomp/src/kernel/kernel_bridge.c" --check "$repo/core/gen/bridge_status.inc"
