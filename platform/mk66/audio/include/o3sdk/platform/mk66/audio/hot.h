// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Placement of the real-time path (ADR 0013 D7: the audio HAL/ISR path runs from SRAM_L, zero wait states on the code bus).
// platform/mk66/audio/CMakeLists.txt defines O3SDK_AUDIO_RAMFUNC for the cross build, and a function marked O3SDK_AUDIO_HOT then
// lands in `.ramfunc_l`, which the reset sequence copies from flash before any interrupt is enabled (the same placement as
// O3SDK_RAMFUNC_L in sections.h, spelled here so that the pure library, which the host tests build, needs no platform header).
// `long_call` because flash and SRAM_L are too far apart for a plain BL; `noinline` so the placement survives optimisation.
// On the host the macro is empty.
//
// THE RULE: code in `.ramfunc_l` calls nothing in flash directly (tools/check_ramfunc.py refuses the image otherwise). Indirect
// calls are allowed, because they are visible in the source: the cycle counter and the block callback are the two the pipeline
// makes. Helpers the hot functions use are O3SDK_AUDIO_INLINE, so they are folded in instead of being outlined into flash.
#if defined(O3SDK_AUDIO_RAMFUNC) && O3SDK_AUDIO_RAMFUNC
#define O3SDK_AUDIO_HOT [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call]]
#else
#define O3SDK_AUDIO_HOT
#endif

// A hot function that also inlines every call it makes (GCC's `flatten`, except the callees that are themselves noinline): for ISR-path code that calls small helpers of
// a lower layer (core::SpscRing::pop, size) which -Os would otherwise leave out of line, in flash, where the check of tools/check_ramfunc.py rightly refuses a call
// from SRAM_L. Where this is used the helpers it flattens are copied into the function (a few instructions each). (Used by platform/mk66/midi, the DIN emitter.)
#if defined(O3SDK_AUDIO_RAMFUNC) && O3SDK_AUDIO_RAMFUNC
#define O3SDK_AUDIO_HOT_FLAT [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call, gnu::flatten]]
#else
#define O3SDK_AUDIO_HOT_FLAT
#endif

// For the tiny ISR-path helpers that must be folded into their caller (which is itself in SRAM_L). The GNU spelling, not [[gnu::]],
// so that it may stand anywhere in the specifiers (`static O3SDK_AUDIO_INLINE ...`).
#define O3SDK_AUDIO_INLINE __attribute__((always_inline)) inline
