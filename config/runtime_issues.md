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

## Gameplay graphics vs the speed patch (2026-11-11, after cf98cfa)

The question was whether the broken water and terrain (teal shards, torn cliffs) came from the speed patch.
**Answer: no.** All runs below reached the same ship-deck scene (New Game, about 30–58 s), with
`BLACKKAT_FAKEKEY=x`. Shots are in build/shots/ (copies in ~/blackkat_shots/).

| run | config | look | vsync/s (t 30→59) |
|---|---|---|---|
| a | defaults (speed patch on) | same as c | 2.06 |
| b | `PS2X_GS_NEAREST=1 PS2X_GS_TEXCACHE_OFF=1` | same geometry, blockier textures | 2.83 |
| c | `PS2X_VU1_NOHINT=1 PS2X_GS_NOSPAN=1 PS2X_GS_NOFASTMATH=1` (= pre-speed code) | reference | 1.85 |
| d | b + Z clamp/precision fix | same as b | 2.90 |
| e | defaults + pixel probe | same as a | 1.99 |

Runs a, c and e show the same geometry, draw order and teal patches at the bottom right.
Camera timing differs between runs, so the comparison is visual, not per pixel.
Code review agrees:
- The span bounds are the exact edge functions, widened by one pixel, and the per-pixel test is unchanged.
- The VU1 hint is only ever lowered at each readyCycle insertion and is recomputed after every full scan.
- `x+0.5` equals `lround` and `fastFloor` equals `floor` on the ranges used.
- TEXCACHE_OFF calls the same per-PSM read functions without the page cache.

NEAREST is a visual trade-off: it makes magnified textures, such as water and cliffs, look blocky.

New opt-in debug tools:
- `PS2X_SHOT=30,40,50` saves the presented frame as PNG at those seconds. Set the file prefix with `PS2X_SHOT_PREFIX=dir/name`.
- A/B switches that bring back the pre-speed code paths at runtime: `PS2X_VU1_NOHINT=1`, `PS2X_GS_NOSPAN=1`, `PS2X_GS_NOFASTMATH=1`.
- `PS2X_GS_PROBE="x,y;x,y"` with `PS2X_GS_PROBE_T=sec` logs every pixel write at those framebuffer coordinates for 0.3 s: prim, TEST, ALPHA, TEX0, Z and RGBA.
- `PS2X_GS_ZSTATS=1` logs Z buffer formats and how many Z values fall outside the format's range.

Z correctness fixes (kept; no visible change in this scene):
- Packed XYZ2/XYZF2/XYZ3/XYZF3 Z was stored via float, which loses bits above 2^24 on Z32. It is now stored as double.
- WritePixel now clamps Z to the Z buffer format's maximum before the test and write (Z24 0xFFFFFF, Z16/16S 0xFFFF), as the GS does. `PS2X_GS_ZCLAMP_OFF=1` turns this off.
- ZSTATS showed no over-range Z in this scene. The main 3D pass uses Z16S at 0x3A, zbp 0xD2, with max Z 0xFFFF.

Probe findings for the terrain and water pixels:
- Every pixel gets two passes with ALPHA = (Cs−Cd)·As+Cd.
  - Pass 1: TEX0 0x2FC0 (CT32, 256², tcc=0), TEST 0x30000 = ZTE on, **ZTST=ALWAYS**.
  - Pass 2: TEX0 0x3660 (CT16, 128², tcc=1), TEST 0x50000 = ZTST GEQUAL.
- The base terrain pass is therefore not depth-tested at all: whatever draws last wins. Draw order and correct
  primitive culling matter, not the Z buffer.
- Next suspects for the sea-battle artefacts (not reachable inside 60 s): VU1 clipping and culling of
  off-screen vertices (CLIP flag / ADC), and the 16-bit Z16S layout. Probe a sea-battle frame next.

## GS rasterizer on a worker thread (2026-11-11, patches/ps2recomp-gs-thread.patch)

`GSThreadedBackend` lives in gs_frontend.cpp, wrapping GSCpuBackend. It is on by default; `PS2X_GS_SYNC=1` gives the old single-thread path.
- **Queued in order on one worker:** Submit, LoadClut, BeginTransfer (all directions, including the
  local→host fill), UploadImage (data copied), TextureFlush and WriteVram.
- **Drain first (wait for everything queued before the call):** Flush, Sync (FINISH, presentation, reset,
  debug), Present, ClearFramebuffer, ConsumeLocalToHostBytes (BITBLT readback), ReadVram,
  SnapshotVram, Reset and Initialize.
- **No drain:** GetTransferSnapshot, which is debug only.
- No header changes.
- `PS2X_GS_THREAD_STATS=1` logs drains per reason, how many waited, and the wait time.

Results (gameplay vsync/s, t≈30→59 s, `BLACKKAT_FAKEKEY=x`):

| config | sync (old) | threaded |
|---|---|---|
| defaults | 2.06 (run a) | 2.33 (+13%) |
| TEXCACHE_OFF | not measured alone; 2.83 with NEAREST too | 2.67 |

- Shots f_thread_* and g_thread_tcoff_50s match the sync shots: same scene, no visual difference.
- **Bottleneck is now the rasterizer itself.** Stats: about 5 FINISH drains per vsync, and the EE waited about 21 s of 60 s on them.
- **Tried and rejected: skipping the drain on FINISH.** Run h_thread_nofinish:
  - The EE ran millions of commands ahead and the host present stalled for about 43 s.
  - Frames showed exactly the reported corruption: floating hull slabs and teal terrain shards.
  - Kept the drain. Hypothesis to check: some of Stelios's in-game corruption may be ordering between FINISH,
    present and VRAM state, not geometry.
- **Next steps for real speed:**
  - Split rasterization across N workers by screen tiles or scanline bands.
  - Run VU1 and GIF ahead with a bounded queue, only once present is double-buffered.

## Banded multi-thread rasterizer (2026-11-11, patches/ps2recomp-gs-bands.patch, on top of gs-thread)

**Design:**
- Worker k owns framebuffer rows with (y/16) % N == k. Each primitive goes to all N workers, and each
  submits it once per owned 16-row band with the scissor narrowed to that band.
- Triangle and sprite interpolation use the unclipped vertices, so pixels are identical to a single
  thread. Lines and points are rejected per pixel by the scissor.
- Each worker has its own GSCpuBackend (texture page cache, CLUT) on the shared VRAM.
- LoadClut and TextureFlush are sent to every worker in stream order.
- **Barriers:** BeginTransfer, UploadImage and WriteVram are queued barriers. All workers arrive, the last one
  executes the operation on worker 0's backend, then every texture cache is flushed.
- **Hazard barriers:** a textured draw whose TEX0 range overlaps frame or Z memory written since the last barrier gets a
  barrier first (render-to-texture across bands). So does a CLUT load whose CBP overlaps that memory.
  Ranges are conservative, rounded to 8 KiB pages plus one page.
- ClearFramebuffer drains everything, then flushes the caches.
- All earlier drain points stay: FINISH, present, readback, ReadVram and snapshots.

**Settings:** N = min(4, hw threads/2), which is 4 on this 16-thread CPU. `PS2X_GS_THREADS=N` overrides it,
`PS2X_GS_THREADS=1` gives the single-worker path, and `PS2X_GS_SYNC=1` gives no worker at all.

**Results** (gameplay, t≈30→59 s, `BLACKKAT_FAKEKEY=x`):

