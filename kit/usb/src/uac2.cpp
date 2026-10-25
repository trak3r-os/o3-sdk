// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/uac2.h>

namespace o3sdk::usb {
namespace {

constexpr std::uint8_t kTypeClass = 1;
constexpr std::uint8_t kRecipientInterface = 1;
constexpr std::uint8_t kReqCur = 0x01;
constexpr std::uint8_t kReqRange = 0x02;
constexpr std::uint8_t kCsSamFreq = 0x01;  // Clock Source: sampling frequency control selector

void put32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

}  // namespace

ControlReply handle_audio_control_request(const SetupPacket& setup) noexcept {
  ControlReply reply;  // Stall
  const std::uint32_t entity = static_cast<std::uint32_t>(setup.index >> 8);
  const std::uint32_t interface = static_cast<std::uint32_t>(setup.index & 0xFFu);
  const std::uint32_t selector = static_cast<std::uint32_t>(setup.value >> 8);
  const std::uint32_t channel = static_cast<std::uint32_t>(setup.value & 0xFFu);

  if (setup.type() != kTypeClass || setup.recipient() != kRecipientInterface || !setup.device_to_host() ||
      interface != layout::kIfAudioControl || entity != layout::kEntClock || selector != kCsSamFreq ||
      channel != 0u) {
    return reply;
  }
  std::uint32_t length = 0;
  if (setup.request == kReqCur) {
    put32(reply.data, kDeclaredRateHz);
    length = 4;
  } else if (setup.request == kReqRange) {
    reply.data[0] = 1;  // wNumSubRanges
    reply.data[1] = 0;
    put32(reply.data + 2, kDeclaredRateHz);   // dMIN
    put32(reply.data + 6, kDeclaredRateHz);   // dMAX
    put32(reply.data + 10, 0);                // dRES: discrete
    length = 14;
  } else {
    return reply;
  }
  reply.length = static_cast<std::uint16_t>(length < setup.length ? length : setup.length);
  reply.status = ControlStatus::Data;
  return reply;
}

bool set_audio_alt(AudioStreamState& state, std::uint8_t interface, std::uint8_t alt) noexcept {
  if (alt > 1u) {
    return false;
  }
  if (interface == layout::kIfAudioIn) {
    state.in_alt = alt;
    return true;
  }
  if (interface == layout::kIfAudioOut) {
    state.out_alt = alt;
    return true;
  }
  return false;
}

bool get_audio_alt(const AudioStreamState& state, std::uint8_t interface, std::uint8_t& alt) noexcept {
  if (interface == layout::kIfAudioIn) {
    alt = state.in_alt;
    return true;
  }
  if (interface == layout::kIfAudioOut) {
    alt = state.out_alt;
    return true;
  }
  return false;
}

}  // namespace o3sdk::usb
