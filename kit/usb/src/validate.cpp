// SPDX-License-Identifier: GPL-3.0-or-later
//
// Descriptor validator: parses the bytes (never the builder's inputs) and checks the structure described in
// validate.h. It is a test and bring-up tool; the firmware does not link it unless it calls it.
#include <o3sdk/usb/validate.h>

namespace o3sdk::usb {
namespace {

using E = ValidateError;

constexpr std::uint32_t rd16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }

constexpr std::uint32_t kMaxEndpoints = 24;
constexpr std::uint32_t kMaxEntities = 32;

// Everything the walk remembers. One instance per validate_config_descriptor call, on the stack.
struct Walk {
  const std::uint8_t* d;
  std::size_t n;
  const Config* cfg;
  std::uint32_t str_count;

  // interfaces
  std::uint32_t next_if = 0;     // next alt-0 interface number expected
  bool have_if = false;
  std::uint32_t cur_if = 0, cur_alt = 0, cur_class = 0, cur_sub = 0, cur_proto = 0;
  std::uint32_t decl_eps = 0, seen_eps = 0;
  // IADs
  std::uint32_t iad_count = 0, iad_next = 0;
  bool iad_open = false;
  std::uint32_t iad_class = 0, iad_first = 0, iad_left = 0;
  // endpoints
  std::uint8_t eps[kMaxEndpoints] = {};
  std::uint32_t n_eps = 0;
  // class-specific accounting
  std::uint32_t ac_decl = 0, ac_sum = 0;
  bool ac_open = false;
  std::uint32_t ms_decl = 0, ms_sum = 0;
  bool ms_open = false;
  bool saw_endpoint_in_alt = false;
  // entities (UAC2 ids and MIDI jack ids live in separate namespaces; one is used per interface)
  bool id_seen[kMaxEntities] = {};
  std::uint32_t n_entities = 0;
  std::uint8_t id_channels[kMaxEntities] = {};
  bool jack_seen[kMaxEntities] = {};
  // AudioStreaming alt
  std::uint32_t as_channels = 0, as_slot = 0, as_link = 0;
  bool as_have_general = false, as_have_format = false;
  bool as_data_out = false, as_data_in = false, as_feedback = false;
  std::uint32_t as_in_channels_seen = 0, as_out_channels_seen = 0;
  std::uint32_t n_as_in = 0, n_as_out = 0;
  // the Mass Storage interface (DISK mode): how many, and which bulk endpoints it declared
  std::uint32_t n_msc = 0;
  bool msc_out = false, msc_in = false;
  // interface numbers of the two AudioControl functions (0xFF = absent), for the per-mode layout check
  std::uint32_t uac2_ac_if = 0xFFu, midi_ac_if = 0xFFu;

  bool is_uac2_ac() const { return cur_class == 1 && cur_sub == 1 && cur_proto == 0x20; }
  bool is_uac2_as() const { return cur_class == 1 && cur_sub == 2 && cur_proto == 0x20; }
  bool is_midi_ac() const { return cur_class == 1 && cur_sub == 1 && cur_proto == 0; }
  bool is_midi_ms() const { return cur_class == 1 && cur_sub == 3 && cur_proto == 0; }
  bool is_cdc() const { return cur_class == 2 && cur_sub == 2; }
  bool is_msc() const { return cur_class == 8 && cur_sub == 6 && cur_proto == 0x50; }
  bool bad_string(std::uint32_t idx) const { return idx >= str_count; }

