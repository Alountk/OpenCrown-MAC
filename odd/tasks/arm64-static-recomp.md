# arm64 static recompilation (native Apple Silicon, no Rosetta)
Objective: run OpenCrown natively on arm64 macOS by statically recompiling the user's own decrypted eboot to arm64 C++ on their machine.
Branch: feat/macos-x86_64-build (phase 0, uncommitted) -> new branch per task from main. Delivery: ask-on-risk. Runner: ctest in linux-port (build via `DCP_RENDER=OFF sh linux-port/build.sh`).

## Specs
S1. User: "creo que el way to go es ir por el camino B , aunque tardemos más ganaremos en compatibilidad con metal, y hacerlo de otra manera es muy delicado a actualizaciones." (Path B = native arm64, no Rosetta. Verified: Metal itself works under Rosetta, so the real driver is Rosetta's end of full support after macOS 27.)
S2. User, choosing static recompilation over a JIT: "si, creo que tiene mejor pinta".
S3. User's original goal: "La idea es de coger este recomp del juego dragon's crown pro multiplayer port a que corra nativamente en mac."
S4. (assumption) The recompiler is a local tool run on the user's own dump; its output (generated C++) is never committed or distributed. No game bytes in the repo.
S5. (assumption) Guest pointers are host pointers (identity mapping): the image is PIE and the kernel shims already allocate from the host, so recompiled loads/stores use native pointers and the existing HLE needs no address translation.
S6. (assumption) The Rosetta x86_64 build from phase 0 is the reference oracle for differential tests.

## Tasks
- [x] T1 (S1,S2,S4) Decoder + block discovery + .eh_frame parser (Zydis v4.1.0 via FetchContent), hand-assembled tests, no game files; builds and passes natively on arm64 (test_recomp). Route: inline. Commit: - (uncommitted)
- [x] T2 (S2) Integer lifting to C++ per function: flags are per-flag locals (the compiler deletes the unread ones, which is what makes them lazy), semantics live in recomp/runtime/dcp_rt.h. 100% of the eboot's non-VEX instructions are lifted. Verified by recomp/tests/test_lift_diff.cpp: 326 real x86-64 cases, 130,400 random trials, native (Rosetta) vs lifted, 0 failures. Route: inline. Commit: - (uncommitted)
- [ ] T3 (S2) SSE/AVX1/BMI1/LZCNT/F16C lifting (122k VEX ops in the eboot). Route: inline, in units under ~500 lines, one PR each.
  - [x] T3a vector state (ymm in Ctx), moves, scalar moves, bitwise ops. Commit: PR #2
  - [ ] T3b scalar float arithmetic, compares, conversions (x86 NaN/min/max/cvtt semantics)
  - [ ] T3c packed float and integer SIMD, shuffles, blends, 128-bit lane ops
- [ ] T4 (S2,S5) Indirect calls, jump tables and the guest-address -> function table. Route: inline. Commit: -
- [ ] T5 (S5) arm64 build of the platform layer (loader, HLE, net, render) with recompiled code stubbed; arm64 ABI bridge for HLE shims (DCP_SYSV, variadics: Apple arm64 passes them on the stack). Route: inline. Commit: -
- [ ] T6 (S4) User-side pipeline: recompile dump -> generated sources -> build -> dcp_run. Route: inline. Commit: -
- [ ] T7 (S6) Differential tests vs the Rosetta reference (needs the decrypted dump). Route: inline. Commit: -

## Log
L1 (2026-11-18, user, verbatim): crea una carpeta para un projecto que sera un fork del siguiente repositorio: https://github.com/Nezara/OpenCrown-PC. La idea es de coger este recomp del juego dragon's crown pro multiplayer port a que corra nativamente en mac. crea el nombre de folder y repo para tal tarea.
L2 (2026-11-18): phase 0 done: linux-port builds as Mach-O x86_64 and passes 10/10 ctest under Rosetta; Rosetta runs AVX/AVX2/BMI1/LZCNT/F16C/FMA (probe). Changes uncommitted on feat/macos-x86_64-build.
L3 (2026-11-18, user, verbatim): creo que el way to go es ir por el camino B , aunque tardemos más ganaremos en compatibilidad con metal, y hacerlo de otra manera es muy delicado a actualizaciones.
L4 (2026-11-18, user, verbatim): si, creo que tiene mejor pinta
Evidence: x86_64 process under Rosetta gets a full Metal device (Apple M2 Pro, BC supported). Rosetta: full support through macOS 27, limited from macOS 28 (AppleInsider, MacRumors).
T7 unblocked (L6): decrypted dump now at CUSA10454/ (git-ignored by the whitelist). The old PKG/ file was an encrypted PKG and is unusable.
L5 (2026-11-18): T1 done. linux-port/recomp/{src/decoder,cfg,tests/test_recomp}; ctest `recomp` passes as native arm64. Not yet covered: PT_GNU_EH_FRAME lookup in the real eboot (needs T7 dump), instruction semantics (T2).
L6 (2026-11-18, user, verbatim): ya tengo el juego en esta ruta de la raiz del proyecto /CUSA10454
L7 (2026-11-18): real-eboot scan with recomp/tools/dcp_scan (read-only, counts only): 29,819 functions (matches README), 205,574 blocks, 1,291,961 insns, 0 undecodable (AVX/BMI included), 0 bad. 3,649 functions end by running into padding right after a call (noreturn, no ud2): recorded as fell_off_end, not an error. For T4: 2,679 functions have indirect jumps (jump tables), 15,461 direct jumps leave their FDE range (cold/split code, must become entries), 5,374 direct calls (350 distinct targets) go to addresses that are not an FDE start (likely import stubs). dcp_loader still needs x86_64 (compat.h); isolating ELF parsing from mapping is part of T5. Libraries to recompile too: sce_module/libc.prx, libSceFios2.prx, libSceNpToolkit2.prx.
L8 (2026-11-18, user, verbatim): si, dale con el T2
L9 (2026-11-18): T2 done. Design as proposed: generated functions are extern "C" void f_<hex>(Ctx*), load r[16] into locals, spill at calls/returns (DCP_SAVE/DCP_LOAD), pushes a real return address on call. Whole-eboot lift: 29,819 functions, 1,291,961 insns, 117 MB of C++ (about 40% of it comments; strip them for production), unsupported = exactly the 122,576 VEX insns (T3). A 1,500-function slice compiles in 8 s at -O1, so the full 29,819 is about 160 s single-core: build time is not a problem. Open for later tasks: direct calls to non-FDE targets use call_va, indirect calls/jumps use call_ind/jmp_ind (T4 defines them); jump tables are not converted yet; no division-by-zero/overflow recovery beyond __builtin_trap (same as the hardware fault). lift_diff compares only architecturally defined flags per case (masks in the test); Rosetta is the oracle, so a Rosetta flag bug would also be in the baseline. Test bug found and fixed on the way: a counted-loop case ran 2^32 iterations when ecx was 0 (270 s test), now 2.6 s.
L10 (2026-11-18): T3a done (TDD: decoder test and 61 diff cases were RED first). Ctx gains Vec x[16] (ymm, 256-bit) at offset 160, loaded into a local array like r[]; VEX.128 results clear bits 255:128 (vcut). Diff test now loads/saves ymm0-15 in the native trampoline (Rosetta runs AVX) and compares them: 387 cases, 154,800 trials, 0 failures. Eboot unsupported insns went from 122,576 (9.5%) to 39,584 (3.06%) in 3,624 functions. Next: T3b (scalar float math needs exact x86 NaN propagation, minss/maxss operand rule and cvtt overflow to the integer indefinite value, because arm64 differs).
