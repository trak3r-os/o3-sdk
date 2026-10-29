// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/settings.h>

#include <o3sdk/formats/chunk_file.h>

namespace o3sdk::app::settings {

namespace chunk = formats::chunk;

const char* result_name(Result r) noexcept {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::Truncated: return "truncated";
    case Result::BadMagic: return "bad magic";
    case Result::BadCrc: return "bad crc";
    case Result::Malformed: return "malformed";
    case Result::NewerMajor: return "newer major version";
  }
  return "?";
}

namespace {

static_assert(kPadLights == kKeyCount && kBrightness == kUsbStem11 + 1u && kUsbStem1 + hal::usb_audio::kMaxChannels - 1u == kUsbStem11, "the keys are 1 .. kKeyCount, in order");

constexpr std::uint16_t kChunkVersion = 0x0100;  // APPV and LRNB, major 1

struct Entry {
  std::uint16_t key;
  float value;
};

// APPV into `next`: false when this version cannot read it (a newer major, or a shape that does not fit): the chunk is then counted as unknown. Out-of-range values are the runtime's to refuse
// (it knows the descriptors); a count past the slots keeps the first kAppValueSlots.
bool read_appv(const chunk::Chunk& c, KitValues& next) noexcept {
  chunk::Fields f(c);
  const std::uint16_t version = f.u16();
  const std::uint16_t count = f.u16();
  if (!f.ok() || (version >> 8) != (kChunkVersion >> 8) || f.left() < static_cast<std::size_t>(count) * 6u) {
    return false;
  }
  next.app_count = 0;
  for (unsigned i = 0; i < count; ++i) {
    const std::uint16_t key = f.u16();
    const float value = f.f32();
    if (next.app_count < kAppValueSlots && key != 0u && value == value) {  // (a NaN is no value)
      next.app_key[next.app_count] = key;
      next.app_value[next.app_count] = value;
      ++next.app_count;
    }
  }
  return true;
}

// LRNB into `next`: the bindings that are valid for a unit's parameter, kept in order while the store has room; false as read_appv. A binding that is not valid, or that listens to a message another
// one has, or that names a parameter another one has, is dropped (core::learn_plan decides).
bool read_lrnb(const chunk::Chunk& c, KitValues& next, Report& rep) noexcept {
  chunk::Fields f(c);
  const std::uint16_t version = f.u16();
  const std::uint16_t count = f.u16();
  if (!f.ok() || (version >> 8) != (kChunkVersion >> 8) || f.left() < static_cast<std::size_t>(count) * 8u) {
    return false;
  }
  core::LearnBinding fresh[kLearnStoreSlots] = {};
  for (unsigned i = 0; i < count; ++i) {
    core::LearnBinding b;
    b.param = static_cast<core::ParamId>(f.u32());
    b.cc = f.u8();
    b.channel = f.u8();
    b.mode = f.u8();
    b.flags = f.u8();
    core::LearnPlan plan;
    if (!core::learn_valid(b) || core::param_kind(b.param) != core::ParamKind::Unit || (b.flags & core::kLearnFollow) != 0u || !core::learn_plan(fresh, kLearnStoreSlots, b, plan)) {
      ++rep.skipped_values;
      continue;
    }
    for (unsigned s = 0; s < kLearnStoreSlots; ++s) {
      if ((plan.clear >> s) & 1u) {
        fresh[s] = core::LearnBinding{};
      }
    }
    fresh[plan.write] = b;
  }
  for (unsigned s = 0; s < kLearnStoreSlots; ++s) {
    next.learn[s] = fresh[s];
  }
  return true;
}

// The settings as (key, value) in key order: the file's SETS entries.
void gather(const KitValues& v, Entry (&out)[kKeyCount]) noexcept {
  const hal::midi::Config& m = v.midi;
  out[0] = {kTheme, static_cast<float>(static_cast<unsigned>(v.theme))};
  out[1] = {kMidiChannel, static_cast<float>(m.channel)};
  out[2] = {kMidiOutChannel, static_cast<float>(m.out_channel)};
  out[3] = {kMidiOutPorts, static_cast<float>(m.out_ports)};
  out[4] = {kMidiClockOut, m.clock_out ? 1.0f : 0.0f};
  out[5] = {kMidiFollow, m.follow ? 1.0f : 0.0f};
  out[6] = {kMidiFollowPort, static_cast<float>(static_cast<unsigned>(m.follow_port))};
  out[7] = {kMidiTransportIn, m.transport_in ? 1.0f : 0.0f};
  out[8] = {kMidiTransportOut, m.transport_out ? 1.0f : 0.0f};
  out[9] = {kInputSource, static_cast<float>(v.in_source)};
  out[10] = {kInputGain, static_cast<float>(v.in_gain)};
  out[11] = {kInputMonitor, static_cast<float>(v.in_monitor)};
  out[12] = {kRadioRegion, static_cast<float>(v.radio_region)};
  out[13] = {kRadioFreq, static_cast<float>(v.radio_freq)};
  out[14] = {kUsbAudio, v.usb_audio ? 1.0f : 0.0f};
  out[15] = {kUsbProfile, static_cast<float>(v.usb_profile)};
  out[16] = {kUsbDuplex, v.usb_duplex ? 1.0f : 0.0f};
  out[17] = {kUsbReturnLevel, static_cast<float>(v.usb_level)};
  out[18] = {kUsbMapSet, v.usb_map_set ? 1.0f : 0.0f};
  for (unsigned i = 0; i < hal::usb_audio::kMaxChannels; ++i) {
    out[19 + i] = {static_cast<std::uint16_t>(kUsbStem1 + i), static_cast<float>(v.usb_stem[i])};
  }
  out[30] = {kBrightness, static_cast<float>(v.brightness)};
  out[31] = {kPadLights, static_cast<float>(v.pad_lights)};
}

// Applies one entry to `v`: 1 applied, 0 the key is not known, -1 the value is not one this key takes.
int apply(KitValues& v, std::uint16_t key, float value) noexcept {
  if (key < kTheme || key > kPadLights) {
    return 0;
  }
  // Every setting is a small whole number (a NaN fails the first test); the station and the stem masks are the ones that do not fit a byte.
  const float limit = key == kRadioFreq || key >= kUsbStem1 ? 65535.0f : 255.0f;
  if (!(value >= 0.0f && value <= limit) || static_cast<float>(static_cast<unsigned>(value)) != value) {
    return -1;
  }
  const auto n = static_cast<unsigned>(value);
  hal::midi::Config& m = v.midi;
  switch (key) {
    case kTheme:
      if (n >= ui::kThemeCount) {
        return -1;
      }
      v.theme = ui::theme_id_at(n);
      return 1;
    case kMidiChannel:
      if (n > 16u) {
        return -1;
      }
      m.channel = static_cast<std::uint8_t>(n);
      return 1;
    case kMidiOutChannel:
      if (n < 1u || n > 16u) {
        return -1;
      }
      m.out_channel = static_cast<std::uint8_t>(n);
      return 1;
    case kMidiOutPorts:
      if ((n & ~static_cast<unsigned>(core::kMidiAllPorts)) != 0u) {
        return -1;
      }
      m.out_ports = static_cast<std::uint8_t>(n);
      return 1;
    case kMidiClockOut:
    case kMidiFollow:
    case kMidiTransportIn:
    case kMidiTransportOut:
      if (n > 1u) {
        return -1;
      }
      (key == kMidiClockOut ? m.clock_out : key == kMidiFollow ? m.follow : key == kMidiTransportIn ? m.transport_in : m.transport_out) = n != 0u;
      return 1;
    case kMidiFollowPort:
      if (n >= hal::midi::kPortCount) {
        return -1;
      }
      m.follow_port = static_cast<hal::midi::Port>(n);
      return 1;
    case kInputSource:
      if (n >= hal::capture::kSourceCount) {
        return -1;
      }
      v.in_source = static_cast<std::uint8_t>(n);
      return 1;
    case kInputGain:
    case kInputMonitor:
      if (n > 100u) {
        return -1;
      }
      (key == kInputGain ? v.in_gain : v.in_monitor) = static_cast<std::uint8_t>(n);
      return 1;
    case kRadioRegion:
      if (n >= hal::capture::kRegionCount) {
        return -1;
      }
      v.radio_region = static_cast<std::uint8_t>(n);
      return 1;
    case kRadioFreq:
      if (n < 7600u || n > hal::capture::kBandTop) {
        return -1;
      }
      v.radio_freq = static_cast<std::uint16_t>(n);
      return 1;
    case kUsbAudio:
    case kUsbDuplex:
    case kUsbMapSet:
      if (n > 1u) {
        return -1;
      }
      (key == kUsbAudio ? v.usb_audio : key == kUsbDuplex ? v.usb_duplex : v.usb_map_set) = n != 0u;
      return 1;
    case kUsbProfile:
      if (n >= hal::usb_audio::kProfileCount) {
        return -1;
      }
      v.usb_profile = static_cast<std::uint8_t>(n);
      return 1;
    case kUsbReturnLevel:
      if (n > 100u) {
        return -1;
      }
      v.usb_level = static_cast<std::uint8_t>(n);
      return 1;
    case kBrightness:
    case kPadLights:
      if (n > 2u) {
        return -1;
      }
      (key == kBrightness ? v.brightness : v.pad_lights) = static_cast<std::uint8_t>(n);
      return 1;
    default:
      if (key >= kUsbStem1 && key <= kUsbStem11) {
        v.usb_stem[key - kUsbStem1] = static_cast<std::uint16_t>(n);
        return 1;
      }
      return 0;
  }
}

}  // namespace

