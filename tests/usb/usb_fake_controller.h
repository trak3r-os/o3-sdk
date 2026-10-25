// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b test double: a model of the K66's USB device controller (the serial interface engine behind ISTAT / STAT / the buffer
// descriptor table / ENDPTn) and of a host that talks to it. FakeController is the register policy `UsbDevice` is written against
// (usb/device.h); Host drives bus transactions at it the way a real host's tokens would arrive, and the controller answers like
// the hardware: NAK while a descriptor is not owned by the controller, DATA toggle checking when DTS is set, ping-pong between the
// even and the odd descriptor, a 4-deep STAT FIFO, the freeze after a SETUP until CTL is written, STALL when an endpoint is
// halted or a transmit descriptor carries OWN|STALL (answered without being consumed). After every transaction that completes a descriptor it calls the interrupt (a function pointer, so this runs on QEMU
// too: no heap, no std containers).
//
// What it deliberately does not model: bus timing, CRC, retries after a NAK (the test decides whether to retry) and the clock.
#pragma once

#include <o3sdk/usb/device.h>
#include <o3sdk/usb/endpoint.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usb_fake {

using namespace o3sdk::usb;

enum class Reply : std::uint8_t { Ack, Nak, Stall, Ignored };

class FakeController {
 public:
  explicit FakeController(BdtTable& bdt) noexcept : bdt_(bdt) {}

