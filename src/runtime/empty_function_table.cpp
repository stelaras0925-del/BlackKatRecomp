// SPDX-License-Identifier: GPL-3.0-only
//
// Stand-in for the register_functions.cpp that ps2_recomp writes into
// build/generated. ps2xRuntime looks these symbols up for every guest call.
// The range matches the placeholder shipped in PS2Recomp's own runner so an
// unrecompiled boot can still resolve addresses inside the default window.
// It is not linked once build/generated/register_functions.cpp exists.

#include "ps2_runtime.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount =
    (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};
