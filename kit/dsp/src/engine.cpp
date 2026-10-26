// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/dsp/engine.h>

#include <o3sdk/core/audio_clock.h>

#include <cmath>

namespace o3sdk::dsp {

namespace {

constexpr float kMaxMixGain = 1.5f;
constexpr float kQuarterPi = 0.785398163f;

float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
float absf(float v) noexcept { return v < 0.0f ? -v : v; }

// An argument of a refused setup call, kept in a byte (a larger one is recorded as 255).
std::uint8_t clip8(unsigned v) noexcept { return static_cast<std::uint8_t>(v < 0xFFu ? v : 0xFFu); }

// Linear under 0.8, then a soft knee that reaches 1.0 asymptotically: x' = 0.8 + 0.2 * s(u), s(u) = u / (1 + u), u = (|x| - 0.8) / 0.2.
float soft_clip(float x) noexcept {
  const float a = absf(x);
  if (a <= 0.8f) {
    return x;
  }
  const float u = (a - 0.8f) * 5.0f;
  const float y = 0.8f + 0.2f * (u / (1.0f + u));
  return x < 0.0f ? -y : y;
}

void clear(float* p, std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    p[i] = 0.0f;
  }
}

// Stable insertion sort by frame: a unit's list is short (kMaxEventsPerUnit) and mostly sorted already.
void sort_by_frame(Event* e, unsigned n) noexcept {
  for (unsigned i = 1; i < n; ++i) {
    const Event x = e[i];
    unsigned j = i;
    while (j > 0 && e[j - 1u].frame > x.frame) {
      e[j] = e[j - 1u];
      --j;
    }
    e[j] = x;
  }
}

float default_mix(MixField f) noexcept {
  switch (f) {
    case MixField::Level:
      return 1.0f;
    case MixField::Pan:
    case MixField::SendA:
    case MixField::SendB:
    case MixField::Mute:
      break;
  }
  return 0.0f;
}

float clamp_mix(MixField f, float v) noexcept {
  if (v != v) {  // NaN
    return default_mix(f);
  }
  switch (f) {
    case MixField::Pan:
      return clampf(v, -1.0f, 1.0f);
    case MixField::Mute:
      return v >= 0.5f ? 1.0f : 0.0f;
    case MixField::Level:
    case MixField::SendA:
    case MixField::SendB:
      break;
  }
  return clampf(v, 0.0f, kMaxMixGain);
}

}  // namespace

bool BlockContext::emit(UnitId unit, const Event& e) noexcept { return engine_ != nullptr && engine_->queue_event(unit, e); }

bool BlockContext::set_mix(unsigned slot, MixField field, float value) noexcept {
  if (engine_ == nullptr || slot >= kMaxSlots || static_cast<unsigned>(field) >= kMixFieldCount) {
    return false;
  }
  engine_->apply_mix(slot, field, clamp_mix(field, value));
  return true;
}

bool BlockContext::send_midi(const core::MidiMessage& m) noexcept { return engine_ != nullptr && engine_->queue_midi(m); }

float BlockContext::mix(unsigned slot, MixField field) const noexcept {
  if (engine_ == nullptr || slot >= kMaxSlots || static_cast<unsigned>(field) >= kMixFieldCount) {
    return 0.0f;
  }
  const Engine::Slot& s = engine_->slots_[slot];
  switch (field) {
    case MixField::Level:
      return s.level;
    case MixField::Pan:
      return s.pan;
    case MixField::SendA:
      return s.send[0];
    case MixField::SendB:
      return s.send[1];
    case MixField::Mute:
      return s.mute ? 1.0f : 0.0f;
  }
  return 0.0f;
}

Engine::Engine() noexcept {
  for (auto& slot : mix_main_) {
    for (unsigned f = 0; f < kMixFieldCount; ++f) {
      slot[f] = default_mix(static_cast<MixField>(f));
    }
  }
  transport_.tempo_mbpm = tempo_main_;
  transport_.inc = ticks_per_sample_q32(tempo_main_);
  own_tempo_ = tempo_main_;
  own_inc_ = transport_.inc;
  inc_min_ = ticks_per_sample_q32(kMinTempoMilliBpm);
  inc_max_ = ticks_per_sample_q32(kMaxTempoMilliBpm);
  ctx_.engine_ = this;
  ctx_.transport_ = &transport_;
}