  // ---- the register policy (usb/device.h) ----
  std::uint8_t istat() {
    if (token_at_stall_ && (istat_ & sie::kIstatStall) != 0u && fifo_n_ < kFifoDepth && ++stall_flag_reads_ == 2u) {
      // model_token_at_stall_seen: the interrupt's own read of the raised STALL flag is the first look, the stall handler's is the second; a token finishes right then.
      token_at_stall_ = false;
      fifo_[fifo_n_++] = 0xF0;  // endpoint 15: a finished token no endpoint here owns (counted stray, nothing else happens)
      istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatTokDne);
    }
    return istat_;
  }
  void istat_clear(std::uint8_t bits) {
    istat_ = static_cast<std::uint8_t>(istat_ & ~bits);
    if ((bits & sie::kIstatTokDne) != 0u && fifo_n_ != 0u) {  // clearing TOKDNE pops the FIFO
      for (unsigned i = 1; i < fifo_n_; ++i) {
        fifo_[i - 1u] = fifo_[i];
      }
      --fifo_n_;
      if (fifo_n_ != 0u) {
        istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatTokDne);
      }
    }
  }
  std::uint8_t stat() { return fifo_n_ != 0u ? fifo_[0] : std::uint8_t{0}; }
  std::uint8_t errstat() { return errstat_; }
  void errstat_clear(std::uint8_t bits) { errstat_ = static_cast<std::uint8_t>(errstat_ & ~bits); }
  void ctl(std::uint8_t value) {
    ctl_ = value;
    ++ctl_writes_;
    if ((value & sie::kCtlOddRst) != 0u) {
      for (unsigned n = 0; n < 16u; ++n) {
        rx_odd_[n] = false;
        tx_odd_[n] = false;
      }
    }
    frozen_ = false;  // any write of CTL clears TXSUSPENDTOKENBUSY here (the driver writes it alone)
    if (late_stall_freeze_ && held_in_) {
      // The hypothesis of OS-USBDISK int.13 (a model, not a measurement): an IN token that reached the controller while it was frozen is answered when the freeze ends; with EPSTALL set that is a
      // STALL handshake (no TOKDNE), and the controller leaves TXSUSPENDTOKENBUSY set again, deaf to every token until the next CTL write.
      held_in_ = false;
      if ((endpt_[0] & kEndptStall) != 0u) {
        istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatStall);
        frozen_ = true;
      }
    }
  }
  // A token that finishes while the stall handler runs (its TOKDNE is pending when the handler looks): one-shot.
  void model_token_at_stall_seen() {
    token_at_stall_ = true;
    stall_flag_reads_ = 0;
  }
  // A STALL handshake of some endpoint, as the ISTAT flag shows it.
  void raise_stall_flag() { istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatStall); after(); }
  unsigned ctl_writes() const { return ctl_writes_; }
  // The readback of CTL: what was written, with TXSUSPENDTOKENBUSY (0x20) while the model is frozen.
  std::uint8_t ctl() { return static_cast<std::uint8_t>(ctl_ | (frozen_ ? sie::kCtlTxSuspendTokenBusy : 0u)); }
  // Switches the model above on: an IN token to endpoint 0 during the freeze is held (no answer), and a SETUP during the freeze gets none either.
  void model_late_stall_freeze(bool on) {
    late_stall_freeze_ = on;
    held_in_ = false;
  }
  // The pessimistic reading of the reference manual (nobody could confirm it): while ENDPT0[EPSTALL] is set a SETUP token gets STALL too and is not taken. The driver must not depend on the
  // controller accepting a SETUP with EPSTALL set; the enumeration tests run with this switch on and off.
  void model_epstall_blocks_setup(bool on) { epstall_blocks_setup_ = on; }
  // The other reading of a BD-level stall (open hardware question, OS-USBDISK int.15 section 2; a model, not a measurement): a STALL handshake answered by an OWN|STALL transmit descriptor advances the transmit
  // ping-pong pointer although the descriptor is not consumed. The EPSTALL handshake (the first of a stall) does not touch it. Off by default: the controller then keeps its pointer, as the driver assumes.
  void model_bd_stall_toggles_pointer(bool on) { bd_stall_toggles_ = on; }
  void set_address(std::uint8_t address) { address_ = address; }
  void set_inten(std::uint8_t value) { inten_ = value; }
  void set_erren(std::uint8_t value) { erren_ = value; }
  std::uint8_t endpt(unsigned number) { return endpt_[number]; }
  void endpt(unsigned number, std::uint8_t value) { endpt_[number] = value; }
  void dmb() {}
  // A fake cost of 40 cycles per call: nothing here measures real time. With a SOF period set (set_sof_period) the counter instead stands still between
  // start-of-frame events and jumps by the period at each one, so the rate estimator (usb/rate.h) sees exact SOF stamps.
  std::uint32_t cycles() { return sof_period_ != 0u ? cycles_ : (cycles_ += 40); }
  void set_sof_period(std::uint32_t cycles_per_frame) { sof_period_ = cycles_per_frame; }
  std::uint32_t frame() { return frame_ & 0x7FFu; }  // FRMNUML/FRMNUMH: the 11-bit frame number, one more per sof()

  // ---- inspection ----
  std::uint8_t address() const { return address_; }
  std::uint8_t inten() const { return inten_; }
  bool frozen() const { return frozen_; }
  unsigned fifo_depth() const { return fifo_n_; }
  std::uint32_t toggle_mismatches() const { return toggle_mismatches_; }
  std::uint32_t isr_calls() const { return isr_calls_; }
  bool tx_next_odd(unsigned n) const { return tx_odd_[n]; }
  bool rx_next_odd(unsigned n) const { return rx_odd_[n]; }

  // ---- the interrupt line ----
  void bind(void (*isr)(void*), void* context) {
    isr_ = isr;
    isr_context_ = context;
  }
  void set_auto_isr(bool on) { auto_isr_ = on; }
  // Runs the interrupt while any enabled flag is set, as the NVIC would keep entering it.
  void interrupt() {
    for (unsigned guard = 0; guard < 64u && (istat_ & inten_) != 0u && isr_ != nullptr; ++guard) {
      ++isr_calls_;
      isr_(isr_context_);
    }
  }

  // ---- bus events, from the host's side ----
  void bus_reset() {
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatUsbRst);
    fifo_n_ = 0;
    frozen_ = false;
    held_in_ = false;
    after();
  }
  void sof() {
    ++frame_;
    cycles_ += sof_period_;
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatSofTok);
    after();
  }
  void bus_idle() {
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatSleep);
    after();
  }
  void bus_resume() {
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatResume);
    after();
  }
  void bus_error(std::uint8_t errstat_bits) {
    errstat_ = static_cast<std::uint8_t>(errstat_ | errstat_bits);
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatError);
    after();
  }

  // SETUP token plus its 8-byte data packet to endpoint 0.
  Reply setup(const std::uint8_t* raw8) {
    if (late_stall_freeze_ && frozen_) {
      return Reply::Ignored;  // deaf: the host sees no handshake
    }
    if (epstall_blocks_setup_ && (endpt_[0] & kEndptStall) != 0u) {
      stalled();
      return Reply::Stall;
    }
    BdtEntry& b = bdt_.entry[bdt_index(0, false, rx_odd_[0])];
    if (!bd::owned(b.desc) || fifo_n_ == kFifoDepth) {
      return Reply::Nak;
    }
    std::memcpy(reinterpret_cast<void*>(b.addr), raw8, 8);
    finish(b, 0, false, rx_odd_[0], 8, bd::kPidSetup);
    rx_odd_[0] = !rx_odd_[0];
    frozen_ = true;  // TXSUSPENDTOKENBUSY: nothing else is accepted until software clears it
    after();
    return Reply::Ack;
  }

  // OUT token plus a data packet (the host's toggle `data1`). Endpoint 0 receive does not check the toggle in this model
  // (the status stage of a control transfer arrives as DATA1 whatever the descriptor says; DTS is applied to the other endpoints).
  Reply out(unsigned ep, const std::uint8_t* data, std::uint32_t len, bool data1) {
    if (frozen_) {
      return Reply::Nak;
    }
    if ((endpt_[ep] & kEndptRxEn) == 0u) {
      return Reply::Ignored;
    }
    if ((endpt_[ep] & kEndptStall) != 0u) {
      stalled();
      return Reply::Stall;
    }
    BdtEntry& b = bdt_.entry[bdt_index(ep, false, rx_odd_[ep])];
    if (bd::owned(b.desc) && (b.desc & bd::kStall) != 0u) {  // the BD-level stall: STALL handshake, the descriptor is not consumed
      stalled();
      return Reply::Stall;
    }
    if (!bd::owned(b.desc) || fifo_n_ == kFifoDepth) {
      return Reply::Nak;
    }
    const bool dts = (b.desc & bd::kDts) != 0u;
    const bool expect = (b.desc & bd::kData1) != 0u;
    if (ep != 0u && dts && expect != data1) {
      ++toggle_mismatches_;  // the controller acknowledges and discards a retransmission: the descriptor stays armed
      return Reply::Ack;
    }
    const std::uint32_t room = bd::bytes(b.desc);
    const std::uint32_t n = len < room ? len : room;
    if (n != 0u) {
      std::memcpy(reinterpret_cast<void*>(b.addr), data, n);
    }
    finish(b, ep, false, rx_odd_[ep], n, bd::kPidOut);
    rx_odd_[ep] = !rx_odd_[ep];
    after();
    return Reply::Ack;
  }

  // IN token: the data the device sends, or NAK. `data1_seen` is the PID the device put on the bus (its descriptor's DATA bit when
  // DTS is set, DATA0 otherwise).
  Reply in(unsigned ep, std::uint8_t* data, std::uint32_t cap, std::uint32_t& len, bool& data1_seen) {
    len = 0;
    if (frozen_) {
      if (late_stall_freeze_ && ep == 0u) {
        held_in_ = true;
        return Reply::Ignored;
      }
      return Reply::Nak;
    }
    if ((endpt_[ep] & kEndptTxEn) == 0u) {
      return Reply::Ignored;
    }
    if ((endpt_[ep] & kEndptStall) != 0u) {
      stalled();
      return Reply::Stall;
    }
    BdtEntry& b = bdt_.entry[bdt_index(ep, true, tx_odd_[ep])];
    if (bd::owned(b.desc) && (b.desc & bd::kStall) != 0u) {  // the BD-level stall: STALL handshake, the descriptor is not consumed (OWN stays, no TOKDNE)
      if (bd_stall_toggles_) {
        tx_odd_[ep] = !tx_odd_[ep];  // model_bd_stall_toggles_pointer
      }
      stalled();
      return Reply::Stall;
    }
    if (!bd::owned(b.desc) || fifo_n_ == kFifoDepth) {
      return Reply::Nak;
    }
    len = bd::bytes(b.desc);
    data1_seen = (b.desc & bd::kDts) != 0u && (b.desc & bd::kData1) != 0u;
    const std::uint32_t n = len < cap ? len : cap;
    if (n != 0u) {
      std::memcpy(data, reinterpret_cast<const void*>(b.addr), n);
    }
    finish(b, ep, true, tx_odd_[ep], len, bd::kPidIn);
    tx_odd_[ep] = !tx_odd_[ep];
    after();
    return Reply::Ack;
  }

 private:
  static constexpr unsigned kFifoDepth = 4;

  void finish(BdtEntry& b, unsigned ep, bool tx, bool odd, std::uint32_t bytes, std::uint32_t pid) {
    b.desc = (bytes << 16) | (pid << 2);  // OWN cleared, count and PID written back
    fifo_[fifo_n_++] = static_cast<std::uint8_t>((ep << 4) | (tx ? 8u : 0u) | (odd ? 4u : 0u));
    istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatTokDne);
  }
  void stalled() { istat_ = static_cast<std::uint8_t>(istat_ | sie::kIstatStall); after(); }
  void after() {
    if (auto_isr_) {
      interrupt();
    }
  }

  BdtTable& bdt_;
  std::uint8_t istat_ = 0, inten_ = 0, erren_ = 0, errstat_ = 0, ctl_ = 0, address_ = 0;
  std::uint8_t endpt_[16] = {};
  std::uint8_t fifo_[kFifoDepth] = {};
  unsigned fifo_n_ = 0;
  bool rx_odd_[16] = {};
  bool tx_odd_[16] = {};
  bool frozen_ = false;
  bool late_stall_freeze_ = false;  // model_late_stall_freeze
  bool held_in_ = false;
  bool epstall_blocks_setup_ = false;  // model_epstall_blocks_setup
  bool bd_stall_toggles_ = false;  // model_bd_stall_toggles_pointer
  bool token_at_stall_ = false;  // model_token_at_stall_seen
  unsigned stall_flag_reads_ = 0;
  unsigned ctl_writes_ = 0;
  bool auto_isr_ = true;
  std::uint32_t cycles_ = 0;
  std::uint32_t frame_ = 0;
  std::uint32_t sof_period_ = 0;
  std::uint32_t toggle_mismatches_ = 0;
  std::uint32_t isr_calls_ = 0;
  void (*isr_)(void*) = nullptr;
  void* isr_context_ = nullptr;
};

