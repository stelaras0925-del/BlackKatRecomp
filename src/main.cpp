#include <algorithm>
// SPDX-License-Identifier: GPL-3.0-only
//
// Host entry for the Black Kat recompilation. Recompiled guest code is not
// required to link this binary: until ps2_recomp has written build/generated,
// the function table is empty and loadELF still boots the runtime.

#include "ps2_runtime.h"
#include "launcher.h"
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
#include <sys/mman.h>
#include <map>
#include <cstring>
#include <pthread.h>

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

    // PS2X_PROF=1: once per second print the CPU% of every thread (summed by
    // thread name, from /proc/self/task/*/schedstat = ns on CPU), e.g.
    //   [prof] t=31 GameThread=99 VU1Worker=41 GSStage=8 GSGpu=23 blackkat=12 ...
    // 100 = one core fully busy. Reads /proc only; no hooks in the hot paths.
    void startThreadProf()
    {
        const char *e = std::getenv("PS2X_PROF");
        if (!e || !*e || *e == '0')
            return;
        std::thread([]()
                    {
                        pthread_setname_np(pthread_self(), "Prof");
                        std::map<int, std::pair<std::string, uint64_t>> last;
                        auto t0 = std::chrono::steady_clock::now();
                        auto prev = t0;
                        for (;;)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            const auto now = std::chrono::steady_clock::now();
                            const double wallNs = std::chrono::duration<double, std::nano>(now - prev).count();
                            prev = now;
                            std::map<int, std::pair<std::string, uint64_t>> cur;
                            std::error_code ec;
                            for (const auto &de : std::filesystem::directory_iterator("/proc/self/task", ec))
                            {
                                const std::string dir = de.path().string();
                                const int tid = std::atoi(de.path().filename().c_str());
                                unsigned long long ns = 0;
                                char name[64] = "?";
                                if (FILE *f = std::fopen((dir + "/schedstat").c_str(), "r"))
                                {
                                    if (std::fscanf(f, "%llu", &ns) != 1)
                                        ns = 0;
                                    std::fclose(f);
                                }
                                if (FILE *f = std::fopen((dir + "/comm").c_str(), "r"))
                                {
                                    if (std::fgets(name, sizeof(name), f))
                                        name[strcspn(name, "\n")] = 0;
                                    std::fclose(f);
                                }
                                cur[tid] = {name, ns};
                            }
                            if (last.empty())
                            {
                                last = std::move(cur); // first pass: baseline only
                                continue;
                            }
                            std::map<std::string, double> byName;
                            double total = 0.0;
                            for (const auto &[tid, v] : cur)
                            {
                                auto it = last.find(tid);
                                const uint64_t before = (it != last.end()) ? it->second.second : 0;
                                if (it == last.end() && !last.empty())
                                    continue; // new thread: skip its first partial second
                                const double pct = 100.0 * double(v.second - before) / wallNs;
                                std::string n = v.first;
                                if (n.rfind("GSRast", 0) == 0)
                                    n = "GSRast*";
                                byName[n] += pct;
                                total += pct;
                            }
                            last = std::move(cur);
                            std::vector<std::pair<double, std::string>> v;
                            for (const auto &[n, p] : byName)
                                if (p >= 0.5)
                                    v.push_back({p, n});
                            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
                            std::string line;
                            char buf[96];
                            for (const auto &[p, n] : v)
                            {
                                std::snprintf(buf, sizeof(buf), " %s=%.0f", n.c_str(), p);
                                line += buf;
                            }
                            std::fprintf(stderr, "[prof] t=%.0f total=%.0f%s\n",
                                         std::chrono::duration<double>(now - t0).count(), total, line.c_str());
                        } })
            .detach();
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

    // BLACKKAT_WATCH="guestStart,guestEnd[,seconds]": write-watch on guest RDRAM. The host pages
    // backing the range are made read-only; each write fault records the host RIP (offset from the
    // exe base, for addr2line -> generated FUN_xxx) plus the guest address, unprotects the page and
    // a helper thread re-protects every 2 ms. Writes blackkat_watch.txt after <seconds> (default 40).
    struct WatchHit { uint64_t rip; uint32_t guest; uint32_t ms; };
    constexpr size_t kWatchMax = 1u << 16;
    WatchHit g_watchHits[kWatchMax];
    std::atomic<size_t> g_watchN{0};
    uintptr_t g_watchLo = 0, g_watchHi = 0, g_watchRam = 0;
    struct sigaction g_watchPrev{};
    std::chrono::steady_clock::time_point g_watchT0;

    void watchHandler(int sig, siginfo_t *si, void *uc)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
        if (a >= g_watchLo && a < g_watchHi)
        {
            const size_t i = g_watchN.fetch_add(1, std::memory_order_relaxed);
            if (i < kWatchMax)
            {
                const auto *u = static_cast<const ucontext_t *>(uc);
                g_watchHits[i].rip = static_cast<uint64_t>(u->uc_mcontext.gregs[REG_RIP]);
                g_watchHits[i].guest = static_cast<uint32_t>(a - g_watchRam);
                g_watchHits[i].ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              std::chrono::steady_clock::now() - g_watchT0).count());
            }
            mprotect(reinterpret_cast<void *>(a & ~uintptr_t(4095)), 4096, PROT_READ | PROT_WRITE);
            return;
        }
        if (g_watchPrev.sa_flags & SA_SIGINFO)
        {
            if (g_watchPrev.sa_sigaction) { g_watchPrev.sa_sigaction(sig, si, uc); return; }
        }
        else if (g_watchPrev.sa_handler != SIG_DFL && g_watchPrev.sa_handler != SIG_IGN && g_watchPrev.sa_handler)
        {
            g_watchPrev.sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
    }

    void startWatch(uint8_t *rdram)
    {
        const char *e = std::getenv("BLACKKAT_WATCH");
        if (!e || !*e)
            return;
        unsigned long gs = 0, ge = 0; int secs = 40;
        if (std::sscanf(e, "%lx,%lx,%d", &gs, &ge, &secs) < 2 || ge <= gs)
            return;
        g_watchRam = reinterpret_cast<uintptr_t>(rdram);
        g_watchLo = (g_watchRam + gs + 4095) & ~uintptr_t(4095);
        g_watchHi = (g_watchRam + ge) & ~uintptr_t(4095);
        if (g_watchHi <= g_watchLo)
            return;
        g_watchT0 = std::chrono::steady_clock::now();
        struct sigaction sa{};
        sa.sa_sigaction = watchHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_watchPrev);
        std::cerr << "[blackkat] write-watch guest 0x" << std::hex << (g_watchLo - g_watchRam) << "-0x"
                  << (g_watchHi - g_watchRam) << std::dec << " for " << secs << "s" << std::endl;
        std::thread([secs]()
                    {
                        const auto end = g_watchT0 + std::chrono::seconds(secs);
                        while (std::chrono::steady_clock::now() < end)
                        {
                            mprotect(reinterpret_cast<void *>(g_watchLo), g_watchHi - g_watchLo, PROT_READ);
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        mprotect(reinterpret_cast<void *>(g_watchLo), g_watchHi - g_watchLo, PROT_READ | PROT_WRITE);
                        const size_t n = std::min(g_watchN.load(), kWatchMax);
                        const uintptr_t base = exeBase();
                        if (FILE *f = std::fopen("blackkat_watch.txt", "w"))
                        {
                            for (size_t i = 0; i < n; ++i)
                                std::fprintf(f, "%u %08x %llx\n", g_watchHits[i].ms, g_watchHits[i].guest,
                                             static_cast<unsigned long long>(g_watchHits[i].rip - base));
                            std::fclose(f);
                        }
                        std::cerr << "[blackkat] write-watch wrote " << n << " hits" << std::endl; })
            .detach();
    }
}

