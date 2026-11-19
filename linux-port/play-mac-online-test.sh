#!/bin/sh
# Try the online mode on one Mac: a local lobby server and two copies of the game that find each other through it.
#   ./play-mac-online-test.sh <folder holding eboot.bin | path to eboot.bin>
# Copy A (PlayerA) is the one you drive with the keyboard and mouse; copy B (PlayerB, silent) takes the gamepad when its window
# has the focus. Each copy has its own user folder, so its own online account, input.ini and saves, under
# $DCP_WORK_DIR/online/a and /b (default ~/Library/Application Support/OpenCrown/build). Needs the game ready for play-mac.sh
# (the first run recompiles your dump, a few minutes). Options (environment):
#   DCP_ONLINE_SAVES=<folder holding SAVEDATA00>   copied into both copies the first time (they need a hero to go online)
#   DCP_LOBBY_PORT=<port>                           local lobby port (default 47834)
# Close both game windows (or press Ctrl-C) to stop everything. DCP_PLAY_DRY=1 prints the commands without running them.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
die() { echo "play-mac-online-test: $*" >&2; exit 1; }
[ $# -ge 1 ] || { echo "usage: $0 <folder holding eboot.bin | eboot.bin>" >&2; exit 2; }
arg=$1
case "$arg" in *.bin) eboot=$arg ;; *) eboot=$arg/eboot.bin ;; esac
[ -f "$eboot" ] || die "no eboot.bin at $eboot"
eboot=$(cd "$(dirname "$eboot")" && pwd)/$(basename "$eboot")

dry=${DCP_PLAY_DRY:-0}
work=${DCP_WORK_DIR:-$HOME/Library/Application Support/OpenCrown/build}
port=${DCP_LOBBY_PORT:-47834}
lobby_url="ws://127.0.0.1:$port/"
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
run() {
  echo "+ $*"
  [ "$dry" = 1 ] || "$@"
}

saves_src=${DCP_ONLINE_SAVES:-}
if [ -n "$saves_src" ] && [ ! -d "$saves_src/SAVEDATA00" ]; then die "DCP_ONLINE_SAVES=$saves_src has no SAVEDATA00 folder"; fi

# 1. The game itself, prepared by play-mac.sh (it also refuses a work folder inside a git repository).
echo "+ DCP_PLAY_BUILD_ONLY=1 $here/play-mac.sh $eboot"
DCP_PLAY_BUILD_ONLY=1 "$here/play-mac.sh" "$eboot"
run cmake --build "$work/run" --target dcp_lobby -j"$jobs"

# 2. Saves for both copies (only the first time: later runs keep their progress).
if [ -n "$saves_src" ]; then
  for who in a b; do
    dst="$work/online/$who/saves"
    if [ "$dry" = 1 ] || [ ! -d "$dst/SAVEDATA00" ]; then
      [ "$dry" = 1 ] || mkdir -p "$dst"
      run cp -R "$saves_src/." "$dst/"
    fi
  done
fi

pids=""
cleanup() { for p in $pids; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

# 3. The local lobby, then the two copies.
echo "+ $work/run/dcp_lobby --port $port --quiet"
if [ "$dry" != 1 ]; then
  mkdir -p "$work/online/a" "$work/online/b"
  "$work/run/dcp_lobby" --port "$port" --quiet > "$work/online/lobby.log" 2>&1 &
  pids="$!"
  sleep 1
fi

start() {  # name, user dir, udp port, extra environment
  name=$1 dir=$2 udp=$3
  shift 3
  echo "+ env DCP_USER_DIR=$dir DCP_LOBBY=$lobby_url DCP_NET_PORT=$udp DCP_NP_NAME=$name $* $work/run/dcp_run $eboot --window --renderer gl --size 960x540"
  if [ "$dry" != 1 ]; then
    mkdir -p "$dir"
    env DCP_USER_DIR="$dir" DCP_LOBBY="$lobby_url" DCP_NET_PORT="$udp" DCP_NP_NAME="$name" "$@" \
      "$work/run/dcp_run" "$eboot" --window --renderer gl --size 960x540 > "$dir/last_run.log" 2>&1 &
    pids="$pids $!"
  fi
}
start PlayerA "$work/online/a" 3658
start PlayerB "$work/online/b" 3659 DCP_NO_AUDIO=1

if [ "$dry" != 1 ]; then
  echo "play-mac-online-test: PlayerA (keyboard and mouse) and PlayerB (gamepad, silent) are starting."
  echo "  logs: $work/online/a/last_run.log, $work/online/b/last_run.log, $work/online/lobby.log"
  echo "  Close both game windows or press Ctrl-C to stop."
  # The lobby ends only when asked: wait for the two game copies.
  for p in ${pids#* }; do wait "$p" || true; done
fi
