// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/usb_types.h>

#include <cstddef>
#include <cstdint>

// What the USB device controller's endpoint hardware needs from software, as pure data and arithmetic (OS-006b): the buffer
// descriptor word, the per-direction ping-pong and data-toggle bookkeeping, and the endpoint table of a mode. The register
// layer (platform/mk66/usb) only moves these values in and out of the chip, so all of the interesting rules are host and
// QEMU tested.
namespace o3sdk::usb {

// ---- Buffer descriptors (K66 Sub-Family Reference Manual, USB OTG FS: "Buffer Descriptor Table") --------------------------
// The controller reads and writes a table of 8-byte descriptors in RAM: a control/status word and a buffer address. There
// are four per endpoint, in the order (receive, even), (receive, odd), (transmit, even), (transmit, odd); the descriptor of
// endpoint e, direction d (0 = receive/OUT, 1 = transmit/IN), parity p is entry (e << 2) | (d << 1) | p, which is also what
// the STAT register reports for the token the controller just finished (STAT >> 2). The controller serves the even and the
// odd descriptor of one direction of one endpoint alternately, starting with even after a reset (CTL[ODDRST]).
//
// Word 0, written by software to give a descriptor to the controller (OWN = 1):
//   bits 25..16 byte count (transmit: bytes to send; receive: room in the buffer), bit 7 OWN, bit 6 DATA0/DATA1 (transmit:
//   the PID sent; receive: the PID expected when DTS is set), bit 3 DTS (data toggle synchronisation on), bit 2 STALL.
// When the controller finishes the token it clears OWN, writes the byte count actually moved and, in bits 5..2, the PID of
// the token (TOK_PID: 0x1 OUT, 0x9 IN, 0xD SETUP). Software must not touch a descriptor while OWN is set.
namespace bd {
inline constexpr std::uint32_t kOwn = 0x80u;
inline constexpr std::uint32_t kData1 = 0x40u;
inline constexpr std::uint32_t kDts = 0x08u;
inline constexpr std::uint32_t kStall = 0x04u;
inline constexpr std::uint32_t kPidOut = 0x1u;
inline constexpr std::uint32_t kPidIn = 0x9u;
inline constexpr std::uint32_t kPidSetup = 0xDu;

// A descriptor handed to the controller: `bytes` to send or to receive at most, data toggle checked/sent as `data1`.
constexpr std::uint32_t arm(std::uint32_t bytes, bool data1) noexcept {
  return (bytes << 16) | kOwn | kDts | (data1 ? kData1 : 0u);
}
// An isochronous descriptor: no handshake and no data toggle (DTS off, DATA0 sent), so only the count and OWN. K66 RM, buffer descriptor format;
// an isochronous endpoint has EPHSHK clear in ENDPTn too (endpoint_direction_bits). hw-session item: the PID on the bus is DATA0.
constexpr std::uint32_t arm_iso(std::uint32_t bytes) noexcept { return (bytes << 16) | kOwn; }
constexpr bool owned(std::uint32_t desc) noexcept { return (desc & kOwn) != 0u; }
constexpr std::uint32_t pid(std::uint32_t desc) noexcept { return (desc >> 2) & 0xFu; }
constexpr std::uint32_t bytes(std::uint32_t desc) noexcept { return (desc >> 16) & 0x3FFu; }
}  // namespace bd

// One descriptor. On the MK66 `addr` is the 32-bit address of the packet buffer, which is what makes the entry 8 bytes (a
// static_assert below, and a QEMU test, hold it to that). On a 64-bit host `std::uintptr_t` is 8 bytes wide so a pointer
// fits and the host tests can run the same code against a fake controller; the indexing is by entry, not by byte offset,
// so the layout difference never matters there. Both words are volatile: the controller is a second bus master.
struct BdtEntry {
  volatile std::uint32_t desc;
  volatile std::uintptr_t addr;
};
static_assert(sizeof(void*) != 4 || sizeof(BdtEntry) == 8, "a buffer descriptor is two 32-bit words on the MK66");

// Endpoint numbers 0 (control) .. kBdtEndpoints-1 have descriptors. AUDIO mode uses numbers up to 5, STANDARD up to 3; the
// table is sized for the larger so the driver does not change shape with the mode.
inline constexpr std::uint32_t kBdtEndpoints = 6;
inline constexpr std::uint32_t kBdtEntries = kBdtEndpoints * 4u;
// The controller needs the table on a 512-byte boundary (BDT_PAGE1..3 hold address bits 31..9). Declared, never placed (ADR 0011
// lesson 1): the platform also checks the address before it pulls the line up.
inline constexpr std::size_t kBdtAlign = 512;
struct alignas(kBdtAlign) BdtTable {
  BdtEntry entry[kBdtEntries];
};
static_assert(alignof(BdtTable) == kBdtAlign, "the BDT is 512-byte aligned by type");

constexpr std::uint32_t bdt_index(std::uint32_t endpoint, bool transmit, bool odd) noexcept {
  return (endpoint << 2) | (transmit ? 2u : 0u) | (odd ? 1u : 0u);
}

// ---- Ping-pong and data-toggle bookkeeping -----------------------------------------------------------------------------
// One direction of one endpoint has two descriptors that the controller serves strictly alternately, and a data toggle that
// flips after every packet the host acknowledges. Software has to hand descriptors over in the same order and with the
// right toggle, and to know how many are still with the controller. That is all this class keeps.
//
//   arm()       the next descriptor to fill (its parity) and the DATA PID to put in it; precondition can_arm()
//   complete(p) the controller finished the descriptor of parity p (from STAT), in order
//   flush()     every descriptor was taken back by software (a configuration event, ClearFeature(ENDPOINT_HALT)): nothing is armed,
//               the next arm() goes to the descriptor the controller serves next and starts at DATA0 (USB 2.0 9.1.1.5, 9.4.5)
//   reset()     the controller's own pointers went back to even (CTL[ODDRST] at a bus reset): same, with even next
// All of it runs in the one context that owns the endpoint (the USB interrupt), so nothing here is atomic.
class PingPong {
 public:
  struct Slot {
    bool odd;
    bool data1;
  };

