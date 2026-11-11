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
#include <vector>
#include <atomic>
#include <csignal>
#include <link.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <execinfo.h>
#include <unistd.h>

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

    // BLACKKAT_PROF="start,dur": SIGPROF sampler (1 kHz of process CPU time),
    // records the interrupted RIP + tid from <start>s for <dur>s, then writes
    // blackkat_prof.txt ("tid offset" per line, offset relative to the main
    // executable's load base) for addr2line. perf is not installed.
    constexpr size_t kProfMax = 1u << 20;
    uint64_t g_profRip[kProfMax];
    uint32_t g_profTid[kProfMax];
    constexpr int kProfDepth = 10;
    constexpr size_t kProfStackMax = 1u << 18;
    void *g_profStack[kProfStackMax][kProfDepth];
    uint8_t g_profStackN[kProfStackMax];
    std::atomic<size_t> g_profN{0};
    std::atomic<bool> g_profOn{false};

    void profHandler(int, siginfo_t *, void *uc)
    {
        if (!g_profOn.load(std::memory_order_relaxed))
            return;
        const size_t i = g_profN.fetch_add(1, std::memory_order_relaxed);
        if (i >= kProfMax)
            return;
        const auto *u = static_cast<const ucontext_t *>(uc);
        g_profRip[i] = static_cast<uint64_t>(u->uc_mcontext.gregs[REG_RIP]);
        g_profTid[i] = static_cast<uint32_t>(syscall(SYS_gettid));
        if (i < kProfStackMax)
            g_profStackN[i] = static_cast<uint8_t>(backtrace(g_profStack[i], kProfDepth));
    }

    uintptr_t exeBase()
    {
        uintptr_t base = 0;
        dl_iterate_phdr([](dl_phdr_info *info, size_t, void *data) -> int
                        {
                            *static_cast<uintptr_t *>(data) = info->dlpi_addr;
                            return 1; // first entry = main executable
                        },
                        &base);
        return base;
    }

    void startProfiler()
    {
        const char *e = std::getenv("BLACKKAT_PROF");
        if (!e || !*e)
            return;
        int start = 25, dur = 20;
        std::sscanf(e, "%d,%d", &start, &dur);
        struct sigaction sa{};
        sa.sa_sigaction = profHandler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        {
            void *warm[4];
            backtrace(warm, 4); // load libgcc's unwinder outside the handler
        }
        sigaction(SIGPROF, &sa, nullptr);
        std::thread([start, dur]()
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(start));
                        itimerval it{};
                        it.it_interval.tv_usec = 1000;
                        it.it_value.tv_usec = 1000;
                        g_profOn = true;
                        setitimer(ITIMER_PROF, &it, nullptr);
                        std::cerr << "[blackkat] profiler on for " << dur << "s" << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(dur));
                        itimerval off{};
                        setitimer(ITIMER_PROF, &off, nullptr);
                        g_profOn = false;
                        const size_t n = std::min(g_profN.load(), kProfMax);
                        const uintptr_t base = exeBase();
                        if (FILE *f = std::fopen("blackkat_prof.txt", "w"))
                        {
                            for (size_t i = 0; i < n; ++i)
                            {
                                std::fprintf(f, "%u %llx %llx", g_profTid[i],
                                             static_cast<unsigned long long>(g_profRip[i] - base),
                                             static_cast<unsigned long long>(g_profRip[i]));
                                if (i < kProfStackMax)
                                    for (int k = 0; k < g_profStackN[i]; ++k)
                                        std::fprintf(f, " %llx", static_cast<unsigned long long>(
                                                                     reinterpret_cast<uintptr_t>(g_profStack[i][k])));
                                std::fputc('\n', f);
                            }
                            if (FILE *m = std::fopen("/proc/self/maps", "r"))
                            {
                                if (FILE *o = std::fopen("blackkat_prof_maps.txt", "w"))
                                {
                                    char buf[4096];
                                    size_t r;
                                    while ((r = std::fread(buf, 1, sizeof buf, m)) > 0)
                                        std::fwrite(buf, 1, r, o);
                                    std::fclose(o);
                                }
                                std::fclose(m);
                            }
                            std::fclose(f);
                        }
                        std::cerr << "[blackkat] profiler wrote " << n << " samples (base 0x" << std::hex << base
                                  << std::dec << ")" << std::endl;
                    })
            .detach();
    }
}

// Debug: BLACKKAT_FAKEKEY="x,enter,down" synthesises real key events through
// raylib's own GLFW key callback (the exact path a physical key takes:
// GLFW -> raylib KeyCallback -> CORE key state -> IsKeyDown). One step every
// BLACKKAT_FAKEKEY_MS (default 1500), held BLACKKAT_FAKEKEY_HOLD_MS (150),
// first after BLACKKAT_FAKEKEY_DELAY_MS (7000); the list repeats.
extern "C"
{
    typedef void (*BlackkatGlfwKeyFun)(void *, int, int, int, int);
    BlackkatGlfwKeyFun glfwSetKeyCallback(void *window, BlackkatGlfwKeyFun cb);
    void *GetWindowHandle(void);
}
static void blackkatStartFakeKeys()
{
    const char *e = std::getenv("BLACKKAT_FAKEKEY");
    if (!e || !e[0])
        return;
    auto num = [](const char *n, int d)
    { const char *v = std::getenv(n); return (v && v[0]) ? std::atoi(v) : d; };
    std::vector<int> keys;
    std::stringstream ss(e);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        int k = 0;
        if (tok == "x") k = 88;
        else if (tok == "z") k = 90;
        else if (tok == "c") k = 67;
        else if (tok == "v") k = 86;
        else if (tok == "enter") k = 257;
        else if (tok == "up") k = 265;
        else if (tok == "down") k = 264;
        else if (tok == "left") k = 263;
        else if (tok == "right") k = 262;
        else if (tok == "space") k = 32;
        if (k) keys.push_back(k);
    }
    if (keys.empty())
        return;
    const int everyMs = num("BLACKKAT_FAKEKEY_MS", 1500);
    const int holdMs = num("BLACKKAT_FAKEKEY_HOLD_MS", 150);
    const int delayMs = num("BLACKKAT_FAKEKEY_DELAY_MS", 7000);
    std::thread([keys, everyMs, holdMs, delayMs]()
                {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        void *win = nullptr;
        for (int i = 0; i < 200 && !(win = GetWindowHandle()); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!win) { std::cerr << "[fakekey] no window" << std::endl; return; }
        BlackkatGlfwKeyFun cb = glfwSetKeyCallback(win, nullptr);
        glfwSetKeyCallback(win, cb);
        if (!cb) { std::cerr << "[fakekey] no raylib key callback" << std::endl; return; }
        std::cerr << "[fakekey] injecting via raylib KeyCallback" << std::endl;
        for (size_t i = 0;; ++i)
        {
            const int k = keys[i % keys.size()];
            std::cerr << "[fakekey] press key " << k << std::endl;
            cb(win, k, 0, 1 /*GLFW_PRESS*/, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            cb(win, k, 0, 0 /*GLFW_RELEASE*/, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(everyMs > holdMs ? everyMs - holdMs : 1));
        } })
        .detach();
}

int main(int argc, char *argv[])
{
    setupTerminateLogger();
    startProfiler();

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

        blackkatStartFakeKeys();

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