| config | vsync/s | CPU (user+sys over 60 s) |
|---|---|---|
| PS2X_GS_THREADS=1 (previous build's behaviour) | 2.39 | ≈1.2 cores |
| 4 bands, defaults | 4.27 (+79%) | ≈2.0 cores |
| 4 bands + PS2X_GS_TEXCACHE_OFF=1 | 4.33 | — |

- Shots i_thr1_*, j_thr4_* and k_thr4_tcoff_* show no band seams and look the same as the single-thread
  ones. The faster runs get further into the scene in 60 s, so the camera differs.
- Barriers per 60 s: 13k with 1 worker, about 127k with 4 (mostly hazards) and 243k with TEXCACHE_OFF.
- FINISH drains still cost about 5 s of EE wait per run.

The earlier note on the no-FINISH-drain experiment needs correcting. Its odd frame may partly have been a
later scene, since the game had run further ahead. The 43 s present stall was real.

## VU1 fast mode, first step (2026-11-11, patches/ps2recomp-vu1-fast.patch)

**Status:** opt-in. `PS2X_VU1_FAST=1` turns it on. The built-in default is `kVu1FastDefault=false`;
`PS2X_VU1_ACCURATE=1` forces the accurate path.

**What it changes:** only XGKICK. The whole PATH1 packet is transferred and submitted at issue, instead of
one qword per 2 cycles. PATH1 order, flags, Q/P, stalls and E-bit handling are untouched.

**Result:** 4.34 vsync/s against the 4.27 baseline, which is noise. Shot l_vu1fast_50s matches j_thr4_50s, and the run was stable.

**Conclusion:** kick pacing is not the VU1 cost. The cost is the deferred-write design itself:
- Every pair copies old and new VF/ACC/VI values.
- Every result is queued into a pipeline, then committed later.
- calculatePairReadyCycle checks stalls on every pair.
- advanceTo steps one cycle at a time.

A real fast mode needs a separate straight-through executor:
- Write VF/VI/ACC directly. Hardware stalls dependent reads, so the values match.
- Keep only the Q/P and MAC/status/clip flag delay queues, plus the branch VI delay.
- Run without per-cycle stepping.

That is a larger rewrite of the pair loop in ps2_vu1_core.cpp, not a relink tweak. Not started.

### VU1 straight-through executor (now the default)

**Status:** on by default (`kVu1FastDefault=true`). `PS2X_VU1_ACCURATE=1` reverts to the cycle-accurate VU1;
`PS2X_VU1_FAST=1` forces fast. Only VU1 is affected, not VU0.

**What changes:**
- Upper and lower results (VF/ACC/VI) are left in the register file as executed. There is no deferred
  VF/VI/ACC write queue, no restore/requeue copies, no markPairWrites, and no VF/VI/ACC stall scan.
- The only stalls left are FDIV/EFU resource use and WAITQ/WAITP. Q/P still go through their latency queues,
  so DIV/EFU results appear after the proper cycles, counted as one cycle per pair.

**What is kept:** the in-pair shadow (lower sees upper's old dest), the I-bit, MAC/status/clip flag pipelines,
the store pipeline, the branch VI backup, branch delay slots, the E-bit plus its delay slot, and the D/T halts.
XGKICK is immediate, in order.

**Decode cache:** none added, since getDecodedInstructionPairForPc already caches per pc.

**Results** (gameplay vsync/s, t≈30→59 s):

| run | vsync/s |
|---|---|
| baseline, accurate | 4.27 / 4.34 |
| m_vu1exec (FAST=1) | 5.37 |
| n_vu1default (default build) | 5.03 |

That is about +18–25%.

**Visual check:**
- n_vu1default_50s matches the accurate j_thr4_50s view almost pixel for pixel.
- m_vu1exec_57s matches the accurate h_thread_nofinish_57s view of the same later scene.
- The floating hull and flag slabs in the sky appear in both VU1 modes, so they are pre-existing.
- No crashes and no reserved-instruction reports.

## Broken in-game geometry: FIXED (2026-11-15, patches/ps2recomp-geometry-fix.patch)

Applies on top of gs-ab-shot-probe-z, gs-thread, gs-bands and vu1-fast (same files, relink-only).

**Symptom:** teal water/terrain "shards", cliffs where the sea should be, floating hull/flag slabs, and
(after the first fix below) fan-shaped stretched triangles and horizontal streaks across the water.

**Root cause 1: DMA chain tag cap (ps2_memory.cpp).** Source-chain DMA gathering stopped after a hard
`kMaxChainTags = 4096`. The game's VIF1 display list for a gameplay frame is ~1.7 MB with thousands of REF
tags, so everything after tag 4096 was silently dropped: the sea surface (reflection-mapped, tex 0x2BC0 =
the 256x256 reflection rendered at the start of the frame), fish/reef overlays etc. never reached VU1/GS.
What we saw as "water shards" was the teal-tinted underwater terrain (sunken reefs) with no sea over it.
The cut also left a dangling UNPACK at the end of the buffer (`[vif:drop]`, 36-94 per minute).
Fix: cap raised to 1<<20 tags (`PS2X_DMA_MAXTAGS=N` overrides, logs `[dma] ... hit tag cap` if reached).
After the fix: 0 dropped unpacks.

**Root cause 2: VU1 fast mode had no data-hazard stalls (ps2_vu1_core.cpp).** Once the sea was drawn,
fast mode showed fans/streaks: sea vertices behind the camera (24-bit Z wrapped to ~0xFC0000) were not
culled. The sea microprogram culls with CLIP + FCAND -> ADC bit. Without stalls the pair loop ran fewer
cycles between CLIP and FCAND than the hardware does, so FCAND read clip flags that were still in the
4-cycle flag pipeline. Evidence: ADC kicks on the sea run 1601/2606 in fast mode vs 2035/2202 accurate;
accurate mode looked right. Fix: fast mode now uses the existing cheap ready-cycle scan
(calculatePairReadyCycle + markPairWrites) for stall timing; values are still written straight into the
register file (no deferred-write queues). ADC now 2049/2216, zero out-of-range-Z triangles.
`PS2X_VU1_FAST_NOSTALL=1` restores the old stall-free fast mode.

**Also corrected (no visible change in this scene, matches PCSX2 Vif_Unpack.cpp):** VIF1 UNPACK
V2 writes x,y,x,y; V3 takes W from the next stream element; V4-5 expands 5-bit channels (<<3, A->0x80);
MODE 3 sets ROW = data. `PS2X_VIF_UNPACK_LEGACY=1` restores the old behaviour.
Not done: VIF0 UNPACK still only handles 32-bit (vl=0) formats; generated code for VCALLMSR (0x2B47DC)
reads `ctx->vi[27]` out of bounds instead of CMSAR0 (function has no static callers; override if ever hit).

**Speed (gameplay vsync/s, t=30 -> ~56 s, BLACKKAT_FAKEKEY=x):**
| build | vsync/s | note |
|---|---|---|
| before (n_vu1default) | 5.0 | ~half the frame missing |
| fixed, PS2X_VU1_ACCURATE=1 (w_acc) | 3.6 | full frame |
| fixed, default fast (x_fast) | 4.2 | full frame |
The drop is the extra geometry now drawn (2x ~2200-tri sea passes, overlays), not the fixes themselves.

**Shots:** before build/shots/n_vu1default_{30,40,50,57}s.png, after build/shots/x_fast_*.png
(copies in ~/blackkat_shots/). Accurate reference: ~/blackkat_shots/w_acc_56s.png.

**New debug env (all off by default):**
- `PS2X_GS_DRAWLOG=sec[,dur]`: log draw runs (`[gs:draw]` with prim/fb/zbuf/tex/test/alpha/scissor,
  bbox, z range, ADC count) plus `[gs:huge]` for triangles with Z > 0xFFFF.
- `PS2X_GS_DUMPRUNS=a,b,...` (with DRAWLOG): dump fb + Z (PPM/PGM) to build/shots/dr_<run>_* before
  those runs (slow, debug only).
- `PS2X_VIF_STATS=1`: UNPACK format histogram + `[vif:drop]` details. `PS2X_VU1_STATS=1`: VU1 runs cut
  by the cycle budget (0 seen).

## GPU GS backend (stage one): opt-in, `PS2X_GS_GPU=1`

Status: works for the opening ship-deck scene. The CPU rasterizer stays the default and the fallback: unset the variable, or let it fall back automatically if EGL/GL init fails.
Patch: `patches/ps2recomp-gs-gpu.patch`. Apply it on top of `ps2recomp-geometry-fix.patch`.

**Files (runtime submodule):**
- `src/lib/gs/gs_gpu_backend.inl` (new): the whole backend. It is `#include`d at the end of `gs_frontend.cpp`, so no CMake change or reconfigure is needed.
- `src/lib/gs/gs_frontend.cpp`: `makeDefaultRasterBackend()` selects the GPU backend when `PS2X_GS_GPU=1`. The GPU backend is not wrapped in the band threads.
- `include/runtime/gs/gs_cpu_backend.h` / `.cpp`: two public helpers, `DecodeTextureRgba()` and `ClutHash()`.
  - This header is only included by gs_frontend.cpp and gs_cpu_backend.cpp, not from `ps2_runtime.h`, so the change is relink-only.

**Design:**
- One GL thread with a surfaceless EGL context. libEGL is dlopen'ed and GL entry points come from eglGetProcAddress, so nothing new is linked.
- All backend calls are queued in order. Calls with observable results wait: Present, ReadVram, SnapshotVram, ClearFramebuffer and local->host.
- An internal `GSCpuBackend` on the same VRAM handles transfers, CLUT loads, texture decode, Present (display conversion) and local->host.
- Render targets: one 1024x1024 RGBA16F texture per FRAME.fbp, with alpha stored as A/128.
- Depth: one DEPTH32F texture per ZBUF.zbp, holding z/2^32 clamped to the format max.
- Blending: dual-source, so As, Ad and FIX all work as GS blend factors, and the ABCD modes map to GL blend func/equation.

**Implemented:**
- Points, lines, triangles, strips, fans and sprites, with Gouraud or flat colour.
- TEX0 for CT32/24/16/16S and T8/T4/T8H/T4HL/T4HH via CLUT, decoded on the CPU into an RGBA8 texture cache.
  - The cache is keyed by TEX0, TEXA and a CLUT hash, and checked against per-8KB-page generation counters.
- TFX modulate/decal/highlight/highlight2 and TCC.
- Fog.
- The ABCD alpha blend modes.
- Alpha test (all ATST values; AFAIL done as a second pass).
- DATE/DATM.
- Z test and write, with format max.
- Scissor.
- FBMSK per channel, plus the special case 0x80 on the alpha byte (preserve the MSB).
- FBA.
- CT16 1-bit alpha.
- REPEAT/CLAMP wrap modes.
- Bilinear filtering when TEX1 asks for it.

**Coherency (simple and correct, not fast):**
- A target is uploaded VRAM->GPU when it is created or stale.
- A target is resolved GPU->VRAM (glReadPixels + swizzled write) before:
  - Present;
  - ReadVram or SnapshotVram;
  - any transfer whose source or destination pages overlap it;
  - any texture or CLUT read from its pages. This covers render-to-texture, e.g. the sea reflection: fb 0x46 to tex 0x8C0 to fb 0x15E to tex 0x2BC0;
  - local->local copies.
- CPU-side VRAM writes mark overlapping targets stale.

**Speed** (same ship-deck scene, vsync/s between t~30 s and t~56 s, `BLACKKAT_FAKEKEY=x`):
- CPU rasterizer (default, 4 band threads): 4.1 vsync/s.
- GPU: 4.6–4.7 vsync/s.
- With the GPU backend, the GS work takes about 11% of one thread. The game thread (EE, VIF, VU1 and the GS frontend, all inline) is pegged at 100%. **The GS is no longer the bottleneck;** further speed has to come from the game thread.

**Known gaps / approximations (each is logged once as `[gs:gpu] approximated: ...` when it is hit; none were hit in the ship scene):**
- Region CLAMP/REPEAT (wrap modes 2/3) are treated as CLAMP/REPEAT, ignoring MINU/MAXU/MINV/MAXV.
- PABE is ignored.
- The blend coefficient `1+f` (rare ABCD combinations) is approximated as 1.
- Partial alpha FBMSK values other than 0x00, 0x80 and 0xFF are handled as all or nothing.
- Partial RGB FBMSK is per channel only: a channel is written unless its whole byte is masked.
- COLCLAMP=0 (wrap) is not emulated. Blend results above 1.0 are kept in the float target until a resolve clamps them.
- DATE and alpha-MSB preservation read a snapshot of the target taken before each batch, so overlapping primitives inside one batch see stale destination alpha.
- Depth is never resolved to VRAM, so the CPU never sees GPU Z. Z used as a texture, Z readback or Z-to-colour aliasing would be wrong.
  - New depth buffers start cleared to 0 instead of loading VRAM.
- Render-to-texture always goes through a CPU resolve and re-decode (no GPU-side copy yet). Local->local moves and readback also go through the CPU.
- Targets are capped at 1024x1024. Frame widths over 1024 px are clipped.
- Pixel convention copies the CPU rasterizer (integer XYOFFSET, sample at pixel centre), not exact GS hardware.
- AA1, dithering (DTHE), SCANMSK and mipmapping (MXL > 0) are not implemented.
- `PS2X_GS_DRAWLOG` pixel probes still work, but they cost one queue round trip per ReadVram.

**Env:**
- `PS2X_GS_GPU=1`: enables the GPU backend.
- `PS2X_GS_GPU_STATS=1`: prints `[gs:gpu:time]` / `[gs:gpu:stats]` every 5 s (worker busy %, producer waits, resolves, uploads, texture decodes).
- `PS2X_GS_GPU_GLERRORS=1`: checks glGetError after each batch.
- `PS2X_GS_GPU_DEFAULT_DISPLAY=1`: uses eglGetDisplay(EGL_DEFAULT_DISPLAY) instead of the surfaceless Mesa platform.

**Stage two ideas:**
1. Resolve less. Copy GPU->GPU when TEX0 matches a target (sample the FBO texture directly with a format/offset view) instead of resolve + decode. Resolve only the displayed buffer at Present.
2. Present straight from the GPU target (share or blit into raylib's texture) to skip glReadPixels and the CPU display conversion.
3. Decode textures on the GPU: upload the raw VRAM pages and deswizzle plus CLUT in a shader.
4. Implement region clamp, PABE, the exact AFAIL/partial FBMSK paths and depth resolve.
5. The real speed limit is now the game thread: profile and parallelise VU1/VIF, e.g. run VU1 microprograms on their own thread with GIF output queued. Done, see the next section.

## VIF1/VU1/GIF worker thread (2026-11-17, patches/ps2recomp-vu1-thread.patch, default ON)

Patch: `patches/ps2recomp-vu1-thread.patch`, on top of `ps2recomp-gs-gpu.patch`. Runtime .cpp files plus a
private header (`src/lib/ps2_vu1_async.h` / `.inl`, included only by runtime .cpp files), so relink-only.
**Revert switch: `PS2X_VU1_THREAD=0`** (the old fully synchronous path; no worker thread is started).

**Why.** Game-thread profile before (GPU GS, gameplay, 20 s): VU1 interpreter + VIF1 ~63%, IOP emulation ~28%
(IopKernel std::map walks, run from EeScheduler::checkpointDue), guest EE code + GS frontend the rest.

**Design.**
- processPendingTransfers() no longer runs VIF1 (UNPACK/MPG/MSCAL -> VU1 -> XGKICK) and PATH3 GIF DMA on the EE
  thread. It copies the data (chains are already gathered at kick time) into one job per kick and returns.
  One worker thread ("VU1Worker") runs jobs in kick order (GIF before VIF1 inside one batch, as before), so
  PATH3 vs PATH1/PATH2 order into the GS is unchanged. VIF0 and VU0 macro mode stay on the EE thread.
- While a job is in flight its channel keeps CHCR.STR=1. On finish the worker posts an EE scheduler Dmac event;
  the EE thread then publishes the completion like the synchronous path did (STR clear, QWC=0, D_STAT CIS,
  DMAC cause -> handlers). VPU_STAT VU1 D/T stop bits are applied on the EE thread at completion; FBRST
  (DE1/TE1) is snapshotted at kick.
- EE accesses that need the worker's state drain it first: VU1 code/data memory, VIF1 registers
  (0x10003C00..) incl. STAT/MARK/TOP/ITOP/FBRST, VIF1 FIFO writes, GIF control (0x10003000..), GS privileged
  registers (CSR FINISH/SIGNAL, DISPFB...), the native GIF chain shortcuts, and direct GS writes / GIF packets /
  readback from any non-worker thread. D_STAT and GIF/VIF1 channel registers just publish finished jobs.
- **GIF inline fast path:** a GIF-only kick while the worker is idle runs synchronously on the EE thread
  (nothing queued can be overtaken). Without it the front end (tens of thousands of tiny scratchpad PATH3
  sprite/text packets per second) ran at ~4 vsync/s, cycled the attract FMVs and never accepted X on the
  title menu. `PS2X_VU1_THREAD_GIF_INLINE=0` sends every GIF kick to the worker (debug only).
- Backpressure: at most 32 jobs in flight (`PS2X_VU1_THREAD_MAXJOBS`). The banded CPU rasterizer queue is
  now bounded too (`PS2X_GS_QUEUE_MAX`, default 8192 commands, 0 = unbounded; 1024 cost ~15% in sync mode).

**Results** (opening ship-deck scene, `BLACKKAT_FAKEKEY=x BLACKKAT_FAKEKEY_HOLD_MS=700 PS2X_TICK=60`, t=30..56 s,
`python3 build/vt_rate2.py <log>`; vifw/s = VIF register writes per second, which equals vsync/s in sync
mode, used as a rough game-frames proxy):

| GS backend | VU1 thread | vsync/s | vifw/s (~frames/s) |
|---|---|---|---|
| CPU | off (`PS2X_VU1_THREAD=0`) | 4.04 | 4.00 |
| CPU | on (default) | 19.3 | 5.15 |
| GPU | off | 4.54 | 4.54 |
| GPU | on | 21.1 | 6.0-6.15 |

- Guest vsync is EE-cycle paced. With the thread on the EE spins on CHCR/STAT while VU1 works (like real
  hardware), so EE cycles and vsyncs advance ~5x faster: game time runs ~4-5x faster (the mission HUD is
  reached much sooner), the drawn frame rate gains ~30-35%. If a game ever misbehaves from the extra vsyncs
  per frame, `PS2X_VU1_THREAD_DRAIN_ON_POLL=1` makes a CHCR poll wait for the worker instead.
- Stability: 7 runs of 60 s and one of 90 s (CPU and GPU GS) boot -> menu -> New Game -> mission with no
  errors; RSS flat at ~255-265 MB; visuals match the synchronous path.
- Profile with the thread on (CPU GS, 20 s): VU1Worker ~91% VU1 interpreter (execUpper, calculatePairReadyCycle,
  commitReadyPipelines, normalizeFmacResult), ~9% GS frontend; worker busy 90-98% = the new bottleneck. The EE
  thread is ~84% IOP emulation (more EE cycles -> more IOP cycles) plus guest wait loops.

**Env:** `PS2X_VU1_THREAD=0` (off), `PS2X_VU1_THREAD_STATS=1` (`[vu1thread:stats]` every 5 s),
`PS2X_VU1_THREAD_TRACE=1` (top kick shapes), `PS2X_VU1_THREAD_SYNC=1` (worker but wait for each job),
`PS2X_VU1_THREAD_INLINE=1` (jobs inline, hooks active), `PS2X_VU1_THREAD_DRAIN_ON_POLL=1`,
`PS2X_VU1_THREAD_MAXJOBS=N`, `PS2X_VU1_THREAD_GIF_INLINE=0`, `PS2X_GS_QUEUE_MAX=N`. `[run:tick]` also prints
`dispfbw=` (EE DISPFB writes; 0 in this game, it does not flip through those paths).

**Known limits:** VU1 reset/start through VU0 macro CTC2 (FBRST/CMSAR1 from the EE side) is not drained;
VPU_STAT read through CFC2 sees the last completed job. Only the ship-deck scene and the front end were tested.

**Next:** 1) make the VU1 interpreter cheaper (fast mode still runs the pipeline model; a per-microprogram
block cache / VU1 recompiler is the big one); 2) split the GS frontend (GIF parse + vertexKick) off the worker;
3) cut IOP cost (IopKernel std::map walks, or run the IOP on its own thread / at a fixed rate instead of per
EE cycle, since spinning EE cycles now drive it).

