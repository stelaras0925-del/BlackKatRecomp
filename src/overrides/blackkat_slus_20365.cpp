// SPDX-License-Identifier: GPL-3.0-only
//
// Per-build runtime patches for Pirates: The Legend of Black Kat, NTSC-U
// SLUS-20365 v1.00. PS2Recomp applies a registered override from loadELF when
// the ELF basename, entry point, and (if set) file CRC32 all match.
//
// Keyed facts for this disc's SLUS_203.65:
//   basename  SLUS_203.65
//   e_entry   0x002A7B70
//   load VA   0x00100000
//   CRC32     0 (unset). 0 skips the CRC check. Once the ELF is on disk, set
//             the zlib/ISO-HDLC CRC32 of the whole file so a different build
//             cannot take these bindings:
//               python -c "import zlib,sys; print(f'0x{zlib.crc32(open(sys.argv[1],\"rb\").read()) & 0xffffffff:08X}')" game/SLUS_203.65

#include "game_overrides.h"

#include "ps2_runtime.h"

#include <iostream>

namespace
{
    void applyBlackKatNtscU(PS2Runtime &runtime)
    {
        (void)runtime;
        // Address bindings for this ELF only. Names must exist in
        // PS2_SYSCALL_LIST or PS2_STUB_LIST, for example:
        //   ps2_game_overrides::bindAddressHandler(runtime, 0x00123456u, "ret0");
        std::cerr << "[blackkat] SLUS_203.65 v1.00 override registered; no address bindings yet."
                  << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "Pirates: The Legend of Black Kat (NTSC-U v1.00)",
    "SLUS_203.65",
    0x002A7B70u,
    0u,
    applyBlackKatNtscU)
