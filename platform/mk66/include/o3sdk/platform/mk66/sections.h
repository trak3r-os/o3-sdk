// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Placement attributes: where the linker script (mk66.ld, generated from mem_map.h) puts an object. Placement never
// decides alignment (ADR 0011 lesson 1): an object that needs 4- or 8-byte alignment carries alignas and a static_assert
// of its own, whatever section it is in. A big zero-initialised object for the SDRAM is the kit's O3SDK_BIG
// (kit/core/include/o3sdk/core/placement.h: input section `.bss.sdram_bss`, collected into the `.sdram_bss` output section, cleared by
// the reset sequence), not a macro of this header.

// Hot code that runs from SRAM_L (the code bus): copied there from flash by the reset sequence before main. `long_call`
// because flash and SRAM_L are too far apart for a plain BL; `noinline` so the placement is not lost to inlining.
#define O3SDK_RAMFUNC_L [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call]]

// Not-initialised objects (O3SDK_NOINIT, O3SDK_DMA_RING; O3SDK_BIG of the kit's placement.h does the same). Each names an input
// section `.bss.<name>`, on purpose: GCC makes a section whose name starts with `.bss.` NOBITS and **refuses a non-zero
// initialiser in it at compile time** ("only zero initializers are allowed in section"), so a value given to such an object
// cannot be silently dropped, which is what a NOLOAD output section would otherwise do with it (the linker keeps nothing of a
// NOLOAD section's contents). What still slips through, and is the author's to know: a zero initialiser (`= 0`, `= {}`) is
// accepted and is NOT applied either (the object holds whatever the memory held; an O3SDK_BIG object is cleared at boot, so
// zero is what it holds); a constructor does run at boot (it is code, in .init_array) and is the way to give such an object a
// value. The plain section names `.noinit` and `.sdram_bss` are not accepted by the linker script: an object put there by hand
// is a link error (--orphan-handling=error).

// An object in SRAM_U that survives a reset: not initialised, not cleared. Whoever uses it validates it (a magic and a
// CRC, as the crash record does): after a power-up it holds whatever the RAM powered up with.
#define O3SDK_NOINIT [[gnu::section(".bss.noinit")]]

// An audio DMA ring (OS-005a): in `.dma_rings`, a NOLOAD section of SRAM_U ahead of `.data` and aligned to mem_map.h kDmaRingAlign.
// The object still declares its own alignment (alignas = its size) and static_asserts it: placement must never decide alignment.
// Not cleared at reset: the code that starts the DMA zeroes the ring first. The section name begins `.bss.`, so GCC refuses a
// non-zero initialiser at compile time, as for the noinit objects.
#define O3SDK_DMA_RING [[gnu::section(".bss.dma_rings")]]