// ---- Setup ------------------------------------------------------------------------------------------------------------------

const char* Engine::setup_call_name(SetupCall call) noexcept {
  switch (call) {
    case SetupCall::Attach:
      return "attach";
    case SetupCall::SetSource:
      return "set_source";
    case SetupCall::SetInsert:
      return "set_insert";
    case SetupCall::SetSendEffect:
      return "set_send_effect";
    case SetupCall::SetMasterInsert:
      return "set_master_insert";
    case SetupCall::AddHook:
      return "add_hook";
  }
  return "?";
}

const char* Engine::setup_reason_name(SetupReason reason) noexcept {
  switch (reason) {
    case SetupReason::AfterStart:
      return "the engine has started";
    case SetupReason::Full:
      return "no room left";
    case SetupReason::TooManyParams:
      return "more parameters than kMaxParamsPerUnit";
    case SetupReason::OutOfRange:
      return "slot, bus or index out of range";
    case SetupReason::UnknownUnit:
      return "the unit id was never attached";
    case SetupReason::WrongKind:
      return "the unit is the wrong kind (an instrument is a source, an effect an insert or send)";
  }
  return "?";
}

bool Engine::refuse(SetupCall call, SetupReason reason, unsigned slot, unsigned index, unsigned unit) noexcept {
  if (setup_error_count_ < kMaxSetupErrors) {
    setup_error_log_[setup_error_count_] = SetupError{call, reason, clip8(slot), clip8(index), clip8(unit)};
  }
  if (setup_error_count_ != ~0u) {  // a runaway caller saturates the count instead of wrapping it to 0
    ++setup_error_count_;
  }
  return false;
}

UnitId Engine::attach(Unit& unit) noexcept {
  const UnitInfo& info = unit.info();
  if (started_ || unit_count_ >= kMaxUnits || info.param_count > kMaxParamsPerUnit) {
    const SetupReason why = started_ ? SetupReason::AfterStart : (unit_count_ >= kMaxUnits ? SetupReason::Full : SetupReason::TooManyParams);
    (void)refuse(SetupCall::Attach, why, 0, 0, kNoUnit);
    return kNoUnit;
  }
  const auto id = static_cast<UnitId>(unit_count_++);
  units_[id] = &unit;
  for (unsigned p = 0; p < info.param_count; ++p) {
    values_[id][p] = info.params[p].def;
  }
  unit.prepare(core::kRefRateHz);
  return id;
}

bool Engine::set_source(unsigned slot, UnitId instrument) noexcept {
  if (started_) {
    return refuse(SetupCall::SetSource, SetupReason::AfterStart, slot, 0, instrument);
  }
  if (slot >= kMaxSlots) {
    return refuse(SetupCall::SetSource, SetupReason::OutOfRange, slot, 0, instrument);
  }
  if (instrument >= unit_count_) {
    return refuse(SetupCall::SetSource, SetupReason::UnknownUnit, slot, 0, instrument);
  }
  if (units_[instrument]->info().kind != UnitKind::Instrument) {
    return refuse(SetupCall::SetSource, SetupReason::WrongKind, slot, 0, instrument);
  }
  slots_[slot].source = instrument;
  return true;
}

bool Engine::set_insert(unsigned slot, unsigned index, UnitId effect) noexcept {
  if (started_) {
    return refuse(SetupCall::SetInsert, SetupReason::AfterStart, slot, index, effect);
  }
  if (slot >= kMaxSlots || index >= kMaxInserts) {
    return refuse(SetupCall::SetInsert, SetupReason::OutOfRange, slot, index, effect);
  }
  if (effect >= unit_count_) {
    return refuse(SetupCall::SetInsert, SetupReason::UnknownUnit, slot, index, effect);
  }
  if (units_[effect]->info().kind != UnitKind::Effect) {
    return refuse(SetupCall::SetInsert, SetupReason::WrongKind, slot, index, effect);
  }
  slots_[slot].inserts[index] = effect;
  return true;
}

