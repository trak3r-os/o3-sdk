// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// o3sdk port: the meters' store is cut (the engine has its own peaks); the sizes below are track3r-os's, without the meters' 2.4 KB.
//
// The taps' bulk memory, in one object (OS-005b review: the stem pool and the scope, spectrum and meter rings leave SRAM_U). Everything the render
// WRITES a block at a time for a reader in another context (the USB interrupt, the main loop's UI and `diag`) and that the render itself does not read
// back sits here: the stem pool and its ring (14.5 KB), the two sample rings (2 KB each) and the meters' published words and history (2.4 KB), about 21 KB. The
// graph's working buffers (the accumulators, the planes, the strips) stay with the Graph, in SRAM_U, where the DMA masters are and the render reads and writes
// them sample by sample.
//
// Placement is the platform's: the MK66 image puts one TapMem in SRAM_L's data area (platform/mk66 sections.h O3SDK_TAP_MEM_L, mem_map.h
// kSramLDataBudget), a test or a simulator makes its own. The Graph takes a reference and resets it; the object has no other owner. A TapMem is plain
// storage with a constructor that zeroes it (the SRAM_L section is not cleared by the reset sequence, the constructor runs before main).

#include <o3sdk/platform/mk66/audio/taps.h>

namespace o3sdk::platform::mk66::audio {

struct TapMem {
  ScopeRing scope;
  SpectrumRing spectrum;
  StemPool stem;

  TapMem() noexcept { reset(); }
  void reset() noexcept {
    scope.reset();
    spectrum.reset();
    stem.reset();
  }
};

}  // namespace o3sdk::platform::mk66::audio
