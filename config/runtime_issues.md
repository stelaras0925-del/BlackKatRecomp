# Runtime issues found while bringing up SLUS_203.65 (not fixable in generated code)

## dispatchGuestBranch: re-entrant call preempted at entry aliases "returned without setting pc"
File: third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp, PS2Runtime::dispatchGuestBranch

    if (ctx->pc == entryPc) ctx->pc = fallthroughPc;

If F calls (directly or via a callback) F again and the EE checkpoint fires at
the inner call, the unwind arrives at the outer dispatch with ctx->pc == F ==
entryPc, so the outer caller falls through with the inner caller's sp and
runs its epilogue on the wrong frame (ra loaded as 0 -> thread dies at PC 0).
Seen: main -> 0x2B0890 -> 0x2B6C28 -> 0x2B6198 -> 0x2B6B58 -> 0x2B6198 (files.viv addbig).

Workaround (game side): src/overrides/blackkat_slus_20365.cpp re-entry wrappers.
Suggested runtime fix (one-file recompile): also capture entrySp and only apply
the fallthrough when GPR sp == entrySp, or have checkpoint preemption set an
explicit "preempted" flag that dispatchGuestBranch checks instead of pc.

### Status (2026-11-11)
Applied in third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp (original
kept as ps2_runtime.cpp.bak): entrySp is captured before the call and the
fallthrough is taken only if pc == entryPc AND sp == entrySp. Saved as
patches/ps2recomp-reentry-sp.patch; build/rebuild3.sh re-applies it if a
submodule reset dropped it:
    git -C third_party/PS2Recomp apply ../../patches/ps2recomp-reentry-sp.patch
