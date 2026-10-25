// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/units/synth/synth.h>

#include <cmath>

namespace o3sdk::units {

namespace {

using core::ParamCurve;
using core::ParamUnit;

constexpr const char* kWaveNames[] = {"SIN", "TRI", "SAW", "SQR"};

// name, unit text, names, knots, min, max, default, detents, coarse, list count, display, curve, flags, page
constexpr core::ParamDesc kParams[Synth::kParamCount] = {
    {"WAVE", "", kWaveNames, nullptr, 0.0f, 3.0f, 2.0f, 3, 1, 4, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"CUTOFF", "Hz", nullptr, nullptr, 40.0f, 16000.0f, 2400.0f, 120, 10, 0, ParamUnit::Hz, ParamCurve::Log, core::kParamSmooth, 0},
    {"RESO", "%", nullptr, nullptr, 0.0f, 1.0f, 0.2f, 100, 10, 0, ParamUnit::NormPct, ParamCurve::Linear, 0, 0},
    {"ATTACK", "ms", nullptr, nullptr, 1.0f, 2000.0f, 5.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Log, 0, 0},
    {"DECAY", "ms", nullptr, nullptr, 10.0f, 4000.0f, 300.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Log, 0, 0},
    {"SUSTAIN", "%", nullptr, nullptr, 0.0f, 1.0f, 0.6f, 100, 10, 0, ParamUnit::NormPct, ParamCurve::Linear, 0, 0},
    {"RELEASE", "ms", nullptr, nullptr, 5.0f, 4000.0f, 250.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Log, 0, 0},
    {"VOLUME", "dB", nullptr, nullptr, -48.0f, 6.0f, -9.0f, 108, 12, 0, ParamUnit::Fixed1, ParamCurve::Linear, core::kParamSmooth, 0},
};

constexpr dsp::UnitInfo kInfo{"SYNTH", dsp::UnitKind::Instrument, kParams, Synth::kParamCount, nullptr, 0};

constexpr float kPi = 3.14159265f;
constexpr float kQuiet = 1.0e-4f;

// The PolyBLEP residual at phase t (0..1) for a step at t = 0, with phase increment dt.
float blep(float t, float dt) noexcept {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

// sin(2 pi t) for t in [0, 1): sin(2 pi t) = -sin(pi u) with u = 2t - 1; sin(pi u) is a parabola, 4u(1 - |u|), with one correction
// step (y + 0.225 (y|y| - y)): within 0.1 % of full scale.
float fast_sin(float t) noexcept {
  const float u = 2.0f * t - 1.0f;
  const float y = 4.0f * u * (1.0f - (u < 0.0f ? -u : u));
  return -(0.225f * (y * (y < 0.0f ? -y : y) - y) + y);
}

}  // namespace

const dsp::UnitInfo& Synth::info() const noexcept { return kInfo; }

void Synth::prepare(float sample_rate) noexcept {
  fs_ = sample_rate;
  for (auto& v : voices_) {
    v = Voice{};
  }
  for (unsigned p = 0; p < kParamCount; ++p) {
    set(static_cast<std::uint16_t>(p), kParams[p].def);
  }
  g_ = g_target_;
  clock_ = 0;
}

unsigned Synth::active_voices() const noexcept {
  unsigned n = 0;
  for (const auto& v : voices_) {
    n += v.stage != Stage::Idle ? 1u : 0u;
  }
  return n;
}

float Synth::coeff_ms(float ms) const noexcept { return std::exp(-1000.0f / (ms * fs_)); }

void Synth::set(std::uint16_t id, float v) noexcept {
  if (id >= kParamCount) {
    return;
  }
  values_[id] = v;
  switch (id) {
    case Cutoff: {
      const float fc = v < 0.45f * fs_ ? v : 0.45f * fs_;
      g_target_ = std::tan(kPi * fc / fs_);
      break;
    }
    case Reso:
      k_ = 2.0f - 1.95f * v;
      break;
    case Attack:
      attack_step_ = 1000.0f / (v * fs_);
      break;
    case Decay:
      decay_coef_ = coeff_ms(v * 0.25f);  // the ms value is about the whole fall (four time constants)
      break;
    case Release:
      release_coef_ = coeff_ms(v * 0.25f);
      break;
    case Volume:
      gain_ = 0.3f * std::pow(10.0f, v * 0.05f);
      break;
    default:
      break;
  }
}

void Synth::apply(const dsp::Event& e) noexcept {
  switch (e.kind) {
    case dsp::EventKind::NoteOn:
      note_on(e.note, e.velocity);
      break;
    case dsp::EventKind::NoteOff:
      note_off(e.note);
      break;
    case dsp::EventKind::Param:
      set(e.param, e.value);
      break;
    case dsp::EventKind::AllNotesOff:
      for (auto& v : voices_) {
        if (v.stage != Stage::Idle) {
          v.stage = Stage::Release;
        }
      }
      break;
  }
}

void Synth::note_on(std::uint8_t note, std::uint8_t velocity) noexcept {
  // A free voice, else the oldest releasing one, else the oldest.
  Voice* pick = nullptr;
  for (auto& v : voices_) {
    if (v.stage == Stage::Idle) {
      pick = &v;
      break;
    }
  }
  for (int pass = 0; pick == nullptr && pass < 2; ++pass) {
    for (auto& v : voices_) {
      if ((pass == 0 && v.stage != Stage::Release) || (pick != nullptr && v.age >= pick->age)) {
        continue;
      }
      pick = &v;
    }
  }
  Voice& v = *pick;
  const bool fresh = v.stage == Stage::Idle;
  v.stage = Stage::Attack;
  v.note = note;
  v.age = ++clock_;
  v.inc = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f) / fs_;
  v.vel = static_cast<float>(velocity) / 127.0f;
  if (fresh) {
    v.phase = 0.0f;
    v.env = 0.0f;
    v.ic1 = 0.0f;
    v.ic2 = 0.0f;
  }
}

void Synth::note_off(std::uint8_t note) noexcept {
  for (auto& v : voices_) {
    if (v.note == note && v.stage != Stage::Idle && v.stage != Stage::Release) {
      v.stage = Stage::Release;
    }
  }
}

void Synth::render(float* out, std::uint32_t from, std::uint32_t to) noexcept {
  for (std::uint32_t i = from; i < to; ++i) {
    out[i] = 0.0f;
  }
  // The cutoff glides towards its target once a run (runs are a block or less): no zipper on a turn.
  g_ += (g_target_ - g_) * 0.5f;
  const float a1 = 1.0f / (1.0f + g_ * (g_ + k_));
  const float a2 = g_ * a1;
  const float a3 = g_ * a2;
  const auto wave = static_cast<int>(values_[Wave] + 0.5f);
  const float sustain = values_[Sustain];

  for (auto& v : voices_) {
    if (v.stage == Stage::Idle) {
      continue;
    }
    const float amp = v.vel * gain_;
    for (std::uint32_t i = from; i < to; ++i) {
      v.phase += v.inc;
      if (v.phase >= 1.0f) {
        v.phase -= 1.0f;
      }
      float x;
      switch (wave) {
        case 0:
          x = fast_sin(v.phase);
          break;
        case 1:
          x = 1.0f - 4.0f * (v.phase < 0.5f ? 0.5f - v.phase : v.phase - 0.5f);
          break;
        case 3: {
          float t2 = v.phase + 0.5f;
          t2 = t2 >= 1.0f ? t2 - 1.0f : t2;
          x = (v.phase < 0.5f ? 1.0f : -1.0f) + blep(v.phase, v.inc) - blep(t2, v.inc);
          break;
        }
        default:
          x = 2.0f * v.phase - 1.0f - blep(v.phase, v.inc);
          break;
      }

      switch (v.stage) {
        case Stage::Attack:
          v.env += attack_step_;
          if (v.env >= 1.0f) {
            v.env = 1.0f;
            v.stage = Stage::Decay;
          }
          break;
        case Stage::Decay:
          v.env = sustain + (v.env - sustain) * decay_coef_;
          if (v.env - sustain < kQuiet) {
            v.stage = Stage::Sustain;
          }
          break;
        case Stage::Sustain:
          v.env = sustain;
          break;
        case Stage::Release:
          v.env *= release_coef_;
          if (v.env < kQuiet) {
            v.env = 0.0f;
            v.stage = Stage::Idle;
          }
          break;
        case Stage::Idle:
          break;
      }

      const float v3 = x - v.ic2;
      const float v1 = a1 * v.ic1 + a2 * v3;
      const float v2 = v.ic2 + a2 * v.ic1 + a3 * v3;
      v.ic1 = 2.0f * v1 - v.ic1;
      v.ic2 = 2.0f * v2 - v.ic2;
      out[i] += v2 * v.env * amp;
      if (v.stage == Stage::Idle) {
        break;
      }
    }
  }
}

void Synth::process(const dsp::ProcessArgs& args) noexcept {
  dsp::for_each_run(
      args, [&](std::uint32_t from, std::uint32_t to) { render(args.out_l, from, to); },
      [&](const dsp::Event& e) { apply(e); });
  for (std::uint32_t i = 0; i < args.frames; ++i) {
    args.out_r[i] = args.out_l[i];
  }
}

}  // namespace o3sdk::units
