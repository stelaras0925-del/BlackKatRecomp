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
    // BLACKKAT_PLAY_FMV=1 keeps the original.
    constexpr uint32_t kFmvPlayFile = 0x001E1F88u;

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
            if (first || cbNow != lastCb || std::memcmp(f, lastFlags, sizeof f) != 0)
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
            SET_GPR_U32(ctx, 4, 0u);
            ctx->pc = 0x002B31E0u;
            if (!runtime->dispatchGuestBranch(rdram, ctx, 0x002B31E0u, ra - 8u, ra,
                                              PS2Runtime::GuestBranchKind::DirectCall, "FMVPUMP"))
                return; // preempted: resumes at ra
            SET_GPR_U32(ctx, 31, ra);
            ctx->pc = ra;
            const uint32_t cb = pl ? pl[0] : 0u;
            if (cb != 0u && runtime->hasFunction(cb))
            {
                SET_GPR_U32(ctx, 4, player);
                ctx->pc = cb;
                if (!runtime->dispatchGuestBranch(rdram, ctx, cb, ra - 8u, ra,
                                                  PS2Runtime::GuestBranchKind::IndirectCall, "FMVFRAMECB"))
                    return;
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

    // FMV start (0x1E1828(player, name, 0)): log the clip name so front-end
    // state changes are visible in the log. Wraps the generated function.
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

    void blackkatScePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = getRegU32(ctx, 31);
        const uint32_t port = getRegU32(ctx, 4), slot = getRegU32(ctx, 5), dataAddr = getRegU32(ctx, 6);
        ps2_stubs::scePadRead(rdram, ctx, runtime);
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

        if (const char *pf = std::getenv("BLACKKAT_PLAY_FMV"); !(pf && pf[0] == '1'))
        {
            const bool fmvOk = runtime.replaceFunction(kFmvPlayFile, &blackkatFmvSkip) &&
                               runtime.replaceFunction(kFmvDecodeFrame, &blackkatFmvFrameEnd);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: FMV skip (no IPU emulation) "
                      << (fmvOk ? "bound" : "FAILED") << std::endl;
        }

        if (const char *sq = std::getenv("BLACKKAT_SND_QUEUE_REAL"); !(sq && sq[0] == '1'))
        {
            const bool sndOk = runtime.replaceFunction(kSndSendNext, &blackkatSndSendNext);
            std::cerr << "[blackkat] SLUS_203.65 v1.00 override: snd transfer queue auto-complete "
                      << (sndOk ? "bound" : "FAILED") << std::endl;
        }

        if (autoPress().on)
        {
            const bool padOk = runtime.replaceFunction(kScePadRead, &blackkatScePadRead);
            std::cerr << "[blackkat] autopress: " << autoPress().steps.size() << " step(s) every "
                      << autoPress().everyMs << " ms after " << autoPress().delayMs << " ms, pad read hook "
                      << (padOk ? "bound" : "FAILED") << std::endl;
        }
        g_origFmvStart = runtime.lookupFunction(kFmvStart);
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