// A host: control transfers and bulk transfers with the toggles a real host keeps.
class Host {
 public:
  explicit Host(FakeController& c) noexcept : c_(c) {}

  struct Result {
    bool ok = false;           // the transfer completed (not stalled, no NAK that never cleared)
    bool stalled = false;
    std::uint32_t length = 0;  // bytes received in the data stage
    std::uint32_t packets = 0; // data-stage packets (a zero-length closing packet counts)
    bool zlp_closed = false;   // the data stage ended with a zero-length packet
    std::uint32_t toggle_errors = 0;
  };

  // An IN control transfer: SETUP, IN packets with DATA1 then alternating until a short packet or wLength bytes, status OUT.
  Result control_in(const std::uint8_t setup[8], std::uint8_t* data, std::uint32_t cap) {
    Result r;
    const std::uint32_t want = static_cast<std::uint32_t>(setup[6] | (setup[7] << 8));
    if (c_.setup(setup) != Reply::Ack) {
      return r;
    }
    bool expect_data1 = true;
    while (true) {
      std::uint32_t len = 0;
      bool seen_data1 = false;
      const Reply a = c_.in(0, data + r.length, r.length < cap ? cap - r.length : 0, len, seen_data1);
      if (a == Reply::Stall) {
        r.stalled = true;
        return r;
      }
      if (a != Reply::Ack) {
        return r;  // a NAK: the device did not arm a packet
      }
      if (seen_data1 != expect_data1) {
        ++r.toggle_errors;
      }
      expect_data1 = !expect_data1;
      r.length += len;
      ++r.packets;
      if (len < kEp0 || r.length >= want) {
        r.zlp_closed = len == 0u && r.packets > 1u;
        break;
      }
    }
    // Status stage: an OUT zero-length packet, DATA1.
    const Reply s = c_.out(0, nullptr, 0, true);
    r.ok = s == Reply::Ack;
    return r;
  }

