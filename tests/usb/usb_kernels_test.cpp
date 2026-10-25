// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: interleave / deinterleave / slot-sum kernels, word-at-a-time against scalar (ADR 0012 S2).
#include <doctest/doctest.h>

#include <o3sdk/usb/kernels.h>
#include <o3sdk/usb/usb_types.h>

#include "usb_test_util.h"

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb;
using usb_test::Rng;

namespace {

constexpr std::uint32_t kMaxFrames = 140;
constexpr std::uint32_t kGuard = 8;  // canary samples around every output
constexpr std::int16_t kCanary = 0x5A5A;

std::int16_t sat(std::int32_t v) { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

std::int16_t random_sample(Rng& rng, bool extreme) {
  if (extreme && rng.below(3) == 0) {
    static const std::int16_t special[] = {32767, -32768, 32767, -32768, 16384, -16384, 0, 1, -1};
    return special[rng.below(9)];
  }
  return static_cast<std::int16_t>(rng.next() & 0xFFFFu);
}

}  // namespace

TEST_CASE("usb kernels: interleave matches the definition and the scalar reference at random shapes") {
  Rng rng(1);
  for (int iter = 0; iter < 6000; ++iter) {
    const std::uint32_t channels = 1 + rng.below(kMaxChannels);
    const std::uint32_t offset = rng.below(5);
    const std::uint32_t frames = rng.below(kMaxFrames);
    // Planes live in one big aligned buffer; each starts at a random sample offset, so some planes are 4-byte
    // aligned and some are not (the kernel must fall back, never fault, never read out of bounds).
    alignas(4) static std::int16_t store[kMaxChannels][kMaxFrames + 16];
    const std::int16_t* planes[kMaxChannels];
    const bool misalign = rng.below(4) == 0;
    for (std::uint32_t c = 0; c < channels; ++c) {
      const std::uint32_t base = (misalign && rng.below(2) == 0) ? 1u : 0u;
      for (std::uint32_t i = 0; i < kMaxFrames + 16; ++i) {
        store[c][i] = random_sample(rng, false);
      }
      planes[c] = store[c] + base;
    }
    const std::uint32_t out_base = rng.below(3);  // destination alignment 0..2 samples
    alignas(4) static std::int16_t got[kGuard + 3 + kMaxChannels * kMaxFrames + kGuard];
    alignas(4) static std::int16_t want[kGuard + 3 + kMaxChannels * kMaxFrames + kGuard];
    const std::uint32_t total = kGuard + 3 + channels * frames + kGuard;
    for (std::uint32_t i = 0; i < total; ++i) {
      got[i] = want[i] = kCanary;
    }
    interleave(planes, channels, offset, frames, got + kGuard + out_base);
    interleave_scalar(planes, channels, offset, frames, want + kGuard + out_base);
    bool same = std::memcmp(got, want, total * sizeof(std::int16_t)) == 0;
    // And against the definition itself.
    bool def = true;
    for (std::uint32_t f = 0; f < frames && def; ++f) {
      for (std::uint32_t c = 0; c < channels; ++c) {
        def = def && got[kGuard + out_base + f * channels + c] == planes[c][offset + f];
      }
    }
    if (!same || !def) {
      INFO("iter " << iter << " channels " << channels << " offset " << offset << " frames " << frames << " misalign "
                   << misalign << " out_base " << out_base);
      CHECK(same);
      CHECK(def);
      return;
    }
  }
}

TEST_CASE("usb kernels: interleave of the profile shapes with aligned planes") {
  // The real case: block planes alignas(4), offsets that advance by 44 or 45 each packet.
  alignas(4) static std::int16_t plane[kMaxChannels][kStemBlockFrames];
  const std::int16_t* planes[kMaxChannels];
  for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
    planes[c] = plane[c];
    for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
      plane[c][i] = static_cast<std::int16_t>(c * 1000 + i);
    }
  }
  const std::uint32_t channel_counts[] = {2, 8, 10, 11};
  for (std::uint32_t channels : channel_counts) {
    for (std::uint32_t offset = 0; offset + 45 <= kStemBlockFrames; ++offset) {
      for (std::uint32_t frames = 44; frames <= 45; ++frames) {
        alignas(4) std::int16_t out[kMaxChannels * 45];
        interleave(planes, channels, offset, frames, out);
        for (std::uint32_t f = 0; f < frames; ++f) {
          for (std::uint32_t c = 0; c < channels; ++c) {
            if (out[f * channels + c] != static_cast<std::int16_t>(c * 1000 + offset + f)) {
              CHECK(false);
              return;
            }
          }
        }
      }
    }
  }
}