Remaining caveat: a genuine self tail-jump (F's epilogue restores sp, then `j F`)
that is preempted at the jump still looks like "returned" (same pc, same sp).
Not seen in this game; an explicit preemption flag would close it.

## (2026-11-11, runs 17-19) Hand overrides must set pc = ra
Generated stub wrappers do `ctx->pc = ra` before ps2_stubs::X. A
replaceFunction() override that doesn't is re-invoked forever by
EeScheduler::run() when the thread was preempted at the dispatch checkpoint
just before the call (resume calls the function at pc directly; pc unchanged).
Hit with the sceCdRead shim (main re-issued one sceCdRead endlessly). Fixed
in src/overrides/blackkat_slus_20365.cpp. Possible runtime-side fix: in
EeScheduler::run(), if a non-generated function returns with pc unchanged,
set pc = ra.

## No IPU emulation -> FMVs hang
EA IpuMpeg (FMV::PlayFile 0x1E1F88) drives IPU regs directly; IPU_CMD reads
back the last command, so next_start_code (0x2C77E8) spins forever. Skipped
in the override (BLACKKAT_PLAY_FMV=1 restores).

## GS CSR FIFO field read as 00
CSR was returned raw (FIFO bits 14..15 = 00). Game waits for 01 (empty) at
0x2D38C8 after drawing "Loading" -> spin. patches/ps2recomp-csr-fifo.patch
(ps2_memory.cpp) reports FIFO empty on every CSR read. Built, NOT yet tested.

## [run:tick] status line
patches/ps2recomp-run-tick-env.patch: PS2X_TICK=<frames> (or BLACKKAT_TICK)
prints pc/ra/sp, vsync, eeCycle, dma/gif counters and every EE thread and
blocked sema/evf, independent of AGRESSIVE_LOGS.

## (runs 20-23) "Saving PIRATES data" hang = SNDDRV transfer queue, not MC
MC path works (save written to game/mc0/BASLUS-20365 at 15:21; later boots
Chdir/GetDir=14/Open/Read 4+68/Close all succeed). The stall behind that
notice was the EE->IOP sound upload queue (0x6914C0): SNDDRV.IRX is only
load-emulated, so its completion SIF cmd (handler 0x2D1188 -> 0x2F4BE0)
never arrives; BNKl loader 0x2B9078 polls forever at 0x2B9398. Override of
0x2F45D0 completes queued transfers immediately (BLACKKAT_SND_QUEUE_REAL=1
restores). Real fix: HLE SNDDRV (reply type-2 cmd with the transfer id).

## Front end is FMV-driven
0x1E6C40 (front end/menu) drives MPC movies frame by frame via 0x1E1B00.
Stubbed (no IPU): flag 0 -> 0 (keep playing), flag 1 -> 1. Menu loop runs
(pc 0x1E7800..0x1E7984, ~18 vsync/s) but draws nothing (dma/gif frozen at
393): the menu visuals are the movies. BLACKKAT_FMV_FRAMES=N (untested)
ends each movie after N frames. Needs IPU emulation or a host MPC decoder
replacing 0x1E1B00.

## Front-end menu input / timer-callback pump (2026-11-11, runs 24-26)
- The FMV-driven front end (0x1E6C40) never calls the timer-callback pump 0x2B31E0 itself; with the
  frame decode (0x1E1B00) stubbed, callbacks in the table at 0x7E2900 (pad read 0x2AD758 etc.) only ran
  once per FMV::Start (0x1E1A18). Run 24: a single scePadRead after t=6s.
- Fix: blackkatFmvFrameEnd now dispatches 0x2B31E0(0) on every flag-0 frame (default on,
  BLACKKAT_FMV_NOPUMP=1 disables). Run 25/26: pad reads continue (autopress logged every step),
  CD reads continue at ~8-9/s (streamed audio).
- Still open: menu flags 0x3A3DD4..0x3A3DF0 stay 0 and the menu clip restarts forever; the menu
  input handler (callback s3+0x750 registered at 0x1E71B4) is not in the callback table at t=55s.
  The writers of those flags are 0x1E8028, 0x1E8DA8, 0x1EC8F0, 0x1F04D0.
- The screen keeps showing the last GS frame ("Loading"): dma/gif freeze at 393 at t~6s, while the
  EE runs the menu loop (pc 0x1E7800..0x1E7984 / memset 0x2C33A8 from 0x1E2038).

## IN-GAME reached (2026-11-11 ~15:50 EET, runs 27-28)
Path: boot -> EA logo (skipped) -> Loading -> memcard check -> FMV front end -> main menu drawn
(New Game / Sea Battle / Continue / Options / Scrapbook) -> New Game -> SEQ_01_E.MPC (skipped)
-> level loading screen -> 3D gameplay (Katarina on ship deck, island, sky, sea).

What got it there (all relink-only, src/overrides/blackkat_slus_20365.cpp unless noted):
1. CD shim, heap split, re-entry wrappers, runtime sp patch (earlier sessions).
2. Hand hooks set ctx->pc = ra on entry (scePadPortOpen, EndOfHeap, sceCdCallback, sceCdRead).
3. FMV::PlayFile 0x1E1F88 skipped (no IPU). BLACKKAT_PLAY_FMV=1 = real (hangs).
4. GS CSR FIFO bits forced to "empty" for guest reads (ps2_memory.cpp, patches/ps2recomp-csr-fifo.patch).
5. SNDDRV transfer queue 0x2F45D0 auto-complete (BLACKKAT_SND_QUEUE_REAL=1 = real, hangs "Saving").
6. FMV frame decode 0x1E1B00 stub, now emulating the real control flow:
   abort flag p+0x50 -> return 2; flag!=0 -> 1; else pump timer callbacks 0x2B31E0(0) and run the
   player's per-frame callback p+0x0 (menu logic 0x1E8028), storing its result in p+0x50; return 0.
   THIS made the menu draw and accept input (dma 393 -> 85k+). BLACKKAT_FMV_NOPUMP=1 disables.
7. FMV::Stop 0x1E2038 wrapper: sets abort flag before the audio-stream "state 3" wait that never ends
   (stream side not emulated). BLACKKAT_FMV_STOP_WAIT=1 disables.
8. Diagnostics: PS2X_TICK=N / BLACKKAT_TICK (run:tick, ps2_runtime.cpp), BLACKKAT_AUTOPRESS
   (+_MS/_HOLD_MS/_DELAY_MS; pad 0 injection via scePadRead 0x2C93F0), FMV start/menu-state logs,
   BLACKKAT_RAMDUMP, BLACKKAT_TRACE_FS, BLACKKAT_TRACE_CD. BLACKKAT_FMV_FRAMES=N is NOT needed now.
Defaults (no env vars) include 3-7. Command (repo root, one instance only):
  ./build/linux-release/bin/blackkat game/SLUS_203.65 "/mnt/Media3TB/PS2 GAMES/Pirates - Legend of Black Kat.iso"
Unattended: BLACKKAT_AUTOPRESS=x BLACKKAT_AUTOPRESS_MS=1500 BLACKKAT_AUTOPRESS_HOLD_MS=150 BLACKKAT_AUTOPRESS_DELAY_MS=7000

Known issues in-game (run 28):
- Speed: guest vsync ~2/s in gameplay (vsync 242 @24s -> 298 @52s) vs ~17-22/s in the menu;
  eeCycle ~10M/s (menu ~95M/s). EE pc mostly 0x15BA28 / 0x15B468..0x15B470; vif counter climbing
  (VIF/VU1 path suspected). Main next priority: profile (perf) where host time goes.
- Leftover "Loading" text on the menu (FMV background never decoded, old frame not cleared).
- No FMV video (IPU missing), no sound (SNDDRV/SPU2 stream only load-emulated).

## Gameplay speed (2026-11-11, prof runs 1-4)
Profiler: perf is not installed and ptrace_scope=1, so main.cpp has an opt-in SIGPROF sampler:
BLACKKAT_PROF="start,dur" (1 kHz of process CPU, RIP + 10-frame backtrace) -> blackkat_prof.txt +
blackkat_prof_maps.txt; report with `python3 tools/prof_report.py blackkat_prof.txt blackkat_prof_maps.txt`.
Findings (gameplay, one EE thread at ~100% of a core, everything synchronous on it):
- GS software rasterizer ~55% (GIF -> vertexKick -> GSCpuBackend::DrawTriangle; per-pixel
  SampleTexture bilinear = 4 swizzled reads + CLUT; the 1-page texture "cache" memcpy'd 8 KB per
  page change, ~6%; lerpChannel called libm lround per channel).
- VU1 interpreter ~25-30% (cycle-accurate pipeline model: commitReadyPipelines/calculatePairReadyCycle).
- IOP emulation ~12-16% (std::map walks in IopKernel::beginNextReady/endTimeslice).
- Guest EE code small. 0x15B468 loop = frame limiter polling 0x188F60 (vsync counter 0x330D58, +100
  per vsync via 0x188F48), not a hardware busy-wait. Guest vsync is cycle-paced, so a slow host
  => slow game clock (~4.9M EE cycles per vsync).
Changes (runtime lib only, patches/ps2recomp-gs-vu1-speed.patch):
- lround -> inline round, std::floor -> inline floor; per-row span clipping in DrawTriangle
  (exact per-pixel test kept); VU1 commitReadyPipelines early-out via file-local min-ready hint
  (header untouched -> no full rebuild). Default on.
- Opt-in PS2X_GS_NEAREST=1 (point sampling) and PS2X_GS_TEXCACHE_OFF=1 (direct VRAM texel reads).
Results (vsync/s in gameplay, t=30..58s): baseline 2.0; defaults-only 2.1; + both opt-ins 2.8.
Next: move GS rasterization to a worker thread (or GPU backend), VU1 fast (non-cycle-accurate) path,
IOP ready-queue data structure.

## Real keyboard / gamepad input (2026-11-11)

**Symptom:** on the main menu, holding X on the keyboard did nothing (autopress worked).

**Cause:** `PSPadBackend::readState` (ps2xRuntime `src/lib/ps2_pad.cpp`) was either/or:
when raylib reported gamepad 0 as available it read **only** that gamepad and skipped the
keyboard. On this PC raylib/GLFW enumerates three "gamepads": 0 = "Generic X-Box pad"
(js0, a wireless receiver), 1 = "2.4G XBOX 360 For Windows Keyboard", and 2 = "Logitech G502 X PLUS".
Gamepad 0 always exists, so the keyboard was never read. The guest path (scePadRead stub →
0x2AD758 timer pump → 0x687090 → menu) was fine; the multitap shim and autopress (bound
only when `BLACKKAT_AUTOPRESS` is set) were not involved. IsKeyDown on the EE thread
reads raylib's CORE key array, which the main loop updates in EndDrawing/PollInputEvents. That is fine.

**Fix (relink-only, patches/ps2recomp-pad-input.patch):** the keyboard is always merged in.
Buttons from all available gamepads 0–3 are ORed together. Sticks come from the most deflected
gamepad, with a 0.2 deadzone. KP_ENTER is added as Start.
`PS2X_PAD_EXCLUSIVE=1` restores the old behaviour.

**Debug env:**
- `PS2X_PAD_LOG=1` (or `BLACKKAT_PAD_LOG=1`) logs the detected gamepads and every change of
  kb/gp pressed masks plus data[2..3] and sticks per port.
- `BLACKKAT_FAKEKEY=x[,enter,down,...]` (src/main.cpp) injects key press/release through
  raylib's own GLFW key callback, the same path a physical key uses. Timing is set by
  `BLACKKAT_FAKEKEY_MS`/`_HOLD_MS`/`_DELAY_MS` (default 1500/150/7000).

**Verified:**
- build/input1_run.log (`PS2X_PAD_EXCLUSIVE=1`): 36 injected X presses and kb never changed.
  Attract mode looped and the menu was never left.
- build/input2_run.log (default): `kb=0x4000 -> data[2..3]=ff bf`. Then
  `FMV frame cb 0x1e8028 -> 0x1`, `FMV abort=1`, MC GetInfo, and
  `FMV skipped: "SEQ_01_E.MPC"` (New Game started).

Keys: arrows/WASD = d-pad, X/Space = Cross, C = Circle (Esc also closes the window), Z = Square,
V = Triangle, Q/E = L1/R1, LShift/RShift = L2/R2, Enter = Start, Tab = Select.