bool Engine::set_send_effect(unsigned bus, UnitId effect) noexcept {
  if (started_) {
    return refuse(SetupCall::SetSendEffect, SetupReason::AfterStart, bus, 0, effect);
  }
  if (bus >= kSendCount) {
    return refuse(SetupCall::SetSendEffect, SetupReason::OutOfRange, bus, 0, effect);
  }
  if (effect >= unit_count_) {
    return refuse(SetupCall::SetSendEffect, SetupReason::UnknownUnit, bus, 0, effect);
  }
  if (units_[effect]->info().kind != UnitKind::Effect) {
    return refuse(SetupCall::SetSendEffect, SetupReason::WrongKind, bus, 0, effect);
  }
  send_fx_[bus] = effect;
  return true;
}

bool Engine::set_master_insert(unsigned index, UnitId effect) noexcept {
  if (started_) {
    return refuse(SetupCall::SetMasterInsert, SetupReason::AfterStart, 0, index, effect);
  }
  if (index >= kMaxInserts) {
    return refuse(SetupCall::SetMasterInsert, SetupReason::OutOfRange, 0, index, effect);
  }
  if (effect >= unit_count_) {
    return refuse(SetupCall::SetMasterInsert, SetupReason::UnknownUnit, 0, index, effect);
  }
  if (units_[effect]->info().kind != UnitKind::Effect) {
    return refuse(SetupCall::SetMasterInsert, SetupReason::WrongKind, 0, index, effect);
  }
  master_inserts_[index] = effect;
  return true;
}

bool Engine::add_hook(Clocked& hook) noexcept {
  if (started_) {
    return refuse(SetupCall::AddHook, SetupReason::AfterStart, 0, 0, kNoUnit);
  }
  if (hook_count_ >= kMaxHooks) {
    return refuse(SetupCall::AddHook, SetupReason::Full, 0, 0, kNoUnit);
  }
  hooks_[hook_count_++] = &hook;
  return true;
}

void Engine::start() noexcept {
  if (started_) {
    return;
  }
  // Every stored value as an event, in one process() of zero frames per unit: the render is not running yet.
  Event initial[kMaxParamsPerUnit];
  float silence[1] = {0.0f};
  for (unsigned u = 0; u < unit_count_; ++u) {
    const UnitInfo& info = units_[u]->info();
    for (unsigned p = 0; p < info.param_count; ++p) {
      initial[p] = param_change(static_cast<std::uint16_t>(p), values_[u][p]);
    }
    ProcessArgs args{initial, info.param_count, silence, silence, silence, silence, 0u, &transport_};
    units_[u]->process(args);
  }
  for (unsigned s = 0; s < kMaxSlots; ++s) {
    Slot& slot = slots_[s];
    slot.level = mix_main_[s][static_cast<unsigned>(MixField::Level)];
    slot.pan = mix_main_[s][static_cast<unsigned>(MixField::Pan)];
    slot.send[0] = mix_main_[s][static_cast<unsigned>(MixField::SendA)];
    slot.send[1] = mix_main_[s][static_cast<unsigned>(MixField::SendB)];
    slot.mute = mix_main_[s][static_cast<unsigned>(MixField::Mute)] >= 0.5f;
  }
  master_level_ = master_level_main_;
  started_ = true;
}

// ---- Running ----------------------------------------------------------------------------------------------------------------

bool Engine::push(const Command& c) noexcept {
  if (!ring_.push(c)) {
    ++dropped_commands_;
    return false;
  }
  return true;
}

bool Engine::send(UnitId unit, const Event& e) noexcept {
  if (unit >= unit_count_) {
    return false;
  }
  Command c{};
  c.op = Op::UnitEvent;
  c.target = unit;
  c.event = e;
  c.event.frame = 0;
  return push(c);
}

const UnitInfo* Engine::unit_info(UnitId unit) const noexcept { return unit < unit_count_ ? &units_[unit]->info() : nullptr; }

const core::ParamDesc* Engine::param_desc(UnitId unit, std::uint16_t id) const noexcept {
  const UnitInfo* info = unit_info(unit);
  return (info != nullptr && id < info->param_count) ? &info->params[id] : nullptr;
}

float Engine::param(UnitId unit, std::uint16_t id) const noexcept {
  return param_desc(unit, id) != nullptr ? values_[unit][id] : 0.0f;
}

bool Engine::set_param(UnitId unit, std::uint16_t id, float value) noexcept {
  const core::ParamDesc* d = param_desc(unit, id);
  if (d == nullptr) {
    return false;
  }
  const float v = core::param_clamp(*d, value);
  values_[unit][id] = v;
  if (!started_) {
    return true;  // start() delivers it
  }
  return send(unit, param_change(id, v));
}