TEST_CASE("usb kernels: a packet built in two chunks (block boundary), odd channel counts, odd frame counts") {
  // What StemReader does: the first chunk comes from the tail of one block, the second from the head of the next,
  // written at out + frames_done * channels. With 11 channels (profile P11) and an odd first chunk that second
  // chunk starts at a 2-byte offset: the pair stores must not become unaligned word stores.
  alignas(4) static std::int16_t block[2][kMaxChannels][kStemBlockFrames];
  const std::int16_t* planes_a[kMaxChannels];
  const std::int16_t* planes_b[kMaxChannels];
  for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
    planes_a[c] = block[0][c];
    planes_b[c] = block[1][c];
    for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
      block[0][c][i] = static_cast<std::int16_t>(c * 1000 + i);
      block[1][c][i] = static_cast<std::int16_t>(c * 1000 + kStemBlockFrames + i);
    }
  }
  for (std::uint32_t channels = 1; channels <= kMaxChannels; ++channels) {
    for (std::uint32_t offset = 0; offset < kStemBlockFrames; ++offset) {
      for (std::uint32_t frames = 44; frames <= 45; ++frames) {
        alignas(4) std::int16_t out[kMaxChannels * 45 + 4];
        for (auto& v : out) {
          v = kCanary;
        }
        const std::uint32_t room = kStemBlockFrames - offset;
        const std::uint32_t first = frames < room ? frames : room;
        interleave(planes_a, channels, offset, first, out);
        if (first < frames) {
          interleave(planes_b, channels, 0, frames - first, out + first * channels);
        }
        bool ok = out[frames * channels] == kCanary;  // nothing written past the packet
        for (std::uint32_t f = 0; f < frames && ok; ++f) {
          for (std::uint32_t c = 0; c < channels; ++c) {
            ok = ok && out[f * channels + c] == static_cast<std::int16_t>(c * 1000 + offset + f);
          }
        }
        if (!ok) {
          INFO("channels " << channels << " offset " << offset << " frames " << frames);
          CHECK(ok);
          return;
        }
      }
    }
  }
}

#if defined(__ARM_ARCH_7EM__)
// On the Cortex-M4 target: with CCR.UNALIGN_TRP set an unaligned LDR/STR raises a UsageFault (the platform's fault
// handler then fails the whole suite), so this case proves the kernels never rely on unaligned access (ADR 0011
// lesson 1; the MK66 default is to allow it, but a future configuration may not). Every shape that can put a word
// access at a 2-byte offset is here: odd channel counts, odd frame offsets, misaligned planes and destinations.
TEST_CASE("usb kernels: no unaligned word access on the target (CCR.UNALIGN_TRP set)") {
  volatile std::uint32_t* const ccr = reinterpret_cast<volatile std::uint32_t*>(0xE000ED14);
  const std::uint32_t saved = *ccr;
  *ccr = saved | (1u << 3);
  Rng rng(6);
  alignas(4) static std::int16_t store[kMaxChannels][kMaxFrames + 16];
  alignas(4) static std::int16_t dst[kMaxChannels * kMaxFrames + 16];
  alignas(4) static std::uint32_t words[kMaxFrames];
  alignas(4) static std::int16_t lp[kMaxFrames + 4], rp[kMaxFrames + 4];
  bool ran = false;
  for (int iter = 0; iter < 600; ++iter) {
    const std::uint32_t channels = 1 + rng.below(kMaxChannels);
    const std::uint32_t frames = rng.below(kMaxFrames);
    const std::int16_t* planes[kMaxChannels];
    for (std::uint32_t c = 0; c < channels; ++c) {
      for (std::uint32_t i = 0; i < kMaxFrames + 16; ++i) {
        store[c][i] = random_sample(rng, false);
      }
      planes[c] = store[c] + (rng.below(4) == 0 ? 1u : 0u);  // sometimes a 2-byte-offset plane
    }
    interleave(planes, channels, rng.below(5), frames, dst + rng.below(3));
    for (std::uint32_t i = 0; i < kMaxFrames; ++i) {
      words[i] = rng.next();
    }
    deinterleave_stereo(words, frames, lp + rng.below(3), rp + rng.below(3));
    const std::int16_t* src[kSlotSources];
    for (std::uint32_t k = 0; k < kSlotSources; ++k) {
      src[k] = store[k % kMaxChannels] + (rng.below(5) == 0 ? 1u : 0u);
    }
    sum_slots(src, rng.next() & 0xFFFFu, frames, dst + rng.below(2));
    ran = true;
  }
  *ccr = saved;
  CHECK(ran);  // reaching this line is the test: no UsageFault
}
#endif

