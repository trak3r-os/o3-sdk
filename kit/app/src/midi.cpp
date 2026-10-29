// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/midi.h>

#include <cstring>

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

// A parameter's value as the generation core::learn_absolute compares: its bits, so anyone else's change of it (the screen, a load) is noticed.
std::uint32_t generation_of(float value) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

constexpr core::ParamId unit_param(dsp::UnitId unit, std::uint16_t param) noexcept { return core::param_id(core::ParamKind::Unit, unit, param); }

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

// ---- Learn -------------------------------------------------------------------------------------------------------------------------------

bool Midi::learn(dsp::UnitId unit, std::uint16_t param, core::LearnMode mode) noexcept {
  const core::ParamDesc* d = engine_ != nullptr && unit != dsp::kNoUnit && unit < engine_->unit_count() ? engine_->param_desc(unit, param) : nullptr;
  if (d == nullptr || (d->flags & core::kParamApply) != 0u || static_cast<unsigned>(mode) >= core::kLearnModeCount) {
    return false;
  }
  armed_ = true;
  armed_unit_ = unit;
  armed_param_ = param;
  armed_mode_ = mode;
  return true;
}

bool Midi::unlearn(dsp::UnitId unit, std::uint16_t param) noexcept {
  const int at = core::learn_find_param(slot_, kMaxBindings, unit_param(unit, param));
  if (at < 0) {
    return false;
  }
  slot_[at] = core::LearnBinding{};
  state_[at] = core::LearnRuntime{};
  return true;
}

void Midi::unlearn_all() noexcept {
  for (unsigned i = 0; i < kMaxBindings; ++i) {
    slot_[i] = core::LearnBinding{};
    state_[i] = core::LearnRuntime{};
  }
}

unsigned Midi::bindings() const noexcept {
  unsigned n = 0;
  for (const core::LearnBinding& b : slot_) {
    n += core::learn_empty(b) ? 0u : 1u;
  }
  return n;
}

bool Midi::bound(dsp::UnitId unit, std::uint16_t param, std::uint8_t& cc, std::uint8_t& channel) const noexcept {
  const int at = core::learn_find_param(slot_, kMaxBindings, unit_param(unit, param));
  if (at < 0) {
    return false;
  }
  cc = slot_[at].cc;
  channel = slot_[at].channel;
  return true;
}

Midi::Learned Midi::take_cc(const core::MidiMessage& m) noexcept {
  if (!core::midi_is_channel(m.status) || core::midi_kind(m.status) != core::kMidiControl || m.data1 >= core::kCcAllSoundOff || engine_ == nullptr) {
    return Learned::No;
  }
  const unsigned channel = core::midi_channel(m.status);
  if (armed_) {
    armed_ = false;
    core::LearnBinding b;
    b.param = unit_param(armed_unit_, armed_param_);
    b.cc = m.data1;
    b.channel = static_cast<std::uint8_t>(channel);
    b.mode = static_cast<std::uint8_t>(armed_mode_);
    b.flags = 0;
    core::LearnPlan plan;
    if (!core::learn_plan(slot_, kMaxBindings, b, plan)) {
      return Learned::Full;
    }
    for (unsigned i = 0; i < kMaxBindings; ++i) {
      if (((plan.clear >> i) & 1u) != 0u) {
        slot_[i] = core::LearnBinding{};
        state_[i] = core::LearnRuntime{};
      }
    }
    slot_[plan.write] = b;
    state_[plan.write] = core::LearnRuntime{};
    learned_cc_ = m.data1;
    learned_channel_ = b.channel;
    return Learned::Bound;
  }
  bool moved = false;
  for (int at = core::learn_find_cc(slot_, kMaxBindings, channel, m.data1); at >= 0; at = core::learn_find_cc(slot_, kMaxBindings, channel, m.data1, static_cast<unsigned>(at) + 1u)) {
    const core::LearnBinding& b = slot_[at];
    const auto unit = static_cast<dsp::UnitId>(core::param_index(b.param));
    const auto field = static_cast<std::uint16_t>(core::param_field(b.param));
    const core::ParamDesc* d = unit < engine_->unit_count() ? engine_->param_desc(unit, field) : nullptr;
    if (d == nullptr || (d->flags & core::kParamApply) != 0u) {
      continue;  // a binding of a unit or parameter this build does not have, or one that is never learnable: the controller goes to the app
    }
    moved = true;
    const float now = engine_->param(unit, field);
    if (static_cast<core::LearnMode>(b.mode) == core::LearnMode::Relative) {
      const int delta = core::learn_relative_delta(m.data2);
      if (delta != 0) {
        engine_->step_param(unit, field, delta);
      }
      continue;
    }
    float norm = 0.0f;
    if (core::learn_absolute(state_[at], b.param, static_cast<core::LearnMode>(b.mode), m.data2, core::param_norm(*d, now), generation_of(now), norm)) {
      (void)engine_->set_param(unit, field, core::param_denorm(*d, norm));
      core::learn_applied(state_[at], b.param, generation_of(engine_->param(unit, field)));
    }
  }
  return moved ? Learned::Moved : Learned::No;
}

void Midi::fill(KitValues& v) const noexcept {
  for (unsigned i = 0; i < kMaxBindings; ++i) {
    v.learn[i] = slot_[i];
  }
}

// The bindings of the file over the running build's units: a binding whose unit or parameter this build does not have, or that is kParamApply now (the file is older than the unit), is dropped and
// takes no slot; the rest keep their order. (The next save writes what is left.)
void Midi::restore(const KitValues& v) noexcept {
  unsigned n = 0;
  for (unsigned i = 0; i < kMaxBindings; ++i) {
    slot_[i] = core::LearnBinding{};
    state_[i] = core::LearnRuntime{};
  }
  for (unsigned i = 0; i < kMaxBindings; ++i) {
    const core::LearnBinding& b = v.learn[i];
    if (core::learn_empty(b)) {
      continue;
    }
    const auto unit = static_cast<dsp::UnitId>(core::param_index(b.param));
    const core::ParamDesc* d = engine_ != nullptr && unit < engine_->unit_count() ? engine_->param_desc(unit, static_cast<std::uint16_t>(core::param_field(b.param))) : nullptr;
    if (d != nullptr && (d->flags & core::kParamApply) == 0u) {
      slot_[n++] = b;
    }
  }
}

}  // namespace o3sdk::app