float Engine::step_param(UnitId unit, std::uint16_t id, int detents, bool coarse) noexcept {
  const core::ParamDesc* d = param_desc(unit, id);
  if (d == nullptr) {
    return 0.0f;
  }
  const int n = coarse ? detents * static_cast<int>(d->coarse != 0 ? d->coarse : 1) : detents;
  (void)set_param(unit, id, core::param_step(*d, values_[unit][id], n));
  return values_[unit][id];
}

unsigned Engine::format_param(UnitId unit, std::uint16_t id, char* out, unsigned cap) const noexcept {
  const core::ParamDesc* d = param_desc(unit, id);
  if (d == nullptr) {
    if (cap != 0u) {
      out[0] = '\0';
    }
    return 0;
  }
  return core::param_format(*d, values_[unit][id], out, cap);
}

bool Engine::set_mix(unsigned slot, MixField field, float value) noexcept {
  if (slot >= kMaxSlots || static_cast<unsigned>(field) >= kMixFieldCount) {
    return false;
  }
  const float v = clamp_mix(field, value);
  mix_main_[slot][static_cast<unsigned>(field)] = v;
  if (!started_) {
    return true;
  }
  Command c{};
  c.op = Op::Mix;
  c.target = static_cast<std::uint8_t>(slot);
  c.field = static_cast<std::uint8_t>(field);
  c.value = v;
  return push(c);
}

float Engine::mix(unsigned slot, MixField field) const noexcept {
  return (slot < kMaxSlots && static_cast<unsigned>(field) < kMixFieldCount) ? mix_main_[slot][static_cast<unsigned>(field)] : 0.0f;
}

bool Engine::set_master_level(float level) noexcept {
  master_level_main_ = clamp_mix(MixField::Level, level);
  if (!started_) {
    return true;
  }
  Command c{};
  c.op = Op::MasterLevel;
  c.value = master_level_main_;
  return push(c);
}

bool Engine::play() noexcept {
  if (following()) {  // the master's transport is not ours to start: the device starts the run on its next clock
    request_ = core::TransportRequest::Play;
    return true;
  }
  released_seen_ = released_.load(std::memory_order_relaxed);
  playing_main_ = true;
  Command c{};
  c.op = Op::Play;
  return push(c);
}

bool Engine::stop() noexcept {
  if (following()) {
    request_ = core::TransportRequest::Stop;
    return true;
  }
  released_seen_ = released_.load(std::memory_order_relaxed);
  playing_main_ = false;
  Command c{};
  c.op = Op::Stop;
  return push(c);
}

bool Engine::set_tempo(std::uint32_t tempo_mbpm) noexcept {
  tempo_main_ = tempo_mbpm < kMinTempoMilliBpm ? kMinTempoMilliBpm : (tempo_mbpm > kMaxTempoMilliBpm ? kMaxTempoMilliBpm : tempo_mbpm);
  Command c{};
  c.op = Op::Tempo;
  c.arg = tempo_main_;
  return push(c);
}

bool Engine::playing() const noexcept {
  if (following()) {
    return followed_playing_.load(std::memory_order_relaxed);
  }
  // (a master that was let go stopped the transport: what the main loop asked for before it is void until its next play() or stop())
  return playing_main_ && released_.load(std::memory_order_relaxed) == released_seen_;
}

EngineStats Engine::stats() const noexcept {
  EngineStats out;
  for (;;) {
    const std::uint32_t before = stats_seq_.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;  // a write is in progress (on the unit the render preempts this loop and finishes first)
    }
    out = stats_;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (stats_seq_.load(std::memory_order_relaxed) == before) {
      return out;
    }
  }
}

// ---- Render -----------------------------------------------------------------------------------------------------------------

bool Engine::queue_midi(const core::MidiMessage& m) noexcept {
  if (midi_out_n_ >= kMaxMidiOut) {
    ++dropped_midi_;
    return false;
  }
  midi_out_[midi_out_n_++] = m;
  return true;
}

bool Engine::inject(UnitId unit, const Event& e) noexcept {
  if (unit >= unit_count_) {
    return false;
  }
  if (injected_[unit] >= kMaxInjectPerUnit) {
    ++dropped_events_;
    return false;
  }
  if (!queue_event(unit, e)) {
    return false;
  }
  ++injected_[unit];
  return true;
}