  void reset() noexcept {
    done_odd_ = false;
    flush();
  }
  void flush() noexcept {
    armed_ = 0;
    arm_odd_ = done_odd_;
    data1_ = false;
  }

  bool can_arm() const noexcept { return armed_ < 2u; }
  unsigned armed() const noexcept { return armed_; }
  bool next_done_odd() const noexcept { return done_odd_; }

  Slot arm() noexcept {
    const Slot s{arm_odd_, data1_};
    arm_odd_ = !arm_odd_;
    data1_ = !data1_;
    ++armed_;
    return s;
  }

  // Returns true when `odd` was the descriptor expected next. Either way the bookkeeping resynchronises to the controller's
  // report (the descriptor after the one that finished is the one served next), so a lost count cannot persist; a false
  // return is a sign of a bug or of a descriptor taken back while a token was queued.
  bool complete(bool odd) noexcept {
    const bool expected = armed_ != 0u && odd == done_odd_;
    done_odd_ = !odd;
    if (armed_ != 0u) {
      --armed_;
    }
    arm_odd_ = done_odd_ != ((armed_ & 1u) != 0u);  // the parity after the ones still armed
    return expected;
  }

 private:
  bool done_odd_ = false;  // parity of the descriptor the controller serves next
  bool arm_odd_ = false;   // parity of the descriptor arm() fills next
  bool data1_ = false;     // DATA PID of the next armed packet
  unsigned armed_ = 0;     // 0..2 descriptors owned by the controller
};

// ---- The endpoint table of a mode --------------------------------------------------------------------------------------

// ENDPTn register bits (RM "USB OTG FS: Endpoint Control registers"): EPHSHK handshake (every non-isochronous endpoint),
// EPSTALL, EPTXEN / EPRXEN direction enables, EPCTLDIS "no SETUP tokens here" (every endpoint but 0).
inline constexpr std::uint8_t kEndptHandshake = 0x01;
inline constexpr std::uint8_t kEndptStall = 0x02;
inline constexpr std::uint8_t kEndptTxEn = 0x04;
inline constexpr std::uint8_t kEndptRxEn = 0x08;
inline constexpr std::uint8_t kEndptCtlDisable = 0x10;
inline constexpr std::uint8_t kEndpt0 = kEndptRxEn | kEndptTxEn | kEndptHandshake;

// What the endpoint with address `address` (bit 7 = IN) is in `mode`, from the numbers in usb_types.h layout::. `usb_return` (Config::usb_return,
// AUDIO only; no default, so that no caller can forget it) is false when the descriptors carry no return stream: then the OUT data endpoint and
// the feedback endpoint do not exist. STANDARD ignores it.
enum class EndpointKind : std::uint8_t { None, Control, Bulk, Interrupt, Isochronous };
constexpr EndpointKind endpoint_kind(Mode mode, std::uint8_t address, bool usb_return) noexcept {
  switch (address) {
    case 0x00:
    case 0x80:
      return EndpointKind::Control;
    case layout::kEpCdcNotify:
      return EndpointKind::Interrupt;
    case layout::kEpCdcOut:
    case layout::kEpCdcIn:
    case layout::kEpMidiOut:
    case layout::kEpMidiIn:
      return EndpointKind::Bulk;
    case layout::kEpAudioIn:
      return mode == Mode::Audio ? EndpointKind::Isochronous : EndpointKind::None;
    case layout::kEpAudioOut:
    case layout::kEpAudioFeedback:
      // 0x04 / 0x85 are the AUDIO return stream and, in DISK mode, the Mass Storage bulk pair (layout::kEpMscOut / kEpMscIn).
      return mode == Mode::Disk ? EndpointKind::Bulk : (mode == Mode::Audio && usb_return ? EndpointKind::Isochronous : EndpointKind::None);
    default:
      return EndpointKind::None;
  }
}

constexpr std::uint8_t kLastEndpointStandard = 3;
constexpr std::uint8_t kLastEndpointAudio = 5;
constexpr std::uint8_t kLastEndpointAudioNoReturn = 4;  // the feedback endpoint (number 5) is the return stream's
constexpr std::uint8_t kLastEndpointDisk = 5;           // the Mass Storage IN endpoint
constexpr std::uint8_t last_endpoint(Mode mode, bool usb_return) noexcept {
  return mode == Mode::Audio ? (usb_return ? kLastEndpointAudio : kLastEndpointAudioNoReturn) : (mode == Mode::Disk ? kLastEndpointDisk : kLastEndpointStandard);
}
static_assert(kLastEndpointAudio < kBdtEndpoints, "the BDT covers every endpoint number of both modes");

// The ENDPTn bits one direction of endpoint number `number` contributes: its enable bit, plus the handshake bit unless the
// endpoint is isochronous (no handshake, no data toggle). 0 when that direction does not exist in `mode`.
constexpr std::uint8_t endpoint_direction_bits(Mode mode, std::uint8_t number, bool in, bool usb_return) noexcept {
  const EndpointKind kind = endpoint_kind(mode, static_cast<std::uint8_t>(number | (in ? 0x80u : 0x00u)), usb_return);
  if (kind == EndpointKind::None || kind == EndpointKind::Control) {
    return 0;
  }
  const std::uint8_t enable = in ? kEndptTxEn : kEndptRxEn;
  return kind == EndpointKind::Isochronous ? enable : static_cast<std::uint8_t>(enable | kEndptHandshake);
}

// ENDPTn value of endpoint number `number` (1..) in `mode` once configured: the directions that exist (both directions of one
// number share the register, as the K66 has one ENDPTn per number), EPCTLDIS everywhere. 0 for a number the mode does not use.
// A number with a bulk direction and an isochronous one would need a per-direction handshake bit the chip does not have; the
// layout never does that (a test walks the descriptors of every mode against this table).
constexpr std::uint8_t endpoint_config(Mode mode, std::uint8_t number, bool usb_return) noexcept {
  const std::uint8_t out = endpoint_direction_bits(mode, number, false, usb_return);
  const std::uint8_t in = endpoint_direction_bits(mode, number, true, usb_return);
  return (out | in) == 0u ? std::uint8_t{0} : static_cast<std::uint8_t>(out | in | kEndptCtlDisable);
}

}  // namespace o3sdk::usb