TEST_CASE("usb kernels: deinterleave_stereo matches the definition and the scalar reference") {
  Rng rng(2);
  for (int iter = 0; iter < 6000; ++iter) {
    const std::uint32_t frames = rng.below(kMaxFrames);
    alignas(4) static std::uint32_t in[kMaxFrames];
    for (std::uint32_t i = 0; i < kMaxFrames; ++i) {
      in[i] = rng.next();
    }
    alignas(4) static std::int16_t lg[kGuard + 2 + kMaxFrames + kGuard], rg[kGuard + 2 + kMaxFrames + kGuard];
    alignas(4) static std::int16_t lw[kGuard + 2 + kMaxFrames + kGuard], rw[kGuard + 2 + kMaxFrames + kGuard];
    const std::uint32_t total = kGuard + 2 + frames + kGuard;
    for (std::uint32_t i = 0; i < total; ++i) {
      lg[i] = rg[i] = lw[i] = rw[i] = kCanary;
    }
    const std::uint32_t lb = kGuard + rng.below(3);  // misaligned destinations sometimes
    const std::uint32_t rb = kGuard + rng.below(3);
    deinterleave_stereo(in, frames, lg + lb, rg + rb);
    deinterleave_stereo_scalar(in, frames, lw + lb, rw + rb);
    bool ok = std::memcmp(lg, lw, total * sizeof(std::int16_t)) == 0 && std::memcmp(rg, rw, total * sizeof(std::int16_t)) == 0;
    for (std::uint32_t i = 0; i < frames && ok; ++i) {
      std::int16_t l, r;
      const std::uint16_t lu = static_cast<std::uint16_t>(in[i] & 0xFFFFu), ru = static_cast<std::uint16_t>(in[i] >> 16);
      std::memcpy(&l, &lu, 2);
      std::memcpy(&r, &ru, 2);
      ok = lg[lb + i] == l && rg[rb + i] == r;
    }
    if (!ok) {
      INFO("iter " << iter << " frames " << frames);
      CHECK(ok);
      return;
    }
  }
}

TEST_CASE("usb kernels: interleave then deinterleave round-trips a stereo block") {
  Rng rng(3);
  alignas(4) std::int16_t l[kStemBlockFrames], r[kStemBlockFrames], l2[kStemBlockFrames], r2[kStemBlockFrames];
  alignas(4) std::uint32_t frames_words[kStemBlockFrames];
  alignas(4) std::int16_t inter[2 * kStemBlockFrames];
  for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
    l[i] = random_sample(rng, true);
    r[i] = random_sample(rng, true);
  }
  const std::int16_t* planes[2] = {l, r};
  interleave(planes, 2, 0, kStemBlockFrames, inter);
  std::memcpy(frames_words, inter, sizeof inter);  // the memory layout of an OUT packet: L low half, R high half
  deinterleave_stereo(frames_words, kStemBlockFrames, l2, r2);
  CHECK(std::memcmp(l, l2, sizeof l) == 0);
  CHECK(std::memcmp(r, r2, sizeof r) == 0);
}

