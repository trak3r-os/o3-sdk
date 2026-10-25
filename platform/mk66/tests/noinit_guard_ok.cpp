// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture of ctest `mk66_noinit_guard`: objects the noinit macros accept. Must compile.
#include <o3sdk/core/placement.h>
#include <o3sdk/platform/mk66/sections.h>

namespace {
struct Block {
  int a;
  int b[3];
};
struct alignas(1024) Ring {
  short samples[512];
};
}  // namespace

O3SDK_NOINIT Block g_in_sram;
O3SDK_NOINIT int g_word_in_sram;
O3SDK_DMA_RING Ring g_ring;

// O3SDK_BIG: zero-initialised objects for the SDRAM (kit/core placement.h); an explicit zero initialiser is accepted too.
O3SDK_BIG float g_delay[1024];
O3SDK_BIG float g_zeroed[8] = {};
