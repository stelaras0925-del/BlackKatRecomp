# Patches

## `ps2recomp-series/` (what the build uses)

`tools/apply_patches.sh` applies these to `third_party/PS2Recomp` at the pinned commit, in the
order listed in `ps2recomp-series/series`. Together they are the exact PS2Recomp tree the port is
built and tested with. Each patch owns a set of files; no file appears in two patches.

| Patch | Files |
|---|---|
| `0001-recompiler-codegen.patch` | `control_flow_emitter.cpp` (REGIMM 64-bit branches), `ps2_runtime_macros.h` |
| `0002-iop-sound.patch` | IOP emulator/kernel/memory (SPU2 ADMA output, hardware voices, Gaussian interpolation, native SNDDRV mixer, quantum 32, ADMA pacing) |
| `0003-gs-cpu-gpu.patch` | GS frontend, CPU backend, GPU backend (`gs_gpu_backend.inl`), GIF arbiter, draw tagging (`gs_drawtag.inl`), widescreen HUD (`gs_wshud.inl`) |
| `0004-vu1-vif1.patch` | VU1 core/JIT (`ps2_vu1_native.inl`, M3), async VU1 thread, VIF1 interpreter |
| `0005-ee-core-ipu-sched.patch` | EE scheduler, `ps2_memory.cpp` (IPU glue, SPR chain DMA, timer batching), `ps2_runtime.cpp`, `ps2_pad.cpp`, `ps2_ipu.inl` |

```bash
tools/apply_patches.sh            # apply (skips patches that are already applied)
tools/apply_patches.sh --check    # dry run; exit 2 if something is pending, 1 on a conflict
tools/apply_patches.sh --reset    # back to the pinned commit (asks first; -y to skip the question)
```

CMake runs `--check` at configure time (`-DBKR_APPLY_PATCHES=OFF` turns that off) and stops with
a message if the series is missing.

### Refreshing the series after changing the submodule

From `third_party/PS2Recomp`, for a tracked file `git diff --full-index -- <file>`, for a file the
series adds `git diff --no-index --full-index -- /dev/null <file>`, appended to the patch that owns
that file. Then check on a clean copy: `tools/apply_patches.sh --reset -y && tools/apply_patches.sh`.

## `ps2recomp-*.patch` and `blackkat-*.patch` in this directory (history only)

These are the per-feature diffs written while the work was done. Many were made against an
intermediate state of the submodule (or with `third_party/PS2Recomp/` path prefixes), several
overlap or supersede each other (e.g. the SPU2/ADMA patches were regenerated, the VU1 native JIT
went through M2 and M3), and a few changes are in no single patch (e.g. `ps2_gif_arbiter.cpp`).
Applied in commit order on the pinned commit, 32 of 45 fail. They are kept as a record of what each
change was for; do not apply them. `blackkat-*.patch` describe changes to this repository's own
`src/` and are already committed.
