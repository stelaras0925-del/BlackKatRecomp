// SPDX-License-Identifier: GPL-3.0-only
//
// Host entry for the Black Kat recompilation. Recompiled guest code is not
// required to link this binary: until ps2_recomp has written build/generated,
// the function table is empty and loadELF still boots the runtime.

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

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

        // Optional disc image for raw sceCdRead sector access:
        // blackkat [elf] [iso], or BLACKKAT_ISO=/path/to/game.iso
        {
            std::filesystem::path iso;
            if (argc >= 3 && argv[2] != nullptr && argv[2][0] != '\0')
                iso = argv[2];
            else if (const char *env = std::getenv("BLACKKAT_ISO"); env && env[0])
                iso = env;
            if (!iso.empty())
            {
                if (!std::filesystem::exists(iso))
                {
                    std::cerr << "[blackkat] disc image not found: " << iso.string() << std::endl;
                    return 1;
                }
                PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
                paths.cdImage = iso;
                PS2Runtime::setIoPaths(paths);
                std::cerr << "[blackkat] CD image: " << PS2Runtime::getIoPaths().cdImage.string() << std::endl;
            }
        }

        // Debug: BLACKKAT_RAMDUMP="20,50" writes guest RDRAM (32 MB) to
        // ramdump_<sec>.bin in the working directory after each delay.
        if (const char *dumpEnv = std::getenv("BLACKKAT_RAMDUMP"); dumpEnv && dumpEnv[0])
        {
            std::string spec(dumpEnv);
            std::thread([&runtime, spec]()
                        {
                            std::stringstream ss(spec);
                            std::string item;
                            int elapsed = 0;
                            while (std::getline(ss, item, ','))
                            {
                                const int at = std::atoi(item.c_str());
                                if (at > elapsed)
                                    std::this_thread::sleep_for(std::chrono::seconds(at - elapsed));
                                elapsed = at;
                                const std::string name = "ramdump_" + std::to_string(at) + ".bin";
                                if (FILE *f = std::fopen(name.c_str(), "wb"))
                                {
                                    std::fwrite(runtime.memory().getRDRAM(), 1, PS2_RAM_SIZE, f);
                                    std::fclose(f);
                                    std::cerr << "[blackkat] RAM dump written: " << name << std::endl;
                                }
                                const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                                std::cerr << std::hex << "[blackkat] EE snapshot @" << std::dec << at
                                          << "s running=" << snap.runningThreadId << std::endl;
                                for (const EeThreadSnapshot &t : snap.threads)
                                {
                                    std::cerr << "[blackkat]   thread " << std::dec << t.id << std::hex
                                              << " pc=0x" << t.pc << " ra=0x" << t.ra << " sp=0x" << t.sp
                                              << " entry=0x" << t.entry << std::dec
                                              << " prio=" << t.currentPriority
                                              << " status=" << static_cast<int>(t.status)
                                              << " wait=" << static_cast<int>(t.waitReason)
                                              << " waitId=" << t.waitId
                                              << " invDepth=" << t.invocationDepth << std::endl;
                                }
                                for (const EeSemaphoreSnapshot &sm : snap.semaphores)
                                {
                                    std::cerr << "[blackkat]   sema " << sm.id << " count=" << sm.count
                                              << "/" << sm.maxCount << " waiters=" << sm.waiters << std::endl;
                                }
                            }
                        })
                .detach();
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
