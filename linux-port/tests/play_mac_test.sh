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
