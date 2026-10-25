// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where a large object lives. The MK66 has 256 KiB of SRAM and 16 MiB of SDRAM: a buffer of tens or hundreds of kilobytes (a delay
// line, a sample) does not fit the SRAM, and the render reaches the SDRAM at a cost (track3r-os docs/architecture/cpu-budget.md).
//
//   O3SDK_BIG    a zero-initialised object for the SDRAM: input section `.bss.sdram_bss`, which the MK66 linker script collects into its
//                `.sdram_bss` output section (the SDRAM, NOLOAD) and the startup clears before the constructors run. The name starts
//                `.bss.` on purpose: GCC makes such a section NOBITS and refuses a non-zero initialiser in it ("only zero
//                initializers are allowed in section"), so `O3SDK_BIG float k[8] = {1.f};` is a compile error on the unit's
//                compiler instead of an object that is silently all zeros (a NOLOAD section keeps no contents). A constructor still
//                runs at boot and is the way to give an object a value. On the host it is an ordinary object (no error there: build
//                the image to see it).
//
// Use it on a namespace-scope or static object only, never on a local.

#if defined(__arm__) && !defined(O3SDK_HOST_LAYOUT)
#define O3SDK_BIG __attribute__((section(".bss.sdram_bss"), aligned(8)))
#else
#define O3SDK_BIG
#endif
