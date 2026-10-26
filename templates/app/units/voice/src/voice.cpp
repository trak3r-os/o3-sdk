// SPDX-License-Identifier: GPL-3.0-or-later
#include <@APP_ID@/units/voice/voice.h>

#include <cmath>

namespace @APP_ID@ {

namespace {

using o3sdk::core::ParamCurve;
using o3sdk::core::ParamUnit;

constexpr const char* kWaveNames[] = {"SIN", "TRI", "SAW"};

// name, unit text, names, knots, min, max, default, detents, coarse, list count, display, curve, flags, page
constexpr o3sdk::core::ParamDesc kParams[Voice::kParamCount] = {
    {"WAVE", "", kWaveNames, nullptr, 0.0f, 2.0f, 0.0f, 2, 1, 3, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"DECAY", "ms", nullptr, nullptr, 20.0f, 4000.0f, 400.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Log, 0, 0},
    {"LEVEL", "dB", nullptr, nullptr, -48.0f, 6.0f, -12.0f, 108, 12, 0, ParamUnit::Fixed1, ParamCurve::Linear, 0, 0},
};

constexpr o3sdk::dsp::UnitInfo kInfo{"VOICE", o3sdk::dsp::UnitKind::Instrument, kParams, Voice::kParamCount, nullptr, 0};

constexpr float kSilent = 1.0e-4f;        // -80 dB: the note ends below it (and a float never reaches a denormal)
constexpr float kAttackMs = 2.0f;
constexpr float kReleaseMs = 20.0f;
constexpr float kRampMs = 5.0f;

// sin(2 pi t) for t in [0, 1): a parabola with one correction step, within 0.1 % of full scale (as units/synth in o3-sdk does it).
float sine(float t) noexcept {
  const float u = 2.0f * t - 1.0f;
  const float y = 4.0f * u * (1.0f - (u < 0.0f ? -u : u));
  return -(0.225f * (y * (y < 0.0f ? -y : y) - y) + y);
}

}  // namespace

const o3sdk::dsp::UnitInfo& Voice::info() const noexcept { return kInfo; }

float Voice::coeff_ms(float ms) const noexcept { return std::exp(-1000.0f / (ms * fs_)); }

void Voice::prepare(float sample_rate) noexcept {
  fs_ = sample_rate;
  stage_ = Stage::Idle;
  env_ = 0.0f;
  phase_ = 0.0f;
  attack_step_ = 1000.0f / (kAttackMs * fs_);
  release_coef_ = coeff_ms(kReleaseMs * 0.25f);
  gain_.set_length(static_cast<std::uint32_t>(kRampMs * 0.001f * fs_));
  for (unsigned p = 0; p < kParamCount; ++p) {
    set(static_cast<std::uint16_t>(p), kParams[p].def);
  }
  gain_.jump(gain_.target_value());  // the first note starts at the default level, not ramping up to it
}

void Voice::set(std::uint16_t id, float v) noexcept {
  if (id >= kParamCount) {
    return;
  }
  values_[id] = v;
  switch (id) {
    case Decay:
      decay_coef_ = coeff_ms(v * 0.25f);
      break;
    case Level:
      gain_.target(0.5f * std::exp(v * 0.11512925f));  // dB -> linear (ln 10 / 20); 0.5: headroom for the master
      break;
    default:
      break;
  }
}

void Voice::apply(const o3sdk::dsp::Event& e) noexcept {
  using o3sdk::dsp::EventKind;
  switch (e.kind) {
    case EventKind::NoteOn:
      note_ = e.note;
      inc_ = 440.0f * std::exp2((static_cast<float>(e.note) - 69.0f) / 12.0f) / fs_;
      vel_ = static_cast<float>(e.velocity) / 127.0f;
      if (stage_ == Stage::Idle) {
        phase_ = 0.0f;
        env_ = 0.0f;
      }
      stage_ = Stage::Attack;  // from where the envelope stands: a retrigger does not click
      break;
    case EventKind::NoteOff:
      if (stage_ != Stage::Idle && e.note == note_) {
        stage_ = Stage::Release;
      }
      break;
    case EventKind::Param:
      set(e.param, e.value);
      break;
    case EventKind::AllNotesOff:
      if (stage_ != Stage::Idle) {
        stage_ = Stage::Release;
      }
      break;
  }
}

void Voice::render(float* out, std::uint32_t from, std::uint32_t to) noexcept {
  const auto wave = static_cast<int>(values_[Wave] + 0.5f);
  for (std::uint32_t i = from; i < to; ++i) {
    float x = 0.0f;
    if (stage_ != Stage::Idle) {
      phase_ += inc_;
      if (phase_ >= 1.0f) {
        phase_ -= 1.0f;
      }
      switch (wave) {
        case 0:
          x = sine(phase_);
          break;
        case 1:
          x = 1.0f - 4.0f * (phase_ < 0.5f ? 0.5f - phase_ : phase_ - 0.5f);
          break;
        default:
          x = 2.0f * phase_ - 1.0f;
          break;
      }
      switch (stage_) {
        case Stage::Attack:
          env_ += attack_step_;
          if (env_ >= 1.0f) {
            env_ = 1.0f;
            stage_ = Stage::Decay;
          }
          break;
        case Stage::Decay:
          env_ *= decay_coef_;
          break;
        case Stage::Release:
          env_ *= release_coef_;
          break;
        case Stage::Idle:
          break;
      }
      if (env_ < kSilent && stage_ != Stage::Attack) {
        stage_ = Stage::Idle;
        env_ = 0.0f;
      }
    }
    out[i] = x * env_ * vel_ * gain_.next();
  }
}

void Voice::process(const o3sdk::dsp::ProcessArgs& args) noexcept {
  o3sdk::dsp::for_each_run(
      args,
      [&](std::uint32_t from, std::uint32_t to) { render(args.out_l, from, to); },
      [&](const o3sdk::dsp::Event& e) { apply(e); });
  for (std::uint32_t i = 0; i < args.frames; ++i) {
    args.out_r[i] = args.out_l[i];
  }
}

}  // namespace @APP_ID@