## VU1 interpreter speed-up (patches/ps2recomp-vu1-jit.patch, default on, `PS2X_VU1_JIT=0` reverts)

With the VU1 worker thread the worker was 90-98% busy and ~91% of it was the VU1
interpreter (self time: calculatePairReadyCycle 25%, commitReadyPipelines 16%, FMAC
exact-flag math ~15%, normalizeOperand 6%). Hot programs (`PS2X_VU1_PROFILE=1`):
start pc 0x2c18 = 45% of pairs (~1282 pairs/run, ~2000 runs/s), 0x1ee0 17%, 0x2460 7%,
0x1318 5.5%; 19 programs / 1576 distinct pairs, top-64 pairs = 60%; ~44% of VU cycles are stalls.

Chosen approach: keep the exact fast-mode cycle model (clip-flag stall timing, XGKICK at
issue, E-bit/delay slots, Q/P pipeline, MAC/status/clip flags, integer-branch delay) and make
the interpreter cheaper instead of writing a translator:
- decoded pair used by reference from the decode cache (no per-pair struct copy);
- ready-cycle / write-marking scans iterate set bits (ctz) instead of all 32 regs/lanes;
- fast mode: vf/vi/acc write pipelines are never used on VU1, so their commit loops are skipped;
- fast FMAC flags: when the float result is provably normal and not near over/underflow
  (exponent 3..252; product-sum ops also need |r|*2^20 >= |acc|+|r|), flags are just the sign
  and the long-double "exact" path is skipped (`PS2X_VU1_FLAGCHECK=1` compares both: 0
  mismatches over 553M lanes; `PS2X_VU1_EXACTFLAGS=1` forces the exact path);
- advanceTo() jumps over stall cycles where nothing can commit (commit hint) and no XGKICK runs;
- commit hint globals are now thread_local (VU0 on EE thread vs VU1 on worker could race).

Speed (60 s run, in-game, t=30-56 s, thread on): CPU GS vifw/s 5.15 -> 7.28 (+41%, worker now
~78% busy, GS becomes the limit); GPU GS 6.15 -> 10.75 (+75%, worker 96%). Thread off:
CPU 4.00 -> 4.92, GPU 4.54 -> 6.30. `PS2X_VU1_JIT=0`: 6.22 (CPU; the pure refactors remain).
Visuals (sea, terrain, ship) match the baseline; `PS2X_VU1_ACCURATE=1` still runs.
Next steps: translator/block cache for 0x2c18/0x1ee0/0x2460, per-block flag liveness
(skip MAC/status work when no FMAND/FSAND/... reads them before overwrite), splitting the GS
frontend off the VU1 worker.

## VU1 block cache + GS stage (patches/ps2recomp-vu1-blockcache.patch, default on)

Revert switches: `PS2X_VU1_BLOCKCACHE=0` (VU1 part, also implied by `PS2X_VU1_JIT=0`),
`PS2X_GS_STAGE=0` (GS stage). Validation: `PS2X_VU1_BC_CHECK=1` runs every VU1 program twice
(copy of the interpreter on the reference path with XGKICK only hashed, VU data restored, then
the real run with the block cache) and compares VF/VI/ACC/Q/P/I/R, MAC/status/clip, pc, cycles,
control state, VU data memory and the XGKICK packet stream. `PS2X_VU1_BC_BREAK=1` perturbs one
lane on purpose to prove the checker fires. `PS2X_VU1_BC_STATS=1` prints translation/skip counts.

VU1 block cache (ps2_vu1_core.cpp only, header untouched):
- per micro-memory image translation, keyed by a 64-bit FNV hash of the code (reused when the
  same microcode is uploaded again; this game uploads one 2048-pair image, translated once);
- FMAC flag liveness: an FMAC MAC/status entry is dead when the next flag-writing FMAC on the
  straight-line path (<= 8 pairs, no branch/E-bit/D/T in between, not a delay slot) overwrites it
  and no FSEQ/FSAND/FSOR/FMEQ/FMAND/FMOR/FSSET can run before that entry commits (window to
  j+3 pairs; program end is fine because everything flushes in order). Dead entries are not
  queued; their sticky bits are carried into the next entry. Runtime guard: only with >= 1024
  cycles of budget left. 553 of 685 flag-writing FMACs are dead (~15 M skipped entries/s);
- specialised FMAC upper handler (ADD/SUB/MUL/MADD/MSUB bc/q/i/vector, VF or ACC dest) when all
  written lanes are in the fast-flag range; anything else falls back to execUpper unchanged;
- flag/store pipeline commits visit only valid slots (thread_local valid masks per instance,
  same slot order), XGKICK qword copy by memcpy, 64 KB XGKICK buffer not re-zeroed per program,
  decoded-cache currency checked once per run.
Check results: 0 mismatches over 240k (GPU), 160k (CPU) and 100k (thread off) compared runs.

GS stage (ps2_vu1_async.h/.inl, ps2_memory.cpp): on the VU1 worker, GIF arbiter submit/drain
calls (GIF parsing + GS backend) are copied into an ordered queue run by a "GSStage" thread;
the PATH3 mask logic stays on the worker; the worker waits for the stage at the end of each
job, so DMA completions are still published after their GS work (EE drains unchanged). The
stage thread counts as "worker" for drainIfOffWorker. Gains: CPU GS +11% (frontend/CPU raster
setup was ~40% of a core), GPU GS neutral (frontend only ~5% of the worker there).

Speed (vifw/s, t=30-56 s, thread on): CPU GS 7.28 -> 8.78, GPU GS 10.75 -> 13.44; thread off:
CPU 4.92 -> 5.23, GPU 6.30 -> 7.27. Worker is still ~95% busy with GPU GS (VU1 run loop,
ready-cycle scan, generic execUpper for MAX/MINI/FTOI/CLIP/OPMSUB, execLower).
Next: per-pair precomputed stall read lists / fused run loop, specialised lower handlers
(LQ/SQ/IADDIU/branches), CLIP/MAX/MINI/FTOI fast handlers, batching GS stage ops (CPU GS stage
is now ~40% busy and the worker waits on it), the EE thread's IOP emulation
(IopKernel::beginNextReady iterates a std::map: ~30% of the EE thread).

## VU1 run loop + GS stage batching + IOP ready queue (patch: ps2recomp-vu1-loop-iop.patch)

Applies on top of `ps2recomp-vu1-blockcache.patch`. No public header changes (only private `iop_kernel.h`, 5 IOP TUs).

- **VU1 loop** (`ps2_vu1_core.cpp`): each cached translation stores per-pair stall read/write slot lists
  (`Vu1PairStall`), so `pairReady`/write marking no longer decode usages every pair. Fast upper handlers
  for MAX/MINI, ITOF/FTOI, ABS, CLIP and special NOP. Revert: `PS2X_VU1_LOOP=0`.
- **GS stage** (`ps2_vu1_async.inl`): GS ops are batched (byte arena, pooled batches, default 32 ops / 256 KB)
  and the job completion is posted by the stage itself (pipelined), so the VU1 worker no longer waits for the
  stage at job end. EE-visible order unchanged (job completes after its GS work, FIFO).
  Revert: `PS2X_GS_STAGE_PIPE=0` (wait at job end), `PS2X_GS_STAGE_BATCH=1`, `PS2X_GS_STAGE=0`.
- **IOP** (`iop_kernel.cpp/.h`): `beginNextReady` uses a cached id-ordered thread vector and one pass
  (wake due Delay threads + pick lowest priority, then lowest id) — same order as before.
  `nextWakeCycle` uses the list; `cleanupDeadThreads` early-outs. Revert: `PS2X_IOP_FASTSCHED=0`.

Measured (t=30–56 s, VU1 thread on), VIF writes/s | vsync/s:
- GPU GS: 13.44 | 19.77 (02c73e5) -> 15.59 | 28.26
- CPU GS: 8.78 | 14.09 (02c73e5) -> 9.44 | 19.90
- In-game reached at ~12 s instead of ~15 s.
- `PS2X_VU1_BC_CHECK=1`: 0 mismatches (CPU 240k runs, GPU 280k runs).
- CPU GS worker wait for the stage: 640 ms/5 s (pipe off) -> 0 ms (pipe on).