std::size_t write(const KitValues& v, std::uint8_t* out, std::size_t cap) noexcept {
  Entry e[kKeyCount];
  gather(v, e);
  chunk::Writer w(out, cap, kType);
  if (!w.begin("SETS") || !w.u16(kSetsVersion) || !w.u16(static_cast<std::uint16_t>(kKeyCount))) {
    return 0;
  }
  for (const Entry& x : e) {
    if (!w.u16(x.key) || !w.f32(x.value)) {
      return 0;
    }
  }
  if (!w.end()) {
    return 0;
  }
  if (v.app_count != 0u) {
    if (!w.begin("APPV") || !w.u16(kChunkVersion) || !w.u16(v.app_count)) {
      return 0;
    }
    for (unsigned i = 0; i < v.app_count && i < kAppValueSlots; ++i) {
      if (!w.u16(v.app_key[i]) || !w.f32(v.app_value[i])) {
        return 0;
      }
    }
    if (!w.end()) {
      return 0;
    }
  }
  std::uint16_t bound = 0;
  for (const core::LearnBinding& b : v.learn) {
    bound = static_cast<std::uint16_t>(bound + (core::learn_empty(b) ? 0u : 1u));
  }
  if (bound != 0u) {
    if (!w.begin("LRNB") || !w.u16(kChunkVersion) || !w.u16(bound)) {
      return 0;
    }
    for (const core::LearnBinding& b : v.learn) {
      if (!core::learn_empty(b) && (!w.u32(static_cast<std::uint32_t>(b.param)) || !w.u8(b.cc) || !w.u8(b.channel) || !w.u8(b.mode) || !w.u8(b.flags))) {
        return 0;
      }
    }
    if (!w.end()) {
      return 0;
    }
  }
  return w.finish();
}

