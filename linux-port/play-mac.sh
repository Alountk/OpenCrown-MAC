#!/bin/sh
# One command for Apple Silicon: recompile your own decrypted dump, build dcp_run natively (arm64, no JIT) and play.
#   ./play-mac.sh <folder holding eboot.bin | path to eboot.bin> [dcp_run options...]
# The folder must hold eboot.bin and sce_module/libc.prx (your own dump). Needs cmake and the Xcode command line tools
# (ninja is used when installed). Everything generated lives in $DCP_WORK_DIR (default
# ~/Library/Application Support/OpenCrown/build), never in this repository: it is derived from your game files, do not share
# it. The recompile (about 2 minutes) is redone only when the dump or the recompiler changes.
# DCP_PLAY_DRY=1 prints the commands without running them (used by tests/play_mac_test.sh). DCP_PLAY_BUILD_ONLY=1 prepares everything
# and stops before starting the game.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
die() { echo "play-mac: $*" >&2; exit 1; }
usage() { echo "usage: $0 <folder holding eboot.bin | eboot.bin> [dcp_run options...]" >&2; }

[ $# -ge 1 ] || { usage; exit 2; }
arg=$1
shift
case "$arg" in *.bin) eboot=$arg ;; *) eboot=$arg/eboot.bin ;; esac
[ -f "$eboot" ] || die "no eboot.bin at $eboot"
gamedir=$(cd "$(dirname "$eboot")" && pwd)
eboot=$gamedir/$(basename "$eboot")
libc=$gamedir/sce_module/libc.prx
[ -f "$libc" ] || die "libc.prx not found at $libc (it comes with your dump, in sce_module/)"

dry=${DCP_PLAY_DRY:-0}
work=${DCP_WORK_DIR:-$HOME/Library/Application Support/OpenCrown/build}

# Generated sources must stay out of any git work tree.
p=$work
while [ ! -d "$p" ]; do p=$(dirname "$p"); done
while [ "$p" != "/" ] && [ -n "$p" ]; do
  [ -e "$p/.git" ] && die "the work folder $work is inside a git repository ($p): generated code is derived from your game files and must not be committed. Set DCP_WORK_DIR elsewhere."
  p=$(dirname "$p")
done

# What the generated code depends on: the dump, the hook and patch lists, and the recompiler's own sources.
stamp=$( { cat "$eboot" "$libc" "$here/hooks/eboot.txt" "$here/hooks/eboot_patches.txt"
           find "$here/recomp/src" "$here/recomp/tools" "$here/recomp/runtime" -type f | sort | xargs cat
         } | shasum -a 256 | cut -d' ' -f1)
if [ "${DCP_PLAY_PRINT_STAMP:-0}" = 1 ]; then echo "$stamp"; exit 0; fi

if [ "$dry" != 1 ]; then
  [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] || die "this script is for Apple Silicon Macs (on Linux use build.sh)"
  command -v cmake >/dev/null 2>&1 || die "cmake not found (brew install cmake)"
  xcode-select -p >/dev/null 2>&1 || die "the Xcode command line tools are missing (xcode-select --install)"
fi

gen=""
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
run() {
  echo "+ $*"
  [ "$dry" = 1 ] || "$@"
}

[ "$dry" = 1 ] || mkdir -p "$work"
# arm64 builds need DCP_RECOMPILED=ON (compat.h); without DCP_RECOMPILED_DIR this tree builds only the tools.
run cmake -S "$here" -B "$work/tools" $gen -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DDCP_RECOMPILED=ON -DDCP_RENDER=OFF
run cmake --build "$work/tools" --target dcp_recompile -j"$jobs"

if [ "$(cat "$work/gen.stamp" 2>/dev/null || true)" = "$stamp" ] && [ -f "$work/gen/eboot_table.cpp" ] && [ -f "$work/gen/libc_table.cpp" ]; then
  echo "play-mac: generated code is up to date"
else
  echo "play-mac: recompiling your dump (about 2 minutes)"
  [ "$dry" = 1 ] || { rm -rf "$work/gen" "$work/gen.stamp"; mkdir -p "$work/gen"; }
  run "$work/tools/dcp_recompile" "$eboot" "$work/gen" --name eboot --shards 16 --hooks "$here/hooks/eboot.txt" --patches "$here/hooks/eboot_patches.txt"
  run "$work/tools/dcp_recompile" "$libc" "$work/gen" --name libc --shards 4
  [ "$dry" = 1 ] || echo "$stamp" > "$work/gen.stamp"
fi

run cmake -S "$here" -B "$work/run" $gen -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DDCP_RECOMPILED=ON -DDCP_RECOMPILED_DIR="$work/gen" -DDCP_RENDER=ON
run cmake --build "$work/run" --target dcp_run -j"$jobs"

if [ "${DCP_PLAY_BUILD_ONLY:-0}" = 1 ]; then  # everything is ready; the caller starts the game (play-mac-online-test.sh)
  echo "play-mac: build done ($work/run/dcp_run)"
  exit 0
fi

echo "+ $work/run/dcp_run $eboot --window --renderer gl $*"
[ "$dry" = 1 ] || exec "$work/run/dcp_run" "$eboot" --window --renderer gl "$@"