## VU1 zero-result / lower-op fast paths + IOP fast paths (patch: ps2recomp-vu1-lowerops.patch)

Applies on top of `ps2recomp-vu1-loop-iop.patch`. No public header changes (private `iop_memory.h` /
`iop_kernel.h` only, ~15 IOP TUs).

- **VU1 FMAC zero results** (`ps2_vu1_core.cpp`, fast upper): MUL with a zero operand, ADD of opposites,
  SUB of equals, and MADD/MSUB with a zero product are handled inline (same signed zero and Z/S flags,
  product sticky Z|S, as the long-double path). These were ~14% of all upper ops and all fell back to
  execUpper; generic uppers are now ~0.004%. Revert: `PS2X_VU1_FASTZERO=0`.
- **VU1 lower ops**: inline copies of NOP, LQ/SQ, LQI/SQI, MOVE, IADDIU/ISUBIU, IADD/IADDI/IAND/IOR,
  IBEQ/IBNE in the run loop (everything else -> execLower). Revert: `PS2X_VU1_FASTLOWER=0`.
  OPMSUB/OPMULA turned out to be ~0% of executed uppers here, so they are not specialised.
- **IOP** (`iop_emulator.cpp`, `iop_cpu.cpp`, `iop_memory.h`, `iop_kernel.*`): the EE runs the IOP for
  ~1 IOP cycle per EE dispatch (avg 3.8 IOP instructions per timeslice), so per-instruction and
  per-slice overhead dominated. Added: inline `jr ra` pre-check before the import-stub decode, inline
  DMA-start flag check, a due-cycle guard (`dmaMinDue`, only ever stale-low) for the pending DMA
  interrupt map scan, inline RAM fetch (`IopMemory::fetch32`), and a dead-thread flag so
  `cleanupDeadThreads` skips its scan. All produce the same result as before. Revert: `PS2X_IOP_FASTPATH=0`
  (the dead flag follows `PS2X_IOP_FASTSCHED=0`).
- **Experimental, default off:** `PS2X_IOP_QUANTUM=N` runs the IOP only once N IOP cycles are owed.
  N=32 gave GPU vsync/s 36.7 -> 46.6 and in-game looked fine, but it changes EE/IOP interleaving and
  can't be checked automatically (audio/SIF timing), so it is opt-in.
- Diagnostics: `PS2X_VU1_OPSTATS=1` (upper generic/fast + lower op histogram), `PS2X_IOP_PCSTATS=1`
  (hot IOP PCs, timeslice length).

Measured (t=30–56 s, VU1 thread on), VIF writes/s | vsync/s:
- GPU GS: 15.59 | 28.26 (7b97dc8) -> 17.40 | 36.67
- CPU GS: 9.44 | 19.90 (7b97dc8) -> 9.44 | 24.55 (CPU GS rasteriser-bound)
- `PS2X_VU1_BC_CHECK=1`: 0 mismatches (GPU 280k runs, CPU 240k runs).

## Front-end Continue / Options froze on a black screen: FIXED (2026-11-18, src/overrides, patches/blackkat-continue-fmv-transition.patch)

- Symptom: choosing Continue (memory-card load page) or another page change left the screen black; DMA stopped.
- Cause: the front end (0x1E6C40) plays a short transition clip when a page changes. It sets 0x3A3DEC = 1 while
  that clip plays and only switches to the new page when the clip reports end of stream (DecodeFrame -> 1).
  Our FMV stub (`blackkatFmvFrameEnd`, DecodeFrame at 0x1E1B00) never reported the end for these clips, so the
  flag stayed 1 forever and the new page was never drawn.
- Fix: while 0x3A3DEC != 0, the stub returns end-of-stream after `BLACKKAT_FMV_TRANSITION_FRAMES` frames
  (default 40, so the X press that opened the page is released before the page reads the pad; 0 = old behaviour).
- Result: the load page appears and reads /BASLUS-20365/01-04.HDR from game/mc0. The four slots there are all
  empty ("--------", files from 2026-11-11), so choosing one starts mission 1; no progress save exists yet.
  Backup of the card: game/mc0.bak-20261118.

## Terrain / sea floor as tall square columns: FIXED (2026-11-18, patches/ps2recomp-onfoot-geom.patch)

- Symptom: island cliffs, on-foot terrain and the sea floor drawn as tall square columns / blocks (every 4th
  row and column of vertices dropped to height 0). Same with every VU1/VIF A/B switch, so not a speed feature.
- Cause: `FPU_CVT_W_S` (EE FPU CVT.W.S) in ps2xRuntime/include/ps2_runtime_macros.h used `nearbyintf`, i.e.
  round-half-to-even. The EE always truncates toward zero. The terrain builder at 0x163058 upsamples the
  129x129 byte heightmap (struct 0x32FDF0 -> 0x12E4720) to a 258x258 float grid (0x12E8840) with a weight
  table on the scratchpad, splitting each coordinate (step 0.5) into int + fraction with cvt.w.s. With
  rounding, x = 1.5, 3.5, 5.5, ... gave int 2, 4, 6 and fraction -0.5 -> weight-row index -1 -> weights read
  from 0x6FFFFFF0 (zero) -> height 0 for every 4th row and column. The mesh builders copy that grid, so all
  terrain meshes got spikes down to 0.
- Found with BLACKKAT_WATCH (write-watch on guest RAM, src/main.cpp) -> entry_001633dc / FUN_00163058, and a
  Python model of the function reproducing the zero pattern.
- Fix: truncate toward zero and saturate to INT32_MIN/INT32_MAX when |x| >= 2^31 (exponent >= 158), as the EE
  does. VU0 VFTOI already used _mm_cvttps_epi32 (truncation) and is unchanged.
- The header is included by every generated file: a full recompile of build/generated is needed after
  applying the patch (no regeneration needed; 848 generated functions use FPU_CVT_W_S).
- Result: zeros in the 258x258 grid went from ~29k to 8 (real values); island terrain smooth on the ship level.

## Loading a save (load page -> slot) hung on a black screen — FIXED 2026-11-18

Symptom: picking a filled slot on the Continue/load page showed a black screen
forever; the log showed the slot file (0N.SAV, 120004 bytes) being opened/read
again every ~3 frames (sceMcOpen fd numbers climbing into the thousands).
The save file itself was fine (fully written, header "Buccaneer Bay 05:27").

Cause: the load page's frame callback (0x1EC8F0) is run from the DecodeFrame
stub (0x1E1B00 override) through dispatchGuestBranch. Its save load (0x1BDAC8)
waits on sceMcSync, so the callback gets preempted; when the scheduler resumed
it, its `jr ra` returned straight to the stub's caller 0x1E7984 with v0 = 1.
The front end read that as "clip ended" instead of DecodeFrame = 2 (abort set
from the callback result), restarted the page clip and ran the load again.

Fix (src/overrides/blackkat_slus_20365.cpp, patch
patches/blackkat-load-save-fmv-cb-preempt.patch): the pump and frame-callback
calls now return through trampolines at 0x1E1B08 / 0x1E1B10 (inside the
replaced DecodeFrame body) that store the callback result into the abort flag
(player+0x50) and return 0 to the real caller. BLACKKAT_FMV_CB_TRAMPOLINE=0
restores the old behaviour. Verified: menu -> Continue -> slot 4 loads into
Buccaneer Bay on foot (build/slot4_fix_1237.log, shots build/shots/slot4fix_*).

## Teleport hung: VIF1 UNPACK split across DMA transfers — PARTIAL FIX 2026-11-18 (patches/ps2recomp-vif1-carry.patch)

Symptom: using a teleport (Buccaneer Bay -> Skull Islands) hung or showed garbage.
A VIF1 UNPACK/DIRECT whose data ran past the end of one DMA transfer was dropped
and the remaining data of the next transfer was parsed as VIF codes.

Fix (ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp, backup .pre-carry.bak): the
VIF1 interpreter carries an unfinished command and its remaining data into the
next transfer, as the hardware does. PS2X_VIF1_CARRY=0 turns it off.
Result: teleport works. build/teleport2_1321.log: 20 carry events, all with
garbage command words; 3061 "reserved lower" VU1 errors starting at log line
3017, before the first carry (line 3036). So VU1 code corruption that starts
before any carry is still open; the carry only stops the hang.

## Characters missing parts, trees/rocks missing: 64-bit BLTZ/BGEZ/BLEZ/BGTZ tested as 32-bit — FIXED 2026-11-18 (patches/ps2recomp-regimm-branch-64bit.patch)

Symptom: Kat drawn as a small blob (only her sword, pelvis and a few pieces),
trees and rocks missing on Skull Islands, stray polygons.

Cause: the recompiler emitted BLTZ/BGEZ/BLEZ/BGTZ (and their -L/-AL forms) as
`GPR_S32(ctx, rs) < 0` etc. The R5900 tests the full 64-bit register. The model
code loads 64-bit part flags (`ld v1, 0x78(part)`) and uses `bltz` on them
(bit 63 = part hidden). When the low word of the flags had bit 31 set, the part
was treated as hidden:
- FUN_0015AE68 (draw part, `bltz` at 0x15AE90) skipped the part, so no DMA
  CALL tag was added for it (FUN_00154DC0). 116 of Kat's 127 parts were skipped.
- FUN_001E3588 (skin model, `bltz` at 0x1E3618) skipped skinning those parts,
  so once drawn they were still in bind-local space and collapsed to one point.
Found with BLACKKAT_DMALOG / BLACKKAT_DMALOG_PARTS (src/overrides) and
PS2X_DMA_WATCH / PS2X_DMA_CHAINLOG (ps2_memory.cpp), all env-gated diagnostics.

Fix: control_flow_emitter.cpp now emits GPR_S64 for those branches. All other
GPR writes are 64-bit faithful (SET_GPR_U32/S32 sign-extend, ZE32 only for
lbu/lhu/lwu, setReturn* sign-extend), so for 32-bit values the result is the same.
Applied for now by editing only the 32 generated files where the branch
register comes from a 64-bit op in the same block (ld/dsra32/dsll32/dsrl32/
dsll/mflo/pextlw): originals in build/generated.pre-s64/. A full ps2_recomp
regen is still needed to cover all 4347 branch sites (registers set in another
block) and keep build/generated consistent with the generator.
Verified: slot-4 save on foot, Kat fully drawn (build/shots/ab_s64b_*,
before: ab_dl4_*); Kat VU1 0xbd8 runs per frame went from ~10 to ~85.

## Teleport garbage / VU1 "reserved lower 0xfffffffb" — CANDIDATE FIX 2026-11-18, NOT YET VERIFIED (patches/ps2recomp-vif1-direct-carry-mpg-wrap.patch)

Symptom (build/teleport2_1650.log): when the teleport loads the new area, 1051
"[VU1 reserved lower] instruction=0xfffffffb" lines appear (the first at log line
1555, before the first [vif1:carry]), and the carries that follow have garbage
command words (eb7ce35b, ca96d6f9...). The garbage carry commands show the VIF1
stream is already out of step: data words are being parsed as VIFcodes, and
random MPG codes write data into VU1 micro memory.

VIF1 interpreter gaps found by code review:
- DIRECT/DIRECTHL whose payload ran past the end of the DMA transfer was
  truncated (pos = end) with no carry, unlike UNPACK/MPG/STROW. Only IMAGE data was
  continued (m_vif1PendingPath2ImageQwc), so the rest of a PACKED/REGLIST
  payload in the next transfer was parsed as VIFcodes. This is the likely way
  the stream gets out of step during the area load (large DIRECT texture/CLUT
  uploads). It now carries like the other commands. A truncated DIRECT
  (only when carry is off or the 4 MB guard trips) logs [vif1:direct] truncated.
- MPG uploads past the end of micro memory were clipped; the VU wraps the
  address mod 16 KB. Now they wrap, logged as [vif1:mpg] upload wraps.
- Diagnostic: [vif1:direct] IMAGE continues past DIRECT logs when a GIF IMAGE tag
  extends beyond a complete DIRECT (the raw-continuation path then eats the
  following words, which is suspicious on hardware too).
