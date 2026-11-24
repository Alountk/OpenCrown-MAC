# Roadmap: native arm64 macOS port

Approach: static recompilation of the user's own decrypted eboot and PRX modules to C++ (no JIT, no game files in the repo).
Tick a box only after the outcome was observed.

## Phase 1: Recompiler toolchain (done)
- [x] x86-64 decoder, CFG discovery, jump-table recovery
- [x] Lifter to C++ (integer, SSE/AVX, x87, atomics, string ops)
- [x] Differential test harness against Rosetta (1,278 cases, 0 failures)
- [x] Multi-module output, sharded generation, `dcp_recompile` CLI (refuses output inside the repo)

## Phase 2: Runtime (done)
- [x] Function dispatch, indirect calls and jumps, multiple modules
- [x] ABI bridge between guest code and host HLE functions
- [x] Guest threads on private stacks (`run_guest`)
- [x] arm64 build option `DCP_RECOMPILED`, byte-patch hooks disabled

## Phase 3: First boot (in progress)
- [x] eboot and libc recompile and link into `dcp_run`
- [x] libc `module_start` runs and returns
- [x] Remaining unsupported instructions in libc: fxsave, fxrstor, fxtract and moves to/from segment registers lifted (checked against Rosetta). Left: 8 `sldt` that sit in zero-filled bytes decoded as code (never real code); the eboot has 27 more in one function (`0x63bf00`, called from `0x639ff0`) that look like data (pushf/sti/int1/sgdt runs) and trap if ever executed
- [x] Game runs 5 frames natively on arm64 (`--frames 5`), engine threads created, no trap
- [x] Unresolved computed jumps in the eboot (26 sites in about 23 functions, listed by `dcp_scan --indirect --all`): they dispatch by address at run time. Checked with `--indirect --all --targets`: each has its bounds check (`cmp reg, N` / `jnbe`) in the block before, and reading the table at that bound every entry (0 exceptions, 0 outside the function) is already a block of its function, so already recompiled and covered by the dispatch. Two sites were also checked by hand that the index register holds the value the check tested; for the rest the proof is the bound only (the recompiler cannot prove the index copy through a stack slot or an earlier register move). A target that was not compiled would still abort with a clear message, and none did in the hand play above. The 27 data-looking instructions in `0x63bf00` are a separate item
- [x] Compile time under control (functions over 20k lines built with optnone; full build ~2 min)

## Phase 4: Make it playable
- [x] T5f1/T5f2: entry hooks for recompiled functions; render observers installed (21 functions), draws captured on arm64
- [x] T5f3: confirm_button and skip_offers (story/tutorial offers) as run-time switchable byte patches (`dcp_recompile --patches hooks/eboot_patches.txt`)
- [x] T5f3b-1: skip_offers second-ask warning in the recompiled build, through site hooks (`site` lines of the patch list)
- [x] T5f3b-2: own_camera (separate cameras online) in the recompiled build, on the same site hooks; checked by hand with two copies on one Mac
- [x] libSceFios2 and libSceNpToolkit2: not loaded by design (upstream does the same). The libc imports of Fios2 that are reached are shimmed, and NpToolkit2 is emulated in `src/hle/np.cpp` (online checked by hand). Not implemented, as upstream: trophies (`sceNpTrophy*` are logged as UNIMPL, none are recorded). Not checked: a game area never played that calls an uncovered Fios2 function
- [ ] Differential check of recompiled game vs Rosetta reference (T7): `tests/t7_compare.py` written; frames identical through the first screens. Past that, a fixed-vblank input script is timing sensitive (the on-screen keyboard shows three different states in both builds as the press moves), so it needs a deterministic clock or screen-driven input; no recompiler fault shown. x87 last-digit drift not measured
- [x] Audio (CoreAudio, clean by ear); save data written and read back; user dir `~/Library/Application Support/OpenCrown` on macOS
- [x] Input and the remaining filesystem paths on macOS (user folder, logs; the quit combo is touchpad click + Options; after the controller setup Share is left unassigned)
- [x] Menu boot, then first level (played by hand for about two hours: menus, characters, several points of the map and 3 or 4 quests, no abort; the whole game has not been played through)
- [x] Online on one Mac: `play-mac-online-test.sh` starts a local lobby and two game copies (keyboard + gamepad); a room created in copy A is joined from copy B by code (`DCP_JOIN_CODE`), checked by hand
- [x] Online: friend request by code (two launchers, one per account), then join the friend's room from the game, checked by hand on one Mac

