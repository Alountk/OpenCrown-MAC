# Roadmap: native arm64 macOS port

Approach: static recompilation of the user's own decrypted eboot and PRX modules to C++ (no JIT, no game files in the repo).
Detailed task log: `odd/tasks/arm64-static-recomp.md`. Tick a box only after the outcome was observed.

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
- [ ] Remaining unsupported instructions in libc (pcmpistri/m done; enter, pushfw, xlat, fxsave, sldt, segment mov, rcr, fiadd/fisub, std left)
- [x] Game runs 5 frames natively on arm64 (`--frames 5`), engine threads created, no trap
- [ ] Unresolved computed jumps in the eboot (26 sites) verified or fixed (libc memcpy tables done)
- [x] Compile time under control (functions over 20k lines built with optnone; full build ~2 min)

## Phase 4: Make it playable
- [x] T5f1/T5f2: entry hooks for recompiled functions; render observers installed (21 functions), draws captured on arm64
- [x] T5f3: confirm_button and skip_offers (story/tutorial offers) as run-time switchable byte patches (`dcp_recompile --patches hooks/eboot_patches.txt`)
- [x] T5f3b-1: skip_offers second-ask warning in the recompiled build, through site hooks (`site` lines of the patch list)
- [x] T5f3b-2: own_camera (separate cameras online) in the recompiled build, on the same site hooks; checked by hand with two copies on one Mac
- [ ] Load libSceFios2 and libSceNpToolkit2 (currently stubs)
- [ ] Differential check of recompiled game vs Rosetta reference (T7), including x87 last-digit drift
- [x] Audio (CoreAudio, clean by ear); save data written and read back; user dir `~/Library/Application Support/OpenCrown` on macOS
- [ ] Input and the remaining filesystem paths on macOS (game files location, logs)
- [ ] Menu boot, then first level
- [x] Online on one Mac: `play-mac-online-test.sh` starts a local lobby and two game copies (keyboard + gamepad); a room created in copy A is joined from copy B by code (`DCP_JOIN_CODE`), checked by hand
- [x] Online: friend request by code (two launchers, one per account), then join the friend's room from the game, checked by hand on one Mac

## Phase 5: Rendering
- [x] dcp_run --render builds and runs on arm64 (SDL3, GL 4.1 over Metal); capture sees the draws
- [x] First visible frame on arm64 (the "Create save data?" prompt); the black frames were BC7 textures, which Apple's OpenGL rejects (CPU BC7 decoder)
- [ ] Input: answer the prompts and reach the title and the menu
- [ ] Metal render backend (independent task)
- [x] Performance measured (M2 Pro, 90 s of play): 60 fps with ample headroom; main game thread spent 60% of its time in a yield busy-wait, now throttled (CPU 142 s -> 80 s per 91 s of play, same pacing)
- [ ] Performance: remaining hot spots if lower-end Macs need them (guest code on the main thread ~25% of a core, render thread ~33%)

## Phase 6: Delivery
- [x] T6d: one-command pipeline for the user's own dump: `linux-port/play-mac.sh <dump folder>` (from scratch 235 s, then 8 s)
- [x] Launcher (tools/launcher, ImGui) builds and opens on macOS (spawn, `open`, exe path through tools/launcher/platform.h)
- [x] Launcher on macOS: Play runs play-mac.sh and the status line shows its step (preparing / recompiling / building)
- [ ] Launcher on macOS: check every page by hand (controls, display, online); hide the AppImage updater and Steam bits; a Stop button
- [x] Launcher Controls page: visual controller view (PlayStation or Xbox drawing by the connected pad) and Controller setup showing the button it asks for
- [x] Mac guide for users: `docs/MAC.md` (setup, controllers, keeping up with upstream)
- [ ] Packaging
- [x] Linux behaviour kept: macOS-only changes (audio pacing, yield throttle) guarded; headless Linux x86-64 build checked in a container (found and fixed two Linux breakages)
- [ ] Upstream licence / permission
- [ ] Gitea remote (parked)
