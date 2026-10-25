// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006a: UAC2 class requests and AudioStreaming alternate settings (ADR 0012 S1).
#include <doctest/doctest.h>

#include <o3sdk/usb/uac2.h>

#include "usb_test_util.h"

#include <array>
#include <cstdint>
#include <cstring>

using namespace o3sdk::usb;

namespace {

constexpr std::uint16_t kClockIndex = static_cast<std::uint16_t>((layout::kEntClock << 8) | layout::kIfAudioControl);
constexpr std::uint16_t kSamFreq = 0x0100;  // selector 1, channel 0

SetupPacket get(std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
  SetupPacket s;
  s.request_type = 0xA1;  // device to host, class, interface
  s.request = request;
  s.value = value;
  s.index = index;
  s.length = length;
  return s;
}

}  // namespace

TEST_CASE("usb uac2: setup packet decodes little-endian") {
  const std::uint8_t raw[8] = {0xA1, 0x02, 0x00, 0x01, 0x04, 0x01, 0x0E, 0x00};
  const SetupPacket s = parse_setup(raw);
  CHECK(s.request_type == 0xA1);
  CHECK(s.request == 2);
  CHECK(s.value == 0x0100);
  CHECK(s.index == 0x0104);
  CHECK(s.length == 14);
  CHECK(s.device_to_host());
  CHECK(s.type() == 1);
  CHECK(s.recipient() == 1);
  CHECK_FALSE(parse_setup(std::array<std::uint8_t, 8>{0x21, 1, 0, 1, 4, 1, 4, 0}.data()).device_to_host());
}

TEST_CASE("usb uac2: clock source GET CUR returns 44,100 Hz") {
  const ControlReply r = handle_audio_control_request(get(0x01, kSamFreq, kClockIndex, 4));
  CHECK(r.status == ControlStatus::Data);
  CHECK(r.length == 4);
  const std::uint8_t expect[4] = {0x44, 0xAC, 0x00, 0x00};  // 0x0000AC44
  CHECK(std::memcmp(r.data, expect, 4) == 0);
}

TEST_CASE("usb uac2: clock source GET RANGE reports one discrete rate") {
  const ControlReply r = handle_audio_control_request(get(0x02, kSamFreq, kClockIndex, 255));
  CHECK(r.status == ControlStatus::Data);
  CHECK(r.length == 14);
  const std::uint8_t expect[14] = {0x01, 0x00,                    // wNumSubRanges
                                   0x44, 0xAC, 0x00, 0x00,        // dMIN
                                   0x44, 0xAC, 0x00, 0x00,        // dMAX
                                   0x00, 0x00, 0x00, 0x00};       // dRES
  CHECK(std::memcmp(r.data, expect, 14) == 0);
}

