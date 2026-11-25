#!/bin/sh
# One command for Apple Silicon: recompile your own decrypted dump, build dcp_run natively (arm64, no JIT) and play.
#   ./play-mac.sh <folder holding eboot.bin | path to eboot.bin | game.pkg> [dcp_run options...]
# A .pkg (your own fake PKG) is unpacked once, by dcp_pkg, into $DCP_GAME_DIR (default
# ~/Library/Application Support/OpenCrown/game; set it to keep the 6 GB on another disk) and reused after that.
# The folder must hold eboot.bin and sce_module/libc.prx (your own dump). Needs cmake and the Xcode command line tools
# (ninja is used when installed). Everything generated lives in $DCP_WORK_DIR (default
# ~/Library/Application Support/OpenCrown/build), never in this repository: it is derived from your game files, do not share
# it. The recompile (about 2 minutes) is redone only when the dump or the recompiler changes.
# DCP_PLAY_DRY=1 prints the commands without running them (used by tests/play_mac_test.sh). DCP_PLAY_BUILD_ONLY=1 prepares everything
# and stops before starting the game.
set -eu

# An app started from the Finder has a bare PATH (/usr/bin:/bin...): put Homebrew's folders (cmake, ninja) in front.
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
export PATH
if [ "${DCP_PLAY_PRINT_PATH:-0}" = 1 ]; then echo "$PATH"; exit 0; fi

here=$(cd "$(dirname "$0")" && pwd)
die() { echo "play-mac: $*" >&2; exit 1; }
usage() { echo "usage: $0 <folder holding eboot.bin | eboot.bin | game.pkg> [dcp_run options...]" >&2; }

[ $# -ge 1 ] || { usage; exit 2; }
arg=$1
shift
dry=${DCP_PLAY_DRY:-0}
work=${DCP_WORK_DIR:-$HOME/Library/Application Support/OpenCrown/build}

case "$arg" in
  *.pkg|*.PKG)
    [ -f "$arg" ] || die "no package at $arg"
    install=${DCP_GAME_DIR:-$HOME/Library/Application Support/OpenCrown/game}
    if [ ! -f "$install/eboot.bin" ] || [ ! -f "$install/sce_module/libc.prx" ]; then
      echo "play-mac: unpacking $arg into $install (once; about 6 GB)"
      if [ "$dry" != 1 ]; then
        # In the app it ships next to the launcher; from a source tree it is built once (needs OpenSSL's static libcrypto, e.g. brew install openssl).
        pkgtool=${DCP_PKG_TOOL:-$here/../../MacOS/dcp_pkg}
        [ -x "$pkgtool" ] || pkgtool=$work/tools/dcp_pkg
        if [ ! -x "$pkgtool" ]; then
          command -v cmake >/dev/null 2>&1 || die "cmake not found (brew install cmake)"
          cmake -S "$here" -B "$work/tools" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DDCP_RECOMPILED=ON -DDCP_RENDER=OFF >/dev/null || die "could not configure the tools"
          cmake --build "$work/tools" --target dcp_pkg || die "could not build dcp_pkg"
        fi
        mkdir -p "$(dirname "$install")" || die "cannot create $(dirname "$install")"
        # Unpack next to the destination (same disk) and rename at the end: a cancelled run leaves no half install.
        stage=$install.partial
        rm -rf "$stage"
        "$pkgtool" extract "$arg" "$stage" | sed -n 's/^progress \([0-9]*\) \([0-9]*\)$/\1 \2/p' | awk '{ p = int($1 * 100 / $2); if (p != l) { printf "play-mac: unpacking %d%%\n", p; l = p } }'
        [ -f "$stage/eboot.bin" ] && [ -f "$stage/sce_module/libc.prx" ] || { rm -rf "$stage"; die "the package did not unpack (is it a fake PKG? see dcp_pkg info)"; }
        rm -rf "$install"
        mv "$stage" "$install"
      fi
    fi
    eboot=$install/eboot.bin ;;
  *.bin) eboot=$arg ;;
  *) eboot=$arg/eboot.bin ;;
esac
case "$arg" in *.pkg|*.PKG) [ "$dry" = 1 ] && [ ! -f "$eboot" ] && { echo "+ (dry run: $eboot would exist after unpacking)"; exit 0; } ;; esac
[ -f "$eboot" ] || die "no eboot.bin at $eboot"
gamedir=$(cd "$(dirname "$eboot")" && pwd)
eboot=$gamedir/$(basename "$eboot")
libc=$gamedir/sce_module/libc.prx
[ -f "$libc" ] || die "libc.prx not found at $libc (it comes with your dump, in sce_module/)"

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
# CMake refuses a build folder configured from another source folder (the repository, an app bundle, a moved or updated app): start those
# afresh. The generated code is kept, it only depends on the dump and the recompiler's sources (the stamp).
if { [ -e "$work/tools" ] || [ -e "$work/run" ]; } && [ "$(cat "$work/source-dir" 2>/dev/null || true)" != "$here" ]; then
  run rm -rf "$work/tools" "$work/run"
fi
[ "$dry" = 1 ] || echo "$here" > "$work/source-dir"
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

echo "+ $work/run/dcp_run $eboot --window --renderer auto $*"
[ "$dry" = 1 ] || exec "$work/run/dcp_run" "$eboot" --window --renderer auto "$@"
