// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/midi.h>

namespace o3sdk::app {

namespace {

constexpr std::uint16_t pack(dsp::UnitId unit, std::uint8_t channel) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(unit) << 8) | channel);
}

// A message a route turns into a note: a note on or off, or the controllers that end every note of the channel.
bool routable(const core::MidiMessage& m) noexcept {
  if (!core::midi_is_channel(m.status)) {
    return false;
  }
  const std::uint8_t kind = core::midi_kind(m.status);
  return kind == core::kMidiNoteOn || kind == core::kMidiNoteOff ||
         (kind == core::kMidiControl && (m.data1 == core::kCcAllSoundOff || m.data1 == core::kCcAllNotesOff));
}

}  // namespace

Midi::Midi() noexcept {
  for (auto& r : route_) {
    r.store(kFree, std::memory_order_relaxed);
  }
}

bool Midi::route(dsp::UnitId unit, std::uint8_t channel) noexcept {
  if (unit == dsp::kNoUnit || channel > 16u || (engine_ != nullptr && unit >= engine_->unit_count())) {
    return false;
  }
  unsigned at = kMaxRoutes;
  for (unsigned i = 0; i < kMaxRoutes; ++i) {
    const std::uint16_t v = route_[i].load(std::memory_order_relaxed);
    if (v != kFree && (v >> 8) == unit) {
      at = i;  // (a unit has one route)
      break;
    }
    if (v == kFree && at == kMaxRoutes) {
      at = i;
    }
  }
  if (at == kMaxRoutes) {
    return false;
  }
  route_[at].store(pack(unit, channel), std::memory_order_relaxed);
  return true;
}

bool Midi::unroute(dsp::UnitId unit) noexcept {
  for (auto& r : route_) {
    const std::uint16_t v = r.load(std::memory_order_relaxed);
    if (v != kFree && (v >> 8) == unit) {
      r.store(kFree, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

unsigned Midi::routes() const noexcept {
  unsigned n = 0;
  for (const auto& r : route_) {
    n += r.load(std::memory_order_relaxed) != kFree ? 1u : 0u;
  }
  return n;
}

void Midi::set_config(const hal::midi::Config& c) noexcept {
  cfg_ = hal::midi::sanitized(c);
  user_channel_.store(cfg_.channel, std::memory_order_relaxed);
  push_config();
}

unsigned Midi::match(const core::MidiMessage& m, dsp::UnitId (&units)[kMaxRoutes]) const noexcept {
  if (!routable(m)) {
    return 0;
  }
  const std::uint8_t channel = core::midi_channel(m.status);
  const std::uint8_t user = user_channel_.load(std::memory_order_relaxed);
  unsigned n = 0;
  for (const auto& r : route_) {
    const std::uint16_t v = r.load(std::memory_order_relaxed);
    if (v == kFree) {
      continue;
    }
    const auto own = static_cast<std::uint8_t>(v & 0xFFu);
    const std::uint8_t listens = own != 0u ? own : user;  // 0: omni
    if (listens == 0u || listens == channel) {
      units[n++] = static_cast<dsp::UnitId>(v >> 8);
    }
  }
  return n;
}

}  // namespace o3sdk::app
