// SPDX-License-Identifier: GPL-3.0-or-later
//
// Endpoint 0 and the device's standard state. Request numbers and rules are USB 2.0 chapter 9 (9.4: the standard requests, 9.1:
// device states, 8.5.3: the control transfer and its toggles), the CDC ACM requests are CDC 1.2 PSTN. No third-party code.
#include <o3sdk/usb/control.h>

namespace o3sdk::usb {
namespace {

constexpr std::uint8_t kTypeStandard = 0;
constexpr std::uint8_t kTypeClass = 1;

constexpr std::uint8_t kRecipientDevice = 0;
constexpr std::uint8_t kRecipientInterface = 1;
constexpr std::uint8_t kRecipientEndpoint = 2;

constexpr std::uint8_t kGetStatus = 0x00;
constexpr std::uint8_t kClearFeature = 0x01;
constexpr std::uint8_t kSetFeature = 0x03;
constexpr std::uint8_t kSetAddress = 0x05;
constexpr std::uint8_t kGetDescriptor = 0x06;
constexpr std::uint8_t kGetConfiguration = 0x08;
constexpr std::uint8_t kSetConfiguration = 0x09;
constexpr std::uint8_t kGetInterface = 0x0A;
constexpr std::uint8_t kSetInterface = 0x0B;

constexpr std::uint8_t kDescDevice = 1;
constexpr std::uint8_t kDescConfig = 2;
constexpr std::uint8_t kDescString = 3;

constexpr std::uint16_t kFeatureEndpointHalt = 0;
constexpr std::uint16_t kLangIdEnglishUs = 0x0409;

constexpr std::uint8_t kCdcSetLineCoding = 0x20;
constexpr std::uint8_t kCdcGetLineCoding = 0x21;
constexpr std::uint8_t kCdcSetControlLineState = 0x22;

constexpr std::uint8_t kMscGetMaxLun = 0xFE;  // Mass Storage Bulk-Only Transport 3.2
constexpr std::uint8_t kMscReset = 0xFF;       // 3.1

}  // namespace

struct ControlPipe::Plan {
  enum class Kind : std::uint8_t { Stall, Status, In, Out };
  Kind kind = Kind::Stall;
  const std::uint8_t* data = nullptr;
  std::uint32_t len = 0;
  Effect effect = Effect::None;
  std::uint8_t arg = 0;

