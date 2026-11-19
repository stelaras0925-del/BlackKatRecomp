# BlackKatRecomp

Native PC port of **Pirates: The Legend of Black Kat** (Westwood Studios / EA, 2002), starting from the NTSC-U disc `SLUS-20365` v1.00. The game is statically recompiled with [PS2Recomp](https://github.com/ran-j/PS2Recomp): the Emotion Engine binary becomes C++, and [ps2xRuntime](third_party/PS2Recomp/ps2xRuntime) runs that code on the host.

The project is a scaffold. It builds the recompiler, the analyzer, and a `blackkat` host that links `ps2xRuntime` plus a game-override module for this ELF. It does not yet boot the game.

Disc layout and ELF facts are in [docs/disc.md](docs/disc.md). The pinned toolchain and how the Linux build is wired are in [docs/ps2recomp.md](docs/ps2recomp.md).

## You need your own disc

BlackKatRecomp does not contain the game, and it will not download it.

- You must own a legal copy and supply your own disc image.
- No ISO, ELF, IRX, VIV, BIOS, or generated recompiled code is committed. `.gitignore` refuses those files.
- PS2 BIOS dumps are not used and are not accepted into the tree.
- The game itself remains copyrighted by its rights holders. This repository is an independent reimplementation of the *program*, in the same spirit as [SafeHouse](https://github.com/FridiNaTor1/SafeHouse): the port runs from the disc you already have.

Do not open pull requests that add retail data, title ids paired with download links, or BIOS files.

## What the scaffold builds

| Target | Role |
| --- | --- |
| `ps2_recomp` | Reads `config/slus_203.65.toml` and writes C++ into `build/generated/`. |
| `ps2_analyzer` | Optional fallback that drafts a TOML from the ELF. Prefer the Ghidra export for this stripped retail binary. |
| `blackkat` | Host executable. Links `ps2xRuntime` and the `SLUS_203.65` override. Uses `build/generated/*.cpp` when `register_functions.cpp` is there; otherwise an empty function table, with a configure message that says so. |
| `recompile` | CMake target that runs `ps2_recomp` on the checked-in TOML. Not part of the default build. |

`ps2xStudio` and `ps2xTest` are off unless you pass `-DPS2X_BUILD_STUDIO=ON` or `-DPS2X_BUILD_TEST=ON`.

## PS2Recomp pin

Submodule: `third_party/PS2Recomp`  
Commit: [`e5636981a1c9de368f6b71e819c3dc188dfedbf4`](https://github.com/ran-j/PS2Recomp/commit/e5636981a1c9de368f6b71e819c3dc188dfedbf4) (2026-10-23, `GIF: treat FLG=3 like IMAGE everywhere a GIFtag is parsed`, #384)  
License: GPL-3.0, same as this repository.

Bump it from the repository root:

```bash
git -C third_party/PS2Recomp fetch origin
git -C third_party/PS2Recomp checkout <full-sha>
git add third_party/PS2Recomp
```

The next configure rebuilds `build/ps2recomp-src` when that SHA changes. Read [docs/ps2recomp.md](docs/ps2recomp.md) before moving the pin: a few upstream `CMakeLists.txt` files use `CMAKE_SOURCE_DIR`, and this repo compiles a symlink overlay rather than patching the submodule.

## Build on Arch and CachyOS

The Linux path is Clang + Ninja. PS2Recomp's own CI is where Linux is exercised upstream; day-to-day upstream testing is still mostly MSVC. Presets `linux-release` and `linux-debug` force Clang and Ninja. The top-level `CMakeLists.txt` adds `-msse4.1` on x86_64 because `ps2_runtime.h` includes `<smmintrin.h>`.

```bash
sudo pacman -S --needed base-devel cmake ninja clang git python pkgconf \
  libglvnd libx11 libxrandr libxinerama libxcursor libxi ffmpeg p7zip xorriso

git clone --recurse-submodules https://github.com/stelaras0925-del/BlackKatRecomp.git
cd BlackKatRecomp

cmake --preset linux-release
cmake --build --preset linux-release
```

Binaries land in `build/linux-release/bin/`:

- `ps2_recomp`
- `ps2_analyzer`
- `blackkat`

Debug preset:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Equivalent configure without presets, still from a Linux host with Clang on `PATH`:

```bash
cmake -S . -B build/linux-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux-release
```

Release builds leave interprocedural optimization off (`-DBKR_ENABLE_LTO=OFF`). Upstream enables it, and linking this tree with LTO wants several extra gigabytes of RAM. Turn it on only if the machine has that headroom.

If `VITASDK` is set in the environment, the top-level CMake unsets it for the host build. PS2Recomp would otherwise pick up the Vita toolchain.

Debian/Ubuntu package names differ (`ninja-build`, `pkg-config`, `p7zip-full`, and the `libavcodec-dev` / `libavformat-dev` / `libavutil-dev` / `libswresample-dev` / `libswscale-dev` split of FFmpeg). Arch ships those headers in `ffmpeg`.

Clang links GCC's libstdc++. `base-devel` installs GCC, which provides it. If the link fails with `cannot find -lstdc++`, Clang has selected a GCC install that has no `libstdc++.so` (`clang++ -v` prints `Selected GCC installation`). Install that GCC version's libstdc++ development files.

The first configure clones PS2Recomp's dependencies (raylib, Dear ImGui, rlImGui, fmt, ELFIO, toml11, libdwarf, rabbitizer, nlohmann-json). It needs network access. libdwarf looks for zstd and continues with zlib if zstd is absent; this ELF has no DWARF, so that warning does not block the port. Clang may also warn in upstream `ps2_gs_memory.h` about switch cases that do not cover every enumerator. Those warnings are not errors.

`check_ipo_supported` can report that link-time optimization is unavailable, depending on the Clang. The presets keep `BKR_ENABLE_LTO` off either way, including on a newer Clang where the probe succeeds and a Release link would otherwise spend a lot of RAM.

## Extract the disc

```bash
tools/extract_iso.sh /path/to/your.iso
```

The script uses `7z` when it is installed, otherwise `xorriso`. It refuses to write `game/` unless `SLUS_203.65` is exactly 6,439,444 bytes and md5 `e6ea18b09f214dd276078872926ca10d`. It copies `SYSTEM.CNF`, that ELF, and `*.IRX` / `*.IMG` (including `IOPRP*.IMG`). It does not extract `FILES.VIV`, `NTSCVID.VIV`, `VID2.VIV`, `VID3.VIV`, or any other `.VIV`.

`game/` is gitignored. Leave it that way.

## Recompile

`config/slus_203.65.toml` matches the TOML `ConfigManager` in the pinned PS2Recomp reads. `input` is `game/SLUS_203.65`. `output` is `build/generated`. `stubs`, `entry_points`, `untracked_stubs`, and `skip` are empty placeholders for a Ghidra export. A missing `config/ghidra/SLUS_203.65.csv` is only a warning upstream.

```bash
cmake --build --preset linux-release --target recompile
cmake --preset linux-release
cmake --build --preset linux-release
```

Reconfigure after `build/generated/register_functions.cpp` appears so `blackkat` compiles the generated C++ instead of the empty table. `build/generated/` is gitignored.

Until that file exists, configure prints that `blackkat` is built with an empty function table and that the game ELF is not required to build the target.

## Ghidra

The retail ELF is stripped (no `.symtab`). Boundaries need to come from Ghidra, not from the analyzer's jump scan.

1. Install [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) into Ghidra 12.1.2 (the processor module the project is developed against). Restart Ghidra so the PS2 Emotion Engine language appears.
2. Import `game/SLUS_203.65`. Select the Emotion Engine processor from that extension when Ghidra asks for a language. Load address of this ELF is `0x00100000`; let Ghidra take the ELF program headers rather than typing a flat image base.
3. Analyze. RTTI class names and `Class::Method` strings in the binary are useful anchors while renaming, but the recompiler consumes the function map, not those strings directly.
4. Script Manager → add the directory `third_party/PS2Recomp/ps2xRecomp/tools/ghidra/` → run `ExportPS2Functions.java`.
5. Save the TOML and the CSV when the script asks. Suggested CSV path: `config/ghidra/SLUS_203.65.csv` (gitignored).
6. The script's TOML points `output` at a folder next to the TOML and sets `single_file_output = false`. Point `output` at `build/generated`, set `single_file_output = true` (or leave it false; CMake compiles every `.cpp` in that directory), and keep `input` on `game/SLUS_203.65`. Copy `stubs` and `untracked_stubs` back into `config/slus_203.65.toml` if you want one checked-in config with repo-relative paths.

`ps2_analyzer game/SLUS_203.65 some.toml` is only a fallback. Upstream documents it as the wrong path for stripped retail games.

Per-build patches that should not live in the global runtime go in `src/overrides/blackkat_slus_20365.cpp`. The pinned API is `PS2_REGISTER_GAME_OVERRIDE(name, elfName, entry, crc32, applyFn)` from `ps2xRuntime/include/game_overrides.h`. This module is keyed to basename `SLUS_203.65` and entry `0x002A7B70`. CRC32 is `0`, which means "do not check" until you set the zlib CRC of the file you extracted. The upstream title database has `SLPS-20365` as a different Japanese game; this port does not use that row.

## Roadmap

1. **Boot.** Recompile `SLUS_203.65`, link it into `blackkat`, and reach the first guest instructions with the IOP modules from the disc (`LIBSD`, `MCMAN`, `MCSERV`, `MTAPMAN`, `PADMAN`, `SDRDRV`, `SIO2MAN`, `SNDDRV`, `USBD`, `IOPRP*.IMG`).
2. **FMV.** The game's video archives are `NTSCVID.VIV`, `VID2.VIV`, and `VID3.VIV` (EA BIGF-style containers; the exact layout is still unverified). Decode IPU MPEG in the runtime, or replace that path with the FFmpeg decoder `ps2xRuntime` already links on Linux.
3. **Menus.** Title screen and front-end flow against the reference PCSX2 run.
4. **First island.** Leave the front end and stand on the first playable space.
5. **Sailing.** The VU1-heavy ship path. `.vutext` on this ELF is `0x3FC0` bytes of VU microcode; expect this milestone to be dominated by VU1 and GIF, not by the EE scalar core.
6. **Native renderer.** Replace the VU1/GS software path with a host renderer once the frame is understood.
7. **Audio, saves, input.** `LIBSD` / `SDRDRV` / `SNDDRV` for sound, `MCMAN` / `MCSERV` for memory-card saves, `PADMAN` / `MTAPMAN` / `SIO2MAN` for controllers.
8. **Installer.** A small installer that accepts a disc image you own, checks the ELF the way `tools/extract_iso.sh` does, and does not redistribute the game. Same rule as the rest of the tree: the port ships code, the player brings the disc.

## License

[GNU General Public License v3.0 only](LICENSE). That matches PS2Recomp, so the submodule and this tree can be built and distributed together. Game content is not covered by this license and is not included.