## Phase 5: Rendering
- [x] dcp_run --render builds and runs on arm64 (SDL3, GL 4.1 over Metal); capture sees the draws
- [x] First visible frame on arm64 (the "Create save data?" prompt); the black frames were BC7 textures, which Apple's OpenGL rejects (CPU BC7 decoder)
- [x] Input: answer the prompts and reach the title and the menu (keyboard and gamepad, checked by hand)
- [x] Metal render backend: the SDL GPU device runs on Metal with MSL shaders generated from the SPIR-V; `auto` picks it on a Mac, OpenGL stays as `--renderer gl` and as the fallback. Checked by hand (smoother than OpenGL, no abort), FSR and sharpen start and show on screen; frame stats and `dcp_bench` in `docs/MAC.md`. Not checked: pixels in a level against OpenGL (only the first screens, identical up to animation), M5 and lower-end Macs
- [x] Performance measured (M2 Pro, 90 s of play): 60 fps with ample headroom; main game thread spent 60% of its time in a yield busy-wait, now throttled (CPU 142 s -> 80 s per 91 s of play, same pacing)
- [ ] Performance: remaining hot spots if lower-end Macs need them (guest code on the main thread ~25% of a core, render thread ~33%)

## Phase 6: Delivery
- [x] T6d: one-command pipeline for the user's own dump: `linux-port/play-mac.sh <dump folder>` (from scratch 235 s, then 8 s)
- [x] Launcher (tools/launcher, ImGui) builds and opens on macOS (spawn, `open`, exe path through tools/launcher/platform.h)
- [x] Launcher on macOS: Play runs play-mac.sh and the status line shows its step (preparing / recompiling / building)
- [x] Launcher on macOS: every page checked by hand; AppImage updater, Steam Deck / touch options and Report a Bug hidden; renderer choice (Auto, SDL GPU (Metal), OpenGL), FSR 1 and sharpening shown since Metal works; a Stop button
- [x] Launcher Controls page: visual controller view (PlayStation or Xbox drawing by the connected pad) and Controller setup showing the button it asks for
- [x] Mac guide for users: `docs/MAC.md` (setup, controllers, keeping up with upstream)
- [x] Packaging: `OpenCrown.app` built by `linux-port/packaging/make-app-mac.sh` (ad hoc signed)
- [x] Packaging: app icon (`linux-port/packaging/AppIcon.png`)
- [x] Packaging: `OpenCrown-<version>.dmg` (the app and a link to Applications)
- Packaging: notarization dropped (needs a paid Apple Developer account); the app stays ad hoc signed, first open with right click > Open
- [x] Linux behaviour kept: macOS-only changes (audio pacing, yield throttle) guarded; headless Linux x86-64 build checked in a container (found and fixed two Linux breakages)
- [x] Upstream permission: the upstream author allowed (by message on Discord) a Mac version and release builds, asked for credit and for the public lobby to stay the default. No formal licence file exists upstream
- [x] Versioned releases `<upstream>-mac.<n>` with a DMG each, made by hand (1.1-mac.1 to 1.1-mac.3, 1.2-mac.1); steps in `docs/MAC.md`
- [x] Synced with upstream OpenCrown 1.2 (merge, tests, played by hand, released as 1.2-mac.1)
- [x] Repository public as OpenCrown-MAC with the author's agreement; `docs/NOTICE.md` (upstream's code stays theirs, the fork's own files are MIT), landing README in `.github/` that credits the author first
- [ ] Gitea remote (parked)
