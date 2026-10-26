#!/usr/bin/env bash
# lab/build.sh <out dir>: builds every lab program into <out dir>/<name>/default.xbe beside the files of its disc/
# folder, so each <name> folder is a disc directory for `xbrt_desktop --run`. Needs NXDK_DIR (an nxdk checkout with
# its lib/pdclib submodule) and clang, clang++, lld-link, llvm-lib and llvm-dlltool from LLVM_BIN (default: PATH).
# Each lab enters at lab_entry, not nxdk's crt0: crt0 starts main() on a second thread, and U2 parks spawned threads.
set -euo pipefail
lab=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
: "${NXDK_DIR:?NXDK_DIR must name an nxdk checkout}"
bin=${LLVM_BIN:+$LLVM_BIN/}
cxbe=$NXDK_DIR/tools/cxbe/cxbe
[ -x "$cxbe" ] || make -C "$NXDK_DIR/tools/cxbe" >/dev/null
obj=$out/.obj
mkdir -p "$obj"

flags=(-target i386-pc-win32 -march=pentium3 -O2 -ffreestanding -nostdlib -fno-builtin -Wall -Werror
       -Wno-format-security -I"$NXDK_DIR/lib" -isystem "$NXDK_DIR/lib/pdclib/include"
       -I"$NXDK_DIR/lib/pdclib/platform/xbox/include" -I"$NXDK_DIR/lib/winapi" -I"$NXDK_DIR/lib/xboxrt/vcruntime"
       -DNXDK -D__STDC__=1 -Wno-builtin-macro-redefined)
"${bin}llvm-dlltool" -m i386 -k -d "$NXDK_DIR/lib/xboxkrnl/xboxkrnl.exe.def" -l "$obj/libxboxkrnl.lib"
"${bin}clang++" "${flags[@]}" -fno-exceptions -Wno-error -c "$NXDK_DIR/lib/xboxrt/vcruntime/excpt.cpp" -o "$obj/excpt.obj"
"${bin}llvm-lib" -out:"$obj/libseh.lib" "$obj/excpt.obj"

for dir in "$lab"/*/; do
    name=$(basename "$dir")
    [ -f "$dir/main.c" ] || continue
    "${bin}clang" "${flags[@]}" -c "$dir/main.c" -o "$obj/$name.obj"
    "${bin}lld-link" -subsystem:windows -fixed -base:0x00010000 -stack:65536 -entry:lab_entry -nodefaultlib -safeseh:no \
        -out:"$obj/$name.exe" "$obj/$name.obj" "$obj/libseh.lib" "$obj/libxboxkrnl.lib"
    rm -rf "${out:?}/$name"
    mkdir -p "$out/$name"
    "$cxbe" -OUT:"$out/$name/default.xbe" -TITLE:"xbrt lab: $name" "$obj/$name.exe" >/dev/null
    python3 -I "$lab/check-sites.py" "$dir/main.c" "$out/$name/default.xbe"
    if [ -d "$dir/disc" ]; then cp -R "$dir/disc/." "$out/$name/"; fi
    if [ -f "$dir/make-disc.py" ]; then python3 -I "$dir/make-disc.py" "$out/$name"; fi
    echo "built $out/$name/default.xbe"
done