  static Plan status(Effect effect = Effect::None, std::uint8_t arg = 0) noexcept {
    Plan p;
    p.kind = Kind::Status;
    p.effect = effect;
    p.arg = arg;
    return p;
  }
  static Plan in(const std::uint8_t* data, std::uint32_t len) noexcept {
    Plan p;
    p.kind = Kind::In;
    p.data = data;
    p.len = len;
    return p;
  }
  static Plan out(std::uint32_t len) noexcept {
    Plan p;
    p.kind = Kind::Out;
    p.len = len;
    return p;
  }
};

ControlPipe::ControlPipe(const Config& cfg, const std::uint8_t* device, std::size_t device_len, const std::uint8_t* config,
                         std::size_t config_len) noexcept
    : cfg_(cfg), device_(device), device_len_(device_len), config_(config), config_len_(config_len) {}

void ControlPipe::rebind(const Config& cfg, const std::uint8_t* device, std::size_t device_len, const std::uint8_t* config,
                         std::size_t config_len) noexcept {
  cfg_ = cfg;
  device_ = device;
  device_len_ = device_len;
  config_ = config;
  config_len_ = config_len;
  bus_reset();
}

void ControlPipe::bus_reset() noexcept {
  ++counters_.bus_resets;
  state_.store(static_cast<std::uint8_t>(DeviceState::Default), std::memory_order_relaxed);
  suspended_.store(false, std::memory_order_relaxed);
  address_ = 0;
  address_pending_ = false;
  halted_ = 0;
  phase_ = Phase::Idle;
  audio_ = AudioStreamState{};
  cdc_.control.store(0, std::memory_order_relaxed);
}

void ControlPipe::on_suspend() noexcept {
  ++counters_.suspends;
  suspended_.store(true, std::memory_order_relaxed);
}

void ControlPipe::on_resume() noexcept {
  ++counters_.resumes;
  suspended_.store(false, std::memory_order_relaxed);
}

bool ControlPipe::halted(std::uint8_t endpoint_address) const noexcept { return (halted_ & halt_bit(endpoint_address)) != 0u; }

bool ControlPipe::valid_interface(std::uint32_t interface) const noexcept {
  return interface < layout::interface_count(cfg_.mode, cfg_.usb_return);
}

ControlStep ControlPipe::stall(const SetupPacket& s) noexcept {
  ++counters_.stalls;
  counters_.last_stall = s;
  phase_ = Phase::Idle;
  address_pending_ = false;
  ControlStep step;
  step.kind = ControlStep::Kind::Stall;
  return step;
}

// ---- The three events ------------------------------------------------------------------------------------------------

ControlStep ControlPipe::on_setup(const std::uint8_t* raw8) noexcept {
  ++counters_.setups;
  setup_ = parse_setup(raw8);
  phase_ = Phase::Idle;  // a SETUP always ends whatever transfer was in progress (USB 2.0 8.5.3)
  more_ = false;
  address_pending_ = false;

  Plan plan = dispatch(setup_);
  if (plan.kind == Plan::Kind::In && setup_.length == 0u) {
    plan.kind = Plan::Kind::Status;  // nothing was asked for: no data stage
  }
  ControlStep step;
  switch (plan.kind) {
    case Plan::Kind::Stall:
      return stall(setup_);
    case Plan::Kind::Status:
      if (plan.effect == Effect::SetAddress) {
        pending_address_ = plan.arg;  // applied when the status stage has been acknowledged (USB 2.0 9.4.6)
        address_pending_ = true;
      } else {
        step.effect = plan.effect;
        step.arg = plan.arg;
      }
      phase_ = Phase::StatusIn;
      step.kind = ControlStep::Kind::In;
      step.len = 0;
      step.data = small_;
      step.data1 = true;
      return step;
    case Plan::Kind::In:
      reply_ = plan.data;
      reply_len_ = plan.len < setup_.length ? plan.len : setup_.length;
      sent_ = 0;
      toggle_ = true;  // the first packet of a data stage is DATA1
      phase_ = Phase::DataIn;
      return send_chunk();
    case Plan::Kind::Out:
      out_expected_ = plan.len;
      phase_ = Phase::DataOut;
      return step;  // the receive descriptors are armed already: nothing to do until the data arrives
  }
  return stall(setup_);  // not reached: every Kind is handled above
}

ControlStep ControlPipe::send_chunk() noexcept {
  ControlStep s;
  const std::uint32_t remaining = reply_len_ - sent_;
  const std::uint32_t n = remaining > kEp0Size ? kEp0Size : remaining;
  s.kind = ControlStep::Kind::In;
  s.data = n != 0u ? reply_ + sent_ : small_;
  s.len = static_cast<std::uint8_t>(n);
  s.data1 = toggle_;
  toggle_ = !toggle_;
  sent_ += n;
  // Another packet follows if bytes are left, or if the last packet was full and shorter than what the host asked for: the host
  // cannot tell the data stage is over until a short packet arrives (USB 2.0 5.5.3), so a zero-length one closes it.
  if (n == 0u) {
    more_ = false;
  } else if (sent_ < reply_len_) {
    more_ = true;
  } else {
    more_ = n == kEp0Size && reply_len_ < setup_.length;
  }
  return s;
}

ControlStep ControlPipe::on_in_done() noexcept {
  ControlStep s;
  switch (phase_) {
    case Phase::DataIn:
      if (more_) {
        return send_chunk();
      }
      phase_ = Phase::StatusOut;
      break;
    case Phase::StatusIn:
      phase_ = Phase::Idle;
      if (address_pending_) {
        address_pending_ = false;
        address_ = pending_address_;
        state_.store(static_cast<std::uint8_t>(address_ != 0u ? DeviceState::Address : DeviceState::Default),
                     std::memory_order_relaxed);
        s.effect = Effect::SetAddress;
        s.arg = address_;
      }
      break;
    default:
      break;  // a completion nobody is waiting for (a stale one after a new SETUP): ignored
  }
  return s;
}

ControlStep ControlPipe::on_out_done(const std::uint8_t* data, std::uint32_t bytes) noexcept {
  ControlStep s;
  switch (phase_) {
    case Phase::DataOut:
      if (bytes != out_expected_) {
        ++counters_.out_stage_errors;
        return stall(setup_);
      }
      // SET_LINE_CODING is the only request with an OUT data stage (cdc_request).
      cdc_.coding = LineCoding::parse(data);
      ++cdc_.coding_sets;
      phase_ = Phase::StatusIn;
      s.kind = ControlStep::Kind::In;
      s.len = 0;
      s.data = small_;
      s.data1 = true;
      return s;
    case Phase::DataIn:
    case Phase::StatusOut:
      phase_ = Phase::Idle;  // the status stage, or the host ending the data stage early
      break;
    default:
      break;
  }
  return s;
}

// ---- Requests ----------------------------------------------------------------------------------------------------------

ControlPipe::Plan ControlPipe::dispatch(const SetupPacket& s) noexcept {
  switch (s.type()) {
    case kTypeStandard:
      return standard(s);
    case kTypeClass:
      return class_request(s);
    default:
      return Plan{};  // vendor and reserved requests
  }
}

ControlPipe::Plan ControlPipe::standard(const SetupPacket& s) noexcept {
  switch (s.request) {
    case kGetStatus:
      return status_request(s);
    case kClearFeature:
      return feature_request(s, false);
    case kSetFeature:
      return feature_request(s, true);
    case kSetAddress:
      // Valid in the Default and Address states only; the address is 7 bits.
      if (s.recipient() != kRecipientDevice || s.device_to_host() || s.index != 0u || s.length != 0u || s.value > 127u ||
          state() == DeviceState::Configured) {
        return Plan{};
      }
      return Plan::status(Effect::SetAddress, static_cast<std::uint8_t>(s.value));
    case kGetDescriptor:
      return get_descriptor(s);
    case kGetConfiguration:
      if (s.recipient() != kRecipientDevice || !s.device_to_host() || s.value != 0u || s.index != 0u || s.length != 1u ||
          state() == DeviceState::Default) {
        return Plan{};
      }
      small_[0] = configuration();
      return Plan::in(small_, 1);
    case kSetConfiguration:
      return set_configuration(s);
    case kGetInterface:
      return interface_request(s, false);
    case kSetInterface:
      return interface_request(s, true);
    default:
      return Plan{};  // SET_DESCRIPTOR, SYNCH_FRAME and anything unknown
  }
}

ControlPipe::Plan ControlPipe::get_descriptor(const SetupPacket& s) noexcept {
  if (s.recipient() != kRecipientDevice || !s.device_to_host()) {
    return Plan{};
  }
  const std::uint8_t type = static_cast<std::uint8_t>(s.value >> 8);
  const std::uint8_t index = static_cast<std::uint8_t>(s.value & 0xFFu);
  switch (type) {
    case kDescDevice:
      return index == 0u && s.index == 0u ? Plan::in(device_, static_cast<std::uint32_t>(device_len_)) : Plan{};
    case kDescConfig:
      return index == 0u && s.index == 0u ? Plan::in(config_, static_cast<std::uint32_t>(config_len_)) : Plan{};
    case kDescString: {
      // wIndex is the language: 0x0409, the one the LANGID descriptor lists; some hosts send 0 for the LANGID descriptor itself.
      if (s.index != kLangIdEnglishUs && s.index != 0u) {
        return Plan{};
      }
      const std::size_t n = build_string_descriptor(cfg_, index, string_, sizeof string_);
      return n != 0u ? Plan::in(string_, static_cast<std::uint32_t>(n)) : Plan{};
    }
    default:
      return Plan{};  // device qualifier, other-speed configuration, BOS, class descriptors: not for a full-speed-only device
  }
}

ControlPipe::Plan ControlPipe::status_request(const SetupPacket& s) noexcept {
  if (!s.device_to_host() || s.value != 0u) {
    return Plan{};
  }
  small_[0] = 0;
  small_[1] = 0;
  switch (s.recipient()) {
    case kRecipientDevice:  // bus powered, no remote wakeup: both bits clear
      return s.index == 0u ? Plan::in(small_, 2) : Plan{};
    case kRecipientInterface:
      return state() == DeviceState::Configured && (s.index >> 8) == 0u && valid_interface(s.index & 0xFFu) ? Plan::in(small_, 2)
                                                                                                           : Plan{};
    case kRecipientEndpoint: {
      const std::uint8_t ep = static_cast<std::uint8_t>(s.index & 0xFFu);
      if ((s.index >> 8) != 0u) {
        return Plan{};
      }
      const bool control = ep == 0x00u || ep == 0x80u;
      const bool known = endpoint_kind(cfg_.mode, ep, cfg_.usb_return) != EndpointKind::None;
      if (!(control ? state() != DeviceState::Default : (state() == DeviceState::Configured && known))) {
        return Plan{};
      }
      small_[0] = halted(ep) ? std::uint8_t{1} : std::uint8_t{0};
      return Plan::in(small_, 2);
    }
    default:
      return Plan{};
  }
}

ControlPipe::Plan ControlPipe::feature_request(const SetupPacket& s, bool set) noexcept {
  // Only ENDPOINT_HALT on a bulk or interrupt endpoint of a configured device. DEVICE_REMOTE_WAKEUP and TEST_MODE are not
  // supported (bmAttributes advertises no remote wakeup; test modes are high speed), and no feature exists for interfaces.
  if (s.device_to_host() || s.length != 0u || s.recipient() != kRecipientEndpoint || s.value != kFeatureEndpointHalt ||
      (s.index >> 8) != 0u || state() != DeviceState::Configured) {
    return Plan{};
  }
  const std::uint8_t ep = static_cast<std::uint8_t>(s.index & 0xFFu);
  const EndpointKind kind = endpoint_kind(cfg_.mode, ep, cfg_.usb_return);
  if (kind != EndpointKind::Bulk && kind != EndpointKind::Interrupt) {
    return Plan{};
  }
  if (set) {
    halted_ |= halt_bit(ep);
    return Plan::status(Effect::Halt, ep);
  }
  halted_ &= ~halt_bit(ep);
  return Plan::status(Effect::ClearHalt, ep);  // also resets the toggle, whether or not the endpoint was halted (USB 2.0 9.4.5)
}

ControlPipe::Plan ControlPipe::set_configuration(const SetupPacket& s) noexcept {
  if (s.recipient() != kRecipientDevice || s.device_to_host() || s.index != 0u || s.length != 0u || (s.value >> 8) != 0u ||
      s.value > 1u || state() == DeviceState::Default) {
    return Plan{};
  }
  ++counters_.configurations;
  const std::uint8_t value = static_cast<std::uint8_t>(s.value);
  // A configuration event: every endpoint goes back to its defaults (halts cleared, toggles DATA0, alternate settings 0).
  halted_ = 0;
  audio_ = AudioStreamState{};
  state_.store(static_cast<std::uint8_t>(value != 0u ? DeviceState::Configured : DeviceState::Address), std::memory_order_relaxed);
  return Plan::status(Effect::Configure, value);
}

ControlPipe::Plan ControlPipe::interface_request(const SetupPacket& s, bool set) noexcept {
  if (s.recipient() != kRecipientInterface || (s.index >> 8) != 0u || state() != DeviceState::Configured ||
      !valid_interface(s.index & 0xFFu) || s.device_to_host() == set) {
    return Plan{};
  }
  const std::uint8_t interface = static_cast<std::uint8_t>(s.index & 0xFFu);
  const bool audio_stream = cfg_.mode == Mode::Audio &&
                            (interface == layout::kIfAudioIn || (interface == layout::kIfAudioOut && cfg_.usb_return));
  if (!set) {
    if (s.value != 0u) {
      return Plan{};
    }
    std::uint8_t alt = 0;
    if (audio_stream) {
      get_audio_alt(audio_, interface, alt);
    }
    small_[0] = alt;
    return Plan::in(small_, 1);
  }
  if (s.length != 0u || (s.value >> 8) != 0u) {
    return Plan{};
  }
  const std::uint8_t alt = static_cast<std::uint8_t>(s.value);
  if (audio_stream) {
    return set_audio_alt(audio_, interface, alt) ? Plan::status(Effect::SetInterface, interface) : Plan{};
  }
  return alt == 0u ? Plan::status() : Plan{};  // every other interface has its one setting
}

ControlPipe::Plan ControlPipe::class_request(const SetupPacket& s) noexcept {
  if (s.recipient() != kRecipientInterface || state() != DeviceState::Configured) {
    return Plan{};
  }
  // wIndex: the interface in the low byte; the UAC2 clock requests carry the entity in the high byte (uac2.h).
  const std::uint8_t interface = static_cast<std::uint8_t>(s.index & 0xFFu);
  if (interface == layout::kIfCdcComm) {
    return (s.index >> 8) == 0u ? cdc_request(s) : Plan{};
  }
  if (cfg_.mode == Mode::Audio && interface == layout::kIfAudioControl) {
    audio_reply_ = handle_audio_control_request(s);
    if (audio_reply_.status != ControlStatus::Data) {
      return Plan{};
    }
    return Plan::in(audio_reply_.data, audio_reply_.length);
  }
  if (cfg_.mode == Mode::Disk && interface == layout::kIfMsc) {
    return (s.index >> 8) == 0u ? msc_request(s) : Plan{};
  }
  return Plan{};  // MIDI streaming has no class requests; anything else is not ours
}

// DISK mode, the Mass Storage interface (Bulk-Only Transport 3.1, 3.2): GET MAX LUN answers one byte, 0 (one LUN); the Bulk-Only Mass Storage Reset has no data stage.
ControlPipe::Plan ControlPipe::msc_request(const SetupPacket& s) noexcept {
  if (s.value != 0u) {
    return Plan{};
  }
  switch (s.request) {
    case kMscGetMaxLun:
      if (!s.device_to_host() || s.length != 1u) {
        return Plan{};
      }
      small_[0] = 0;
      return Plan::in(small_, 1);
    case kMscReset:
      return !s.device_to_host() && s.length == 0u ? Plan::status(Effect::MscReset) : Plan{};
    default:
      return Plan{};
  }
}

ControlPipe::Plan ControlPipe::cdc_request(const SetupPacket& s) noexcept {
  switch (s.request) {
    case kCdcSetLineCoding:
      return !s.device_to_host() && s.length == kLineCodingBytes ? Plan::out(kLineCodingBytes) : Plan{};
    case kCdcGetLineCoding:
      if (!s.device_to_host()) {
        return Plan{};
      }
      cdc_.coding.serialize(line_);
      return Plan::in(line_, kLineCodingBytes);
    case kCdcSetControlLineState:
      if (s.device_to_host() || s.length != 0u) {
        return Plan{};
      }
      cdc_.control.store(static_cast<std::uint8_t>(s.value & (kControlLineDtr | kControlLineRts)), std::memory_order_relaxed);
      ++cdc_.control_sets;
      return Plan::status();
    default:
      return Plan{};  // SEND_BREAK and the rest: not advertised in the ACM functional descriptor
  }
}

}  // namespace o3sdk::usb
