# OpenCrown on macOS (Apple Silicon)

This repository is a fork of [Nezara/OpenCrown-PC](https://github.com/Nezara/OpenCrown-PC) that runs **Dragon's Crown Pro natively on Apple Silicon**. All credit for the project itself (the loader, the PS4 system libraries, the renderer, saves, online play) goes to the upstream author. This page covers only what the fork adds. The main [README](../README.md) is upstream's and is left as it is, so that upstream changes merge cleanly.

> [!IMPORTANT]
> **No game files are included, and none will ever be.** You need your own decrypted dump of Dragon's Crown Pro (CUSA10454): the folder with `eboot.bin` and `sce_module/libc.prx`.

## How it differs from upstream

Upstream runs the game's x86-64 code directly on the CPU. An Apple Silicon Mac cannot do that, so this fork **statically recompiles your own dump to C++** and builds it natively for arm64 (no JIT, no emulator). The recompiler (`linux-port/recomp/`) reads your `eboot.bin` and PRX modules and writes C++ into a folder outside this repository. Nothing derived from your game files is ever committed. The rest of the stack (HLE, renderer, audio, input, online) is upstream's, with Mac-specific fixes.

Progress and open items are tracked in [ROADMAP.md](ROADMAP.md).

## Requirements

- A Mac with Apple Silicon and macOS with the Xcode command line tools (`xcode-select --install`)
- `cmake` (and `ninja`, used when installed)
- Your own dump, as described above

## Play

```sh
cd linux-port
./play-mac.sh /path/to/your/dump/CUSA10454          # the folder holding eboot.bin
./play-mac.sh /path/to/eboot.bin --fullscreen       # extra options go to dcp_run
```

The first run recompiles your dump and builds the game (a few minutes); later runs take seconds, until the dump or the recompiler changes. Generated code lives in `~/Library/Application Support/OpenCrown/build` (override with `DCP_WORK_DIR`; the script refuses a folder inside a git repository). Saves, settings and logs are in `~/Library/Application Support/OpenCrown/`.

## The launcher on macOS

```sh
cd linux-port
cmake -S . -B build -DDCP_RECOMPILED=ON
cmake --build build --target opencrown_launcher
./build/opencrown-launcher
```

The launcher is upstream's, adapted for macOS:

- **Play** runs `play-mac.sh` and shows its step (1 preparing, 2 recompiling, 3 building) with a progress bar; the last files of step 3 are your game's own generated code and take the longest. **Stop** ends it.
- There is no AppImage updater on macOS.
- In the game, hold for 2 seconds **touchpad click + Options** on a PlayStation pad (**View + Menu** on an Xbox pad) to quit.

## As an app

```sh
cd linux-port
sh packaging/make-app-mac.sh          # writes linux-port/dist/OpenCrown.app and OpenCrown-<version>-mac.<n>.dmg
```

The app is the launcher plus a copy of the source tree. Play works as above: the first run recompiles your dump and builds the game on your Mac, so it needs `cmake` and the Xcode command line tools too. The `.dmg` holds the app and a link to Applications: open it and drag the app across. The app is signed ad hoc, not notarized (that needs a paid Apple Developer account, which this free project does not use): the first time, right click it and choose **Open**. Its icon is `linux-port/packaging/AppIcon.png`, made from the project logo (with its own background) by `make-icon.py`. No game files are inside it.

## Controllers

PlayStation and Xbox controllers work out of the box: the game reads them through SDL's gamepad API, which already maps them. The launcher's Controls page draws the controller you have connected (PlayStation's for any other or none) with a label on every button showing the key bound to it. Hover a label to colour its button; click it to rebind.

![The controller drawings of the Controls page: PlayStation and Xbox, front and top views](img/mac-controllers.png)

For any other controller, or if buttons do the wrong thing, use **Set up a gamepad...** on the Controls page. It asks for each button in turn, shows the drawing with the button it wants, and saves the mapping in your user folder (`gamepad_mappings.txt`), which the game loads at start.

On an Xbox pad the View button takes the place of the PlayStation touchpad click.

## Checking the recompiler

`linux-port/tests/t7_compare.py` runs your dump twice with the same scripted input (a Cross press every 150 vblanks) and compares frames at fixed vblanks: the recompiled arm64 `dcp_run` against the x86-64 build running under Rosetta 2 (build it with `build.sh`; Rosetta only advertises AVX when `ROSETTA_ADVERTISE_AVX=1`, which the script sets). It needs python3 with Pillow and keeps the frames outside the repository. Result so far (M2 Pro, `--noise` shows how much the reference varies by itself):

- Up to the title (vblank 1200: the save prompt and the first screens) the arm64 frames are identical to the reference, within 1 of 255 on any channel.
- On the character creation screen they differ: the random default name, the animated knight and the ornaments at the ends of the name bar. The reference differs from itself only in the name. This looks like animation driven by real time, but that is not proven.
- Later frames were compared the same way and are not conclusive; nothing past the character screen was compared by hand.

## Online

Online play is upstream's. On one Mac you can test it with two copies of the game:

```sh
cd linux-port
./play-mac-online-test.sh /path/to/your/dump/CUSA10454
```

It starts a local lobby and two game copies (keyboard and mouse in one, gamepad in the other). Separate cameras (`own_camera`) apply from the start of a real adventure, not in the first screens (stables, lobby).

## Versions

The fork numbers its releases `<upstream version>-mac.<n>` (for example `1.1-mac.1`, tag `v1.1-mac.1`) and lists them in [CHANGELOG.md](CHANGELOG.md). The revision lives in `OPENCROWN_MAC_REVISION` (`linux-port/CMakeLists.txt`); `OPENCROWN_VERSION` stays upstream's, so merging upstream keeps working.

Releases are made by hand (the repository is private, and macOS runners are not worth the minutes):

1. Raise `OPENCROWN_MAC_REVISION` and add its entry to `docs/CHANGELOG.md`, in a pull request merged into `main`.
2. On an up-to-date `main`: `git tag v<upstream>-mac.<n>` and `git push origin <tag>`.
3. `cd linux-port && sh packaging/make-app-mac.sh` writes `dist/OpenCrown-<upstream>-mac.<n>.dmg`.
4. `gh release create <tag> dist/OpenCrown-<upstream>-mac.<n>.dmg -R Alountk/opencrown-mac -t "OpenCrown <upstream>-mac.<n>" -F <notes>`, with the changelog entry as notes.

## Keeping up with upstream

The remotes are `origin` (this fork) and `upstream` (Nezara/OpenCrown-PC). **Never push or open a pull request to `upstream`.**

Most of the fork's work is in new files (`linux-port/recomp/`, `play-mac*.sh`, the launcher's controller view and tests), which upstream cannot conflict with. Where the fork edits files upstream also has, conflicts are possible:

| Area | Files |
| --- | --- |
| Build | `linux-port/CMakeLists.txt`, `linux-port/build.sh`, `.gitignore` |
| HLE | `linux-port/src/hle/` (`gnm.cpp`, `np.cpp`, `matching2.cpp`, `own_camera.cpp`, `skip_offers.cpp`, `confirm_button.cpp`, `kernel/`) |
| Loader | `linux-port/src/loader/` |
| Network | `linux-port/src/net/` |
| Renderer | `linux-port/src/render/` |
| Game runner and launcher | `linux-port/tools/dcp_run/`, `linux-port/tools/launcher/` |

To bring in upstream changes without breaking what works:

1. Start from a clean `main` and tag it: `git tag mac-before-sync-<date>`.
2. `git fetch upstream`, then look at `git log --oneline main..upstream/main`.
3. Branch (`sync/upstream-<date>`) and `git merge upstream/main`. Merge, do not rebase, so the history stays readable.
4. Resolve conflicts keeping the macOS behaviour (it is guarded with `__APPLE__`, so Linux and Windows stay as upstream has them).
5. Build, run the tests (`ctest`), and play from the title screen to a level with `play-mac.sh` before opening the pull request into `main`. On Apple Silicon, `cpu_compat` is not run (it needs x86-64) and `test_net` has a timing check that can flake; both are known and unrelated.

If the result is wrong, `git reset --hard mac-before-sync-<date>` on the sync branch (or just delete the branch) puts everything back.

## Legal

This is a hobby project, not affiliated with or endorsed by Vanillaware, Atlus, Sony or the upstream author. The upstream author gave permission for a Mac version and for release builds, on two conditions this fork keeps: credit to OpenCrown and its author (above, and in every release), and the public lobby as the default, so the player base is not split. There is no formal licence file upstream; the permission was given by message.