bool Engine::queue_event(UnitId unit, const Event& e) noexcept {
  if (unit >= unit_count_) {
    return false;
  }
  if (event_count_[unit] >= kMaxEventsPerUnit) {
    ++dropped_events_;
    return false;
  }
  events_[unit][event_count_[unit]++] = e;
  return true;
}

// The one place a mixer target is written on the render side: the main loop's commands (drain) and the hooks (BlockContext::set_mix)
// both come here, so a hook's write is ramped by the mixer exactly as a main-loop write is.
void Engine::apply_mix(unsigned slot, MixField field, float value) noexcept {
  Slot& s = slots_[slot];
  switch (field) {
    case MixField::Level:
      s.level = value;
      break;
    case MixField::Pan:
      s.pan = value;
      break;
    case MixField::SendA:
      s.send[0] = value;
      break;
    case MixField::SendB:
      s.send[1] = value;
      break;
    case MixField::Mute:
      s.mute = value >= 0.5f;
      break;
  }
}

void Engine::drain() noexcept {
  Command c;
  for (unsigned n = 0; n < kMaxCommandsPerBlock && ring_.pop(c); ++n) {
    switch (c.op) {
      case Op::UnitEvent:
        (void)queue_event(c.target, c.event);
        break;
      case Op::Mix:
        apply_mix(c.target, static_cast<MixField>(c.field), c.value);
        break;
      case Op::MasterLevel:
        master_level_ = c.value;
        break;
      case Op::Play:
        if (!followed_) {  // (a master's transport is the device's: a command queued before it was followed is void)
          transport_.playing = true;
          transport_.started = true;
          transport_.pos = 0;
          transport_.stopped = false;
        }
        break;
      case Op::Stop:
        if (!followed_) {
          transport_.playing = false;
          transport_.started = false;
          transport_.pos = 0;
          transport_.stopped = true;
        }
        break;
      case Op::Tempo:
        own_tempo_ = c.arg;
        own_inc_ = ticks_per_sample_q32(c.arg);
        if (!followed_) {
          transport_.tempo_mbpm = own_tempo_;
          transport_.inc = own_inc_;
        }
        break;
    }
  }
}

// ---- MIDI: a master's transport ----------------------------------------------------------------------------------------------

void Engine::follow(const core::ClockFrame& f) noexcept {
  follow_in_ = f;
  follow_pending_ = f.follow;
}

void Engine::apply_follow() noexcept {
  const core::ClockFrame& f = follow_in_;
  const bool was_playing = transport_.playing;
  const bool joining = !followed_;
  followed_ = true;
  transport_.following = true;
  if (f.tempo_mbpm != 0u) {
    transport_.tempo_mbpm = f.tempo_mbpm < kMinTempoMilliBpm ? kMinTempoMilliBpm : (f.tempo_mbpm > kMaxTempoMilliBpm ? kMaxTempoMilliBpm : f.tempo_mbpm);
  }
  // The increment is the master's, held between the slowest and the fastest legal tempo (a hook's loop over a block is bounded by it).
  if (f.inc != 0u) {
    transport_.inc = f.inc < inc_min_ ? inc_min_ : (f.inc > inc_max_ ? inc_max_ : f.inc);
  } else {
    transport_.inc = ticks_per_sample_q32(transport_.tempo_mbpm);
  }
  transport_.playing = f.playing;
  transport_.started = f.playing && f.started;
  // A run of the engine's own ends when the master takes over (or stops): the hooks see a stop once, and a master that plays starts them again.
  transport_.stopped = f.stopped || (was_playing && (!f.playing || joining));
  transport_.pos = f.playing ? f.pos : 0u;
  following_.store(true, std::memory_order_relaxed);
  followed_tempo_.store(transport_.tempo_mbpm, std::memory_order_relaxed);
  followed_playing_.store(f.playing, std::memory_order_relaxed);
}

