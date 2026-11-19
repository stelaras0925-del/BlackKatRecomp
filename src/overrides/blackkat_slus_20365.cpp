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
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "runtime/ee_scheduler.h"
extern "C" void ps2x_spu2_upload(uint32_t spuByteAddr, const uint8_t *src, uint32_t bytes);

namespace
{
    // scePadPortOpen (libpad) at 0x002C91A8.
    // ps2pad.c (0x002ACDB8) opens slots 0..3 on ports 0..1 and treats any
    // 0 return as fatal: "ERROR: scePadPortOpen error on port %d, slot %d."
    // The fatal handler (0x002C4038) runs the shutdown hooks registered via
    // 0x002D8C98, which destroy every MEM heap (0x002B7D78), then hits a MIPS
    // `break`. The runtime keeps going, so the next allocation spins forever
    // in FUN_001f7620 at 0x1F7810 walking a zeroed heap table.
    // Real libpad opens multitap slots 1..3 even with no multitap (they just
    // report DISCONNECTED). The runtime stub only models slot 0, so pass slot
    // 0 through and accept slots 1..3 here.
    constexpr uint32_t kScePadPortOpen = 0x002C91A8u;

    // NOTE on hand-written leaf overrides: generated stub wrappers do
    // `ctx->pc = ra` before calling ps2_stubs::*. A replaceFunction() override
    // must do the same. Called through dispatchGuestBranch a missing pc update
    // is harmless (pc==entry -> fallthrough), but when the thread was preempted
    // at the dispatch checkpoint right before the call, EeScheduler::run()
    // resumes it by calling the override directly, and with pc left at the
    // override's own address it calls it again, forever. That is what hung
    // sceCdRead (run 18: main re-issued the same sceCdRead, each one
    // signalling CdReadCompleteThread, with no progress).
    void blackkatScePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t port = getRegU32(ctx, 4);
        const uint32_t slot = getRegU32(ctx, 5);
        if (slot == 0u || port > 1u || slot > 3u)
        {
            ps2_stubs::scePadPortOpen(rdram, ctx, runtime);
            return;
        }
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        if (uint8_t *dma = getMemPtr(rdram, dmaAddr))
        {
            std::memset(dma, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    // 0x001BCF98: FontIt allocator, slot +0x14 of the font vtable at
    // 0x0034A1C0 (reached only by JALR from 0x002BF374). Ghidra missed this
    // function start, so ps2_recomp emitted no code for it and the runtime
    // reports guest-branch:missing-target. Original body:
    //   a1 = a0; a2 = *(0x326A20) | *(0x326A2C) | 0x40; a0 = 0x6B9510;
    //   jal 0x001F7CC8 (MEM alloc wrapper); return its v0.
    // Implemented as a tail call: ra still holds the caller's return address,
    // so 0x001F7CC8 returns straight to it.
    constexpr uint32_t kFontItAlloc = 0x001BCF98u;
    constexpr uint32_t kMemAllocNamed = 0x001F7CC8u;

    void blackkatFontItAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        auto read32 = [rdram](uint32_t addr) -> uint32_t
        {
            uint32_t v = 0u;
            if (const uint8_t *p = getMemPtr(rdram, addr))
                std::memcpy(&v, p, sizeof(v));
            return v;
        };
        const uint32_t size = getRegU32(ctx, 4);
        const uint32_t flags = read32(0x00326A20u) | read32(0x00326A2Cu) | 0x40u;
        SET_GPR_U32(ctx, 2, read32(0x00326A20u));
        SET_GPR_U32(ctx, 3, 0x00320000u);
        SET_GPR_U32(ctx, 4, 0x006B9510u);
        SET_GPR_U32(ctx, 5, size);
        SET_GPR_U32(ctx, 6, flags);
        ctx->pc = kMemAllocNamed;
    }

    // Guest memory split. crt0 calls SetupHeap(_end=0x8042AC, -1), and the
    // runtime then hands out its own guestMalloc blocks (SIF command packets,
    // IOP host buffers, ...) starting at 0x8042B0. That is the same RAM that
    // the game's newlib sbrk gives to malloc, so the runtime overwrote the
    // "RAM" MEM heap header at 0x8042D0 (its 'BS' free-list sentinel), and
    // FUN_002b2fb0 then spins forever at 0x2B2FFC. Keep the two apart:
    //   game newlib sbrk : 0x008042AC .. kGameHeapEnd   (EndOfHeap returns it)
    //   runtime heap     : kGameHeapEnd .. 0x01F00000
    // The game peaked at sbrk 0x01AE4000 in test runs.
    // Set BLACKKAT_NO_HEAP_SPLIT=1 to turn this off.
    constexpr uint32_t kGameHeapEnd = 0x01C00000u;
    constexpr uint32_t kRuntimeHeapLimit = 0x01F00000u;
    constexpr uint32_t kEndOfHeapStub = 0x002B8340u;   // EndOfHeap syscall wrapper (stubbed)
    constexpr uint32_t kCrtInitAfterSetupHeap = 0x002AE540u; // first jal after SetupHeap in crt0
    PS2Runtime::RecompiledFunction g_origCrtInit = nullptr;

    void blackkatEndOfHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        ctx->pc = getRegU32(ctx, 31);
        setReturnU32(ctx, kGameHeapEnd);
    }