  // A control transfer with no data stage: SETUP, then the status stage the device sends (a zero-length IN, DATA1).
  Result control_none(const std::uint8_t setup[8]) {
    Result r;
    if (c_.setup(setup) != Reply::Ack) {
      return r;
    }
    std::uint8_t dummy[1];
    std::uint32_t len = 0;
    bool seen_data1 = false;
    const Reply a = c_.in(0, dummy, 0, len, seen_data1);
    if (a == Reply::Stall) {
      r.stalled = true;
      return r;
    }
    if (a != Reply::Ack || len != 0u) {
      return r;
    }
    if (!seen_data1) {
      ++r.toggle_errors;
    }
    r.ok = true;
    return r;
  }

  // A control transfer with an OUT data stage (one packet), then the zero-length IN status.
  Result control_out(const std::uint8_t setup[8], const std::uint8_t* data, std::uint32_t len) {
    Result r;
    if (c_.setup(setup) != Reply::Ack) {
      return r;
    }
    const Reply d = c_.out(0, data, len, true);
    if (d == Reply::Stall) {
      r.stalled = true;
      return r;
    }
    if (d != Reply::Ack) {
      return r;
    }
    std::uint8_t dummy[1];
    std::uint32_t got = 0;
    bool seen_data1 = false;
    const Reply a = c_.in(0, dummy, 0, got, seen_data1);
    if (a == Reply::Stall) {
      r.stalled = true;
      return r;
    }
    r.ok = a == Reply::Ack && got == 0u;
    if (r.ok && !seen_data1) {
      ++r.toggle_errors;
    }
    return r;
  }

