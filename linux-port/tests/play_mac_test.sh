#!/bin/sh
# Tests of play-mac.sh in dry mode (DCP_PLAY_DRY=1 prints the commands instead of running them). Usage: play_mac_test.sh <play-mac.sh>
script="$1"
fail=0
check() { if ! eval "$2"; then echo "FAIL: $1"; fail=1; fi; }

tmp=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/dcp_play_XXXXXX")" && pwd)  # normalised: the script prints canonical paths
trap 'rm -rf "$tmp"' EXIT
game="$tmp/game/CUSA00000"
mkdir -p "$game/sce_module"
echo fake-eboot > "$game/eboot.bin"
echo fake-libc > "$game/sce_module/libc.prx"
work="$tmp/work"
export DCP_WORK_DIR="$work" DCP_PLAY_DRY=1

# A game directory: tools, recompile of both modules, build of dcp_run, then the game.
out=$("$script" "$game" 2>&1); rc=$?
check "dry run succeeds" '[ $rc -eq 0 ]'
check "builds dcp_recompile" 'echo "$out" | grep -q "^+ cmake .*--target dcp_recompile"'
check "recompiles the eboot with hooks and patches" 'echo "$out" | grep "^+ .*dcp_recompile" | grep "eboot.bin" | grep -q -- "--hooks .*hooks/eboot.txt" && echo "$out" | grep "dcp_recompile.*eboot.bin" | grep -q -- "--patches .*hooks/eboot_patches.txt"'
check "tools tree is an arm64 DCP_RECOMPILED build without generated sources" 'echo "$out" | grep "^+ cmake -S" | head -1 | grep -q "DCP_RECOMPILED=ON" && ! echo "$out" | grep "^+ cmake -S" | head -1 | grep -q DCP_RECOMPILED_DIR'
check "recompiles libc" 'echo "$out" | grep -q "^+ .*dcp_recompile .*libc.prx .* --name libc"'
check "builds dcp_run from the generated sources" 'echo "$out" | grep "^+ cmake -S" | grep -q "DCP_RECOMPILED=ON" && echo "$out" | grep -q "DCP_RECOMPILED_DIR=$work/gen"'
check "launches the game last" 'echo "$out" | tail -1 | grep -q "^+ .*dcp_run $game/eboot.bin --window"'
check "nothing is written outside the work dir" '[ ! -e "$work/gen" ]'

# The eboot.bin path itself works, extra arguments reach dcp_run.
out=$("$script" "$game/eboot.bin" --frames 5 2>&1)
check "eboot.bin path accepted, arguments passed on" 'echo "$out" | tail -1 | grep -q "dcp_run $game/eboot.bin --window .*--frames 5"'

# A .pkg: unpacked once into DCP_GAME_DIR (a missing package is refused; with the install already there nothing is unpacked).
out=$("$script" "$tmp/none.pkg" 2>&1); rc=$?
check "missing package is refused" '[ $rc -ne 0 ] && echo "$out" | grep -q "no package"'
echo fake > "$tmp/g.pkg"
out=$(DCP_GAME_DIR="$tmp/install" "$script" "$tmp/g.pkg" 2>&1)
check "package not yet unpacked says where it goes" 'echo "$out" | grep -q "unpacking .*g.pkg into $tmp/install"'
cp -R "$game" "$tmp/install"
out=$(DCP_GAME_DIR="$tmp/install" "$script" "$tmp/g.pkg" 2>&1)
check "an existing install is reused, not unpacked again" '! echo "$out" | grep -q "unpacking" && echo "$out" | tail -1 | grep -q "dcp_run $tmp/install/eboot.bin"'

# Build only (used by the online test script): everything is prepared, the game is not started.
out=$(DCP_PLAY_BUILD_ONLY=1 "$script" "$game" 2>&1)
check "build-only prepares dcp_run" 'echo "$out" | grep -q "^+ cmake --build .* --target dcp_run"'
check "build-only does not launch the game" '! echo "$out" | grep -q "^+ .*run/dcp_run $game"'

# CMake refuses a build folder configured from another source folder (the repository, an app bundle, a moved app): those are started afresh,
# a build folder made from this very tree is kept, and a work folder with no builds has nothing to remove.
here=$(cd "$(dirname "$script")" && pwd)
w2="$tmp/work2"; mkdir -p "$w2/tools" "$w2/run"
out=$(DCP_WORK_DIR="$w2" "$script" "$game" 2>&1)
check "builds of unknown origin are removed" 'echo "$out" | grep -q "^+ rm -rf $w2/tools $w2/run"'
echo "/somewhere/else/linux-port" > "$w2/source-dir"
out=$(DCP_WORK_DIR="$w2" "$script" "$game" 2>&1)
check "builds from another source folder are removed" 'echo "$out" | grep -q "^+ rm -rf $w2/tools $w2/run"'
echo "$here" > "$w2/source-dir"
out=$(DCP_WORK_DIR="$w2" "$script" "$game" 2>&1)
check "builds from this source folder are kept" '! echo "$out" | grep -q "^+ rm -rf"'
out=$(DCP_WORK_DIR="$tmp/work3" "$script" "$game" 2>&1)
check "a fresh work folder removes nothing" '! echo "$out" | grep -q "^+ rm -rf"'

# An app started from the Finder has a bare PATH (/usr/bin:/bin...): Homebrew's folders (cmake, ninja) are added in front.
out=$(PATH=/usr/bin:/bin DCP_PLAY_PRINT_PATH=1 "$script" "$game")
check "Homebrew folders are on the PATH, ahead of the system ones" '[ "$out" = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin" ]'

# Unchanged inputs and an existing build: the recompile is skipped.
stamp=$(DCP_PLAY_PRINT_STAMP=1 "$script" "$game")
mkdir -p "$work/gen"; touch "$work/gen/eboot_table.cpp" "$work/gen/libc_table.cpp"
echo "$stamp" > "$work/gen.stamp"
out=$("$script" "$game" 2>&1)
check "up to date: recompile skipped" 'echo "$out" | grep -q "generated code is up to date" && ! echo "$out" | grep -q "dcp_recompile .*eboot.bin"'
echo changed > "$game/eboot.bin"
out=$("$script" "$game" 2>&1)
check "a different eboot is recompiled" 'echo "$out" | grep -q "dcp_recompile .*eboot.bin"'

# Refusals.
out=$("$script" "$tmp/nowhere" 2>&1); rc=$?
check "missing game dir is refused" '[ $rc -ne 0 ] && echo "$out" | grep -q "eboot.bin"'
rm "$game/sce_module/libc.prx"
out=$("$script" "$game" 2>&1); rc=$?
check "missing libc.prx is refused" '[ $rc -ne 0 ] && echo "$out" | grep -q "libc.prx"'
out=$("$script" 2>&1); rc=$?
check "no argument prints usage" '[ $rc -ne 0 ] && echo "$out" | grep -qi "usage"'
mkdir -p "$tmp/repo/.git"; touch "$game/sce_module/libc.prx"
out=$(DCP_WORK_DIR="$tmp/repo/build" "$script" "$game" 2>&1); rc=$?
check "work dir inside a git repository is refused" '[ $rc -ne 0 ] && echo "$out" | grep -qi "git"'

[ $fail -eq 0 ] && echo "play-mac: all tests passed"
exit $fail
