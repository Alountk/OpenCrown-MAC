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
- [ ] T5f: overrides of recompiled functions (replace the byte-patch hooks)
- [ ] Load libSceFios2 and libSceNpToolkit2 (currently stubs)
- [ ] Differential check of recompiled game vs Rosetta reference (T7), including x87 last-digit drift
- [ ] Audio, input, save data, filesystem paths on macOS
- [ ] Menu boot, then first level

## Phase 5: Rendering
- [ ] GL on macOS 4.1 (stopgap, to see pixels)
- [ ] Metal render backend (independent task)
- [ ] Performance pass

## Phase 6: Delivery
- [ ] T6d: one-command pipeline (recompile, build, run) for the user's own dump
- [ ] Packaging and docs
- [ ] Upstream licence / permission
- [ ] Gitea remote (parked)
