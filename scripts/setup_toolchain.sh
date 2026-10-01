#!/usr/bin/env bash
# Build the QVM toolchain (q3lcc + patched q3asm) into toolchain/build.
#
#   scripts/setup_toolchain.sh                 # downloads ioUrbanTerror from GitHub
#   scripts/setup_toolchain.sh /path/to/ioUrbanTerror   # use a local copy
#
# q3asm gets toolchain/q3asm-graft.patch (adds -cb/-db so new code can be
# assembled at the end of an existing QVM). Needs git, gcc, make, patch.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-}
BUILD=$HERE/toolchain/build

if [ -z "$SRC" ]; then
	SRC=$HERE/toolchain/ioUrbanTerror
	if [ ! -d "$SRC" ]; then
		git clone --depth 1 https://github.com/urbanterror/ioUrbanTerror "$SRC"
	fi
fi
for f in code/tools/lcc code/tools/asm/q3asm.c code/qcommon code/game/g_syscalls.asm; do
	[ -e "$SRC/$f" ] || { echo "missing $SRC/$f - not an ioUrbanTerror/ioq3 source tree?" >&2; exit 1; }
done

rm -rf "$BUILD"
mkdir -p "$BUILD/code/tools" "$BUILD/code/game"
cp -r "$SRC/code/qcommon" "$BUILD/code/"
cp -r "$SRC/code/tools/lcc" "$SRC/code/tools/asm" "$BUILD/code/tools/"
cp "$SRC/code/game/g_syscalls.asm" "$BUILD/code/game/"

patch -d "$BUILD" -p1 < "$HERE/toolchain/q3asm-graft.patch"

# old C: needs gnu89 and -fcommon with modern gcc
CFLAGS="-O2 -w -fno-strict-aliasing -std=gnu89 -fcommon"
make -C "$BUILD/code/tools/lcc" PLATFORM=linux ARCH=x86_64 LCC_CFLAGS="$CFLAGS" all >/dev/null
make -C "$BUILD/code/tools/asm" Q3ASM_CFLAGS="$CFLAGS" >/dev/null

echo "toolchain ready:"
ls "$BUILD/code/tools/lcc/build-linux-x86_64/q3lcc" "$BUILD/code/tools/asm/q3asm"
