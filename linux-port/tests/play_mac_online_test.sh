#!/bin/sh
# Tests of play-mac-online-test.sh in dry mode (DCP_PLAY_DRY=1 prints the commands instead of running them).
# Usage: play_mac_online_test.sh <play-mac-online-test.sh>
script="$1"
fail=0
check() { if ! eval "$2"; then echo "FAIL: $1"; fail=1; fi; }

tmp=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/dcp_online_XXXXXX")" && pwd)
trap 'rm -rf "$tmp"' EXIT
game="$tmp/game/CUSA00000"
mkdir -p "$game/sce_module" "$tmp/saves/SAVEDATA00"
echo fake-eboot > "$game/eboot.bin"
echo fake-libc > "$game/sce_module/libc.prx"
echo save > "$tmp/saves/SAVEDATA00/SAVE0.DAT"
work="$tmp/work"
export DCP_WORK_DIR="$work" DCP_PLAY_DRY=1

out=$("$script" "$game" 2>&1); rc=$?
check "dry run succeeds" '[ $rc -eq 0 ]'
check "prepares the build first, without starting the game" 'echo "$out" | head -20 | grep -q "play-mac.sh"'
check "builds the lobby server" 'echo "$out" | grep -q "^+ cmake --build .* --target dcp_lobby"'
check "starts the local lobby" 'echo "$out" | grep -q "^+ .*dcp_lobby .*--quiet"'
lobby_line=$(echo "$out" | grep -n "dcp_lobby --\|dcp_lobby .*--quiet" | head -1 | cut -d: -f1)
a_line=$(echo "$out" | grep -n "PlayerA" | head -1 | cut -d: -f1)
check "the lobby starts before the players" '[ -n "$lobby_line" ] && [ -n "$a_line" ] && [ "$lobby_line" -lt "$a_line" ]'
a=$(echo "$out" | grep "^+ .*PlayerA" | head -1)
b=$(echo "$out" | grep "^+ .*PlayerB" | head -1)
check "two game copies" '[ -n "$a" ] && [ -n "$b" ]'
check "each has its own user dir" 'echo "$a" | grep -q "DCP_USER_DIR=$work/online/a" && echo "$b" | grep -q "DCP_USER_DIR=$work/online/b"'
check "different UDP ports" 'echo "$a" | grep -q "DCP_NET_PORT=3658" && echo "$b" | grep -q "DCP_NET_PORT=3659"'
check "both use the local lobby" 'echo "$a" | grep -q "DCP_LOBBY=ws://127.0.0.1:47834/" && echo "$b" | grep -q "DCP_LOBBY=ws://127.0.0.1:47834/"'
check "only the second copy is silent" '! echo "$a" | grep -q DCP_NO_AUDIO && echo "$b" | grep -q "DCP_NO_AUDIO=1"'
check "both run dcp_run on the dump's eboot, in a window" 'echo "$a" | grep -q "dcp_run $game/eboot.bin --window" && echo "$b" | grep -q "dcp_run $game/eboot.bin --window"'
check "no saves copied by default" '! echo "$out" | grep -q "^+ cp "'

# Saves from a folder holding SAVEDATA00 are copied into both user dirs.
out=$(DCP_ONLINE_SAVES="$tmp/saves" "$script" "$game" 2>&1)
check "saves copied to both copies" 'echo "$out" | grep "^+ cp " | grep -q "online/a/saves" && echo "$out" | grep "^+ cp " | grep -q "online/b/saves"'

# Refusals.
out=$("$script" 2>&1); rc=$?
check "no argument prints usage" '[ $rc -ne 0 ] && echo "$out" | grep -qi usage'
out=$(DCP_ONLINE_SAVES="$tmp/nowhere" "$script" "$game" 2>&1); rc=$?
check "a missing saves folder is refused" '[ $rc -ne 0 ] && echo "$out" | grep -q "SAVEDATA00"'
mkdir -p "$tmp/repo/.git"
out=$(DCP_WORK_DIR="$tmp/repo/build" "$script" "$game" 2>&1); rc=$?
check "work dir inside a git repository is refused" '[ $rc -ne 0 ] && echo "$out" | grep -qi git'

[ $fail -eq 0 ] && echo "play-mac-online-test: all tests passed"
exit $fail