Checked and fine: MPG NUM=0 means 256, UNPACK NUM=0 means 256, VIF0 MPG goes to VU0,
DMA CALL depth (PS2X_DMA_CHAINLOG shows no overflow on Buccaneer Bay).
The normal Buccaneer Bay slot-4 scene logs none of the new messages. It still needs a
teleport run: record the walk with BLACKKAT_PADREC (below) and replay it.

## Pad record / replay (src/overrides, patches/blackkat-pad-record-replay.patch)

BLACKKAT_PADREC=<file> records pad 0 (mode byte, buttons, 4 analog axes) on
every change, as "readIndex ms mode b2 b3 lx ly rx ry". BLACKKAT_PADPLAY=<file>
replays it, frame-locked by pad-read index (BLACKKAT_PADPLAY_MODE=time for
wall-clock). Real input returns once the file ends. Recording from game start
(title -> Continue -> slot) replays the whole session. Verified: a slot-4 load
recorded through BLACKKAT_KEYSCRIPT replays to the same in-game spot.

## Skull Islands banding: "[VU1 reserved lower] instruction=0xfffffffb" is an XGKICK overflow (2026-11-18)

- `0xFFFFFFFB` is not a VU opcode. `ps2_vu1_core.cpp` `progressXgkick()` reports it as a sentinel when a PATH1 XGKICK packet overflows the 64 KB kick buffer: a GIF tag with a huge NLOOP*NREG, or a tag chain with no EOP. It then drops the packet and stops the VU1 program (`m_stopRequested`). The `pc=` values are the XGKICK sites in the microcode.
- Only logs with Skull Islands or teleport scenes show it (teleport_1313, teleport2_1321/1650, teleport3, tp_cpu, tp_accurate). The ship and Buccaneer Bay logs (ship_rg, ship_rg2, ab_rg) have 0. The warnings always come before the first `[vif1:carry]`, and teleport_1313 had them without any carry code, so the carry didn't cause them.
- Meaning: the GIF data in VU1 data memory at the kick address is bad, or the microprogram is stale or wrong.
- Many of the carried "VIFcodes" look like misparsed data: the IRQ bit is set on all of them, and two are invalid UNPACK formats, 0x6B (V3 vl=3) and 0x67 (V2 vl=3). Those are signs of a VIF1 stream desync earlier in the same packet.
- Added `patches/ps2recomp-xgkick-overflow-diag.patch`, which prints `[xgkick:overflow]` with the offending GIF tag.
- Added `patches/ps2recomp-vif1-mpg-addr-mask.patch`. VU1 MPG ADDR is now masked to 11 bits, as the VU0 path already did. Before this, ADDR >= 0x800 skipped the upload without any message. The patch logs `[vif1:mpg] addr has upper bits` when this happens.
- UNPACK payload size now matches PCSX2 (`Vif_Unpack.cpp` `vifUnpackSetup`/`nVifT`). Patch: `patches/ps2recomp-vif1-unpack-size-pcsx2.patch`.
  - Reserved formats with VL=3 and VN<3 read 0 bytes per vector. We used to read 16-bit components for them.
  - WL=0 now means 256, and CL=0 is kept as 0. We used to force both to 1.
  - Two of the carried commands were reserved-format UNPACKs: `eb7ce35b` (V3-?, need 748) and `e77bd2d7` (V2-?, need 496). PCSX2 gives both 0 bytes, so we were eating real VIFcodes after them.
  - Unusual cases are logged as `[vif1:unpack-odd]`. `PS2X_VIF_UNPACK_PCSX2SIZE=0` restores the old sizing.
- Run 2026-11-18 18:xx (`build/tp_diag.log`): the scene drew more, then the game froze. That run had MPG masking on, and VU1 micro memory got clobbered (`reserved upper pc=0x0 0x4effffff`).
  - The first reserved-format UNPACK came at pos=4 of a 16384-byte VIF1 transfer. Its words look like 16-bit texels (`e33adf3a`, `50505050`), so texture data was being parsed as VIFcodes.
  - PCSX2 check: `nVifT` really gives VL=3/VN<3 a size of 0. But PCSX2 maps 0x67/0x77/0x7B to `vifCode_Null`, a bad-VIFcode error and stall, and its unpacker for the others is `UNPACK_INVALID`. So reserved formats only appear once the stream is already out of sync. Re-sizing them is not a fix.
  - New defaults: MPG with ADDR >= 0x800 is skipped and logged (`PS2X_VIF1_MPG_MASK=1` masks instead). PCSX2 UNPACK sizing is off (`PS2X_VIF_UNPACK_PCSX2SIZE=1` turns it on).
  - `patches/ps2recomp-vif1-desync-trace.patch` adds a 32-entry VIF1 ring trace (`[vif1:trace]`, off with `PS2X_VIF1_TRACE=0`). It is dumped once per anomaly kind: 0 reserved UNPACK, 1 MPG upper bits, 2 carry over 64 KB, 3 unknown VIFcode, 4 XGKICK overflow. Each dump shows the transfer src, qwc and chain size, plus the raw bytes around the last code.
- Likely root cause of the teleport/Skull Islands corruption (from `build/tp_diag2.log` `[vif1:trace]`):
  - The first bad code is at pos=0 of VIF1 xfer#2173, a normal-mode transfer with src=01161680, qwc=1024, 16 KB, made entirely of texel-looking words.
  - The transfer just before it, xfer#2172, is normal mode with src=009f8ef0, qwc=8: `NOP NOP FLUSH DIRECT 7`, then 7 qw of GIF data. That is a GS transfer-register setup (BITBLTBUF/TRXPOS/TRXREG/TRXDIR).
  - So xfer#2173 looks like a GS->memory download: VIF1 DMA with CHCR.DIR=0, after TRXDIR=1.
  - The DMA code ignored CHCR.DIR and parsed the destination buffer (old texels) as VIFcodes. That gave the desync, the reserved UNPACKs, the bad MPG/carries and the XGKICK overflows, and the readback never filled the buffer, which explains the mosaic after re-upload.
  - Fix in `patches/ps2recomp-vif1-readback-dir0.patch`: VIF1 normal mode with DIR=0 now drains the queued VIF1 work, copies `GS::consumeLocalToHostBytes` into MADR, and completes the channel. It logs `[vif1:readback]`, and `PS2X_VIF1_READBACK=0` restores the old behaviour. The patch also clears the VIF1 carry on FBRST.RST.
  - Not yet confirmed: the 7 qw GIF payload was not dumped. The first `[vif1:readback]` line will show chcr, and DIR=0 confirms it.

## IOP speed: QUANTUM=32 default + native SNDDRV mixer (2026-11-18, patches/ps2recomp-iop-quantum32-default.patch, patches/ps2recomp-iop-snddrv-native-mixer.patch)

- `PS2X_IOP_QUANTUM` now defaults to 32. Stelios played through the teleport and Skull Islands with it and saw no glitches. Earlier measurement: GPU vsync/s 36.7 -> 46.6. `PS2X_IOP_QUANTUM=1` restores the old per-dispatch interleaving.
- `PS2X_IOP_PCSTATS=1` (`build/is_gpu.log`) showed almost every IOP instruction spread evenly over 0x4fc40-0x4fd68.
  - That is SNDDRV.IRX (base 0x49e00), function `SNDIOP_ee36_iop36` (module offset 0x5c54, from the IRX symtab). It is the sound driver's 36->48 kHz 3:4 resampler and mixer.
  - Each call processes 2x2 blocks of 192 s16 source samples plus int32 accumulators (>>7) into 256 saturated s16 outputs, keeps 2 s16 history values, and zeroes the accumulators. That is 15,200 interpreted instructions per call.
  - It is not an idle loop, so idle-skipping can't remove it.
- Fix: the call now runs natively in C++ with the same memory effects, in the same order.
  - Detection: module name SNDDRV, plus a 192-word fingerprint of the function's non-relocated code. Table, state and pointer-table addresses are decoded from the relocated lui/addiu/lw words.
  - Calls are hooked at function entry (not in a branch delay slot; a pending load is committed first). After the native run the CPU returns to `ra`, and 15,200 IOP cycles/instructions are charged so guest timing stays the same (`PS2X_IOP_SNDMIX_CYCLES=N` overrides).
  - Toggles: `PS2X_IOP_SNDMIX=0` disables it. `PS2X_IOP_SNDMIX=check` lets the interpreter run the call and compares every byte the native port would write, logging `[iop:sndmix] check #N ... differ`.
- Offline check: `build/sndmix_test/harness.cpp` runs a small MIPS-I interpreter over the real SNDDRV.IRX code against the extracted port, for 200 random trials including saturation. All 200 matched byte for byte, at 15,200 instructions per call.
- Expected: IOP emulation was most of the EE thread (73% in the earlier profile), and nearly all IOP instructions were in this function. The IOP cost on the EE thread should mostly disappear; the rest is scheduler and timer bookkeeping.

## Per-thread CPU profiling: PS2X_PROF=1 (2026-11-18, patches/ps2recomp-thread-prof.patch)

- `PS2X_PROF=1` starts a reporter thread (`src/main.cpp`, `startThreadProf`). Once a second it prints
  `[prof] t=N total=T Name=pct ...`: CPU% per thread name, read from `/proc/self/task/*/schedstat`
  (100 = one core busy). GSRast workers are summed as `GSRast*`. It reads /proc only and adds no hooks to
  hot paths. Off unless the variable is set.
- New thread names: `GSGpu` (GPU GS backend thread, `gs_gpu_backend.inl`) and `GSRast<k>` (CPU raster
  workers, `gs_frontend.cpp`). `GameThread`, `VU1Worker` and `GSStage` already had names. The main/window
  thread shows as `blackkat`.
- Combine it with `BLACKKAT_PROF=start,dur` (SIGPROF hot spots by tid, `python3 tools/prof_report.py` /
  `build/prof_report_tid.py`) and `PS2X_VU1_THREAD_STATS=1` (EE wait-for-VU1 time, GS stage wait).
- Note for speed comparisons: guest vsync/s is EE-cycle paced (the EE spins on CHCR/STAT while VU1 works), so
  use vif writes/s (~game frames/s) as well. speed_test vs speed_old (in-game +2..+6 s): vsync 26.4 vs 23.2
  (+14%), vif/s 19.0 vs 12.5 (+52%).

## IOP profile in PS2X_PROF (2026-11-18, patches/ps2recomp-iop-prof.patch, applies inside third_party/PS2Recomp)

- Finding from prof_run (t=25..45 s, gameplay): the EE GameThread (87% CPU) spends ~66% of its samples in the IOP
  interpreter (`IopEmulator::runCycles` 6622 of 9977; `executeInstruction` self 3016), even with the native
  SNDDRV mixer on. Spinning on VU1 is small (`chcrReadWhileBusy` ~4.5%); recompiled game code is the rest.
  So the game thread is not mainly waiting for VU1. It is emulating a busy IOP.
- VU1 worker (82-84% CPU) split: `run` self 3037, `doUpper` lambda 2713, `pairReady` (stall scan) 2229,
  `commitReadyPipelines` 1398, `fastLower` 884, `updateFmacFlags` 478 (of 11869).
- `PS2X_PROF=1` now also prints `[iop:prof]` once a second: IOP instr/s, cycles/s, native mixer calls/s,
  import calls/s, slices/s, idle skips/s, and the top 16 IOP PCs (every 16th instruction sampled), each as
  `pc(module+offset)=share`. Off unless PS2X_PROF is set.

## IOP clock ran 2.6x ahead after the native mixer: cycle debt + native SNDI_memcpy (2026-11-18, patches/ps2recomp-iop-cycledebt-sndcpy.patch, applies inside third_party/PS2Recomp on top of ps2recomp-iop-prof.patch)