    void blackkatCrtInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool moved = false;
        if (!moved && runtime)
        {
            moved = true;
            const uint32_t usedEnd = runtime->guestHeapEnd();
            runtime->configureGuestHeap(kGameHeapEnd, kRuntimeHeapLimit);
            std::cerr << "[blackkat] runtime guest heap moved to 0x" << std::hex << kGameHeapEnd
                      << "-0x" << kRuntimeHeapLimit << " (previous end 0x" << usedEnd
                      << "); game sbrk capped at 0x" << kGameHeapEnd << std::dec << std::endl;
        }
        if (g_origCrtInit)
            g_origCrtInit(rdram, ctx, runtime);
    }

    // CD read completion. EA's ps2/cddrv.c registers 0x002AEE10 with
    // sceCdCallback and starts reads with sceCdRead (0x002D6A18). The
    // callback does iSignalSema(*(u32*)0x0068795C) for reason 1 (READ), which
    // wakes CdReadCompleteThread (0x002AEF30); that thread finishes the
    // FILESYS op (0x002B74F0). The runtime's sceCdCallback is a no-op and its
    // sceCdRead completes synchronously without calling any callback, so the
    // first read of "files.viv" never completes and the main thread polls
    // FUN_002b6440 (0x2B64B0) forever.
    constexpr uint32_t kSceCdCallback = 0x002B4A78u;
    constexpr uint32_t kSceCdRead = 0x002D6A18u;
    constexpr uint32_t kEaCdCallback = 0x002AEE10u;
    constexpr uint32_t kEaCdReadSemaAddr = 0x0068795Cu;
    uint32_t g_cdCallback = 0u;

    void blackkatSceCdCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t previous = g_cdCallback;
        g_cdCallback = getRegU32(ctx, 4);
        std::cerr << "[blackkat] sceCdCallback(0x" << std::hex << g_cdCallback << ")" << std::dec << std::endl;
        setReturnU32(ctx, previous);
    }

    // BLACKKAT_TRACE_CD=1: log sceCdRead calls (args, result, host time).
    bool cdTraceOn()
    {
        static const bool on = []()
        {
            const char *e = std::getenv("BLACKKAT_TRACE_CD");
            return e && e[0] == '1';
        }();
        return on;
    }

    void blackkatSceCdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t s_cdCalls = 0;
        static const auto s_cdT0 = std::chrono::steady_clock::now();
        const bool trace = cdTraceOn();
        const uint32_t inLbn = getRegU32(ctx, 4), inSec = getRegU32(ctx, 5), inBuf = getRegU32(ctx, 6),
                       inMode = getRegU32(ctx, 7), inRa = getRegU32(ctx, 31);
        const auto callT0 = std::chrono::steady_clock::now();
        ctx->pc = inRa;
        ps2_stubs::sceCdRead(rdram, ctx, runtime);
        const uint32_t result = getRegU32(ctx, 2);
        ++s_cdCalls;
        if (trace && (s_cdCalls <= 3000u || (s_cdCalls % 256u) == 0u || result == 0u))
        {
            const auto now = std::chrono::steady_clock::now();
            std::cerr << "[cdtrace] #" << s_cdCalls << " t="
                      << std::chrono::duration<double, std::milli>(now - s_cdT0).count() << "ms dur="
                      << std::chrono::duration<double, std::micro>(now - callT0).count() << "us lbn=0x" << std::hex
                      << inLbn << " sec=0x" << inSec << " buf=0x" << inBuf << " mode=0x" << inMode << " ra=0x" << inRa
                      << std::dec << " -> " << result << std::endl;
        }
        if (result == 0u || g_cdCallback == 0u)
            return;
        if (g_cdCallback != kEaCdCallback)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                std::cerr << "[blackkat] sceCdRead: unknown CD callback 0x" << std::hex << g_cdCallback
                          << " not invoked" << std::dec << std::endl;
            }
            return;
        }
        uint32_t sema = 0u;
        if (const uint8_t *p = getMemPtr(rdram, kEaCdReadSemaAddr))
            std::memcpy(&sema, p, sizeof(sema));
        const uint32_t savedA0 = getRegU32(ctx, 4);
        SET_GPR_U32(ctx, 4, sema);
        ps2_syscalls::iSignalSema(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 4, savedA0);
        SET_GPR_U32(ctx, 2, result);
    }

    // 0x2B97A0 (missed by Ghidra, 4-insn leaf reached via JALR from 0x2FA86C through
    // the hook pointer at gp-0x6E94):
    //   addiu v0,zero,0x3A ; beql a0,v0,+1 ; sw a1,-0x7158(gp) ; jr ra ; nop
    void blackkatHook2B97A0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        SET_GPR_S32(ctx, 2, 0x3A);
        if (getRegU32(ctx, 4) == 0x3Au)
        {
            const uint32_t addr = getRegU32(ctx, 28) - 0x7158u;
            const uint32_t val = getRegU32(ctx, 5);
            std::memcpy(rdram + (addr & 0x1FFFFFFu), &val, 4);
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    // Re-entrancy preemption fix.
    // PS2Runtime::dispatchGuestBranch treats "ctx->pc == entryPc after the call"
    // as "callee returned without setting pc" and falls through. When a function
    // F is re-entered (F -> cb -> F, e.g. FILESYS_callbackop 0x2B6198 -> addbig
    // callback 0x2B6B58 -> 0x2B6198) and the EE checkpoint preempts exactly at the
    // inner call, ctx->pc == F and the OUTER dispatch wrongly falls through with
    // the inner frame's sp -> epilogue loads ra=0 -> main thread dies at PC 0
    // (sp=0x1FFFDD0). Wrapper: if F's body unwinds with pc==F but sp != entry sp,
    // execute F's first instruction (addiu sp,sp,-N) here and resume at F+4, which
    // is an exact equivalent and no longer aliases entryPc.
    constexpr uint32_t kReentryAddrs[] = {0x002B6198u, 0x002B6B58u, 0x002B6A10u, 0x002B6510u, 0x002B6858u};
    constexpr size_t kReentryCount = sizeof(kReentryAddrs) / sizeof(kReentryAddrs[0]);
    PS2Runtime::RecompiledFunction g_reentryOrig[kReentryCount] = {};
    unsigned g_reentryHits = 0;

    template <size_t I>
    void blackkatReentryWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fn = kReentryAddrs[I];
        const uint32_t entrySp = getRegU32(ctx, 29);
        g_reentryOrig[I](rdram, ctx, runtime);
        if (ctx->pc != fn || getRegU32(ctx, 29) == entrySp)
            return;
        uint32_t insn = 0;
        std::memcpy(&insn, rdram + (fn & 0x1FFFFFFu), 4);
        if ((insn & 0xFFFF0000u) != 0x27BD0000u) // addiu $sp,$sp,imm
            return;
        if (!runtime->hasFunction(fn + 4u)) // no resume slot: leave it to the runtime's sp check
            return;
        const int32_t imm = static_cast<int16_t>(insn & 0xFFFFu);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(getRegU32(ctx, 29) + static_cast<uint32_t>(imm)));
        ctx->pc = fn + 4u;
        if (g_reentryHits < 64)
        {
            ++g_reentryHits;
            std::cerr << std::hex << "[blackkat] re-entrant preemption at 0x" << fn << " (outer sp=0x" << entrySp
                      << ", inner sp=0x" << (getRegU32(ctx, 29) - static_cast<uint32_t>(imm))
                      << ") -> resume at 0x" << ctx->pc << std::dec << std::endl;
        }
    }

    template <size_t... Is>
    void bindReentryWrappers(PS2Runtime &runtime, std::index_sequence<Is...>)
    {
        ((g_reentryOrig[Is] = runtime.lookupFunction(kReentryAddrs[Is]),
          (g_reentryOrig[Is] ? (void)runtime.replaceFunction(kReentryAddrs[Is], &blackkatReentryWrapper<Is>) : (void)0)),
         ...);
    }

    // FMV skip. 0x001E1F88 is FMV::PlayFile(player, "<name>.MPC", flag):
    //   open (0x1E1FF8); start (0x1E1828); while (!frame(0x1E1B00)); stop
    //   (0x1E2038); close (0x1E2158). Frames are decoded with EA's IpuMpeg
    //   code driving the IPU registers directly (FDEC/VDEC/IDEC via
    //   0x10002000/0x10002010, e.g. next_start_code at 0x2C77E8). PS2Recomp has
    //   no IPU emulation (IPU_CMD reads back the last written command), so the
    //   first start-code search never finds 0x000001 and main() spins at
    //   0x2C7828..0x2C7838 forever (main -> 0x1014F4 -> 0x1E1F88 -> 0x1E1B00 ->
    //   0x2B3B50 -> 0x2B3E00 -> 0x2C78C0 -> 0x2C77E8). Callers: main intro
    //   (0x1014F4), 0x101CA8, 0x1E6784; all ignore the (void) result and free
    //   the player themselves. Skip by returning at once.
    // Movies play by default now (IPU model on): BLACKKAT_PLAY_FMV=0 (or PS2X_IPU=0) restores these stubs.
    constexpr uint32_t kFmvPlayFile = 0x001E1F88u;
    // BLACKKAT_PLAY_FMV: default 1 (play movies through the IPU model); 0 = skip them (stubs).
    // PS2X_IPU=0 (IPU model off) also selects the stubs, since the player would spin forever.
    bool blackkatPlayFmv()
    {
        static const bool on = []() {
            const char *pf = std::getenv("BLACKKAT_PLAY_FMV");
            const char *ipu = std::getenv("PS2X_IPU");
            return !(pf && pf[0] == '0') && !(ipu && ipu[0] == '0');
        }();
        return on;
    }

    void blackkatFmvSkip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        char name[64] = {};
        const uint32_t nameAddr = getRegU32(ctx, 5);
        if (const uint8_t *p = getMemPtr(rdram, nameAddr))
        {
            for (size_t i = 0; i + 1 < sizeof(name); ++i)
            {
                const char c = static_cast<char>(p[i]);
                if (c == '\0' || c < 0x20 || c > 0x7e)
                    break;
                name[i] = c;
            }
        }
        std::cerr << "[blackkat] FMV skipped: \"" << name << "\" (ra=0x" << std::hex << getRegU32(ctx, 31)
                  << std::dec << ")" << std::endl;
        ctx->pc = getRegU32(ctx, 31);
    }

    // FMV::DecodeFrame(player, flag) at 0x1E1B00: returns 1 at end of stream,
    // 2 if aborted, 0 after decoding a frame (IPU). The front end (0x1E6C40,
    // jal at 0x1E797C / 0x1E7944) drives movies frame by frame itself, not
    // through 0x1E1F88, so it hits the same IPU start-code spin. Report end of
    // stream so the caller stops/loops the movie and carries on.
    constexpr uint32_t kFmvDecodeFrame = 0x001E1B00u;

    // Return trampolines for the guest calls made from the DecodeFrame stub
    // (pump 0x2B31E0 and the page frame callback). If such a call is preempted
    // (the load page's callback 0x1EC8F0 waits on sceMcSync inside the save
    // load 0x1BDAC8), the scheduler later resumes it and its "jr ra" used to
    // land straight on the stub's caller (0x1E7984) with the callback's v0.
    // The load callback returns 1 ("leave front end"), which the front end
    // then read as "clip ended" instead of abort=1/DecodeFrame=2, so it
    // restarted the page clip and re-ran the load forever: black screen after
    // picking a save slot. The calls now return here (addresses inside the
    // replaced DecodeFrame body, never reached by guest code) and finish the
    // stub's work: abort flag = callback result, DecodeFrame returns 0.
    // BLACKKAT_FMV_CB_TRAMPOLINE=0 restores the old behaviour.
    constexpr uint32_t kFmvPumpReturn = 0x001E1B08u;
    constexpr uint32_t kFmvFrameCbReturn = 0x001E1B10u;
    uint32_t g_fmvCbPlayer = 0u;
    uint32_t g_fmvCbRa = 0u;

    bool fmvCbTrampolineOn()
    {
        static const bool on = []() {
            const char *e = std::getenv("BLACKKAT_FMV_CB_TRAMPOLINE");
            return !(e && e[0] == '0');
        }();
        return on;
    }

    void blackkatFmvPumpReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Pump finished after a preemption: report "frame decoded"; the
        // callback runs on the next DecodeFrame call.
        static unsigned logged = 0;
        if (logged++ < 20)
            std::cerr << "[blackkat] FMV pump resumed -> ra=0x" << std::hex << g_fmvCbRa << std::dec << std::endl;
        setReturnS32(ctx, 0);
        SET_GPR_U32(ctx, 31, g_fmvCbRa);
        ctx->pc = g_fmvCbRa;
    }

    void blackkatFmvFrameCbReturn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t res = getRegU32(ctx, 2);
        if (uint32_t *pl = reinterpret_cast<uint32_t *>(getMemPtr(rdram, g_fmvCbPlayer)))
            pl[0x50 / 4] = res;
        static unsigned logged = 0;
        if (logged++ < 40)
            std::cerr << "[blackkat] FMV frame cb resumed -> 0x" << std::hex << res << " (player=0x" << g_fmvCbPlayer
                      << " ra=0x" << g_fmvCbRa << ")" << std::dec << std::endl;
        setReturnS32(ctx, 0);
        SET_GPR_U32(ctx, 31, g_fmvCbRa);
        ctx->pc = g_fmvCbRa;
    }

    void blackkatFmvFrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        static unsigned logged = 0;
        if (logged < 4)
        {
            ++logged;
            std::cerr << "[blackkat] FMV frame stub flag=" << getRegU32(ctx, 5) << " (ra=0x" << std::hex
                      << getRegU32(ctx, 31) << std::dec << ")" << std::endl;
        }
        // flag 0: per-frame call from the front-end loop (0x1E797C). Returning
        // 1 there ends the movie, and the front end then stops/restarts it every
        // pass (0x1E2038 -> big memset) without drawing anything (run 22).
        // Return 0 ("frame decoded") so it carries on with its menu path.
        // flag 1: blocking prime/first-frame loops (0x1E7944, PlayFile) need a
        // nonzero result to exit.
        // BLACKKAT_FMV_FRAMES=N (opt-in, untested): report end of stream after N
        // flag-0 frames, so menu transition movies end and the front end can
        // advance. Default 0 = never end (tested in run 23).
        static const uint32_t s_fmvFrames = []() -> uint32_t {
            const char *e = std::getenv("BLACKKAT_FMV_FRAMES");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 0u;
        }();
        static uint32_t s_player = 0, s_count = 0;
        const uint32_t player = getRegU32(ctx, 4);
        const uint32_t flag = getRegU32(ctx, 5);
        const uint32_t ra = getRegU32(ctx, 31);
        uint32_t *pl = reinterpret_cast<uint32_t *>(getMemPtr(rdram, player));
        {
            // Log front-end state changes: frame callback (menu page) and the
            // menu flags 0x3A3DD4..0x3A3DFC.
            static uint32_t lastCb = ~0u, lastFlags[11] = {};
            static bool first = true;
            const uint32_t cbNow = pl ? pl[0] : 0u;
            uint32_t f[11];
            for (int i = 0; i < 11; ++i)
                std::memcpy(&f[i], rdram + 0x3A3DD4u + 4u * i, 4);
            // The flags include per-frame counters, so this fired ~2000 times/s on the main menu
            // (std::endl flush each time), halving the EE speed there (vsync/s ~29, ADMA audio at
            // half rate -> music skipping). Log the first 64 changes; BLACKKAT_MENU_LOG=1 logs all.
            static const bool s_menuLogAll = []() { const char *e = std::getenv("BLACKKAT_MENU_LOG"); return e && e[0] == '1'; }();
            static uint32_t s_menuLogged = 0;
            if ((first || cbNow != lastCb || std::memcmp(f, lastFlags, sizeof f) != 0) && (s_menuLogAll || s_menuLogged++ < 64u))
            {
                first = false;
                lastCb = cbNow;
                std::memcpy(lastFlags, f, sizeof f);
                std::cerr << "[blackkat] menu state: cb=0x" << std::hex << cbNow << " flags(3DD4..3DFC)=";
                for (int i = 0; i < 11; ++i)
                    std::cerr << f[i] << (i < 10 ? "," : "");
                std::cerr << std::dec << " flag=" << flag << std::endl;
            }
        }
        // Real 0x1E1B00 (see 0x1E1B48 / 0x1E1CF8 / 0x1E1DB4):
        //   if (stream ended) return 1; if (p->abort@0x50) return 2;
        //   decode rows, pumping 0x2B31E0(0) per row;
        //   if (p->frameCb@0x0) p->abort = p->frameCb();   // menu: 0x1E8028
        //   if (flag && pad(0x312538) & 0x9F0) p->abort = 1; return 0;
        // The front end's menu logic lives in that per-frame callback, so the
        // stub has to run it (run 26: callback never ran, menu ignored input).
        if (pl && pl[0x50 / 4] != 0u)
        {
            static unsigned abortLogged = 0;
            if (abortLogged++ < 50)
                std::cerr << "[blackkat] FMV abort=" << pl[0x50 / 4] << " player=0x" << std::hex << player
                          << " ra=0x" << ra << std::dec << " -> 2" << std::endl;
            setReturnS32(ctx, 2);
            return;
        }
        if (flag != 0u)
        {
            s_player = player;
            s_count = 0;
            setReturnS32(ctx, 1);
            return;
        }
        if (player != s_player)
        {
            s_player = player;
            s_count = 0;
        }
        // Front-end page transitions (Continue -> load page, Options, ...): the
        // front end (0x1E7988..0x1E7C58) sets 0x3A3DEC = 1 while a transition clip
        // plays and only moves to the new page when that clip reports end of
        // stream (DecodeFrame -> 1). The page callback skips all drawing while
        // 0x3A3DEC != 0, so with the stub never ending a clip the screen froze on
        // the main menu (Continue "did nothing"). End transition clips after
        // BLACKKAT_FMV_TRANSITION_FRAMES flag-0 frames (default 40, so the X press that
        // opened the page is released before the page reads the pad; 0 = old hang).
        {
            static const uint32_t s_transFrames = []() -> uint32_t {
                const char *e = std::getenv("BLACKKAT_FMV_TRANSITION_FRAMES");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 40u;
            }();
            static uint32_t s_transCount = 0;
            uint32_t inTransition = 0;
            std::memcpy(&inTransition, rdram + 0x3A3DECu, 4);
            if (s_transFrames != 0u && inTransition != 0u)
            {
                if (++s_transCount >= s_transFrames)
                {
                    s_transCount = 0;
                    static unsigned transLogged = 0;
                    if (transLogged++ < 20)
                        std::cerr << "[blackkat] FMV transition clip ended (player=0x" << std::hex << player
                                  << std::dec << ")" << std::endl;
                    setReturnS32(ctx, 1);
                    return;
                }
            }
            else
            {
                s_transCount = 0;
            }
        }
        if (s_fmvFrames != 0u && ++s_count >= s_fmvFrames)
        {
            s_count = 0;
            setReturnS32(ctx, 1);
            return;
        }
        static const bool s_pump = []() {
            const char *e = std::getenv("BLACKKAT_FMV_NOPUMP");
            return !(e && e[0] == '1');
        }();
        if (s_pump && runtime)
        {
            const bool tramp = fmvCbTrampolineOn();
            const uint32_t pumpRet = tramp ? kFmvPumpReturn : ra;
            g_fmvCbPlayer = player;
            g_fmvCbRa = ra;
            SET_GPR_U32(ctx, 4, 0u);
            SET_GPR_U32(ctx, 31, pumpRet);
            ctx->pc = 0x002B31E0u;
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x002B31E0u, ra - 8u, pumpRet,
                                              PS2Runtime::GuestBranchKind::DirectCall, "FMVPUMP"))
                return; // preempted: resumes at pumpRet (trampoline -> ra)
            SET_GPR_U32(ctx, 31, ra);
            ctx->pc = ra;
            const uint32_t cb = pl ? pl[0] : 0u;
            if (cb != 0u && runtime->hasFunction(cb))
            {
                const uint32_t cbRet = tramp ? kFmvFrameCbReturn : ra;
                g_fmvCbPlayer = player;
                g_fmvCbRa = ra;
                SET_GPR_U32(ctx, 4, player);
                SET_GPR_U32(ctx, 31, cbRet);
                ctx->pc = cb;
                if (!runtime->dispatchGuestBranch(rdram, ctx, cb, ra - 8u, cbRet,
                                                  PS2Runtime::GuestBranchKind::IndirectCall, "FMVFRAMECB"))
                    return; // preempted: resumes at cbRet (trampoline sets abort, returns 0 to ra)
                const uint32_t res = getRegU32(ctx, 2);
                pl = reinterpret_cast<uint32_t *>(getMemPtr(rdram, player));
                if (pl)
                    pl[0x50 / 4] = res;
                static unsigned cbLogged = 0;
                if (cbLogged < 3 || (res != 0u && cbLogged < 60))
                {
                    ++cbLogged;
                    std::cerr << "[blackkat] FMV frame cb 0x" << std::hex << cb << " -> 0x" << res << std::dec
                              << std::endl;
                }
                SET_GPR_U32(ctx, 31, ra);
                ctx->pc = ra;
            }
        }
        // Real DecodeFrame waits for the IPU to decode a movie frame; the stub returns at once, so
        // the main menu loop spun ~4000 times per emulated second and the EE could only reach
        // ~150-170 Mcyc/s there (vsync/s ~29-34, ADMA audio at half rate -> menu music skipping).
        // Charge one vsync worth of EE time per completed flag-0 frame (after the pump and the
        // frame callback ran; charging before them made every pump get preempted, so the menu
        // callback never ran) instead (an "IPU decode" that takes
        // a frame). BLACKKAT_FMV_STUB_CYCLES=N overrides (0 = old behaviour).
        {
            static const uint32_t s_stubCycles = []() -> uint32_t {
                const char *e = std::getenv("BLACKKAT_FMV_STUB_CYCLES");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 4915200u;
            }();
            if (s_stubCycles != 0u && runtime)
                runtime->eeScheduler().accountCycles(s_stubCycles);
        }
        setReturnS32(ctx, 0);
    }

    // FMV::Stop (0x1E2038): when the player has an audio stream (p+0x74 >= 0)
    // it spins on 0x2BEF90(stream) until the stream reports state 3 or the
    // abort flag p+0x50 is set. The SPU/stream side is only load-emulated, so
    // the state never reaches 3 (run 27: hung at 0x1E207C after the attract
    // clip sizzle01_e.mpc). Set the abort flag before calling it so the wait
    // ends at once. BLACKKAT_FMV_STOP_WAIT=1 disables.
    constexpr uint32_t kFmvStop = 0x001E2038u;
    PS2Runtime::RecompiledFunction g_origFmvStop = nullptr;

    void blackkatFmvStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (uint32_t *pl = reinterpret_cast<uint32_t *>(getMemPtr(rdram, getRegU32(ctx, 4))))
        {
            if (static_cast<int32_t>(pl[0x74 / 4]) >= 0 && pl[0x50 / 4] == 0u)
            {
                static unsigned logged = 0;
                if (logged++ < 20)
                    std::cerr << "[blackkat] FMV stop: stream 0x" << std::hex << pl[0x74 / 4]
                              << " -> abort flag set (ra=0x" << getRegU32(ctx, 31) << std::dec << ")" << std::endl;
                pl[0x50 / 4] = 1u;
            }
        }
        g_origFmvStop(rdram, ctx, runtime);
    }

    // FMV idle skip (BLACKKAT_FMV_IDLE_CYCLES, default 16384, 0 = off). EA's movie player spins in two
    // polling loops, both only reached while it has nothing to do:
    //  * 0x1E1E70: pump(0x2B31E0); chunk = 0x2B2868(stream); if (!chunk && !0x2B2B90(stream)) repeat
    //    (waits for the CD stream to deliver the next MPC chunk; 0x2B2B90 has no other caller and is
    //    only called when no chunk is ready);
    //  * 0x1E1D80: pump; while (GS CSR & FIELD) repeat (waits for the next field/vsync).
    // Each iteration costs ~1 us of host time but only a few hundred EE cycles, so emulated time (and
    // with it the IOP/CD and vsync) crawled. Charge extra EE cycles per idle iteration so the wait ends
    // after a handful of iterations; deadlines (vsync etc.) still fire at the next checkpoint.
    constexpr uint32_t kStreamWaitCheck = 0x002B2B90u;
    constexpr uint32_t kTaskPump = 0x002B31E0u;
    PS2Runtime::RecompiledFunction g_origStreamWaitCheck = nullptr;
    PS2Runtime::RecompiledFunction g_origTaskPump = nullptr;
    uint32_t fmvIdleCycles()
    {
        static const uint32_t v = []() -> uint32_t {
            const char *e = std::getenv("BLACKKAT_FMV_IDLE_CYCLES");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 0)) : 16384u;
        }();
        return v;
    }
    uint64_t g_fmvIdleCharged = 0;

    void blackkatStreamWaitCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origStreamWaitCheck(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) == 0u && runtime) // stream not finished, no chunk yet: idle
        {
            runtime->eeScheduler().accountCycles(fmvIdleCycles());
            g_fmvIdleCharged += fmvIdleCycles();
        }
    }

    void blackkatTaskPump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = getRegU32(ctx, 31);
        g_origTaskPump(rdram, ctx, runtime);
        if (ra == 0x001E1D88u && runtime) // FIELD wait loop in DecodeFrame
        {
            runtime->eeScheduler().accountCycles(fmvIdleCycles());
            g_fmvIdleCharged += fmvIdleCycles();
        }
    }

    // FMV frame pacing (BLACKKAT_FMV_PACE, default 1, 0 = off). DecodeFrame (0x1E1B00) decodes one
    // picture and then only waits while GS CSR FIELD is set (odd field); during an even field it
    // returns at once. On hardware the IPU decode takes most of a field, so that gives ~29.97 fps.
    // Here the IPU is instant, so several pictures went out per even field: the video ran ahead
    // until the CD stream ran dry (38 pictures in the first 54 vsyncs, ~66 pictures / 2.2 s ahead of
    // the audio, then stutter while waiting for data). Hold each call until 2 vsyncs after the
    // previous one (absolute schedule, rebased after a stall of > 8 vsyncs or a new player) by
    // blocking the EE thread in the scheduler's vsync wait and re-entering 0x1E1B00 afterwards.
    PS2Runtime::RecompiledFunction g_origFmvDecodeFrame = nullptr;
    bool fmvPaceOn()
    {
        static const bool on = []() {
            const char *e = std::getenv("BLACKKAT_FMV_PACE");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    uint64_t g_fmvPaceSlot = 0;
    uint32_t g_fmvPacePlayer = 0;
    bool g_fmvPaceValid = false;
    uint64_t g_fmvPaceWaits = 0, g_fmvPaceRebases = 0;

    void blackkatFmvDecodePaced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        const uint64_t now = runtime->eeScheduler().currentVSyncTick();
        if (!g_fmvPaceValid || player != g_fmvPacePlayer || now > g_fmvPaceSlot + 8u || now + 64u < g_fmvPaceSlot)
        {
            if (g_fmvPaceValid && g_fmvPaceRebases < 8u)
                std::cerr << "[blackkat] FMV pace rebase at vsync " << now << " (slot " << g_fmvPaceSlot << ", waits "
                          << g_fmvPaceWaits << ")" << std::endl;
            ++g_fmvPaceRebases;
            g_fmvPaceSlot = now;
            g_fmvPacePlayer = player;
            g_fmvPaceValid = true;
        }
        if (now < g_fmvPaceSlot)
        {
            ++g_fmvPaceWaits;
            runtime->eeWaitVSyncTicks(static_cast<uint32_t>(g_fmvPaceSlot - now), kFmvDecodeFrame); // re-enters here
        }
        g_fmvPaceSlot += 2u;
        g_origFmvDecodeFrame(rdram, ctx, runtime);
    }

    // FMV start (0x1E1828(player, name, 0)): log the clip name so front-end
    // state changes are visible in the log. Wraps the generated function.
    // BLACKKAT_SKINLOG=N (debug): log the first N calls of the EE skinning
    // routine 0x1BC920 (3-matrix vertex transform, called from 0x1E3748):
    // inputs, matrices and a few transformed vertices.
    constexpr uint32_t kSkinPos = 0x001BC920u;
    PS2Runtime::RecompiledFunction g_origSkinPos = nullptr;

    void blackkatSkinLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_max = []() { const char *e = std::getenv("BLACKKAT_SKINLOG"); return e ? std::atoi(e) : 0; }();
        static int s_n = 0;
        const uint32_t src = getRegU32(ctx, 4), idx = getRegU32(ctx, 5), m1 = getRegU32(ctx, 6), m2 = getRegU32(ctx, 7),
                       m3 = getRegU32(ctx, 8), dst = getRegU32(ctx, 9), cnt = getRegU32(ctx, 10), ra = getRegU32(ctx, 31);
        g_origSkinPos(rdram, ctx, runtime);
        if (s_n >= s_max || ctx->pc != ra)
            return;
        ++s_n;
        auto f = [&](uint32_t a) { float v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 4); return v; };
        auto b = [&](uint32_t a) { int v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) v = static_cast<int8_t>(*p); return v; };
        std::fprintf(stderr, "[skin] #%d src=%x idx=%x m=%x,%x,%x dst=%x n=%u\n", s_n, src, idx, m1, m2, m3, dst, cnt);
        for (uint32_t m : {m1, m2, m3})
            std::fprintf(stderr, "[skin]   M %x: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n", m,
                         f(m), f(m + 4), f(m + 8), f(m + 12), f(m + 16), f(m + 20), f(m + 24), f(m + 28),
                         f(m + 32), f(m + 36), f(m + 40), f(m + 44), f(m + 48), f(m + 52), f(m + 56), f(m + 60));
        for (uint32_t i = 0; i < cnt && i < 4; ++i)
            std::fprintf(stderr, "[skin]   v%u i=%d in=(%g %g %g %g) out=(%g %g %g)\n", i, b(idx + i),
                         f(src + i * 16), f(src + i * 16 + 4), f(src + i * 16 + 8), f(src + i * 16 + 12),
                         f(dst + i * 12), f(dst + i * 12 + 4), f(dst + i * 12 + 8));
    }

    // BLACKKAT_BONELOG=N (debug): log the bone matrix concatenation 0x1BC790
    // (out = local(a0) * parent(a1)) when called from the skeleton update 0x1A89A8.
    constexpr uint32_t kMatMul = 0x001BC790u;
    PS2Runtime::RecompiledFunction g_origMatMul = nullptr;

    void blackkatBoneLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_max = []() { const char *e = std::getenv("BLACKKAT_BONELOG"); return e ? std::atoi(e) : 0; }();
        static int s_n = 0;
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6), ra = getRegU32(ctx, 31);
        auto f = [&](uint32_t a) { float v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 4); return v; };
        const float lt[4] = {f(a0 + 48), f(a0 + 52), f(a0 + 56), f(a0 + 60)};
        const float lr0[4] = {f(a0), f(a0 + 4), f(a0 + 8), f(a0 + 12)};
        g_origMatMul(rdram, ctx, runtime);
        static const float s_lo = []() { const char *e = std::getenv("BLACKKAT_BONELOG_X"); return e ? std::strtof(e, nullptr) : -1e30f; }();
        const float px = f(a1 + 48);
        if (s_n >= s_max || px < s_lo || px > s_lo + 200.0f)
            return;
        ++s_n;
        std::fprintf(stderr, "[bone] #%d ra=%x local=%x r0=(%g %g %g %g) t=(%g %g %g %g) parent=%x t=(%g %g %g %g) out=%x t=(%g %g %g %g)\n",
                     s_n, ra, a0, lr0[0], lr0[1], lr0[2], lr0[3], lt[0], lt[1], lt[2], lt[3], a1, f(a1 + 48), f(a1 + 52), f(a1 + 56), f(a1 + 60),
                     a2, f(a2 + 48), f(a2 + 52), f(a2 + 56), f(a2 + 60));
    }

    // BLACKKAT_PARTLOG=N (debug): log the first N model draws (0x1E3588) whose first
    // bone matrix lies near BLACKKAT_PARTLOG_X,Z (+-2 units): per part the 64-bit
    // flags at +0x78, +0x86, bone indices and the batch chain at +0x6C.
    constexpr uint32_t kModelSkin = 0x001E3588u;
    PS2Runtime::RecompiledFunction g_origModelSkin = nullptr;

    void blackkatPartLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const int s_max = []() { const char *e = std::getenv("BLACKKAT_PARTLOG"); return e ? std::atoi(e) : 0; }();
        static const float s_x = []() { const char *e = std::getenv("BLACKKAT_PARTLOG_X"); return e ? std::strtof(e, nullptr) : 0.0f; }();
        static const float s_z = []() { const char *e = std::getenv("BLACKKAT_PARTLOG_Z"); return e ? std::strtof(e, nullptr) : 0.0f; }();
        static int s_n = 0;
        const uint32_t obj = getRegU32(ctx, 4);
        auto u32 = [&](uint32_t a) { uint32_t v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 4); return v; };
        auto u64 = [&](uint32_t a) { uint64_t v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 8); return v; };
        auto u16 = [&](uint32_t a) { uint16_t v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 2); return v; };
        auto f = [&](uint32_t a) { float v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 4); return v; };
        bool hit = false;
        const uint32_t parts = u32(obj), nparts = u16(obj + 4), skel = u32(obj + 0x1C);
        const uint32_t mats = skel ? u32(skel + 0x10) : 0u;
        if (s_n < s_max && parts && nparts && mats)
        {
            const int16_t b0 = static_cast<int16_t>(u16(parts + 0x80));
            const uint32_t m = mats + static_cast<uint32_t>(b0) * 128u;
            hit = std::fabs(f(m + 48) - s_x) < 2.0f && std::fabs(f(m + 56) - s_z) < 2.0f;
        }
        if (hit)
        {
            ++s_n;
            std::fprintf(stderr, "[part] #%d obj=%x flags6=%04x parts=%x n=%u skel=%x mats=%x ra=%x\n", s_n, obj, u16(obj + 6), parts, nparts, skel, mats, getRegU32(ctx, 31));
            for (uint32_t i = 0; i < nparts && i < 64; ++i)
            {
                const uint32_t pp = parts + i * 0x90u;
                const uint64_t fl = u64(pp + 0x78);
                uint32_t nb = 0, nv = 0;
                for (uint32_t b = u32(pp + 0x6C); b && nb < 256; b = u32(b + 0x20)) { ++nb; nv += u32(b + 0x24) >> 5; }
                const int16_t i0 = static_cast<int16_t>(u16(pp + 0x80));
                const uint32_t m0 = mats + static_cast<uint32_t>(i0) * 128u;
                std::fprintf(stderr, "[part]   %2u fl=%016llx cnt=%llu f86=%04x bones=%d,%d,%d skinb=%u skinv=%u m0t=(%.3f %.3f %.3f)\n", i,
                             (unsigned long long)fl, (unsigned long long)((fl >> 47) & 0x3FF), u16(pp + 0x86), i0,
                             static_cast<int16_t>(u16(pp + 0x82)), static_cast<int16_t>(u16(pp + 0x84)), nb, nv, f(m0 + 48), f(m0 + 52), f(m0 + 56));
            }
        }
        g_origModelSkin(rdram, ctx, runtime);
        if (hit)
        {
            for (uint32_t i = 0; i < nparts && i < 128; ++i)
            {
                const uint32_t pp = parts + i * 0x90u;
                for (uint32_t b = u32(pp + 0x6C), k = 0; b && k < 4; b = u32(b + 0x20), ++k)
                {
                    const uint32_t pk = u32(b + 0x1C);
                    std::fprintf(stderr, "[partpk] %3u.%u batch=%x pkt=%x len=%x e=%x q0=%016llx %016llx q1=%016llx %016llx\n", i, k, b, pk, u32(b + 0x24), u16(b + 0xE),
                                 (unsigned long long)u64(pk), (unsigned long long)u64(pk + 8), (unsigned long long)u64(pk + 16), (unsigned long long)u64(pk + 24));
                }
            }
        }
    }

    // BLACKKAT_DMALOG=lo:hi (debug): log 0x154DC0 (bucket DMA tag append) calls whose
    // CALL target (a1 >> 32) is in [lo,hi), and 0x15AE68 (part draw) calls whose part
    // pointer (a0) is in [lo2,hi2) given by BLACKKAT_DMALOG_PARTS=lo:hi.
    constexpr uint32_t kDmaAppend = 0x00154DC0u;
    constexpr uint32_t kPartDraw = 0x0015AE68u;
    PS2Runtime::RecompiledFunction g_origDmaAppend = nullptr;
    PS2Runtime::RecompiledFunction g_origPartDraw = nullptr;
    PS2Runtime::RecompiledFunction g_origUvAnim = nullptr;
    unsigned g_dmaAppendCalls = 0, g_uvAnimCalls = 0;
    bool g_inKatPart = false;
    void blackkatUvAnimLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_uvAnimCalls;
        const uint32_t a0 = getRegU32(ctx, 4), ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
        g_origUvAnim(rdram, ctx, runtime);
        if (g_inKatPart)
            std::fprintf(stderr, "[uvanim] batch=%x ra=%x -> pc=%x sp %x->%x\n", a0, ra, ctx->pc, sp, getRegU32(ctx, 29));
    }
    bool parseRange(const char *e, uint32_t &lo, uint32_t &hi)
    {
        if (!e) return false;
        char *x = nullptr;
        lo = static_cast<uint32_t>(std::strtoul(e, &x, 16));
        hi = (x && *x == ':') ? static_cast<uint32_t>(std::strtoul(x + 1, nullptr, 16)) : 0u;
        return hi > lo;
    }
    void blackkatDmaAppendLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t lo = 0, hi = 0;
        static const bool on = parseRange(std::getenv("BLACKKAT_DMALOG"), lo, hi);
        static unsigned n = 0;
        const uint64_t a1 = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx->r[5]));
        const uint32_t tgt = static_cast<uint32_t>(a1 >> 32);
        auto u32 = [&](uint32_t a) { uint32_t v = 0; if (const uint8_t *p = getMemPtr(rdram, a)) std::memcpy(&v, p, 4); return v; };
        if (on && tgt >= lo && tgt < hi && n < 20000u)
        {
            ++n;
            const uint32_t b = getRegU32(ctx, 4);
            const uint32_t ent = 0x7F8968u + b * 12u;
            std::fprintf(stderr, "[dmaapp] tgt=%x tag=%08x bucket=%u a2=%x a3=%x list=%x bkt={%x %x %x} ra=%x\n", tgt, static_cast<uint32_t>(a1), b, getRegU32(ctx, 6),
                         getRegU32(ctx, 7), u32(0x723210u), u32(ent), u32(ent + 4), u32(ent + 8), getRegU32(ctx, 31));
        }
        ++g_dmaAppendCalls;
        g_origDmaAppend(rdram, ctx, runtime);
    }
    void blackkatPartDrawLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t lo = 0, hi = 0;
        static const bool on = parseRange(std::getenv("BLACKKAT_DMALOG_PARTS"), lo, hi);
        static unsigned n = 0;
        const uint32_t a0 = getRegU32(ctx, 4);
        if (on && a0 >= lo && a0 < hi && n < 20000u)
        {
            ++n;
            uint64_t fl = 0;
            uint16_t f86 = 0;
            if (const uint8_t *p = getMemPtr(rdram, a0 + 0x78)) std::memcpy(&fl, p, 8);
            if (const uint8_t *p = getMemPtr(rdram, a0 + 0x86)) std::memcpy(&f86, p, 2);
            std::fprintf(stderr, "[partdraw] part=%x idx=%u fl=%016llx f86=%04x ra=%x\n", a0, (a0 - lo) / 0x90u, (unsigned long long)fl, f86, getRegU32(ctx, 31));
            const unsigned d0 = g_dmaAppendCalls, u0 = g_uvAnimCalls;
            g_inKatPart = true;
            g_origPartDraw(rdram, ctx, runtime);
            g_inKatPart = false;
            std::fprintf(stderr, "[partdraw]   -> pc=%x appends=%u uvanim=%u\n", ctx->pc, g_dmaAppendCalls - d0, g_uvAnimCalls - u0);
            return;
        }
        g_origPartDraw(rdram, ctx, runtime);
    }

    constexpr uint32_t kFmvStart = 0x001E1828u;
    PS2Runtime::RecompiledFunction g_origFmvStart = nullptr;

    void blackkatFmvStartLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned logged = 0;
        if (logged < 200)
        {
            ++logged;
            char name[64] = {};
            if (const uint8_t *p = getMemPtr(rdram, getRegU32(ctx, 5)))
                for (size_t i = 0; i + 1 < sizeof(name) && p[i] >= 0x20 && p[i] < 0x7f; ++i)
                    name[i] = static_cast<char>(p[i]);
            std::cerr << "[blackkat] FMV start \"" << name << "\" idx=" << static_cast<int32_t>(getRegU32(ctx, 6)) << " (ra=0x" << std::hex << getRegU32(ctx, 31)
                      << std::dec << ")" << std::endl;
        }
        g_origFmvStart(rdram, ctx, runtime);
    }

    // BLACKKAT_AUTOPRESS="start,x,pause,..." : inject pad 0 presses for unattended
    // tests. One step every BLACKKAT_AUTOPRESS_MS (default 2000), each press
    // held for BLACKKAT_AUTOPRESS_HOLD_MS (default 200), first step after
    // BLACKKAT_AUTOPRESS_DELAY_MS (default 6000); the list repeats.
    // Names: start select up down left right x/cross o/circle sq/square
    // tri/triangle l1 r1 l2 r2, "pause" = no press. Real input still works.
    constexpr uint32_t kScePadRead = 0x002C93F0u;
    struct AutoPress
    {
        std::vector<uint16_t> steps; // bit mask: high byte -> data[2], low -> data[3]
        std::vector<std::string> names;
        uint32_t everyMs = 2000, holdMs = 200, delayMs = 6000;
        bool on = false;
    };

    const AutoPress &autoPress()
    {
        static const AutoPress ap = []()
        {
            AutoPress a;
            const char *e = std::getenv("BLACKKAT_AUTOPRESS");
            if (!e || !*e)
                return a;
            auto num = [](const char *n, uint32_t d) -> uint32_t
            {
                const char *v = std::getenv(n);
                return (v && *v) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : d;
            };
            a.everyMs = num("BLACKKAT_AUTOPRESS_MS", 2000);
            a.holdMs = num("BLACKKAT_AUTOPRESS_HOLD_MS", 200);
            a.delayMs = num("BLACKKAT_AUTOPRESS_DELAY_MS", 6000);
            std::string spec(e), item;
            std::stringstream ss(spec);
            while (std::getline(ss, item, ','))
            {
                uint16_t m = 0;
                if (item == "select") m = 0x0100; else if (item == "l3") m = 0x0200;
                else if (item == "r3") m = 0x0400; else if (item == "start") m = 0x0800;
                else if (item == "up") m = 0x1000; else if (item == "right") m = 0x2000;
                else if (item == "down") m = 0x4000; else if (item == "left") m = 0x8000;
                else if (item == "l2") m = 0x0001; else if (item == "r2") m = 0x0002;
                else if (item == "l1") m = 0x0004; else if (item == "r1") m = 0x0008;
                else if (item == "tri" || item == "triangle") m = 0x0010;
                else if (item == "o" || item == "circle") m = 0x0020;
                else if (item == "x" || item == "cross") m = 0x0040;
                else if (item == "sq" || item == "square") m = 0x0080;
                else if (item != "pause" && !item.empty())
                {
                    std::cerr << "[blackkat] autopress: unknown button '" << item << "' ignored" << std::endl;
                    continue;
                }
                a.steps.push_back(m);
                a.names.push_back(item);
            }
            a.on = !a.steps.empty() && a.everyMs > 0;
            return a;
        }();
        return ap;
    }

    // BLACKKAT_PADREC=<file>: record pad 0 (buttons + analog sticks, bytes 2..7 of the
    // scePadRead buffer) every time it changes, as "readIndex ms mode b2 b3 b4 b5 b6 b7" (hex bytes).
    // BLACKKAT_PADPLAY=<file>: replay such a file on pad 0, replacing the real input while it
    // runs. Steps by pad-read index (frame-locked, default) or by time with
    // BLACKKAT_PADPLAY_MODE=time. Real input comes back after the last line.
    struct PadRecLine { uint64_t idx, ms; uint8_t mode; uint8_t b[6]; };
    struct PadRec
    {
        FILE *rec = nullptr;
        std::vector<PadRecLine> play;
        bool byTime = false;
        bool on() const { return rec || !play.empty(); }
    };
    PadRec &padRec()
    {
        static PadRec pr = []()
        {
            PadRec r;
            if (const char *f = std::getenv("BLACKKAT_PADREC"); f && *f)
            {
                r.rec = std::fopen(f, "w");
                std::cerr << "[padrec] recording pad 0 to " << f << (r.rec ? "" : " FAILED") << std::endl;
                if (r.rec)
                    std::fprintf(r.rec, "# blackkat pad recording: readIndex ms mode b2 b3 lx ly rx ry (b2/b3 active-low)\n");
            }
            if (const char *f = std::getenv("BLACKKAT_PADPLAY"); f && *f)
            {
                if (FILE *in = std::fopen(f, "r"))
                {
                    char line[256];
                    while (std::fgets(line, sizeof(line), in))
                    {
                        PadRecLine l{};
                        unsigned m, b[6];
                        unsigned long long idx, ms;
                        if (line[0] != '#' && std::sscanf(line, "%llu %llu %x %x %x %x %x %x %x", &idx, &ms, &m, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 9)
                        {
                            l.idx = idx; l.ms = ms; l.mode = static_cast<uint8_t>(m);
                            for (int i = 0; i < 6; ++i) l.b[i] = static_cast<uint8_t>(b[i]);
                            r.play.push_back(l);
                        }
                    }
                    std::fclose(in);
                }
                const char *mode = std::getenv("BLACKKAT_PADPLAY_MODE");
                r.byTime = mode && std::string(mode) == "time";
                std::cerr << "[padrec] replaying " << r.play.size() << " pad changes from " << f << " by " << (r.byTime ? "time" : "pad-read index") << std::endl;
            }
            return r;
        }();
        return pr;
    }
    void padRecApply(uint8_t *data)
    {
        PadRec &pr = padRec();
        static uint64_t s_idx = 0;
        static const auto t0 = std::chrono::steady_clock::now();
        const uint64_t idx = s_idx++;
        const uint64_t ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (!pr.play.empty())
        {
            static size_t s_next = 0;
            static bool s_active = false, s_done = false;
            // Load-gap anchors: a recorded change right after a >1.5 s pause in pad reads
            // (a loading screen) marks the first read after that load. When the replay
            // sees its own load pause, re-align the pad-read index to that anchor, so a
            // different number of menu/loading reads does not shift the in-game input.
            static std::vector<uint64_t> s_anchors = [&]()
            {
                std::vector<uint64_t> a;
                for (size_t i = 1; i < pr.play.size(); ++i)
                    if (pr.play[i].ms > pr.play[i - 1].ms + 1500u && pr.play[i].idx <= pr.play[i - 1].idx + 3u)
                        a.push_back(pr.play[i].idx);
                return a;
            }();
            static size_t s_anchorNext = 0;
            static int64_t s_offset = 0;
            static uint64_t s_lastMs = 0;
            if (!pr.byTime && s_anchorNext < s_anchors.size() && idx > 0 && ms > s_lastMs + 1500u)
            {
                const uint64_t mapped = static_cast<uint64_t>(static_cast<int64_t>(idx) + s_offset);
                if (mapped + 300u >= s_anchors[s_anchorNext])
                {
                    s_offset = static_cast<int64_t>(s_anchors[s_anchorNext]) - static_cast<int64_t>(idx);
                    std::cerr << "[padrec] load pause at read " << idx << " -> aligned to recorded read " << s_anchors[s_anchorNext] << std::endl;
                    ++s_anchorNext;
                }
            }
            s_lastMs = ms;
            const uint64_t now = pr.byTime ? ms : static_cast<uint64_t>(static_cast<int64_t>(idx) + s_offset);
            while (s_next < pr.play.size() && (pr.byTime ? pr.play[s_next].ms : pr.play[s_next].idx) <= now)
            {
                ++s_next;
                s_active = true;
            }
            if (s_active && !s_done)
            {
                const PadRecLine &l = pr.play[s_next - 1];
                if (s_next >= pr.play.size() && now > (pr.byTime ? l.ms + 1000 : l.idx + 60))
                {
                    s_done = true;
                    std::cerr << "[padrec] replay finished at read " << idx << ", t=" << ms << "ms" << std::endl;
                }
                else
                {
                    const bool analog = (data[1] >> 4) == 7u;
                    data[2] = l.b[0];
                    data[3] = l.b[1];
                    if (analog)
                        for (int i = 2; i < 6; ++i) data[2 + i] = l.b[i];
                }
            }
        }
        if (pr.rec)
        {
            static uint8_t s_last[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            if (std::memcmp(s_last, data + 1, 7) != 0)
            {
                std::memcpy(s_last, data + 1, 7);
                std::fprintf(pr.rec, "%llu %llu %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long long)idx, (unsigned long long)ms,
                             data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
                std::fflush(pr.rec);
            }
        }
    }

    void blackkatScePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t port = getRegU32(ctx, 4), slot = getRegU32(ctx, 5), dataAddr = getRegU32(ctx, 6);
        ps2_stubs::scePadRead(rdram, ctx, runtime);
        if (padRec().on() && port == 0u && slot == 0u && getRegU32(ctx, 2) != 0u)
            if (uint8_t *pd = getMemPtr(rdram, dataAddr))
                padRecApply(pd);
        const AutoPress &ap = autoPress();
        if (!ap.on || port != 0u || slot != 0u || getRegU32(ctx, 2) == 0u)
            return;
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
            return;
        static const auto t0 = std::chrono::steady_clock::now();
        const uint64_t ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        if (ms < ap.delayMs)
            return;
        const uint64_t rel = ms - ap.delayMs;
        const uint64_t stepNo = rel / ap.everyMs;
        if ((rel % ap.everyMs) >= ap.holdMs)
            return;
        const size_t idx = static_cast<size_t>(stepNo % ap.steps.size());
        const uint16_t m = ap.steps[idx];
        if (m == 0u)
            return;
        data[2] = static_cast<uint8_t>(data[2] & ~(m >> 8));
        data[3] = static_cast<uint8_t>(data[3] & ~(m & 0xFFu));
        static uint64_t lastLogged = ~0ull;
        if (lastLogged != stepNo)
        {
            lastLogged = stepNo;
            std::cerr << "[blackkat] autopress #" << stepNo << " '" << ap.names[idx] << "' at t=" << ms << "ms"
                      << std::endl;
        }
    }

    // EE->IOP sound-transfer queue (EA snd lib, state block at 0x006914C0):
    //   +0x94C s8  pending count      +0x94D s8 index of entry in flight
    //   +0x94E u8  busy (one entry sent to the IOP, waiting for its reply)
    //   +0x950     28 entries x 16 bytes, word 0 = transfer id (0 = free)
    // 0x2D4250 -> 0x2F4A80 queues bank/sample uploads; 0x2F4770 -> 0x2F45D0
    // sends the oldest entry to SNDDRV.IRX with sceSifSendCmd, and the IOP's
    // reply (SIF cmd handler 0x2D1188, type 2) calls 0x2F4BE0(id), which frees
    // the slot, decrements the count and sends the next one. SNDDRV.IRX is only
    // "load-emulated" here, so no reply ever comes: the first bank upload
    // ("BNKl" loader 0x2B9078) polls 0x2D4270 -> 0x2F4B90 forever at
    // 0x2B9398..0x2B93A0 (behind the "Saving PIRATES data" notice).
    // Replacement for 0x2F45D0: complete every queued transfer at once (no SPU2
    // emulation, so the data has nowhere to go anyway).
    // BLACKKAT_SND_QUEUE_REAL=1 keeps the original.
    // Entry layout (from 0x2F45D0): +0 id, +4 EE source, +8 SPU2 RAM byte address,
    // +0xC u16 size, +0xE priority. The original copies EE source -> IOP staging buffer
    // (sceSifSetDma) and sends {id, iopbuf, spuaddr, size} to SNDDRV's cmd handler, which
    // queues sceSdVoiceTrans(core1). Since that SIF cmd path is not wired, the replacement
    // now writes the EE data straight into the emulated SPU2 RAM (ps2x_spu2_upload, used by
    // the SPU2 hardware-voice emulation for SFX) before completing the entry.
    // BLACKKAT_SND_UPLOAD=0 skips the upload (old behaviour).
    constexpr uint32_t kSndSendNext = 0x002F45D0u;
    constexpr uint32_t kSndState = 0x006914C0u;
    uint64_t g_sndCompleted = 0;

    void blackkatSndSendNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        ctx->pc = getRegU32(ctx, 31);
        uint8_t *st = rdram + kSndState;
        unsigned freed = 0;
        for (uint32_t i = 0; i < 28u; ++i)
        {
            uint8_t *slot = st + 0x950u + i * 16u;
            uint32_t id = 0;
            std::memcpy(&id, slot, 4);
            if (id != 0u)
            {
                static const bool s_upload = []() { const char *e = std::getenv("BLACKKAT_SND_UPLOAD"); return !(e && e[0] == '0'); }();
                uint32_t src = 0, spu = 0;
                uint16_t size = 0;
                std::memcpy(&src, slot + 4, 4);
                std::memcpy(&spu, slot + 8, 4);
                std::memcpy(&size, slot + 12, 2);
                const uint32_t srcPhys = src & 0x1FFFFFFFu;
                if (s_upload && size != 0u && srcPhys + size <= 0x02000000u)
                    ps2x_spu2_upload(spu, rdram + srcPhys, size);
                static unsigned upLogged = 0;
                if (upLogged < 12u)
                {
                    ++upLogged;
                    std::cerr << "[blackkat] snd upload id=0x" << std::hex << id << " ee=0x" << src << " spu=0x" << spu
                              << " size=0x" << size << std::dec << (s_upload ? "" : " (skipped)") << std::endl;
                }
                std::memset(slot, 0, 4);
                ++freed;
            }
        }
        st[0x94C] = 0;
        st[0x94E] = 0;
        g_sndCompleted += freed;
        static unsigned logged = 0;
        if (logged < 8 || (g_sndCompleted % 1024u) < freed)
        {
            ++logged;
            std::cerr << "[blackkat] snd queue: completed " << freed << " transfer(s) without IOP (total "
                      << g_sndCompleted << ")" << std::endl;
        }
    }

    // Debug: BLACKKAT_TRACE_FS=1 logs entry/exit of the FILESYS/bigfile path
    // (main -> 0x2B0890 -> ...) to find where the main thread returns to PC 0.
    constexpr uint32_t kTraceAddrs[] = {
        0x002B0890u, 0x002B6C28u, 0x002B6198u, 0x002B6B58u, 0x002B6858u, 0x002B6A10u,
        0x002B6510u, 0x002B6440u, 0x002B6150u, 0x002B7098u, 0x002B74F0u, 0x002B6690u,
        0x002B7638u, 0x002B77A0u, 0x002B78D0u, 0x002B0900u, 0x002B0700u, 0x002B07E0u,
        0x002B0658u, 0x002B0838u, 0x002B6DB8u, 0x002B6780u, 0x002B6950u, 0x002B1368u,
    };
    constexpr size_t kTraceCount = sizeof(kTraceAddrs) / sizeof(kTraceAddrs[0]);
    PS2Runtime::RecompiledFunction g_traceOrig[kTraceCount] = {};
    unsigned g_traceLines = 0;

    template <size_t I>
    void blackkatTraceWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t ra = getRegU32(ctx, 31);
        if (g_traceLines < 4000)
        {
            ++g_traceLines;
            std::cerr << std::hex << "[fstrace] > 0x" << kTraceAddrs[I] << " sp=0x" << sp << " ra=0x" << ra
                      << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5)
                      << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7) << std::dec << std::endl;
        }
        g_traceOrig[I](rdram, ctx, runtime);
        if (g_traceLines < 4000 || ctx->pc == 0u)
        {
            ++g_traceLines;
            std::cerr << std::hex << "[fstrace] < 0x" << kTraceAddrs[I] << " pc=0x" << ctx->pc
                      << " sp=0x" << getRegU32(ctx, 29) << " ra=0x" << getRegU32(ctx, 31)
                      << " v0=0x" << getRegU32(ctx, 2) << (ctx->pc == 0u ? " PC-ZERO" : "")
                      << (ctx->pc != ra && ctx->pc != 0u ? " (not returned)" : "") << std::dec << std::endl;
        }
    }

    template <size_t... Is>
    void bindTraceWrappers(PS2Runtime &runtime, std::index_sequence<Is...>)
    {
        ((g_traceOrig[Is] = runtime.lookupFunction(kTraceAddrs[Is]),
          (g_traceOrig[Is] ? (void)runtime.replaceFunction(kTraceAddrs[Is], &blackkatTraceWrapper<Is>) : (void)0)),
         ...);
    }

    void applyBlackKatNtscU(PS2Runtime &runtime)
    {
        const bool padOk = runtime.replaceFunction(kScePadPortOpen, &blackkatScePadPortOpen);
        std::cerr << "[blackkat] SLUS_203.65 v1.00 override: scePadPortOpen@0x2C91A8 multitap-slot shim "
                  << (padOk ? "bound" : "FAILED") << std::endl;
        // 0x1BCF98 and 0x2B97A0 were missed by Ghidra. config/missed_functions.csv
        // adds them to the Ghidra CSV, so after a regeneration they are real
        // generated functions (FontIt_alloc_0x1bcf98, hook_set_on_0x3A_0x2b97a0)
        // and the hand-written versions below are only bound as a fallback for
        // builds generated before that (empty table slot).
        if (!runtime.hasFunction(kFontItAlloc))
        {
            const bool fontOk = runtime.replaceFunction(kFontItAlloc, &blackkatFontItAlloc);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FontIt alloc@0x1BCF98 (missed by Ghidra) "
                      << (fontOk ? "bound" : "FAILED") << std::endl;
        }
        else
        {
            std::cerr << "[blackkat] FontIt alloc@0x1BCF98 is generated code now, hand override not bound" << std::endl;
        }

        const bool cdCbOk = runtime.replaceFunction(kSceCdCallback, &blackkatSceCdCallback);
        const bool cdReadOk = runtime.replaceFunction(kSceCdRead, &blackkatSceCdRead);
        std::cerr << "[blackkat] SLUS_203.65 v1.00 override: sceCdCallback/sceCdRead completion shim "
                  << ((cdCbOk && cdReadOk) ? "bound" : "FAILED") << std::endl;

        if (!runtime.hasFunction(0x002B97A0u))
        {
            runtime.replaceFunction(0x002B97A0u, &blackkatHook2B97A0);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: leaf 0x2B97A0 (missed by Ghidra) bound" << std::endl;
        }
        else
        {
            std::cerr << "[blackkat] leaf 0x2B97A0 is generated code now, hand override not bound" << std::endl;
        }

        // Kept on by default: with patches/ps2recomp-reentry-sp.patch applied the
        // runtime handles this case for every function, and these wrappers become
        // a no-op (their rewrite is an exact equivalent of the guest's own first
        // instruction), but they still protect the FILESYS path if the submodule
        // is reset without the patch. BLACKKAT_NO_REENTRY_WRAPPERS=1 disables them.
        if (const char *nr = std::getenv("BLACKKAT_NO_REENTRY_WRAPPERS"); !(nr && nr[0] == '1'))
        {
            bindReentryWrappers(runtime, std::make_index_sequence<kReentryCount>{});
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FILESYS re-entrant preemption fix bound ("
                      << kReentryCount << ")" << std::endl;
        }

        if (!blackkatPlayFmv())
        {
            const bool fmvOk = runtime.replaceFunction(kFmvPlayFile, &blackkatFmvSkip) &&
                               runtime.replaceFunction(kFmvDecodeFrame, &blackkatFmvFrameEnd) &&
                               runtime.replaceFunction(kFmvPumpReturn, &blackkatFmvPumpReturn) &&
                               runtime.replaceFunction(kFmvFrameCbReturn, &blackkatFmvFrameCbReturn);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FMV skip (no IPU emulation) "
                      << (fmvOk ? "bound" : "FAILED") << std::endl;
        }

        if (const char *sq = std::getenv("BLACKKAT_SND_QUEUE_REAL"); !(sq && sq[0] == '1'))
        {
            const bool sndOk = runtime.replaceFunction(kSndSendNext, &blackkatSndSendNext);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: snd transfer queue auto-complete "
                      << (sndOk ? "bound" : "FAILED") << std::endl;
        }

        if (autoPress().on || padRec().on())
        {
            const bool padOk = runtime.replaceFunction(kScePadRead, &blackkatScePadRead);
            std::cerr << "[blackkat] autopress: " << autoPress().steps.size() << " step(s) every "
                      << autoPress().everyMs << " ms after " << autoPress().delayMs << " ms, pad read hook "
                      << (padOk ? "bound" : "FAILED") << std::endl;
        }
        g_origFmvStart = runtime.lookupFunction(kFmvStart);
        if (const char *bl = std::getenv("BLACKKAT_BONELOG"); bl && std::atoi(bl) > 0)
        {
            g_origMatMul = runtime.lookupFunction(kMatMul);
            if (g_origMatMul)
                runtime.replaceFunction(kMatMul, &blackkatBoneLog);
        }
        if (const char *pl = std::getenv("BLACKKAT_PARTLOG"); pl && std::atoi(pl) > 0)
        {
            g_origModelSkin = runtime.lookupFunction(kModelSkin);
            if (g_origModelSkin)
                runtime.replaceFunction(kModelSkin, &blackkatPartLog);
        }
        if (std::getenv("BLACKKAT_DMALOG") || std::getenv("BLACKKAT_DMALOG_PARTS"))
        {
            g_origDmaAppend = runtime.lookupFunction(kDmaAppend);
            g_origPartDraw = runtime.lookupFunction(kPartDraw);
            if (g_origDmaAppend)
                runtime.replaceFunction(kDmaAppend, &blackkatDmaAppendLog);
            if (g_origPartDraw)
                runtime.replaceFunction(kPartDraw, &blackkatPartDrawLog);
            g_origUvAnim = runtime.lookupFunction(0x00156760u);
            if (g_origUvAnim)
                runtime.replaceFunction(0x00156760u, &blackkatUvAnimLog);
        }
        if (const char *sl = std::getenv("BLACKKAT_SKINLOG"); sl && std::atoi(sl) > 0)
        {
            g_origSkinPos = runtime.lookupFunction(kSkinPos);
            if (g_origSkinPos)
                runtime.replaceFunction(kSkinPos, &blackkatSkinLog);
        }
        if (fmvIdleCycles() != 0u)
        {
            g_origStreamWaitCheck = runtime.lookupFunction(kStreamWaitCheck);
            g_origTaskPump = runtime.lookupFunction(kTaskPump);
            const bool ok = g_origStreamWaitCheck && g_origTaskPump &&
                            runtime.replaceFunction(kStreamWaitCheck, &blackkatStreamWaitCheck) &&
                            runtime.replaceFunction(kTaskPump, &blackkatTaskPump);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FMV idle skip (" << fmvIdleCycles()
                      << " cycles/idle poll) " << (ok ? "bound" : "FAILED") << std::endl;
        }
        if (blackkatPlayFmv() && fmvPaceOn())
        {
            g_origFmvDecodeFrame = runtime.lookupFunction(kFmvDecodeFrame);
            const bool ok = g_origFmvDecodeFrame && runtime.replaceFunction(kFmvDecodeFrame, &blackkatFmvDecodePaced);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FMV frame pacing (2 vsyncs/picture) "
                      << (ok ? "bound" : "FAILED") << std::endl;
        }
        if (const char *sw = std::getenv("BLACKKAT_FMV_STOP_WAIT"); !(sw && sw[0] == '1'))
        {
            g_origFmvStop = runtime.lookupFunction(kFmvStop);
            if (g_origFmvStop)
                runtime.replaceFunction(kFmvStop, &blackkatFmvStop);
            std::cerr << "[blackkat] FMV stop stream-wait bypass " << (g_origFmvStop ? "bound" : "FAILED") << std::endl;
        }
        if (g_origFmvStart)
            runtime.replaceFunction(kFmvStart, &blackkatFmvStartLog);

        if (const char *tr = std::getenv("BLACKKAT_TRACE_FS"); tr && tr[0] == '1')
        {
            bindTraceWrappers(runtime, std::make_index_sequence<kTraceCount>{});
            std::cerr << "[blackkat] FILESYS trace wrappers bound (" << kTraceCount << ")" << std::endl;
        }

        const char *noSplit = std::getenv("BLACKKAT_NO_HEAP_SPLIT");
        if (!(noSplit && noSplit[0] == '1'))
        {
            g_origCrtInit = runtime.lookupFunction(kCrtInitAfterSetupHeap);
            const bool initOk = g_origCrtInit &&
                                runtime.replaceFunction(kCrtInitAfterSetupHeap, &blackkatCrtInit);
            const bool eohOk = runtime.replaceFunction(kEndOfHeapStub, &blackkatEndOfHeap);
            // EE callback invocation stacks are carved down from 0x02000000,
            // which is also the top of the main thread stack (SetupThread
            // stack 0x01F00000 + 0x100000). Skip the top 256 KB so interrupt
            // and callback frames don't land on main()'s live frames.
            const uint32_t skipped = runtime.reserveAsyncCallbackStack(0x40000u, 16u);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: heap split "
                      << ((initOk && eohOk) ? "bound" : "FAILED")
                      << ", callback stacks below 0x" << std::hex << (skipped ? skipped - 0x40000u + 0x10u : 0u)
                      << std::dec << std::endl;
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "Pirates: The Legend of Black Kat (NTSC-U v1.00)",
    "SLUS_203.65",
    0x002A7B70u,
    0u,
    applyBlackKatNtscU)
