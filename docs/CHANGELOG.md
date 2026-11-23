# Changelog (macOS fork)

Versions are `<upstream version>-mac.<n>`, for example `1.1-mac.1`: the upstream release this fork is based on, then this fork's own revision (`OPENCROWN_MAC_REVISION` in `linux-port/CMakeLists.txt`). Tags are `v1.1-mac.1`. Upstream's own changes are not listed here.

## 1.1-mac.2

- Fix: the launcher (and the app) kept its settings, logs and saves in `~/.local/share/OpenCrown` while the game started from a terminal used `~/Library/Application Support/OpenCrown`. Both now use the Library folder. Existing data is not moved: copy it by hand from the old folder (keep a copy of the saves already in the new one).

## 1.1-mac.1

First numbered release of the macOS port: Dragon's Crown Pro runs natively on Apple Silicon by statically recompiling your own dump.

- Recompiler (x86-64 to C++), runtime and native arm64 build; first boot, rendering (GL over Metal), audio and saves.
- `play-mac.sh`: one command from your dump to the game (first run recompiles and builds, later runs take seconds).
- Launcher on macOS with a 3-step progress bar, a visual controller view and controller setup.
- Gamepad works without focusing the window; hold touchpad click + Options for 2 seconds to quit.
- Online play on one Mac, including own camera and friend requests.
- `OpenCrown.app` with an icon and `OpenCrown-1.1-mac.1.dmg` (ad hoc signed, not notarized).