- iopprof_run (`PS2X_PROF=1`): IOP cyc/s ~40M while the EE only owed ~15M (122 EE Mcyc/s / 8); sndmix/s ~1500,
  idleSkips 0. Cause: `runCycles` set `target = totalCycles + cycles`, so cycles run past a target were never
  repaid. The native mixer charges 15200 cycles in one step, so every call gave the IOP free time. The sound
  driver's mixing is paced by IOP time (SPU auto-DMA callback -> threadmain -> SNDIOP_processtagbuf ->
  SNDIOP_mix), so it mixed ~2.6x more blocks, and the rest of that path (SNDI_memcpy, SNDIOP_updatevoices
  +0x82d8, LIBSD +0x3558 voice-address read) was interpreted 2.6x as often (~17M real instr/s).
- Fix 1, `PS2X_IOP_CYCLE_DEBT` (default on): overshoot is kept as debt and paid from the next budgets. IOP time
  follows the EE again, like before the native mixer. `PS2X_IOP_CYCLE_DEBT=0` reverts.
- Fix 2, `PS2X_IOP_SNDCPY` (default on): SNDDRV `SNDI_memcpy` (+0x4df0, hand-written memmove, hottest
  interpreted band +0x4e14..0x4ea0) runs as a host byte move. Detected by module name plus all 201 code words
  (relocated `j` at +0x5054 skipped). Falls back to the interpreter for n<=0, n>256 KB, out-of-RAM ranges, or
  overlap in a direction where the guest's chunked copy would not equal memmove. Charges the guest's instruction
  count (formula; harness mean error 0.6%, backward overlap copies less exact). `=0` disables, `=check` runs the
  interpreter and compares the destination bytes on return (`[iop:sndcpy] check #N`).
- Offline: `build/sndmix_test/cpy_harness.cpp` runs the real SNDI_memcpy code from game/SNDDRV.IRX in the MIPS
  mini interpreter against memmove: 19874 random calls (aligned/unaligned, overlapping both ways), 0 mismatches.
- `[iop:prof]` now also prints `debt=` and `sndcpy/s`.

## VU1 static no-stall proof + EE spin counter (2026-11-18, patches/ps2recomp-vu1-staticstall-eespin.patch, applies inside third_party/PS2Recomp)

- debt_run: EE Mcyc/s 122 -> 245 and vsync/s 24.9 -> 49.9 after the IOP cycle-debt fix, but vifw/s stayed 12.3 and
  GameThread stayed ~87%. Doubling the EE's throughput did not change drawn frames, so the EE is not what limits
  them: it now spins/advances vsyncs faster while waiting. Prime suspect: the VU1 worker (83-85%; it also counts
  time waiting for the GS stage as busy), then the GS (GSGpu 52%).
- `PS2X_VU1_THREAD_STATS=1` (existing, every 5 s) now also prints `spin N ms` = EE time from the first busy
  VIF1/GIF CHCR poll until it reads not-busy (EE waiting on the worker by polling). Next to it: `worker busy %`,
  `EE drains waited (ms)` (blocking waits), and `GS stage busy / worker waited for it`.
- `PS2X_VU1_STATICSTALL` (default on, block-cache path only): per translated VU1 image, a pair is marked
  no-stall when none of the `window` = (max write latency - 1) pairs before it writes a slot it reads with a
  latency longer than the distance. At run time the check is used only when that many pairs ran at consecutive
  pcs right before it in the current run (no jump, not the start of the run). Then the per-operand ready-cycle scan
  in `pairReady` is skipped. The FDIV/EFU/WAITQ/WAITP/XGKICK checks stay. Every pair takes >= 1 cycle, so any older
  write is already ready and the result (cycle count) is the same. `PS2X_VU1_STATICSTALL=0` reverts.
  `PS2X_VU1_BC_STATS=1` / `PS2X_VU1_BC_CHECK=1` print `[vu1:bc] static no-stall: X of N pairs (window W)`.
- Verify: `PS2X_VU1_BC_CHECK=1` (reference path has no xlat, so it uses the full scan; cycles are compared) must
  report 0 mismatches.

## VU1 interpreter: SIMD FMAC, skip-commit, flat ready slots, inlined advance (Nov 19 2026, uncommitted)
Patch: `patches/ps2recomp-vu1-simd-skipcommit-flatrdy.patch` (ps2_vu1_core.cpp, ps2_vu1_async.inl).
- `PS2X_VU1_SIMD` (default on, `=0` = scalar per-lane loop): SSE2 bit-exact ADD/SUB/MUL/MADD/MSUB
  in fastUpper (same zero-case/fallback rules, double-precision MADD cancellation check).
  Offline test /tmp/simdtest: 40M cases, both rounding modes, 0 mismatches.
- `PS2X_VU1_SKIPCOMMIT` (default on): skip the loop-top commitReadyPipelines() when the
  end-of-pair advance already committed for this cycle.
- `PS2X_VU1_FLATRDY` (default on): index m_vfReady/m_viReady/m_accReady as one flat array
  (slot encoding = member layout, verified at run time, else falls back) in the stall scan/writes.
- `PS2X_VU1_INLINEADV` (default on; off if PS2X_VU1_NOHINT=1): inlined advanceOneCycle with the
  commit-hint check before the call and progressXgkick only when a kick is active.
- Hoisted tl_runXlat reads out of the pair loop (no behaviour change, no toggle).
- New metric in `PS2X_VU1_THREAD_STATS=1`: `VU1 N Mcyc/busy-s` (VU1 cycles run per worker-busy
  second; scene-independent throughput).
Result (same session, 61 s vt_one runs, GS_GPU=1): all off 19.9 Mcyc/busy-s, vifw/s 11.36;
all on 29.0 Mcyc/busy-s (+45%), vifw/s 15.6. BC_CHECK: 280000 runs, 0 mismatches.

## VU1 interpreter: NOP exit, jump-history stall proof, fixed slot scan, single stall wait (Nov 19 2026, uncommitted)
Patch: `patches/ps2recomp-vu1-nopexit-jumpproof-fixedscan-stallonce.patch` (ps2_vu1_core.cpp only).
All default on; `=0` restores the previous code path.
- `PS2X_VU1_NOPEXIT`: upper NOP (low 11 bits 0x2FF / 0x33C, 33% of upper ops) returns at the top
  of doUpper (only sets m_currentUpperInstruction, as the fastUpper NOP path did). No measurable gain alone.
- `PS2X_VU1_JUMPPROOF`: after a jump from a delay-slot pair s that ran after >= window-1 straight
  pairs, the static no-stall proof is applied to target pairs t..t+window-1 along the known
  history (t+i-1..t, s, s-1, ...), cached per (image, t, s) in a 1024-entry thread-local table.
  No measurable gain alone (hot pairs mostly have real dependencies inside the window).
- `PS2X_VU1_FIXEDSCAN` (needs FLATRDY): pairs with <= 8 reads / writes use a fixed 8-slot unrolled
  ready scan / ready write (unused entries are slot 0 = VF0.x, latency 0, so they can never add a
  stall). About +4% VU1 Mcyc/busy-s.
- `PS2X_VU1_STALLONCE`: after advanceTo(readyCycle) for a fast-mode slot-list pair that is not
  XGKICK, the ready cycle is not recomputed (all terms are fixed until issue, advanceTo lands
  exactly). About +4%.
- Tried and dropped: an inlined copy of commitReadyPipelines' mask path in the inlined advance
  (+0.7%, noise) - not in the patch.
Result (same session, 61 s vt_one, GS_GPU=1): all four off 28.0 Mcyc/busy-s, vifw/s 15.19; all on
30.2 Mcyc/busy-s (+8%), vifw/s 16.15. BC_CHECK (121 s): 620000 runs, 0 mismatches.
Remaining profile: per-pair interpreter overhead is spread over the loop (no single hot spot);
VU1 stall cycles are ~44% of VU1 cycles. Next real step is a per-microprogram/block executor.

## VU1 block executor (Nov 19 2026, uncommitted)
Patch: `patches/ps2recomp-vu1-blockexec.patch` (ps2_vu1_core.cpp only).
- Generic, not per-microprogram: at translation time every pair gets `beLen` = number of
  consecutive "simple" pairs starting there (slot lists <= 16 reads/writes, no E/D/T/M bit, no VF
  shadow, not reserved; a branch pair is allowed only together with its simple, non-branch delay
  slot, which ends the block). At the generic loop top, a run of >= 2 such pairs (and no pending
  branch / E-bit / halt-after-delay-slot) executes in a lean inner loop doing exactly the generic
  steps in the same order: ready scan (fixed 8/16 slots, static no-stall skip inside the block) +
  FDIV/EFU/WAITQ/WAITP/XGKICK resource terms, stall with the same budget handling, upper, lower or
  I immediate, VI branch backup, slot writes, VF0/VI0 reset, pc advance + delay-slot branch
  resolution, cycle advance with the same commit-hint / PATH1 calls, budget/stop checks.
- Coverage 99.7% of executed pairs (PS2X_VU1_BESTATS=1 prints it), so 0x2c18, 0x1ee0, 0x2460 and
  all other programs are covered.
- Flags (all default on, `=0` reverts): `PS2X_VU1_BLOCKEXEC` (whole feature),
  `PS2X_VU1_BLOCKEXEC_BRANCH` (branch + delay slot inside blocks), `PS2X_VU1_BLOCKEXEC_RES`
  (FDIV/EFU/WAITQ/WAITP/XGKICK lowers inside blocks). Diagnostic: `PS2X_VU1_BESTATS=1`.
- Disabled automatically with PS2X_VU1_PROFILE, PS2X_VU1_OPSTATS, PS2X_VU1_FAST_NOSTALL, accurate
  mode, or when the decoded cache is not current.
Results (same session, 61 s vt_one, GS_GPU=1): BLOCKEXEC=0 29.9 Mcyc/busy-s, vifw/s 16.01;
on 38.5 Mcyc/busy-s (+29%), vifw/s 19.83 (+24%). Separate session: no-branch variant 36.3,
with branches 39.7 vs 29.5 off. BC_CHECK (compares vf/vi/acc/q/p/i/r/mac/status/clip/pc/cycles/
control/XGKICK hash/VU data): 181 s run, 1040000 runs, 0 mismatches (plus 60 s runs for each stage).
Remaining cost is the ops themselves (doUpper SIMD FMAC + flag queue, fastLower, commits).

## VU1 block executor: pre-decoded upper + inline FMAC flag queue (Nov 19 2026, uncommitted)
Patch: `patches/ps2recomp-vu1-predecode-flagsfast.patch` (ps2_vu1_core.cpp only).
- `PS2X_VU1_PREDECODE` (default on): each image's uppers are classified once (Vu1Xlat::upPre via
  vu1UpperPre(), the same dispatch as fastUpper: NOP / FMAC kind+source+ACC dest / other). Block
  pairs call the FMAC core (split out of fastUpper as fmacCore, unchanged) or set the NOP directly;
  other uppers still go through doUpper. fastUpper itself now ends in fmacCore (same code).
- `PS2X_VU1_FLAGSFAST` (default on): fmacCore queues FMAC flags through an inline copy of
  updateFmacFlags with the run-constant checks hoisted (same dead-entry rule incl. budget/stop
  condition, sticky carry, first free slot, entry contents, commit hint). Off in the compare-mode
  reference run (tl_bcSuppress), so BC_CHECK compares it against updateFmacFlags.
- Tried and dropped: per-block static dependency lists (producer issue cycle + latency) with
  ready-slot writes deferred to block exit - BC_CHECK clean (300000 runs) but no measurable gain
  (38.5 vs 38.4 Mcyc/busy-s), so not in the patch (copy at /tmp/core_with_deps.cpp on the PC).
Results (same session while Stelios was streaming, so absolute numbers are lower than last night;
61 s vt_one, GS_GPU=1): PREDECODE=0 FLAGSFAST=0 35.4 Mcyc/busy-s, vifw/s 18.25; on 38.4 (+8.5%),
vifw/s 19.36. BC_CHECK 181 s: 1020000 runs, 0 mismatches.
Remaining VU1 worker profile: fmacCore ~27%, run/block loop ~34%, fastLower ~10%,
commitReadyPipelines ~9% (one call per live FMAC flag entry), execLower ~3%.

