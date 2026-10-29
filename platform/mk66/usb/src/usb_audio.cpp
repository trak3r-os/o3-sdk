// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/mem_map.h>
#include <o3sdk/platform/mk66/sections.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>

namespace o3sdk::platform::mk66::usb_audio {

namespace {

// The stems' pool and ring: SRAM_L's free half on the unit (mem_map.h kSramLDataBudget; the linker script asserts it), .bss on the host. The StemTap's constructor zeroes it before main.
#if defined(O3SDK_AUDIO_RAMFUNC)
O3SDK_TAP_MEM_L
#endif
audio::StemPool g_pool;
static_assert(alignof(audio::StemPool) >= 4u, "the pool is word aligned (the interleave kernel loads words)");
static_assert(sizeof(audio::StemPool) <= mem::kSramLDataBudget, "the stems' pool fits its SRAM_L budget");

audio::StemTap g_tap(g_pool);

}  // namespace

audio::StemTap& tap() noexcept { return g_tap; }

}  // namespace o3sdk::platform::mk66::usb_audio