  // Bulk IN: one packet if the device has one armed. Keeps the host's toggle per endpoint (starts DATA0).
  Reply bulk_in(unsigned ep, std::uint8_t* data, std::uint32_t cap, std::uint32_t& len) {
    bool seen = false;
    const Reply a = c_.in(ep, data, cap, len, seen);
    if (a == Reply::Ack) {
      if (seen != in_toggle_[ep]) {
        ++toggle_errors_;
      }
      in_toggle_[ep] = !in_toggle_[ep];
    }
    return a;
  }
  // Bulk OUT: the host sends with its own toggle; a mismatch with what the device expects is the device's problem to survive.
  Reply bulk_out(unsigned ep, const std::uint8_t* data, std::uint32_t len) {
    const Reply a = c_.out(ep, data, len, out_toggle_[ep]);
    if (a == Reply::Ack) {
      out_toggle_[ep] = !out_toggle_[ep];
    }
    return a;
  }
  // ClearFeature(ENDPOINT_HALT) and a configuration both put the host's toggles back to DATA0.
  void reset_toggles() {
    for (unsigned n = 0; n < 16u; ++n) {
      in_toggle_[n] = false;
      out_toggle_[n] = false;
    }
  }
  std::uint32_t toggle_errors() const { return toggle_errors_; }

  static constexpr std::uint32_t kEp0 = 64;

 private:
  FakeController& c_;
  bool in_toggle_[16] = {};
  bool out_toggle_[16] = {};
  std::uint32_t toggle_errors_ = 0;
};

inline void make_setup(std::uint8_t raw[8], std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                       std::uint16_t length) {
  raw[0] = type;
  raw[1] = request;
  raw[2] = static_cast<std::uint8_t>(value & 0xFFu);
  raw[3] = static_cast<std::uint8_t>(value >> 8);
  raw[4] = static_cast<std::uint8_t>(index & 0xFFu);
  raw[5] = static_cast<std::uint8_t>(index >> 8);
  raw[6] = static_cast<std::uint8_t>(length & 0xFFu);
  raw[7] = static_cast<std::uint8_t>(length >> 8);
}

}  // namespace usb_fake
