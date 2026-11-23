# Changelog (macOS fork)

Versions are `<upstream version>-mac.<n>`, for example `1.1-mac.1`: the upstream release this fork is based on, then this fork's own revision (`OPENCROWN_MAC_REVISION` in `linux-port/CMakeLists.txt`). Tags are `v1.1-mac.1`. Upstream's own changes are not listed here.

This is a fork of [OpenCrown PC](https://github.com/Nezara/OpenCrown-PC) by Nezara, used with the author's permission. The loader, HLE, renderer, audio, input and online play are theirs.

## 1.2-mac.2

- **Metal is now the default renderer.** The game draws through SDL GPU on Metal (MSL shaders generated from the SPIR-V) instead of OpenGL. On an M2 Pro it removes the CPU decoding of BC7 textures that held up the render thread (frame hitches of up to 160 ms) and cuts the render cost per frame to about a fifth; in a play session windows below 55 fps went from 13% to 3%. OpenGL is still there: pick it in Settings > Renderer, or run `play-mac.sh <dump> --renderer gl`.
- The launcher shows Renderer, FSR 1 upscaling and Sharpening on macOS, and the renderer you pick now reaches the game (before, `play-mac.sh` forced OpenGL).
- Not checked: pixels in a level against OpenGL (only the first screens, identical up to animation), and Macs other than an M2 Pro. If something looks wrong on Metal, choose OpenGL and tell us what you saw.
- Tools: `DCP_RECORD_INPUT` records pad input to a script, `tests/perf_report.py` summarises `DCP_STATS` logs, `dcp_bench` benchmarks the two renderers without game data (see `docs/MAC.md`, Performance checks). The game logs CPU BC7 decodes of 5 ms or more.
- The recompiled game code is unchanged, so your recompiled dump is reused.

## 1.2-mac.1

- Based on upstream OpenCrown 1.2 (Transmogrify and Asia Vita support for Mobile; the Transmogrify source is not public yet, so it is not in this build). The recompiler and game code are unchanged, so your recompiled dump is reused.

## 1.1-mac.3

- Recompiler: `fxsave`, `fxrstor`, `fxtract` and segment register moves are now translated (checked against Rosetta). The first run after updating recompiles your dump again (a few minutes).
- New `tests/t7_compare.py`: runs your dump on arm64 and under Rosetta with the same scripted input and compares frames (see `docs/MAC.md`).

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
