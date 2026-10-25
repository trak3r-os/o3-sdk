// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/cdc.h>
#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/endpoint.h>
#include <o3sdk/usb/uac2.h>
#include <o3sdk/usb/usb_types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace o3sdk::usb {

// The control endpoint (endpoint 0) and the device's standard state, as a pure state machine (OS-006b). A driver reports what the
// controller did (a SETUP arrived, an IN packet was acknowledged, an OUT packet arrived) and gets back what to do on endpoint 0
// next: send this packet with this DATA PID, stall, or nothing; plus at most one side effect for the driver to carry out first
// (the address to switch to, a configuration to apply, a halt to set or clear). No bus, no registers and no heap: the tests
// drive it with hand-written SETUP packets and the host build runs the whole driver against a fake controller.
//
// Contexts. Every method that changes state runs in one context, the USB interrupt. The few fields the main loop reads
// (state(), the CDC control lines) are atomics read with relaxed loads.
//
// Supported (USB 2.0 chapter 9, CDC 1.2 PSTN, UAC2): GET_STATUS (device, interface, endpoint), CLEAR_FEATURE and SET_FEATURE of
// ENDPOINT_HALT on the bulk and interrupt endpoints, SET_ADDRESS (applied after its status stage), GET_DESCRIPTOR (device,
// configuration 0, strings), GET_CONFIGURATION, SET_CONFIGURATION (0 or 1), GET_INTERFACE, SET_INTERFACE, the three CDC ACM
// requests, and (AUDIO mode) the UAC2 clock-source requests and the AudioStreaming alternate settings. Everything else
// stalls: SET_DESCRIPTOR, SYNCH_FRAME, DEVICE_REMOTE_WAKEUP and TEST_MODE (not advertised), the device-qualifier and
// other-speed descriptors (a full-speed-only device answers those with a request error, USB 2.0 9.2.6.6), vendor requests,
// requests invalid in the device's state, malformed requests (a wrong wLength, a wrong direction, a bad recipient).
// An OUT data stage is limited to one packet (64 bytes): the only one this device accepts is SET_LINE_CODING's 7 bytes.

inline constexpr std::uint32_t kEp0Size = 64;

// USB 2.0 9.1.1 device states. Suspended is a flag beside them, not a state of its own: a suspended device keeps its state.
enum class DeviceState : std::uint8_t { Default, Address, Configured };

// What the driver must do before it acts on ControlStep::kind.
enum class Effect : std::uint8_t {
  None,
  SetAddress,    // write `arg` (the new device address, 0..127) to the address register; sent after the status stage completes
  Configure,     // configuration `arg` is selected (0 = none): (re)configure every endpoint of the mode, or disable them for 0
  SetInterface,  // the alternate setting of interface `arg` changed (AUDIO mode: the AudioStreaming interfaces)
  Halt,          // endpoint address `arg` is halted: set its EPSTALL
  ClearHalt,     // endpoint address `arg`: halt cleared, data toggle back to DATA0, descriptors re-armed
  MscReset,      // DISK mode: Bulk-Only Mass Storage Reset (class request 0xFF on the MSC interface): the engine drops the command in flight
};

struct ControlStep {
  enum class Kind : std::uint8_t {
    None,   // nothing to arm (the receive descriptors of endpoint 0 are always armed)
    In,     // arm endpoint 0 transmit with `data`/`len` and DATA PID `data1`; len 0 is a zero-length packet
    Stall,  // stall endpoint 0 (the request is not supported or not valid now)
  };
  Kind kind = Kind::None;
  bool data1 = true;
  std::uint8_t len = 0;                   // In: 0..kEp0Size
  const std::uint8_t* data = nullptr;     // In: valid until the next SETUP
  Effect effect = Effect::None;
  std::uint8_t arg = 0;
};

struct ControlCounters {
  std::uint32_t setups = 0;
  std::uint32_t stalls = 0;           // requests answered with a stall
  std::uint32_t bus_resets = 0;
  std::uint32_t suspends = 0;
  std::uint32_t resumes = 0;
  std::uint32_t configurations = 0;   // SET_CONFIGURATION requests accepted
  std::uint32_t out_stage_errors = 0; // an OUT data stage of the wrong length
  SetupPacket last_stall{};           // the most recent request that was stalled: what a new host asked that we did not expect
};

class ControlPipe {
 public:
  // `device` and `config` are the built descriptors (descriptors.h), which must outlive the pipe; strings are built on demand
  // from `cfg` (its `serial` pointer must outlive the pipe too).
  ControlPipe(const Config& cfg, const std::uint8_t* device, std::size_t device_len, const std::uint8_t* config,
              std::size_t config_len) noexcept;

  // Presents another descriptor set (a mode or profile change, OS-006d): the new Config and the descriptors built from it. Only with the
  // controller detached (the D+ pull-up off and its interrupt masked: the caller guarantees nothing else runs); ends like a bus reset.
  void rebind(const Config& cfg, const std::uint8_t* device, std::size_t device_len, const std::uint8_t* config,
              std::size_t config_len) noexcept;