## GS GPU backend: content-hashed texture cache, lazy Present, faster host->local upload (Nov 19 2026, uncommitted)
Patch: `patches/ps2recomp-gs-gpu-texcache2-lazypresent-uploadfast.patch` (gs_gpu_backend.inl,
gs_cpu_backend.cpp, gs_cpu_backend.h; backups `*.pre-texcache2.bak`). Only affects PS2X_GS_GPU=1,
except UPLOAD_FAST which is in GSCpuBackend::UploadImage (bit-exact, used by both paths).
Why: the generation-only cache missed 88% of the time (6500 decodes vs 900 hits per 5 s). Pages are
re-uploaded every frame with the same bytes, and several textures share VRAM slots.
- `PS2X_GS_TEXCACHE2` (default on, `=0` = old generation-only cache; unrelated to the CPU path's
  PS2X_GS_TEXCACHE_OFF): when the page generations of a texture's range change, the source pages
  are hashed (64-bit, 8 KiB pages, re-hashed only when that page's generation moved). The texture
  key gets that content hash, so identical re-uploads and A/B textures sharing a slot hit instead of
  re-running ReadTexture/DecodeTextureRgba/glTexSubImage. Entries not bound for ~120 presents are
  evicted above 256 MB / 4096 entries (the old clear-all at 768 MB stays as a backstop).
  Result: decodes 6500 -> ~185 per 5 s, hits 900 -> ~7700. The decodes left are real content
  changes (render-to-texture of fbp 70/350/382).
- `PS2X_GS_PRESENT_LAZY` (default on): Present no longer runs resolveAll + a 4 MiB VRAM snapshot.
  It resolves only what Present can read: enabled DISPFBs at their display height and the preferred
  source. The context frames are only read by the fbp-0 all-black fallback, so they are
  resolved after the fact, and the frame redone, only if that fallback could have been taken
  (`PS2X_GS_PRESENT_CTXLAZY=0` resolves them up front). It then converts in place
  (GSCpuBackend::PresentInPlace). It reuses the last converted frame (one per field parity,
  since this game runs interlaced field mode) when the request and the read pages are unchanged.
  Result: the in-progress fbp 70 back buffer is no longer read back every vsync. Resolved px went
  from 75M to 40M per 5 s, and ~58% of presents reuse the converted frame.
- `PS2X_GS_UPLOAD_FAST` (default on): UploadImage steps x/y instead of doing div/mod per pixel,
  and calls the write function directly. Same writes, same order, same transfer state.
Debug compare modes (decode/convert anyway and compare bytes):
  `PS2X_GS_TEXCACHE2_CHECK=1` (texels of every cache hit vs a fresh decode),
  `PS2X_GS_PRESENT_CHECK=1` (every Present vs full resolveAll + original snapshot Present),
  `PS2X_GS_UPLOAD_CHECK=1` (fast vs old upload on the same input: VRAM bytes + transfer state).
  Stats line additions under `PS2X_GS_GPU_STATS=1` (tc2/hash/evict/present counters, resolve
  reasons, present diagnostics).
Verification: 7 check runs: 442269 texture checks with 0 mismatches, 12532 present checks
(1866 of them reused frames) with 0 mismatches, and 49152 upload checks with 0 mismatches.
Screenshots on/off at 30/40/50/57 s
(`build/shots/tcab_{off,on}_*.png`, diff tool `build/imgdiff.py`, heatmaps `build/shots/diff/`)
differ by as much as two "on" runs differ from each other (17-22 dB PSNR, game timing is not
deterministic), with matching mean/std per channel. The in-run compare modes above are the exact test.
Results (same session, 61 s vt_one, GS_GPU=1, all three =0 vs on): GSGpu thread CPU 67.3% -> 37.6%
(worker busy 69% -> 36%), producer sync wait 1.95 s -> 1.0 s per 5 s, vifw/s 20.20 -> 20.90,
vsync/s 48.07 -> 49.65 (VU1 is still the limiter; at 36% busy for ~21 drawn fps, GSGpu would cap near ~55-60 fps).
Remaining GSGpu cost: resolve of render targets used as textures (fbp 70 640x448 + 256x256
effects, ~58/s) + their CPU decode, Present conversion (CT16 ReadCT16) on non-reused vsyncs.

## VU1 native block compiler (JIT), milestone 2 — 2026-11-19 (uncommitted, default OFF)
- Patch: `patches/ps2recomp-vu1-native-jit-m2.patch` (ps2_vu1_core.cpp vs `ps2_vu1_core.cpp.pre-native.bak` + new `vu/ps2_vu1_native.inl`).
- `PS2X_VU1_NATIVE=1` turns it on (default off; =0/unset = previous exact block loop). `PS2X_VU1_NATIVE_FMAC=0` keeps native blocks but calls the C++ FMAC. `PS2X_VU1_NATIVE_STATS=1` logs each compiled block.
- Design: hand-written x86-64 emitter (no deps), per-thread 64 MB RWX arena, blocks = the block executor's blocks (beLen), compiled lazily per Vu1Xlat (keyed by codeSize/dataSize). Per pair: inline fixed-scan stall max (helper only when it would stall / resource terms), native SSE2 FMAC ADD/SUB/MUL/MADD/MSUB (all bc/q/i/vector forms; exact vu1FmacSimd math, rejected lanes jump to an out-of-line call of the C++ path), dead flag entries (flagDead) carried inline into tl_carry, live ones via fmacFlagsFast helper; inline lowers NOP/IADDIU/ISUBIU/IADD/IADDI/IAND/IOR/MOVE/LQ/LQI and I-bit immediates, others via helper; inline ready-slot writes, VF0/VI0 reset, cycle++, commit hint test, xgkick test, exits; pc store deferred until a branch can be pending.
- Reference: PS2X_VU1_BC_CHECK reference path never runs native code (tl_bcSuppress). 1,040,000 runs compared (181 s) + 320,000 (61 s): 0 mismatches.
- A/B (61 s, GS_GPU=1): VU1 Mcyc/busy-s ~37.4 -> ~41.1 (+10%), vifw/s 20.29 -> 22.02, vsync/s 54.53 -> 52.92 (noise range). Program 0x2c18 loop block (77 pairs: 37 FMAC + 30 lowers native) runs natively.
- Remaining: native stall/advanceTo + commit fast path, SQ/SQI (queueStore) and more uppers (MAX/MINI/FTOI/ITOF/OPMULA/OPMSUB/CLIP) inline, VF0-reset elision, register caching of VI.

## VU1 native block compiler (JIT), milestone 3 — 2026-11-19 (uncommitted, now DEFAULT ON)
- Patch: `patches/ps2recomp-vu1-native-jit-m3.patch` (cumulative; supersedes -m2). Backups: `ps2_vu1_core.cpp.pre-native.bak` (original), `.native-m2.bak`, `.native-m3.bak`, `.native-m3b.bak` (+ matching `ps2_vu1_native.inl.*.bak`).
- `PS2X_VU1_NATIVE=0` reverts to the C++ block loop. Sub-switches (default on, =0 disables): `PS2X_VU1_NATIVE_FMAC`, `PS2X_VU1_NATIVE_OPS` (uppers MAX/MINI/ITOF/FTOI/ABS + lowers SQ/SQI/ILW/ILWR/ISW/ISWR/MTIR/MFIR/FCAND/IBEQ/IBNE and VI writes with branch-read backup), `PS2X_VU1_NATIVE_DSTORE` (stores written to VU mem directly instead of queueStore: a queued store is always committed by its own pair's cycle advance, nothing reads VU mem in between), `PS2X_VU1_NATIVE_STALL` (native advanceTo for register stalls ending before the budget), `PS2X_VU1_NATIVE_VF0` (VF0/VI0 reset only after pairs that may write them). `PS2X_VU1_NATIVE_STATS=1` logs blocks + remaining helper ops.
- Verification (BC_CHECK compares vf/vi/acc/q/p/i/r/mac/status/clip/pc/cycles/control/xgkick hash/VU data): final code 1,220,000 runs / 200 s title-path scene + 960,000 runs / 200 s slot-4 save scene (keyscript), 0 mismatches. Earlier m3 stages: 1.22M + 0.98M, 0 mismatches.
- A/B (61 s, GS_GPU=1): NATIVE=0 VU1 ~37.9 Mcyc/busy-s, vifw/s 20.44, vsync/s 51.57 -> default ~60.3 Mcyc/busy-s (+59%), vifw/s 29.78 (+46%), vsync/s 46.03. vsync/s drops because the EE now spends less time waiting on VU1 per game frame (more frames per emulated vsync); VU1 worker busy 84% -> 76%, EE/IOP thread is the next limiter.
- Remaining VU1 profile (native on): JIT code 54%, commitReadyPipelines 8%, live FMAC flag entries (fmacFlagsFast) 7%, processVIF1Data 7%. Not done yet: CLIP/OPMULA/OPMSUB/DIV/SQD natively, native live-flag entries, VI/cycle register caching, NormPs elision via known-normal dataflow.

## 2026-11-19 game-thread speed: IOP SPU2 ADMA pacing (PS2X_IOP_ADMA_PACE, default on)
- Profile (native VU1 on, title path): GameThread ~80% CPU, ~48% of it inline IOP; IOP ran 27M instr/s with
  idleSkips=0, sndmix/s ~1100 (real 48 kHz needs ~94/s for 512-sample blocks). Cause: SPU DMA completion was
  scheduled after transferWords*2 cycles, so SNDDRV's transfer-interrupt handler (+0x7808, registered via
  sceSdSetTransIntrHandler at +0x8fcc) woke the sound thread continuously; the LIBSD +0x3550..0x35e0 /
  SNDDRV +0x8400..0x8700 hot loops were just the per-block voice update running ~12x too often.
- SNDDRV uses core0 AutoDMA: ADMAS is at core+0x1B0 (0x1F9001B0 = 1), BCR 0x00200010 (512 words, double
  buffer 0x12c400/0x12cc00). Fix (iop_memory.cpp writeHardware32): when that core's ADMAS is nonzero, the
  completion delay is transferWords*768 cycles (one stereo sample per 48 kHz tick at 36.864 MHz). Non-ADMA
  SPU DMAs (core1 voice uploads) unchanged. PS2X_IOP_ADMA_PACE=0 reverts. PS2X_IOP_SPUDBG=1 logs DMA starts.
- Result (same session, 45 s title path, GPU on): off vsync/s 47.81 vifw/s 30.33; on vsync/s 60.02 vifw/s 29.83.
  IOP: 2.2M instr/s, sndmix/s 94, idleSkips ~1.08M/s; GameThread 78-80% -> 60%, total CPU ~225% -> ~205%.
  Slot-4 scene also fine (in-game screenshots similar to off; GameThread 78 -> 60).
- Patch: patches/ps2recomp-iop-adma-pace.patch (also contains the PS2X_PROF modules/regions line).

## 2026-11-19 Dashed lines / checkered blocks on the sea: FIXED (patches/ps2recomp-ipu-from-fill.patch)
- Symptom: white/cyan dashed lines along the sea mesh grid and bright checkered blocks under the hull
  (GPU and CPU GS alike; not VU1: same with PS2X_VU1_NATIVE=0 PS2X_VU1_ACCURATE=1).
- Isolation: PS2X_GS_SKIPTEX=33c0 (new debug switch) removes them completely; skipping the reflection
  pass (2bc0) does not. The sea is drawn twice: reflection (tex 0x2BC0, FIX 0x40) then a detail pass
  (tex 0x33C0, CT16 256x256, tcc1, MODULATE, STQ tiled -8..+4, REPEAT, bilinear, ALPHA (Cs-Cd)*FIX+Cd,
  FIX 0x31, ZTST GEQUAL, no Z write).
- 0x33C0 is uploaded by GIF path3 as 16 strips of 16x256 CT16 from EE 0x872200..0x892200 (REF tags
  0x7AAE50..). PS2X_GS_DUMPTEX=33c0,N showed it is ~all black with junk columns (white blocks, colour noise).
- BLACKKAT_WATCH on 0x872200..0x892200: no EE writes after the level loads (only FMV/menu-era code at
  1.4-7 s). PS2X_DMA_STARTLOG=1: the buffer is the destination of a fromIPU DMA (ch 0x1000B000,
  madr 0x872200, qwc 8192) fed by toIPU chunks of ~640 qw at 0x8922E0... => the sea detail texture is an
  IPU-decoded animated (MPEG) texture. There is no IPU emulation, so the buffer kept stale FMV-era data.
- Fix (ps2_memory.cpp, DMA start): on a fromIPU start, fill the destination with a 16-bit pattern,
  default 0xB142 (dark sea blue, A=1) so the detail pass becomes a mild uniform tint.
  PS2X_IPU_FROM_FILL=<hex16> picks the colour, PS2X_IPU_FROM_FILL=off restores the old behaviour.
  NOTE: an all-zero fill hangs the game at the sea load (ticks stop right after the fromIPU start); 0 is
  mapped to 0x0001. Real fix = IPU emulation (IDEC/BDEC + CSC), which would also bring back FMVs.
- Verified: title path sea battle 40 s shot clean (build/shots/wFixB_40s.png vs wGpu_40s.png), vsync/s 60.00
  vifw/s 29.23; slot-4 scene runs (on-foot shoreline water clean); VU1 BC_CHECK 220,000 runs, 0 mismatches.
- New debug env (all off by default): PS2X_GS_SKIPTEX=hex,..; PS2X_GS_DUMPTEX=hex,n (texture -> build/shots/
  tex_<hex>.ppm/_a.pgm); PS2X_GS_WATCHBP=lo,hi (log transfers/draws/uploads into a VRAM block range);
  PS2X_GS_DUMPRUNS now also dumps the run's texture; PS2X_DMA_GIFQWC=N (GIF tags with N qwords);
  PS2X_DMA_SPRLOG=1; PS2X_DMA_STARTLOG=1 (distinct non-VIF1/GIF DMA starts).

## 2026-11-19 Sound: SPU2 ADMA capture + host output path (patches/ps2recomp-spu2-adma-output.patch)
- Audit: there is no SPU2 emulation (no SPU RAM, voices, ADPCM decode, ADSR or reverb); IopMemory only stores
  the registers. Black Kat does not need it for audible output: SNDDRV.IRX mixes every voice in software
  (SNDIOP_ee36_iop36 = native sndMixRun, 36->48 kHz) and streams the mix to core0 by AutoDMA. ADMA
  transfers: BCR 0x00200010 = 512 words, double buffer IOP 0x12c400/0x12cc00, layout 2 x (0x200 B left,
  0x200 B right) = 512 stereo s16 frames, ~93.75/s at 48 kHz thanks to PS2X_IOP_ADMA_PACE.
  Core0 MMIX 0x0FC0 / core1 MMIX 0x0FCC: ADMA input goes out dry (InpWet off), so SPU2 reverb would not
  touch it. Hardware KON writes (~5/s per core) are LIBSD key-on/off of all 24 voices with volume 0
  (PS2X_SPU2_KONLOG=1 logs them), i.e. no audible hardware voices in the scenes checked.
- Host backend: raylib's audio module (miniaudio, built into raylib, no extra package) -> PipeWire via the
  Pulse/ALSA backend; the runtime already calls InitAudioDevice().
- Built (iop_memory.cpp, ps2_runtime.cpp):
  - At each core0 ADMA start (512-word block, ADMAS core+0x1B0 != 0) the block is de-interleaved and pushed
    to (a) PS2X_SPU2_WAV=<path> (48 kHz stereo s16 WAV, header patched every ~1 s so a killed run is valid)
    and (b) a lock-free SPSC ring (32768 frames).
  - PS2X_SPU2_OUT=1 (default OFF): the runtime opens a raylib AudioStream (48000 Hz, 16-bit, 2 ch,
    1024-frame buffers) whose callback pulls from the ring. Drift control: fractional read step 1 +/- 0.5%
    steered by ring fill (target 4096 frames ~85 ms) with linear interpolation; underrun -> silence until
    refilled; > 16384 frames -> skip to target. Offline harness (/tmp/bk/spu2h/h2.cpp, 600 s simulated):
    producer x0.995..x1.004 of device rate -> 0 underruns, 0 drops; x0.98 -> 140 underruns (game < 98% speed).
  - PS2X_SPU2_CAPTURE=0 disables capture entirely. [spu2] status line every 940 blocks (rms, clip, KON
    writes, ring fill, underruns, drops).
- WAV check (title path -> sea battle, 60 s, build/aud3.log): 48000 Hz stereo, 93.6 blocks/s, RMS -25/-24 dBFS,
  peak -4.2 dBFS, 0 clipped samples, L/R corr 0.64, centroid ~770 Hz, 98% of energy < 4 kHz (36 kHz source
  material), silence only during loading (0-6 s), no discontinuities at block boundaries.
  Spectrogram + WAV copied to the box (/workspace/spu2_spectrogram.png, spu2_titlepath_seabattle.wav).
- Speed: same session A/B 45 s: CAPTURE=0 vsync/s 60.00 vifw/s 28.82; CAPTURE=1 60.01 / 28.97 (no change).
- Not yet done: speakers test (PS2X_SPU2_OUT=1), core1/master volume registers (MVOL/AVOL not applied; the
  driver's own mix is used as-is), hardware voices/ADPCM/reverb (not needed for the paths checked; the FMV
  audio would need IPU + its own path).

## 2026-11-19 Sound follow-up: main-menu music skipping, crackle, missing SFX

### Main-menu skipping (fixed)
- Cause: producer side. On the main menu the EE ran at ~143 Mcyc/s (vsync ~29/s), so SNDDRV's
  ADMA stream arrived at ~45 blocks/s instead of 93.75; the game-side WAV was continuous (game
  time) but the live ring underran constantly (menu1: 158 underruns / 28 s of silence in 60 s).
- Two causes of the slow menu: (a) the "menu state" log in blackkatFmvFrameEnd printed ~2000
  lines/s with std::endl (now rate-limited: first 64, all with BLACKKAT_MENU_LOG=1);
  (b) the stubbed FMV::DecodeFrame (flag 0) returned at once, so the menu loop spun thousands of
  times per emulated second. The stub now charges BLACKKAT_FMV_STUB_CYCLES (default 4915200 =
  one vsync) of EE time per completed frame via eeScheduler().accountCycles(). It must be
  charged AFTER the pump/frame callback: charging before them made every pump get preempted and
  the menu stuck on "Loading" (menu3).
- After (menu4): 93.6 blocks/s, vsync 60/s, GameThread 7%, 0 underruns after startup, menu
  renders/animates. vt_one (New Game -> sea battle) and s4_one (slot-4 on foot) unchanged:
  vsync 60, vifw ~29.9 / ~40.

### Crackle
- With the producer at full rate the ring holds 2-4k frames, maxGap ~28-33 ms, drift step
  within +-0.05% once settled; no underruns/drops after the first ~5 s in menu, sea battle or
  on foot (sim consumer). Likely causes of the crackle Stelios heard were the same starvation
  (menu and any slow segment). Startup: 4-6 underruns + ~4 drops in the first 5 s while the
  producer bursts (ring briefly overfills, then drains at +0.5% over ~50 s ~ up to 250 ms latency).
  Needs a real-device listen to confirm.

### Missing SFX (implemented: minimal SPU2 hardware voices)
- In-game SFX (cannon, sword, ambience) are real SPU2 voices keyed by SNDDRV (KON with nonzero
  VOL/PITCH/SSA), not part of the ADMA stream.
- Their sample banks never reached SPU2 RAM: our own override 0x2F45D0 (blackkatSndSendNext)
  "completed" every EE->IOP sound transfer without doing it (EE sceSifSendCmd is not routed to
  IOP cmd handlers). Queue entry: +0 id, +4 EE src, +8 SPU2 byte addr, +0xC u16 size. The
  override now copies EE data into emulated SPU2 RAM (ps2x_spu2_upload); BLACKKAT_SND_UPLOAD=0
  restores the old behaviour. ~611 x 4 KB uploads at scene load.
- iop_memory.cpp Spu2Voices: 2 MB SPU RAM (also DMA ch4/ch7 non-ADMA and data port 0x1AC),
  VAG ADPCM + loop flags, pitch (linear interp), ADSR, fixed volume (sweep = full), KON/KOFF
  (16-bit/split 32-bit writes), VMIX dry/wet enables; rendered per ADMA block and mixed into the
  stream. Register feedback is required: ENVX/VOLX, NAX and ENDX. SNDDRV (updatevoices) only
  re-keys a voice when sceSdGetParam(ENVX)==0 and treats NAX < 0x5040 as finished; LSA must come
  from LSAX (SNDDRV points it at the 0x5000 silent loop). Without ENVX it keyed every SFX off
  after ~50 ms; without the LSAX fix voices never freed and SFX stopped after ~20 key-ons.
- Mix regs seen: core0 MVOL 3fff, BVOL(ADMA in) 7fff, core1 MVOL 3fff, core1 ext-in 7fff,
  MMIX 0fc0/0fcc -> voices and ADMA at unity, as implemented. Not done: reverb, noise, pitch
  mod, sweep volumes, gaussian interpolation.
- Flags: PS2X_SPU2_VOICES=0 (off), PS2X_SPU2_VOICE_GAIN=<f>, PS2X_SPU2_VOICELOG=1,
  PS2X_SPU2_VOICERAW=<path> (raw s16 stereo voice-only dump), PS2X_SPU2_RAMDUMP=<path>,
  PS2X_IOP_PCTRAP=hex,... (IOP PC trap log, diagnostic).
- Patches: patches/ps2recomp-spu2-adma-output.patch (regenerated, ADMA capture/output + sim),
  patches/ps2recomp-spu2-hw-voices.patch, patches/blackkat-menu-pacing-and-snd-upload.patch.

## 2026-11-19 Sound: output on by default, Gaussian voice interpolation, crackle review
- PS2X_SPU2_OUT is now on by default (=0 off, =sim WAV consumer). If the host device or stream
  can't be opened (or PS2X_HOST_AUDIO=0, new: skip InitAudioDevice entirely) the runtime logs
  "[spu2] no host audio device/stream ... sound output off, continuing" and calls
  ps2x_spu2_out_disable(); verified: menu runs at 60 vsync/s with no device.
- Hardware voices: SPU 4-point Gaussian interpolation, table (512 x s16) and formula taken from
  nocash psx-spx, "SPU ADPCM Pitch" -> "4-Point Gaussian Interpolation"
  (problemkaputt.de/psx-spx.htm, mirror psx-spx.consoledev.net): phase = pitch counter bits 4..11,
  out = sum of (gauss[0FFh-i]*oldest, gauss[1FFh-i]*older, gauss[100h+i]*old, gauss[i]*new),
  each SAR 15. Re-sourced from psx-spx (bit-identical to the table used before).
  PS2X_SPU2_INTERP=0 reverts to linear. Sea battle, effects-only audio: energy >12 kHz
  -42.4 dB -> -68.9 dB (rel. total), >16 kHz -43.7 -> -79.5 dB; offline same-sample renders at
  the game's pitches: imaging above source Nyquist 9-14 dB lower, >12 kHz 20-50 dB lower.
- Crackle review: voice sum already saturates (float clamp); no large deltas (>4000) in the
  effects-only audio before or after; envelope steps and loop/end handling follow hardware
  (left as is). The large sample-to-sample deltas in playback (~85/min >4000) are already in the
  game-side stream (~120/min in the pre-ring WAV), none at ADMA block seams -> source content,
  not the output path. Drift step smoothing (<=2.5e-4 per 21 ms callback) is inaudible.
  Cheap wins added: PS2X_SPU2_DRIFT_INTERP (default 1, 4-point Catmull-Rom in the drift resampler
  instead of linear) and PS2X_SPU2_DECLICK (default 1: on underrun the last sample decays instead
  of a hard cut; 64-frame crossfade after an underrun or a kHigh skip). Underruns only occur in the
  first ~5 s of a session.
- Patch: patches/ps2recomp-spu2-gauss-default-out.patch (vs *.pre-gauss.bak).