// Debug: BLACKKAT_FAKEKEY=="x,enter,down" synthesises real key events through
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
// Debug: BLACKKAT_KEYSCRIPT="7000:x:200,8500:down:150,9000:x+e:300" presses key(s) at absolute
// times (ms since start), held for the given ms. Keys: x z c v enter up down left right space
// q e lshift rshift tab w a s d; '+' joins keys pressed together.
static int blackkatKeyCode(const std::string &t)
{
    static const std::pair<const char *, int> m[] = {{"x", 88}, {"z", 90}, {"c", 67}, {"v", 86}, {"enter", 257},
        {"up", 265}, {"down", 264}, {"left", 263}, {"right", 262}, {"space", 32}, {"q", 81}, {"e", 69},
        {"lshift", 340}, {"rshift", 344}, {"tab", 258}, {"w", 87}, {"a", 65}, {"s", 83}, {"d", 68}};
    for (const auto &kv : m)
        if (t == kv.first)
            return kv.second;
    return 0;
}
static void blackkatStartKeyScript()
{
    const char *e = std::getenv("BLACKKAT_KEYSCRIPT");
    if (!e || !e[0])
        return;
    struct Step { int at; std::vector<int> keys; int hold; };
    std::vector<Step> steps;
    std::stringstream ss(e);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        const size_t a = item.find(':'), b = item.rfind(':');
        if (a == std::string::npos || b == a)
            continue;
        Step st{std::atoi(item.substr(0, a).c_str()), {}, std::atoi(item.substr(b + 1).c_str())};
        std::stringstream ks(item.substr(a + 1, b - a - 1));
        std::string k;
        while (std::getline(ks, k, '+'))
            if (int c = blackkatKeyCode(k))
                st.keys.push_back(c);
        if (!st.keys.empty())
            steps.push_back(st);
    }
    std::sort(steps.begin(), steps.end(), [](const Step &x, const Step &y) { return x.at < y.at; });
    const auto t0 = std::chrono::steady_clock::now();
    std::thread([steps, t0]()
                {
        void *win = nullptr;
        for (int i = 0; i < 400 && !(win = GetWindowHandle()); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!win) { std::cerr << "[keyscript] no window" << std::endl; return; }
        BlackkatGlfwKeyFun cb = glfwSetKeyCallback(win, nullptr);
        glfwSetKeyCallback(win, cb);
        if (!cb) { std::cerr << "[keyscript] no raylib key callback" << std::endl; return; }
        for (const Step &st : steps)
        {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(st.at));
            std::cerr << "[keyscript] t=" << st.at << " press";
            for (int k : st.keys) { std::cerr << " " << k; cb(win, k, 0, 1, 0); }
            std::cerr << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(st.hold));
            for (int k : st.keys) cb(win, k, 0, 0, 0);
        } })
        .detach();
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
    startThreadProf();

    try
    {
        // Command line:
        //   blackkat                      launcher (settings from ~/.config/blackkat/config.ini)
        //   blackkat --launcher [opts]    launcher, options pre-fill it
        //   blackkat --play [opts]        start with the saved settings, no launcher
        //   blackkat [elf] [iso]          classic: no launcher, no config file (env vars only)
        // opts: --iso P --elf P --gpu --cpu --scale N --fullscreen --windowed[=WxH] --audio --no-audio
        //       --movies --no-movies. Environment variables override all of these.
        std::filesystem::path cliElf, cliIso;
        {
            bool launcher = (argc <= 1), play = false, classic = false;
            bklaunch::Settings set;
            std::vector<std::string> opts(argv + 1, argv + argc);
            const bool anyDash = std::any_of(opts.begin(), opts.end(), [](const std::string &a) { return a.rfind("--", 0) == 0; });
            if (anyDash)
                bklaunch::loadSettings(set);
            for (size_t i = 0; i < opts.size(); ++i)
            {
                const std::string &a = opts[i];
                auto next = [&]() -> std::string { return i + 1 < opts.size() ? opts[++i] : std::string(); };
                if (a == "--launcher") launcher = true;
                else if (a == "--play") play = true;
                else if (a == "--iso") set.iso = next();
                else if (a == "--elf") set.elf = next();
                else if (a == "--gpu") set.gpu = true;
                else if (a == "--cpu") set.gpu = false;
                else if (a == "--scale") set.scale = std::clamp(std::atoi(next().c_str()), 1, 4);
                else if (a == "--fullscreen") set.fullscreen = true;
                else if (a.rfind("--windowed", 0) == 0) { set.fullscreen = false; int w = 0, h = 0; if (std::sscanf(a.c_str(), "--windowed=%dx%d", &w, &h) == 2) { set.winW = w; set.winH = h; } }
                else if (a == "--monitor") set.monitor = next();
                else if (a == "--widescreen" || a == "--aspect=16:9") set.widescreen = true;
                else if (a == "--aspect=4:3") set.widescreen = false;
                else if (a.rfind("--monitor=", 0) == 0) set.monitor = a.substr(10);
                else if (a == "--audio") set.audio = true;
                else if (a == "--no-audio") set.audio = false;
                else if (a == "--movies") set.movies = true;
                else if (a == "--no-movies") set.movies = false;
                else if (a == "--help" || a == "-h")
                {
                    std::cout << "Usage: blackkat [--launcher|--play] [--iso P] [--elf P] [--gpu|--cpu] [--scale 1-4]\n"
                                 "                [--fullscreen|--windowed[=WxH]] [--monitor NAME|INDEX|primary]\n"
                                 "                [--widescreen|--aspect=4:3|--aspect=16:9]\n"
                                 "                [--audio|--no-audio] [--movies|--no-movies]\n"
                                 "       blackkat [path/to/SLUS_203.65] [path/to/disc.iso]   (classic, no launcher/config)\n"
                                 "Settings: " << bklaunch::configPath() << std::endl;
                    return 0;
                }
                else if (a.rfind("--", 0) == 0) { std::cerr << "blackkat: unknown option " << a << " (see --help)" << std::endl; return 1; }
                else classic = true;
            }
            if (!classic && (launcher || play || anyDash))
            {
                if (argc <= 1)
                    bklaunch::loadSettings(set);
                if (set.elf.empty())
                    set.elf = bklaunch::defaultElf(argv[0]);
                if (launcher && !play)
                {
                    if (!bklaunch::runLauncher(set, argv[0]))
                        return 0;
                }
                else
                {
                    const bklaunch::IsoCheck chk = bklaunch::checkIso(set.iso, &set);
                    if (!chk.ok())
                        std::cerr << "[blackkat] disc image check: " << chk.message << " (" << set.iso << ")" << std::endl;
                }
                bklaunch::applyToEnv(set);
                cliElf = set.elf.empty() ? std::filesystem::path("game") / "SLUS_203.65" : std::filesystem::path(set.elf);
                std::cerr << "[blackkat] launcher settings: iso=" << set.iso << " elf=" << set.elf << " gpu=" << set.gpu
                          << " scale=" << set.scale << " audio=" << set.audio << " movies=" << set.movies
                          << " fullscreen=" << set.fullscreen << " window=" << set.winW << "x" << set.winH
                          << " monitor=" << (set.monitor.empty() ? "primary" : set.monitor)
                          << " aspect=" << (set.widescreen ? "16:9" : "4:3") << std::endl;
            }
        }
        const std::filesystem::path elfPath = cliElf.empty() ? guestElfPath(argc, argv) : cliElf;
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
            if (cliElf.empty() && argc >= 3 && argv[2] != nullptr && argv[2][0] != '\0')
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
        blackkatStartKeyScript();
        startWatch(runtime.memory().getRDRAM());

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