TEST_CASE("usb kernels: slot-mask sum saturates sequentially and matches the scalar reference") {
  Rng rng(4);
  for (int iter = 0; iter < 6000; ++iter) {
    const std::uint32_t frames = rng.below(kMaxFrames);
    alignas(4) static std::int16_t store[kSlotSources][kMaxFrames + 4];
    const std::int16_t* sources[kSlotSources];
    const bool extreme = rng.below(2) == 0;
    for (std::uint32_t s = 0; s < kSlotSources; ++s) {
      for (std::uint32_t i = 0; i < kMaxFrames + 4; ++i) {
        store[s][i] = random_sample(rng, extreme);
      }
      sources[s] = (rng.below(10) == 0) ? nullptr : store[s] + (rng.below(8) == 0 ? 1u : 0u);  // sometimes null / misaligned
    }
    const std::uint32_t mask = rng.next() & ((rng.below(3) == 0) ? 0xFFFFu : (1u << rng.below(16)) | (rng.next() & rng.next() & 0xFFFFu));
    alignas(4) static std::int16_t got[kGuard + 1 + kMaxFrames + kGuard], want[kGuard + 1 + kMaxFrames + kGuard];
    const std::uint32_t total = kGuard + 1 + frames + kGuard;
    for (std::uint32_t i = 0; i < total; ++i) {
      got[i] = want[i] = kCanary;
    }
    const std::uint32_t base = kGuard + rng.below(2);
    sum_slots(sources, mask, frames, got + base);
    sum_slots_scalar(sources, mask, frames, want + base);
    bool ok = std::memcmp(got, want, total * sizeof(std::int16_t)) == 0;
    // The definition, written independently: ascending bit order, saturate after each add.
    for (std::uint32_t f = 0; f < frames && ok; ++f) {
      std::int32_t acc = 0;
      for (std::uint32_t s = 0; s < kSlotSources; ++s) {
        if (((mask >> s) & 1u) != 0 && sources[s] != nullptr) {
          acc = sat(acc + sources[s][f]);
        }
      }
      ok = got[base + f] == acc;
    }
    if (!ok) {
      INFO("iter " << iter << " frames " << frames << " mask " << mask);
      CHECK(ok);
      return;
    }
  }
}

TEST_CASE("usb kernels: saturation is per addition, in ascending bit order") {
  // 30000 + 30000 saturates to 32767, then -30000 gives 2767; the other order would give 30000 - 30000 + 30000.
  alignas(4) std::int16_t a[4] = {30000, 30000, -32768, 100};
  alignas(4) std::int16_t b[4] = {30000, -30000, -32768, 200};
  alignas(4) std::int16_t c[4] = {-30000, 30000, 5, 300};
  const std::int16_t* sources[kSlotSources] = {a, b, c};
  alignas(4) std::int16_t out[4];
  sum_slots(sources, 0x7, 4, out);
  CHECK(out[0] == 2767);    // sat(sat(30000+30000) - 30000) = 32767 - 30000
  CHECK(out[1] == 30000);   // (30000 - 30000) + 30000
  CHECK(out[2] == -32763);  // sat(sat(-32768-32768) + 5) = -32768 + 5
  CHECK(out[3] == 600);
  // Order is by bit, not by position in the array.
  const std::int16_t* shuffled[kSlotSources] = {};
  shuffled[3] = c;
  shuffled[5] = a;
  shuffled[9] = b;
  sum_slots(shuffled, (1u << 3) | (1u << 5) | (1u << 9), 4, out);  // c, then a, then b
  CHECK(out[0] == 30000);   // c, a, b: -30000 + 30000 = 0, then + 30000: a different result from a, b, c above
}

TEST_CASE("usb kernels: mix_slot returns a source pointer for one source, a sum for several, zeros for none") {
  alignas(4) std::int16_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  alignas(4) std::int16_t b[8] = {10, 20, 30, 40, 50, 60, 70, 80};
  const std::int16_t* sources[kSlotSources] = {};
  sources[2] = a;
  sources[11] = b;
  alignas(4) std::int16_t scratch[8];
  std::memset(scratch, 0x55, sizeof scratch);
  CHECK(mix_slot(sources, 1u << 2, 8, scratch) == a);  // one source: its own pointer, nothing written
  CHECK(scratch[0] == 0x5555);
  CHECK(mix_slot(sources, 1u << 11, 8, scratch) == b);
  const std::int16_t* sum = mix_slot(sources, (1u << 2) | (1u << 11), 8, scratch);
  CHECK(sum == scratch);
  CHECK((sum[0] == 11 && sum[7] == 88));
  const std::int16_t* none = mix_slot(sources, 0, 8, scratch);
  CHECK(none == scratch);
  for (int i = 0; i < 8; ++i) {
    CHECK(scratch[i] == 0);
  }
  // Selected null sources count as silence; bits above 15 are ignored.
  CHECK(mix_slot(sources, (1u << 2) | (1u << 5), 8, scratch) == a);
  const std::int16_t* ignored = mix_slot(sources, (1u << 17), 8, scratch);
  CHECK(ignored == scratch);
  CHECK(scratch[3] == 0);
}