void Engine::release_follow() noexcept {
  followed_ = false;
  transport_.following = false;
  transport_.tempo_mbpm = own_tempo_;
  transport_.inc = own_inc_;
  if (transport_.playing) {  // the master's run ends with the master; the engine's own starts when the main loop asks
    transport_.playing = false;
    transport_.started = false;
    transport_.pos = 0;
    transport_.stopped = true;
  }
  following_.store(false, std::memory_order_relaxed);
  released_.store(released_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

void Engine::run_unit(UnitId unit, const float* in_l, const float* in_r, float* out_l, float* out_r,
                      std::uint32_t frames) noexcept {
  if (unit >= unit_count_) {
    return;
  }
  ProcessArgs args{events_[unit], event_count_[unit], in_l, in_r, out_l, out_r, frames, &transport_};
  units_[unit]->process(args);
  event_count_[unit] = 0;  // consumed
}

void Engine::render(const float* in_l, const float* in_r, float* out_l, float* out_r, std::uint32_t frames) noexcept {
  if (frames > kMaxBlockFrames) {
    frames = kMaxBlockFrames;
  }
  const std::uint32_t t0 = cycles_ != nullptr ? cycles_() : 0u;

  float zero[kMaxBlockFrames];
  clear(zero, frames);
  const float* il = in_l != nullptr ? in_l : zero;
  const float* ir = in_r != nullptr ? in_r : zero;

  drain();
  if (follow_pending_) {
    apply_follow();
  } else if (followed_) {
    release_follow();
  }
  follow_pending_ = false;
  transport_.frames = frames;

  // The clocked hooks place their events; a stop is seen once (Transport::stopped, the block after the command) by every hook.
  midi_out_n_ = 0;
  ctx_.in_l_ = il;
  ctx_.in_r_ = ir;
  for (unsigned h = 0; h < hook_count_; ++h) {
    hooks_[h]->block(ctx_);
  }
  for (unsigned i = 0; i < midi_out_n_; ++i) {
    if (midi_out_[i].frame >= frames) {
      midi_out_[i].frame = static_cast<std::uint16_t>(frames - 1u);
    }
  }
  for (unsigned u = 0; u < unit_count_; ++u) {
    sort_by_frame(events_[u], event_count_[u]);
  }

  float master_l[kMaxBlockFrames];
  float master_r[kMaxBlockFrames];
  float send_l[kSendCount][kMaxBlockFrames];
  float send_r[kSendCount][kMaxBlockFrames];
  float buf_l[kMaxBlockFrames];
  float buf_r[kMaxBlockFrames];
  clear(master_l, frames);
  clear(master_r, frames);
  for (unsigned b = 0; b < kSendCount; ++b) {
    clear(send_l[b], frames);
    clear(send_r[b], frames);
  }

  const float inv_frames = 1.0f / static_cast<float>(frames);
  float slot_peaks[kMaxSlots] = {};
  for (unsigned s = 0; s < kMaxSlots; ++s) {
    Slot& slot = slots_[s];
    if (slot.source == kNoUnit) {
      continue;
    }
    run_unit(slot.source, il, ir, buf_l, buf_r, frames);
    for (unsigned i = 0; i < kMaxInserts; ++i) {
      if (slot.inserts[i] != kNoUnit) {
        run_unit(slot.inserts[i], buf_l, buf_r, buf_l, buf_r, frames);
      }
    }
    // Equal-power pan; the gains ramp linearly across the block from where the last block left them.
    const float level = slot.mute ? 0.0f : slot.level;
    const float angle = (slot.pan + 1.0f) * kQuarterPi;
    const float target_l = level * std::cos(angle) * 1.41421356f;
    const float target_r = level * std::sin(angle) * 1.41421356f;
    const float dl = (target_l - slot.gain_l) * inv_frames;
    const float dr = (target_r - slot.gain_r) * inv_frames;
    float target_send[kSendCount];
    float dsend[kSendCount];
    for (unsigned b = 0; b < kSendCount; ++b) {
      target_send[b] = slot.mute ? 0.0f : slot.send[b];
      dsend[b] = (target_send[b] - slot.send_gain[b]) * inv_frames;
    }
    float gl = slot.gain_l;
    float gr = slot.gain_r;
    float gs[kSendCount] = {slot.send_gain[0], slot.send_gain[1]};
    float peak = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i) {
      gl += dl;
      gr += dr;
      const float l = buf_l[i] * gl;
      const float r = buf_r[i] * gr;
      master_l[i] += l;
      master_r[i] += r;
      const float a = absf(l) > absf(r) ? absf(l) : absf(r);
      peak = a > peak ? a : peak;
      for (unsigned b = 0; b < kSendCount; ++b) {
        gs[b] += dsend[b];
        send_l[b][i] += buf_l[i] * gs[b];
        send_r[b][i] += buf_r[i] * gs[b];
      }
    }
    slot.gain_l = target_l;
    slot.gain_r = target_r;
    for (unsigned b = 0; b < kSendCount; ++b) {
      slot.send_gain[b] = target_send[b];
    }
    slot_peaks[s] = peak;
  }

  for (unsigned b = 0; b < kSendCount; ++b) {
    if (send_fx_[b] == kNoUnit) {
      continue;
    }
    run_unit(send_fx_[b], send_l[b], send_r[b], send_l[b], send_r[b], frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
      master_l[i] += send_l[b][i];
      master_r[i] += send_r[b][i];
    }
  }
  for (unsigned i = 0; i < kMaxInserts; ++i) {
    if (master_inserts_[i] != kNoUnit) {
      run_unit(master_inserts_[i], master_l, master_r, master_l, master_r, frames);
    }
  }

  // Events for a unit that is attached but not routed are dropped at the end of the block.
  for (unsigned u = 0; u < unit_count_; ++u) {
    event_count_[u] = 0;
    injected_[u] = 0;
  }

  const float dm = (master_level_ - master_gain_) * inv_frames;
  float gm = master_gain_;
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  for (std::uint32_t i = 0; i < frames; ++i) {
    gm += dm;
    const float l = master_l[i] * gm;
    const float r = master_r[i] * gm;
    peak_l = absf(l) > peak_l ? absf(l) : peak_l;
    peak_r = absf(r) > peak_r ? absf(r) : peak_r;
    out_l[i] = soft_clip(l);
    out_r[i] = soft_clip(r);
  }
  master_gain_ = master_level_;
  const bool clipped = peak_l > 0.8f || peak_r > 0.8f;

  block_ = core::ClockFrame{followed_, transport_.playing, transport_.started, transport_.stopped, transport_.pos, transport_.inc,
                            transport_.tempo_mbpm};
  transport_.started = false;
  transport_.stopped = false;
  if (transport_.playing) {
    transport_.pos += transport_.inc * frames;
  }
  ++blocks_;
  const std::uint32_t spent = cycles_ != nullptr ? cycles_() - t0 : 0u;
  publish(peak_l, peak_r, slot_peaks, spent, clipped);
}

void Engine::publish(float peak_l, float peak_r, const float* slot_peaks, std::uint32_t cycles, bool clipped) noexcept {
  if (cycles > cycles_max_) {
    cycles_max_ = cycles;
  }
  if (clipped) {
    ++clipped_;
  }
  const std::uint32_t seq = stats_seq_.load(std::memory_order_relaxed);
  stats_seq_.store(seq + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  stats_.blocks = blocks_;
  stats_.peak_l = peak_l;
  stats_.peak_r = peak_r;
  for (unsigned s = 0; s < kMaxSlots; ++s) {
    stats_.slot_peak[s] = slot_peaks[s];
  }
  stats_.cycles = cycles;
  stats_.cycles_max = cycles_max_;
  stats_.dropped_events = dropped_events_;
  stats_.clipped = clipped_;
  stats_.playing = transport_.playing;
  stats_.tempo_mbpm = transport_.tempo_mbpm;
  stats_.pos = transport_.pos;
  stats_.dropped_midi = dropped_midi_;
  stats_seq_.store(seq + 2u, std::memory_order_release);
}

void s16_to_planes(const std::int16_t* lr, float* l, float* r, std::uint32_t frames) noexcept {
  constexpr float k = 1.0f / 32768.0f;
  for (std::uint32_t i = 0; i < frames; ++i) {
    l[i] = static_cast<float>(lr[2u * i]) * k;
    r[i] = static_cast<float>(lr[2u * i + 1u]) * k;
  }
}

void planes_to_s16(const float* l, const float* r, std::int16_t* lr, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    const float a = clampf(l[i], -1.0f, 1.0f) * 32767.0f;
    const float b = clampf(r[i], -1.0f, 1.0f) * 32767.0f;
    lr[2u * i] = static_cast<std::int16_t>(a < 0.0f ? a - 0.5f : a + 0.5f);
    lr[2u * i + 1u] = static_cast<std::int16_t>(b < 0.0f ? b - 0.5f : b + 0.5f);
  }
}

}  // namespace o3sdk::dsp