Result read(const std::uint8_t* data, std::size_t n, KitValues& v, Report& report) noexcept {
  report = Report{};
  chunk::Reader r;
  switch (r.open(data, n, kType)) {
    case chunk::Error::None: break;
    case chunk::Error::Truncated: return Result::Truncated;
    case chunk::Error::BadMagic: return Result::BadMagic;
    case chunk::Error::BadCrc: return Result::BadCrc;
    case chunk::Error::Malformed: return Result::Malformed;
  }
  KitValues next = v;  // nothing of a file that fails is applied
  Report rep;
  bool sets_seen = false;
  bool appv_seen = false;
  bool lrnb_seen = false;
  chunk::Chunk c{};
  while (r.next(c)) {
    if (chunk::id_is(c, "APPV") || chunk::id_is(c, "LRNB")) {
      const bool app = chunk::id_is(c, "APPV");
      bool& seen = app ? appv_seen : lrnb_seen;
      if (seen || !(app ? read_appv(c, next) : read_lrnb(c, next, rep))) {  // (a second one, or one this version cannot read, is skipped like any other it does not know)
        ++rep.unknown_chunks;
      }
      seen = true;
      continue;
    }
    if (!chunk::id_is(c, "SETS")) {
      ++rep.unknown_chunks;
      continue;
    }
    if (sets_seen) {
      return Result::Malformed;
    }
    sets_seen = true;
    chunk::Fields f(c);
    const std::uint16_t version = f.u16();
    const std::uint16_t count = f.u16();
    if (!f.ok()) {
      return Result::Malformed;
    }
    if ((version >> 8) == 0u) {
      return Result::Malformed;
    }
    if ((version >> 8) > (kSetsVersion >> 8)) {
      return Result::NewerMajor;
    }
    rep.newer_minor = (version & 0xFFu) > (kSetsVersion & 0xFFu);
    if (f.left() < static_cast<std::size_t>(count) * 6u || (!rep.newer_minor && f.left() != static_cast<std::size_t>(count) * 6u)) {
      return Result::Malformed;
    }
    for (unsigned i = 0; i < count; ++i) {
      const std::uint16_t key = f.u16();
      const float value = f.f32();
      const int took = apply(next, key, value);
      if (took > 0) {
        ++rep.applied;
      } else if (took == 0) {
        ++rep.skipped_keys;
      } else {
        ++rep.skipped_values;
      }
    }
  }
  v = next;
  report = rep;
  return Result::Ok;
}

}  // namespace o3sdk::app::settings