  // Called when the current interface alt ends (next interface, IAD, or end of the buffer).
  E close_alt() {
    if (!have_if) {
      return E::Ok;
    }
    if (seen_eps != decl_eps) {
      return E::EndpointCount;
    }
    if (ac_open && ac_sum != ac_decl) {
      return E::BadTotalLength;
    }
    if (ms_open && ms_sum != ms_decl) {
      return E::BadTotalLength;
    }
    if (is_uac2_as() && cur_alt >= 1) {
      if (!as_have_general || !as_have_format || !(as_data_in || as_data_out)) {
        return E::BadClassSpecific;
      }
      if (as_data_out != as_feedback) {
        return E::BadFeedback;
      }
      if (as_data_in) {
        as_in_channels_seen = as_channels;
        ++n_as_in;
      } else {
        as_out_channels_seen = as_channels;
        ++n_as_out;
      }
    }
    ac_open = false;
    ms_open = false;
    return E::Ok;
  }
};

ValidateResult res(E e, std::size_t off) { return ValidateResult{e, static_cast<std::uint16_t>(off)}; }

std::uint32_t mps_of(const std::uint8_t* ep) { return rd16(ep + 4); }

// Handles one standard endpoint descriptor at d[pos..pos+len).
E on_endpoint(Walk& w, std::size_t pos, std::size_t len) {
  const std::uint8_t* p = w.d + pos;
  if (!w.have_if) {
    return E::EndpointOutsideInterface;
  }
  const bool midi = w.is_midi_ms();
  if (len != (midi ? 9u : 7u)) {
    return E::BadLength;
  }
  const std::uint32_t addr = p[2];
  const std::uint32_t number = addr & 0x0Fu;
  if (number == 0 || (addr & 0x70u) != 0) {
    return E::EndpointNumber;
  }
  for (std::uint32_t i = 0; i < w.n_eps; ++i) {
    if (w.eps[i] == addr) {
      return E::DuplicateEndpoint;
    }
  }
  if (w.n_eps >= kMaxEndpoints) {
    return E::TooManyEndpointNumbers;
  }
  w.eps[w.n_eps++] = static_cast<std::uint8_t>(addr);
  ++w.seen_eps;
  w.saw_endpoint_in_alt = true;

  if (w.is_msc()) {
    // Bulk-Only: exactly one bulk OUT (EP 4) and one bulk IN (EP 5), the wMaxPacketSize of the bulk speed (64 FS, 512 HS).
    if (p[3] != 0x02 || mps_of(p) != (w.cfg->speed == Speed::Full ? 64u : 512u)) {
      return E::BadMaxPacket;
    }
    if (addr != layout::kEpMscOut && addr != layout::kEpMscIn) {
      return E::EndpointNumber;
    }
    (addr == layout::kEpMscIn ? w.msc_in : w.msc_out) = true;
  }

  if (w.is_uac2_as()) {
    const std::uint32_t attrs = p[3];
    const std::uint32_t mps = rd16(p + 4);
    const std::uint32_t usage = (attrs >> 4) & 0x03u;
    const bool fs = w.cfg->speed == Speed::Full;
    if ((attrs & 0x03u) != 0x01u) {
      return E::BadMaxPacket;  // must be isochronous
    }
    if (usage == 1u) {
      if ((attrs & 0xCFu) != 0x01u || (addr & 0x80u) == 0 || mps != (fs ? 3u : 4u)) {
        return E::BadFeedback;
      }
      w.as_feedback = true;
    } else {
      if (usage != 0u || (attrs & 0x0Cu) != 0x04u || !w.as_have_general || !w.as_have_format) {
        return E::BadMaxPacket;  // data endpoint must be asynchronous and follow the format
      }
      const std::uint32_t required = w.as_channels * w.as_slot * frames_hi(w.cfg->speed);
      const std::uint32_t limit = fs ? kMaxIsoPacketFull : kMaxIsoPacketHigh;
      if (mps < required || mps > limit) {
        return E::BadMaxPacket;
      }
      if ((addr & 0x80u) != 0) {
        w.as_data_in = true;
      } else {
        w.as_data_out = true;
      }
    }
  }
  return E::Ok;
}

E on_cs_interface(Walk& w, std::size_t pos, std::size_t len) {
  const std::uint8_t* p = w.d + pos;
  if (!w.have_if || len < 3u) {
    return E::BadClassSpecific;
  }
  const std::uint32_t sub = p[2];

  if (w.is_uac2_ac()) {
    if (!w.ac_open) {
      if (sub != 0x01u || len != 9u || w.cur_alt != 0) {
        return E::BadClassSpecific;
      }
      w.ac_open = true;
      w.ac_decl = rd16(p + 6);
      w.ac_sum = 9;
      return (p[3] == 0x00 && p[4] == 0x02) ? E::Ok : E::BadClassSpecific;  // bcdADC 2.00
    }
    w.ac_sum += static_cast<std::uint32_t>(len);
    const std::uint32_t id = p[3];
    if (id == 0 || id >= kMaxEntities || w.id_seen[id]) {
      return E::BadEntity;
    }
    switch (sub) {
      case 0x0A:  // clock source
        if (len != 8u) {
          return E::BadLength;
        }
        if (w.bad_string(p[7])) {
          return E::BadString;
        }
        break;
      case 0x02: {  // input terminal
        if (len != 17u) {
          return E::BadLength;
        }
        const std::uint32_t clk = p[7];
        if (clk >= kMaxEntities || !w.id_seen[clk]) {
          return E::BadEntity;
        }
        if (w.bad_string(p[16]) || (p[8] != 0 && w.bad_string(p[13] + p[8] - 1u))) {
          return E::BadString;
        }
        w.id_channels[id] = p[8];
        break;
      }
      case 0x03: {  // output terminal
        if (len != 12u) {
          return E::BadLength;
        }
        const std::uint32_t src = p[7];
        const std::uint32_t clk = p[8];
        if (src >= kMaxEntities || !w.id_seen[src] || clk >= kMaxEntities || !w.id_seen[clk]) {
          return E::BadEntity;
        }
        if (w.bad_string(p[11])) {
          return E::BadString;
        }
        w.id_channels[id] = w.id_channels[src];
        break;
      }
      default:
        return E::BadClassSpecific;
    }
    w.id_seen[id] = true;
    ++w.n_entities;
    return E::Ok;
  }

  if (w.is_uac2_as()) {
    if (w.cur_alt == 0) {
      return E::Alt0HasBandwidth;  // alt 0 carries no class-specific stream description either
    }
    if (sub == 0x01u) {
      if (len != 16u || w.as_have_general) {
        return E::BadLength;
      }
      w.as_link = p[3];
      if (w.as_link >= kMaxEntities || !w.id_seen[w.as_link]) {
        return E::BadEntity;
      }
      if (p[5] != 0x01 || p[6] != 0x01 || p[7] != 0 || p[8] != 0 || p[9] != 0) {
        return E::BadClassSpecific;  // Type I, PCM only
      }
      w.as_channels = p[10];
      if (w.as_channels == 0 || w.as_channels != w.id_channels[w.as_link]) {
        return E::BadEntity;
      }
      if (w.bad_string(static_cast<std::uint32_t>(p[15]) + w.as_channels - 1u)) {
        return E::BadString;
      }
      w.as_have_general = true;
      return E::Ok;
    }
    if (sub == 0x02u) {
      if (len != 6u || !w.as_have_general || w.as_have_format || p[3] != 0x01) {
        return E::BadClassSpecific;
      }
      w.as_slot = p[4];
      if (p[4] != kBytesPerSample || p[5] != 16) {
        return E::BadClassSpecific;  // 16-bit in a 2-byte slot only
      }
      w.as_have_format = true;
      return E::Ok;
    }
    return E::BadClassSpecific;
  }

  if (w.is_midi_ac()) {
    if (sub != 0x01u || len != 9u || w.ac_open) {
      return E::BadClassSpecific;
    }
    w.ac_open = true;
    w.ac_decl = rd16(p + 5);
    w.ac_sum = 9;
    return (p[7] == 1 && p[8] == w.cur_if + 1u) ? E::Ok : E::BadClassSpecific;
  }

  if (w.is_midi_ms()) {
    if (!w.ms_open) {
      if (sub != 0x01u || len != 7u) {
        return E::BadClassSpecific;
      }
      w.ms_open = true;
      w.ms_decl = rd16(p + 5);
      w.ms_sum = 7;
      return E::Ok;
    }
    if (w.saw_endpoint_in_alt) {
      return E::BadClassSpecific;  // jacks come before the endpoints
    }
    w.ms_sum += static_cast<std::uint32_t>(len);
    const std::uint32_t id = p[4];
    if (id == 0 || id >= kMaxEntities || w.jack_seen[id]) {
      return E::BadEntity;
    }
    if (sub == 0x02u) {  // MIDI IN jack
      if (len != 6u) {
        return E::BadLength;
      }
      if (w.bad_string(p[5])) {
        return E::BadString;
      }
    } else if (sub == 0x03u) {  // MIDI OUT jack, one input pin
      if (len != 9u || p[5] != 1) {
        return E::BadLength;
      }
      if (p[6] >= kMaxEntities || !w.jack_seen[p[6]]) {
        return E::BadEntity;
      }
      if (w.bad_string(p[8])) {
        return E::BadString;
      }
    } else {
      return E::BadClassSpecific;
    }
    w.jack_seen[id] = true;
    return E::Ok;
  }

  if (w.is_cdc()) {
    switch (sub) {
      case 0x00:
        return len == 5u ? E::Ok : E::BadLength;
      case 0x01:
        return (len == 5u && p[4] == w.cur_if + 1u) ? E::Ok : E::BadClassSpecific;
      case 0x02:
        return len == 4u ? E::Ok : E::BadLength;
      case 0x06:
        return (len == 5u && p[3] == w.cur_if && p[4] == w.cur_if + 1u) ? E::Ok : E::BadClassSpecific;
      default:
        return E::BadClassSpecific;
    }
  }
  return E::BadClassSpecific;
}

E on_cs_endpoint(Walk& w, std::size_t pos, std::size_t len) {
  const std::uint8_t* p = w.d + pos;
  if (!w.have_if || !w.saw_endpoint_in_alt) {
    return E::BadClassSpecific;
  }
  if (w.is_uac2_as()) {
    return (len == 8u && p[2] == 0x01) ? E::Ok : E::BadClassSpecific;
  }
  if (w.is_midi_ms()) {
    if (len != 5u || p[2] != 0x01 || p[3] != 1) {
      return E::BadClassSpecific;
    }
    return (p[4] < kMaxEntities && w.jack_seen[p[4]]) ? E::Ok : E::BadEntity;
  }
  return E::BadClassSpecific;
}

E on_interface(Walk& w, std::size_t, const std::uint8_t* p, std::size_t len) {
  if (len != 9u) {
    return E::BadLength;
  }
  const E closed = w.close_alt();
  if (closed != E::Ok) {
    return closed;
  }
  const std::uint32_t number = p[2];
  const std::uint32_t alt = p[3];
  if (alt == 0) {
    if (number != w.next_if) {
      return E::InterfaceNumbering;
    }
    ++w.next_if;
    if (w.iad_left == 0) {
      // Every interface is inside an IAD in these composites, except the lone Mass Storage interface of DISK mode.
      if (!(w.cfg->mode == Mode::Disk && p[5] == 8)) {
        return E::IadMismatch;
      }
      ++w.n_msc;
    } else {
      --w.iad_left;
    }
  } else if (!w.have_if || number != w.cur_if || alt != w.cur_alt + 1u) {
    return E::AltNumbering;
  }
  w.have_if = true;
  w.cur_if = number;
  w.cur_alt = alt;
  w.cur_class = p[5];
  w.cur_sub = p[6];
  w.cur_proto = p[7];
  w.decl_eps = p[4];
  if (alt == 0 && w.is_uac2_ac()) {
    w.uac2_ac_if = number;
  } else if (alt == 0 && w.is_midi_ac()) {
    w.midi_ac_if = number;
  }
  w.seen_eps = 0;
  w.saw_endpoint_in_alt = false;
  w.ms_open = false;
  w.as_have_general = w.as_have_format = false;
  w.as_data_in = w.as_data_out = w.as_feedback = false;
  if (alt == 0 && w.iad_open && w.iad_first == number && w.iad_class != w.cur_class) {
    return E::IadMismatch;
  }
  if (w.bad_string(p[8])) {
    return E::BadString;
  }
  if (w.is_uac2_as() && alt == 0 && w.decl_eps != 0) {
    return E::Alt0HasBandwidth;
  }
  return E::Ok;
}

}  // namespace