  // A bus reset: back to the Default state, address 0, not configured, nothing halted, no transfer in progress, CDC control
  // lines low (a terminal that was open is gone).
  void bus_reset() noexcept;
  void on_suspend() noexcept;
  void on_resume() noexcept;

  // The three events of endpoint 0. See the file comment for what the returned step means.
  ControlStep on_setup(const std::uint8_t* raw8) noexcept;
  ControlStep on_in_done() noexcept;
  ControlStep on_out_done(const std::uint8_t* data, std::uint32_t bytes) noexcept;

  DeviceState state() const noexcept { return static_cast<DeviceState>(state_.load(std::memory_order_relaxed)); }
  bool configured() const noexcept { return state() == DeviceState::Configured; }
  std::uint8_t address() const noexcept { return address_; }
  std::uint8_t configuration() const noexcept { return state() == DeviceState::Configured ? 1u : 0u; }
  bool suspended() const noexcept { return suspended_.load(std::memory_order_relaxed); }
  // True for an endpoint address (bit 7 = IN) whose number ENDPOINT_HALT was set on and not cleared. The K66 has one stall bit per
  // endpoint NUMBER, so halting one direction halts the other and GET_STATUS reports both the same.
  bool halted(std::uint8_t endpoint_address) const noexcept;
  // The driver halted an endpoint on its own account (the MSC engine's stall, applied by the interrupt): GET_STATUS agrees. USB interrupt only, like every mutator; a CLEAR_FEATURE
  // or a configuration event clears it as for a host's halt.
  void mark_halt(std::uint8_t endpoint_address) noexcept { halted_ |= halt_bit(endpoint_address); }
  const Config& config() const noexcept { return cfg_; }
  const CdcLineState& cdc() const noexcept { return cdc_; }
  const AudioStreamState& audio() const noexcept { return audio_; }
  const ControlCounters& counters() const noexcept { return counters_; }

 private:
  enum class Phase : std::uint8_t {
    Idle,
    DataIn,     // sending the data stage
    DataOut,    // waiting for the one OUT data packet
    StatusIn,   // the zero-length status packet is armed
    StatusOut,  // the data stage is done; the host's status OUT is next
  };

  struct Plan;
  Plan dispatch(const SetupPacket& s) noexcept;
  Plan standard(const SetupPacket& s) noexcept;
  Plan class_request(const SetupPacket& s) noexcept;
  Plan get_descriptor(const SetupPacket& s) noexcept;
  Plan status_request(const SetupPacket& s) noexcept;
  Plan feature_request(const SetupPacket& s, bool set) noexcept;
  Plan set_configuration(const SetupPacket& s) noexcept;
  Plan interface_request(const SetupPacket& s, bool set) noexcept;
  Plan cdc_request(const SetupPacket& s) noexcept;
  Plan msc_request(const SetupPacket& s) noexcept;
  ControlStep send_chunk() noexcept;
  ControlStep stall(const SetupPacket& s) noexcept;
  bool valid_interface(std::uint32_t interface) const noexcept;
  static constexpr std::uint32_t halt_bit(std::uint8_t endpoint_address) noexcept { return 1u << (endpoint_address & 0x0Fu); }

  Config cfg_;
  const std::uint8_t* device_;
  std::size_t device_len_;
  const std::uint8_t* config_;
  std::size_t config_len_;

  std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(DeviceState::Default)};
  std::atomic<bool> suspended_{false};
  std::uint8_t address_ = 0;
  std::uint8_t pending_address_ = 0;
  bool address_pending_ = false;
  std::uint32_t halted_ = 0;  // bit n = endpoint number n, either direction: the chip has one EPSTALL per number (endpoint.h)

  Phase phase_ = Phase::Idle;
  SetupPacket setup_{};
  const std::uint8_t* reply_ = nullptr;
  std::uint32_t reply_len_ = 0;   // bytes of the data stage (already cut to wLength)
  std::uint32_t sent_ = 0;
  bool more_ = false;             // another packet (possibly the closing zero-length one) follows the one just sent
  bool toggle_ = true;            // DATA PID of the next packet of the data stage
  std::uint32_t out_expected_ = 0;

  CdcLineState cdc_;
  AudioStreamState audio_;
  ControlCounters counters_;
  // The buffers the control endpoint's DMA reads a reply from: 4-byte aligned by declaration (ADR 0011 lesson 1), as every packet buffer is.
  alignas(4) ControlReply audio_reply_;                          // the UAC2 reply the data stage points into
  alignas(4) std::uint8_t small_[8] = {};                        // GET_STATUS, GET_CONFIGURATION, GET_INTERFACE replies
  alignas(4) std::uint8_t line_[8] = {};                         // GET_LINE_CODING reply (7 bytes)
  alignas(4) std::uint8_t string_[kMaxStringDescriptorSize] = {};  // the string descriptor being sent
};
static_assert(kLineCodingBytes <= 8, "the line coding reply fits its buffer");
static_assert(alignof(ControlPipe) >= 4, "the control pipe's reply buffers are word aligned");

}  // namespace o3sdk::usb
