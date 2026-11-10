# PS2Recomp in this tree

## Pin

| | |
| --- | --- |
| Path | `third_party/PS2Recomp` |
| URL | https://github.com/ran-j/PS2Recomp |
| Commit | `e5636981a1c9de368f6b71e819c3dc188dfedbf4` |
| Date | 2026-10-23 |
| Subject | GIF: treat FLG=3 like IMAGE everywhere a GIFtag is parsed (#384) |

The gitlink is that commit, not a floating branch. `git submodule update --init` checks it out detached.

## Bumping

From the BlackKatRecomp root:

```bash
git -C third_party/PS2Recomp fetch origin
git -C third_party/PS2Recomp checkout <full-40-character-sha>
git add third_party/PS2Recomp
```

Configure again after the checkout. `cmake/PreparePs2RecompOverlay.cmake` stamps `build/ps2recomp-src` with the submodule SHA and deletes that overlay when the stamp does not match, then recreates the symlinks. You do not edit files inside `third_party/PS2Recomp` to bump it.

After a bump, re-read:

- `ps2xRuntime/include/game_overrides.h` (`PS2_REGISTER_GAME_OVERRIDE` and `ps2_game_overrides::bindAddressHandler`)
- `ps2xRecomp/src/lib/config_manager.cpp` (the TOML keys `config/slus_203.65.toml` is written against)
- `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (what the export writes into `stubs` and `untracked_stubs`)

`config/slus_203.65.toml` was checked against that pin, not against a later `main`.

## Why there is an overlay

Upstream is built as its own CMake top level. `ps2xAnalyzer`, `ps2xRecomp`, `ps2xStudio`, and `ps2xTest` include sibling headers with `${CMAKE_SOURCE_DIR}/...`. Inside `add_subdirectory(third_party/PS2Recomp)` that variable is the BlackKatRecomp root, so those includes miss.

`bkr_prepare_ps2recomp_overlay` builds `build/ps2recomp-src`:

- Unchanged directories are symlinks into the submodule.
- `CMakeLists.txt` for `ps2xRecomp`, `ps2xAnalyzer`, `ps2xStudio`, and `ps2xTest` is copied with `${CMAKE_SOURCE_DIR}` rewritten to `${BKR_PS2RECOMP_ROOT}`.

The submodule working tree stays clean. The overlay is gitignored with the rest of `build/`.

If a future pin replaces those `CMAKE_SOURCE_DIR` uses with paths relative to the current listfile, the overlay can go away and `add_subdirectory(third_party/PS2Recomp)` is enough.

## Linux host choices

- Presets `linux-release` and `linux-debug` use Ninja and `clang` / `clang++`.
- `-msse4.1` is added for x86_64 in the top-level listfile. Upstream's Ubuntu workflow passes the same flag as `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS`.
- `PS2X_BUILD_STUDIO` and `PS2X_BUILD_TEST` default off.
- `BKR_ENABLE_LTO` defaults off. Upstream's `EnableFastReleaseMode` sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE` on the recompiler, analyzer, and runtime. The overlay walk clears that property unless the option is on.
- FFmpeg is found with `pkg-config` (`libavcodec`, `libavformat`, `libavutil`, `libswresample`, `libswscale`). On Arch/CachyOS those headers are in the `ffmpeg` package.
- Raylib 5.5, Dear ImGui, rlImGui, ELFIO, toml11, fmt, libdwarf, and rabbitizer are fetched by PS2Recomp's CMake at configure time. The first configure needs network access and a working `git`.
- Clang links the libstdc++ of the GCC install it selects. A missing `libstdc++.so` on that path fails the compiler probe with `cannot find -lstdc++`.
- libdwarf's configure step warns when zstd is missing and still builds against zlib. Harmless for a stripped retail ELF.
- A successful Clang 18 Release build of this pin warned in `ps2xRuntime/include/runtime/gs/ps2_gs_memory.h` (`-Wswitch`, unhandled enumerators). The tools and `blackkat` still linked.