ValidateResult validate_device_descriptor(const std::uint8_t* d, std::size_t n, const Config& cfg) noexcept {
  if (n < 2u) {
    return res(E::Truncated, 0);
  }
  if (n != kDeviceDescriptorSize || d[0] != kDeviceDescriptorSize) {
    return res(n < kDeviceDescriptorSize ? E::Truncated : E::BadLength, 0);
  }
  if (d[1] != 1 || d[2] != 0x00 || d[3] != 0x02 || d[4] != 0xEF || d[5] != 0x02 || d[6] != 0x01 || d[7] != 64 ||
      d[17] != 1) {
    return res(E::BadHeader, 0);
  }
  const std::uint32_t bcd = device_bcd(cfg.mode);
  if (rd16(d + 8) != kVendorId || rd16(d + 10) != kProductId || rd16(d + 12) != bcd) {
    return res(E::BadHeader, 8);
  }
  const std::uint32_t count = string_count(cfg);
  if (d[14] >= count || d[15] >= count || d[16] >= count || d[14] == 0 || d[15] == 0 || d[16] == 0) {
    return res(E::BadString, 14);
  }
  return res(E::Ok, 0);
}

ValidateResult validate_config_descriptor(const std::uint8_t* d, std::size_t n, const Config& cfg) noexcept {
  if (n < 9u) {
    return res(E::Truncated, 0);
  }
  if (d[0] != 9 || d[1] != 2 || d[5] != 1 || (d[7] & 0x80u) == 0) {
    return res(E::BadHeader, 0);
  }
  if (rd16(d + 2) != n) {
    return res(E::BadTotalLength, 2);
  }
  if (n > 0xFFFFu) {
    return res(E::BadTotalLength, 2);
  }
  Walk w{};
  w.d = d;
  w.n = n;
  w.cfg = &cfg;
  w.str_count = string_count(cfg);
  if (d[6] != 0 && w.bad_string(d[6])) {
    return res(E::BadString, 6);
  }

  std::size_t pos = 9;
  while (pos < n) {
    const std::size_t len = d[pos];
    if (len < 2u) {
      return res(E::BadLength, pos);
    }
    if (pos + len > n) {
      return res(E::Truncated, pos);
    }
    const std::uint8_t* p = d + pos;
    E e = E::Ok;
    switch (p[1]) {
      case 0x0B: {  // interface association
        if (len != 8u) {
          e = E::BadLength;
          break;
        }
        e = w.close_alt();
        if (e != E::Ok) {
          break;
        }
        w.have_if = false;
        if (w.iad_left != 0 || p[2] != w.next_if || p[3] == 0) {
          e = E::IadMismatch;
          break;
        }
        w.iad_open = true;
        w.iad_first = p[2];
        w.iad_left = p[3];
        w.iad_class = p[4];
        ++w.iad_count;
        if (w.bad_string(p[7])) {
          e = E::BadString;
        }
        break;
      }
      case 0x04:
        e = on_interface(w, pos, p, len);
        break;
      case 0x05:
        e = on_endpoint(w, pos, len);
        break;
      case 0x24:
        e = on_cs_interface(w, pos, len);
        break;
      case 0x25:
        e = on_cs_endpoint(w, pos, len);
        break;
      default:
        e = E::BadType;
        break;
    }
    if (e != E::Ok) {
      return res(e, pos);
    }
    pos += len;
  }

  const E closed = w.close_alt();
  if (closed != E::Ok) {
    return res(closed, pos);
  }
  if (w.iad_left != 0) {
    return res(E::IadMismatch, pos);
  }
  if (d[4] != w.next_if) {
    return res(E::BadNumInterfaces, 4);
  }

  // Per-mode shape: interfaces, IADs, endpoints, endpoint numbers, stream channel counts.
  std::uint32_t numbers = 0;
  std::uint32_t seen_number[16] = {};
  for (std::uint32_t i = 0; i < w.n_eps; ++i) {
    const std::uint32_t num = w.eps[i] & 0x0Fu;
    if (seen_number[num] == 0) {
      seen_number[num] = 1;
      ++numbers;
    }
  }
  if (numbers > 15u) {
    return res(E::TooManyEndpointNumbers, 0);
  }
  const bool audio = cfg.mode == Mode::Audio;
  const bool disk = cfg.mode == Mode::Disk;  // STANDARD plus the Mass Storage interface: 2 more endpoints on 2 more numbers, no IAD of its own
  const bool ret = audio && cfg.usb_return;  // the return stream: its AudioStreaming interface, two terminals, data and feedback endpoints
  if (w.next_if != layout::interface_count(cfg.mode, cfg.usb_return) || w.iad_count != (audio ? 3u : 2u) ||
      w.n_eps != (audio ? (ret ? 8u : 6u) : (disk ? 7u : 5u)) || numbers != (audio ? (ret ? 5u : 4u) : (disk ? 5u : 3u))) {
    return res(E::ModeCounts, 0);
  }
  if (w.n_msc != (disk ? 1u : 0u) || (disk && (!w.msc_in || !w.msc_out))) {
    return res(E::ModeCounts, 0);
  }
  // Linux (snd-usb-audio) looks the UAC2 terminals up in the FIRST AudioControl interface: in AUDIO mode that must be
  // the UAC2 one (usb_types.h, namespace layout).
  if (w.midi_ac_if != layout::if_midi_control(cfg.mode, cfg.usb_return) || (audio && w.uac2_ac_if != layout::kIfAudioControl) ||
      (!audio && w.uac2_ac_if != 0xFFu)) {
    return res(E::ModeCounts, 0);
  }
  // The entity graph: the clock, the stems input and output terminals, and with the return stream its two terminals.
  if (audio && (w.n_as_in != 1u || w.n_as_out != (ret ? 1u : 0u) || w.as_in_channels_seen != profile_channels(cfg.profile) ||
                w.as_out_channels_seen != (ret ? kReturnChannels : 0u) || w.n_entities != (ret ? 5u : 3u))) {
    return res(E::BadEntity, 0);
  }
  return res(E::Ok, 0);
}

}  // namespace o3sdk::usb