TEST_CASE("usb uac2: a reply is cut to wLength") {
  const std::uint8_t full[14] = {0x01, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x44, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (std::uint16_t want = 0; want <= 20; ++want) {
    const ControlReply r = handle_audio_control_request(get(0x02, kSamFreq, kClockIndex, want));
    CHECK(r.status == ControlStatus::Data);
    CHECK(r.length == (want < 14 ? want : 14));
    CHECK(std::memcmp(r.data, full, r.length) == 0);
  }
  CHECK(handle_audio_control_request(get(0x01, kSamFreq, kClockIndex, 2)).length == 2);
  CHECK(handle_audio_control_request(get(0x01, kSamFreq, kClockIndex, 0xFFFF)).length == 4);
}

TEST_CASE("usb uac2: everything else stalls") {
  const auto stalls = [](const SetupPacket& s) { return handle_audio_control_request(s).status == ControlStatus::Stall; };
  SetupPacket set = get(0x01, kSamFreq, kClockIndex, 4);
  set.request_type = 0x21;  // SET CUR: the control is read-only
  CHECK(stalls(set));
  SetupPacket set_range = get(0x02, kSamFreq, kClockIndex, 4);
  set_range.request_type = 0x21;
  CHECK(stalls(set_range));
  CHECK(stalls(get(0x03, kSamFreq, kClockIndex, 4)));                       // MEM
  CHECK(stalls(get(0x00, kSamFreq, kClockIndex, 4)));                       // undefined request
  CHECK(stalls(get(0x01, 0x0200, kClockIndex, 4)));                         // clock validity (not declared)
  CHECK(stalls(get(0x01, 0x0101, kClockIndex, 4)));                         // channel 1 of a clock control
  CHECK(stalls(get(0x01, 0x0000, kClockIndex, 4)));                         // selector 0
  CHECK(stalls(get(0x01, kSamFreq, static_cast<std::uint16_t>((2 << 8) | layout::kIfAudioControl), 4)));  // a terminal
  CHECK(stalls(get(0x01, kSamFreq, static_cast<std::uint16_t>((0 << 8) | layout::kIfAudioControl), 4)));  // no entity
  CHECK(stalls(get(0x01, kSamFreq, static_cast<std::uint16_t>((layout::kEntClock << 8) | layout::kIfAudioIn), 4)));
  CHECK(stalls(get(0x01, kSamFreq, static_cast<std::uint16_t>((layout::kEntClock << 8) | layout::if_midi_control(Mode::Audio)), 4)));
  SetupPacket standard = get(0x01, kSamFreq, kClockIndex, 4);
  standard.request_type = 0x81;  // standard type
  CHECK(stalls(standard));
  SetupPacket vendor = get(0x01, kSamFreq, kClockIndex, 4);
  vendor.request_type = 0xC1;
  CHECK(stalls(vendor));
  SetupPacket device = get(0x01, kSamFreq, kClockIndex, 4);
  device.request_type = 0xA0;  // class, recipient device
  CHECK(stalls(device));
  SetupPacket endpoint = get(0x01, kSamFreq, kClockIndex, 4);
  endpoint.request_type = 0xA2;
  CHECK(stalls(endpoint));
  // Feature unit mute / volume: none declared, so any feature-unit entity stalls.
  CHECK(stalls(get(0x01, 0x0100, static_cast<std::uint16_t>((6 << 8) | layout::kIfAudioControl), 1)));
  CHECK(stalls(get(0x02, 0x0200, static_cast<std::uint16_t>((6 << 8) | layout::kIfAudioControl), 8)));
  // A stall carries no data.
  const ControlReply s = handle_audio_control_request(set);
  CHECK(s.length == 0);
}

TEST_CASE("usb uac2: AudioStreaming alternate settings") {
  AudioStreamState st;
  CHECK_FALSE(st.in_active());
  CHECK_FALSE(st.out_active());
  CHECK(set_audio_alt(st, layout::kIfAudioIn, 1));
  CHECK(st.in_active());
  CHECK_FALSE(st.out_active());
  CHECK(set_audio_alt(st, layout::kIfAudioOut, 1));
  CHECK(st.out_active());
  CHECK(set_audio_alt(st, layout::kIfAudioIn, 1));  // same alt again: accepted
  CHECK(st.in_active());
  std::uint8_t alt = 9;
  CHECK(get_audio_alt(st, layout::kIfAudioIn, alt));
  CHECK(alt == 1);
  CHECK(set_audio_alt(st, layout::kIfAudioIn, 0));
  CHECK_FALSE(st.in_active());
  CHECK(get_audio_alt(st, layout::kIfAudioIn, alt));
  CHECK(alt == 0);
  CHECK(st.out_active());
  // Refusals leave the state alone.
  CHECK_FALSE(set_audio_alt(st, layout::kIfAudioIn, 2));
  CHECK_FALSE(set_audio_alt(st, layout::kIfAudioControl, 1));
  CHECK_FALSE(set_audio_alt(st, layout::if_midi_stream(Mode::Audio), 0));
  CHECK_FALSE(set_audio_alt(st, 200, 0));
  CHECK_FALSE(st.in_active());
  CHECK(st.out_active());
  alt = 7;
  CHECK_FALSE(get_audio_alt(st, layout::kIfCdcComm, alt));
  CHECK(alt == 7);
}
