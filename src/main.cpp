// SPDX-License-Identifier: GPL-3.0-only
//
// Host entry for the Black Kat recompilation. Recompiled guest code is not
// required to link this binary: until ps2_recomp has written build/generated,
// the function table is empty and loadELF still boots the runtime.

#include "ps2_runtime.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    std::filesystem::path guestElfPath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0')
        {
            return std::filesystem::path(argv[1]);
        }
        return std::filesystem::path("game") / "SLUS_203.65";
    }

    void setupTerminateLogger()
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[blackkat] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[blackkat] " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[blackkat] non-std exception" << std::endl;
                                   }
                               }
                               std::abort();
                           });
    }
}

int main(int argc, char *argv[])
{
    setupTerminateLogger();

    try
    {
        const std::filesystem::path elfPath = guestElfPath(argc, argv);
        if (!std::filesystem::exists(elfPath))
        {
            std::cerr
                << "BlackKatRecomp: guest ELF not found: " << elfPath.string() << std::endl
                << "This repository does not include the game. Extract an NTSC-U disc you own:" << std::endl
                << "  tools/extract_iso.sh /path/to/your.iso" << std::endl
                << "Then recompile with: cmake --build --preset linux-release --target recompile" << std::endl
                << "Usage: blackkat [path/to/SLUS_203.65]" << std::endl;
            return 1;
        }

        PS2Runtime runtime;
        constexpr const char *kWindowTitle =
            "BlackKatRecomp | Pirates: The Legend of Black Kat (NTSC-U SLUS-20365)";
        if (!runtime.initialize(kWindowTitle))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(elfPath.string()))
        {
            std::cerr << "Failed to load ELF: " << elfPath << std::endl;
            return 1;
        }

        runtime.run();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[blackkat] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[blackkat] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
