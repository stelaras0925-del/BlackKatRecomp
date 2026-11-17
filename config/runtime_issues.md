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
