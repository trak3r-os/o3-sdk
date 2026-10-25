// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/cdc.h>
#include <o3sdk/usb/control.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/endpoint.h>
#include <o3sdk/usb/ep0_trace.h>
#include <o3sdk/usb/iso_audio.h>
#include <o3sdk/usb/msc_link.h>
#include <o3sdk/usb/stream.h>
#include <o3sdk/usb/usb_types.h>

#include <cstdint>
#include <cstring>

// The USB interrupt's logic (OS-006b), written against a register policy so that the same code runs on the MK66's USB0 and on
// the fake controller of the host tests: what to do for a bus reset, a finished token, a start of frame, a suspend; how the
// descriptors, the ping-pong pairs, the rings and the control pipe fit together. The policy (`Hw`, below) is what the
// platform supplies: a handful of one-instruction register accessors.
//
// The controller is the K66's USB OTG module in device mode (USB0, full speed); a policy for any controller with the same
// serial interface engine (the same ISTAT / STAT / BDT / ENDPTn scheme) would do. The register facts are in the platform
// layer (platform/mk66/usb) with their citations; the bit values the engine itself needs are in namespace `sie`.
//
// What runs where (ADR 0013 D1, ADR 0012):
//   * isr() is the whole USB interrupt, at hal::irq::prio::kUsb (64: below the audio DMA, SysTick and MIDI-UART interrupts, above
//     the SDHC / SPI / I2C completions and the render). It moves packets between the buffer descriptors and the rings and
//     does nothing else: no parsing, no formatting, no allocation, no waiting. Its work is bounded: at most kMaxIsrLoops
//     passes, each handling one token (a copy of at most 64 bytes plus a few descriptor writes) or one flag.
//   * The descriptors, the ping-pong pairs, the control pipe and every buffer belong to that interrupt alone.
//   * The rings are the only thing shared: CDC transmit (main loop writes, interrupt reads), CDC receive (interrupt writes, main
//     loop reads), MIDI in (engine writes, interrupt reads), MIDI out (interrupt writes, engine reads). All four are
//     OverwriteRings: lock-free, the producer never waits, an overrun drops the oldest and is counted.
//   * Counters have one writer (the interrupt, or the CDC writer for its own drop count); the main loop reads them as plain words.
//   * AUDIO mode (OS-006d, ADR 0012): endpoint numbers 4 (isochronous stems IN + isochronous return OUT) and 5 (feedback IN) are driven by the
//     same interrupt through an `IsoAudio` bound with bind_audio(). A SET_INTERFACE that selects alt 1 takes the endpoint's descriptors back and
//     starts the stream (stop for alt 0, a configuration event or a bus reset). IN: ONE packet is armed at a time, built at the completion of the
//     previous one (so it is ready a whole frame ahead of the host's token) and at a start of frame when nothing is armed (the first one, or after a
//     missed frame). OUT: both descriptors are always armed, each re-armed as its token completes. Feedback: one packet armed at a SOF when none is
//     pending, so the host always reads a value at most one polling interval old. No handshake, no data toggle on any of them (bd::arm_iso).
//     Everything happens in the one interrupt, bounded: one interleave of at most 45 frames x 11 channels, one ring write of at most 48 words.
namespace o3sdk::usb {

// ---- The serial interface engine's bits (K66 RM "USB OTG FS": ISTAT, INTEN, CTL, ERRSTAT, STAT) ---------------------------
namespace sie {
inline constexpr std::uint8_t kIstatUsbRst = 0x01;
inline constexpr std::uint8_t kIstatError = 0x02;
inline constexpr std::uint8_t kIstatSofTok = 0x04;
inline constexpr std::uint8_t kIstatTokDne = 0x08;
inline constexpr std::uint8_t kIstatSleep = 0x10;
inline constexpr std::uint8_t kIstatResume = 0x20;
inline constexpr std::uint8_t kIstatStall = 0x80;
// The interrupts this driver enables (everything but ATTACH, which is a host-mode flag): it also masks what it reads from ISTAT
// with this, so a flag nobody enabled can never keep the loop busy.
inline constexpr std::uint8_t kIntenWorkingSet = kIstatUsbRst | kIstatError | kIstatSofTok | kIstatTokDne | kIstatSleep |
                                                 kIstatResume | kIstatStall;
inline constexpr std::uint8_t kCtlUsbEnSofEn = 0x01;  // module on; written alone it also clears TXSUSPENDTOKENBUSY
inline constexpr std::uint8_t kCtlOddRst = 0x02;      // every ping-pong pointer back to even
inline constexpr std::uint8_t kCtlTxSuspendTokenBusy = 0x20;  // read-only: set after a SETUP (and by a late stall handshake), the controller takes no token until CTL is written
inline constexpr std::uint8_t kErrstatAll = 0xFF;
// STAT: bits 7..4 endpoint, bit 3 transmit, bit 2 odd.
constexpr unsigned stat_endpoint(std::uint8_t stat) noexcept { return static_cast<unsigned>(stat >> 4); }
constexpr bool stat_transmit(std::uint8_t stat) noexcept { return (stat & 0x08u) != 0u; }
constexpr bool stat_odd(std::uint8_t stat) noexcept { return (stat & 0x04u) != 0u; }
}  // namespace sie

// ---- The register policy ------------------------------------------------------------------------------------------------
// struct Hw {
//   std::uint8_t istat();                    void istat_clear(std::uint8_t bits);     // write 1 to clear
//   std::uint8_t stat();                                                              // the token at the head of the 4-deep STAT FIFO
//   std::uint8_t errstat();                  void errstat_clear(std::uint8_t bits);
//   void ctl(std::uint8_t value);            void set_address(std::uint8_t address);
//   std::uint8_t ctl();                                                               // the readback (TXSUSPENDTOKENBUSY: sie::kCtlTxSuspendTokenBusy)
//   void set_inten(std::uint8_t value);      void set_erren(std::uint8_t value);
//   std::uint8_t endpt(unsigned number);     void endpt(unsigned number, std::uint8_t value);
//   void dmb();                              // a store barrier between filling a descriptor's address and handing it over
//   std::uint32_t cycles();                  // a free-running cycle counter (the DWT's), for the interrupt's own cost
// };

struct DeviceCounters {
  std::uint32_t isr_count = 0;
  std::uint32_t isr_cycles_last = 0;      // cycles inside isr() the last time, entry to exit
  std::uint32_t isr_cycles_max = 0;       // the worst since start (the ADR 0012 validation 5 figure, read by `diag usb`)
  std::uint32_t isr_cycles_sum = 0;       // the cycles of every entry added up, modulo 2^32 (OS-USBDISK U1d: a reader takes the difference of two reads; one wrap is about 79 s at a saturated interrupt)
  std::uint32_t isr_loop_limit = 0;       // isr() stopped at kMaxIsrLoops with flags still set (it is entered again at once)
  std::uint32_t bus_resets = 0;
  std::uint32_t errors[8] = {};           // ERRSTAT bit histogram: 0 PIDERR, 1 CRC5/EOF, 2 CRC16, 3 DFN8, 4 BTO, 5 DMA, 7 BTS
  std::uint32_t tokens_ep0 = 0;
  std::uint32_t tokens_cdc_in = 0;
  std::uint32_t tokens_cdc_out = 0;
  std::uint32_t tokens_midi_in = 0;
  std::uint32_t tokens_midi_out = 0;
  std::uint32_t tokens_stray = 0;         // a finished token no endpoint here owns, a SETUP of the wrong length, a lost descriptor
  std::uint32_t pingpong_resyncs = 0;     // a descriptor finished out of the order the bookkeeping expected
  std::uint32_t ep0_stalls = 0;
  std::uint32_t cdc_tx_bytes = 0;         // bytes handed to the controller
  std::uint32_t cdc_tx_packets = 0;
  std::uint32_t cdc_rx_bytes = 0;         // bytes taken from the host
  std::uint32_t cdc_rx_dropped_chunks = 0;  // must stay 0: the hold below keeps the ring from overflowing
  std::uint32_t cdc_rx_holds = 0;           // times the receive descriptors were left unarmed because the ring held kCdcRxHoldChunks (the host got NAKs)
  std::uint32_t midi_in_packets = 0;
  std::uint32_t midi_out_packets = 0;
  std::uint32_t midi_out_events = 0;
  std::uint32_t midi_out_skipped = 0;     // reserved or padding events
  std::uint32_t midi_out_bad_bytes = 0;
  std::uint32_t midi_out_dropped = 0;     // events overwritten before the engine read them (noticed by the reader)
  std::uint32_t unsupported_interface_sets = 0;  // SET_INTERFACE of an AudioStreaming alternate setting with no IsoAudio bound (or in STANDARD mode)
  // AUDIO mode (OS-006d). One writer: the interrupt.
  std::uint32_t iso_in_starts = 0;        // alt 1 selected on the stems interface
  std::uint32_t iso_in_packets = 0;       // stem packets armed
  std::uint32_t iso_in_missed = 0;        // start of frame with no IN token since the previous one while streaming (the host did not poll, or we were late)
  std::uint32_t iso_out_starts = 0;       // alt 1 selected on the return interface
  std::uint32_t iso_out_empty = 0;        // zero-length OUT packets (the host had nothing for the frame)
  std::uint32_t iso_fb_packets = 0;       // feedback packets armed
  // DISK mode (OS-USBDISK U1c). One writer: the interrupt.
  std::uint32_t msc_out_packets = 0;      // bulk OUT packets published to the pump
  std::uint32_t msc_in_packets = 0;       // bulk IN packets armed
  std::uint32_t msc_in_stale = 0;         // IN slots skipped because their epoch was not the live one
  std::uint32_t msc_stalls = 0;           // EPSTALL applied for the engine (IN markers and OUT requests)
};

// Rare events, reported to an optional hook for the log (ISR context: the hook must be short and non-blocking).
enum class UsbEvent : std::uint8_t { BusReset, Addressed, Configured, Deconfigured, Suspended, Resumed, Halted, HaltCleared };
using UsbEventHook = void (*)(void* context, UsbEvent event, std::uint32_t arg);

// The packet buffers, all in the one struct so that its alignment is declared once (ADR 0011 lesson 1): every buffer is
// 4-byte aligned by type and the controller's DMA and the CPU's word copies agree on it. Two per endpoint direction, one for
// each descriptor of the ping-pong pair.
struct UsbBuffers {
  alignas(4) std::uint8_t ep0_rx[2][kEp0Size];
  alignas(4) std::uint8_t cdc_in[2][kCdcPacketBytes];
  alignas(4) std::uint8_t cdc_out[2][kCdcPacketBytes];
  alignas(4) std::uint8_t midi_in[2][kCdcPacketBytes];
  alignas(4) std::uint8_t midi_out[2][kCdcPacketBytes];
  // AUDIO mode (OS-006d): the isochronous packet buffers; their types declare the alignment. The OUT pair keeps both descriptors armed, so it has one
  // buffer each. The IN stems packet and the feedback packet are armed ONE AT A TIME (arm_iso_in / arm_iso_fb only arm with nothing pending), so the two
  // descriptors of each pair take turns on a single buffer, which is rebuilt only after the controller has finished the previous packet: this saves 1 KB of
  // SRAM_U, which has 4 KB of margin over its 32 KiB floor (ADR 0013 D7).
  InPacketBuffer iso_in;
  OutPacketBuffer iso_out[2];
  FeedbackPacketBuffer iso_fb;
};
static_assert(alignof(UsbBuffers) == 4 && sizeof(UsbBuffers) % 4u == 0u, "packet buffers are word aligned by type");
static_assert(offsetof(UsbBuffers, ep0_rx) % 4u == 0u && offsetof(UsbBuffers, cdc_in) % 4u == 0u &&
                  offsetof(UsbBuffers, cdc_out) % 4u == 0u && offsetof(UsbBuffers, midi_in) % 4u == 0u &&
                  offsetof(UsbBuffers, midi_out) % 4u == 0u && offsetof(UsbBuffers, iso_in) % 4u == 0u &&
                  offsetof(UsbBuffers, iso_out) % 4u == 0u && offsetof(UsbBuffers, iso_fb) % 4u == 0u,
              "every buffer starts on a word");

// At most this many passes of the interrupt's loop per entry. A pass handles one finished token (the STAT FIFO is 4 deep) or the
// pending flags; if more is pending the line stays asserted and the interrupt is entered again straight away, so the bound
// costs nothing but a cap on how long one entry can run (ADR 0013 D1: bounded work).
inline constexpr unsigned kMaxIsrLoops = 16;

// The controller's frame number, when the policy has one (`std::uint32_t frame()`: FRMNUML/FRMNUMH on the chip, 11 bits); policies without it (the
// stuck-controller double of the tests) read 0, which only the rate estimator would notice.
template <class Hw>
auto hw_frame(Hw& hw, int) -> decltype(static_cast<std::uint32_t>(hw.frame())) {
  return static_cast<std::uint32_t>(hw.frame());
}
template <class Hw>
std::uint32_t hw_frame(Hw&, long) {
  return 0;
}

template <class Hw>
class UsbDevice {
 public:
  UsbDevice(Hw& hw, BdtTable& bdt, ControlPipe& control, Counters& audio_counters, CdcTxRing& cdc_tx, CdcRxRing& cdc_rx,
            MidiRing& midi_in, MidiRing& midi_out) noexcept
      : hw_(hw),
        bdt_(bdt),
        ctl_(control),
        audio_(audio_counters),
        cdc_tx_(cdc_tx),
        cdc_rx_(cdc_rx),
        midi_in_(midi_in),
        midi_out_(midi_out) {}

  // AUDIO mode: the isochronous streams (iso_audio.h). Called once before the interrupt is enabled for the first time; with none bound a
  // SET_INTERFACE to an AudioStreaming alternate setting is counted in unsupported_interface_sets and ignored.
  void bind_audio(IsoAudio* iso) noexcept { iso_ = iso; }
  // DISK mode (OS-USBDISK U1c, usb/msc_link.h): the link to the main loop's MSC pump. Called once before the interrupt is enabled; with none bound the MSC endpoints stay unarmed.
  void bind_msc(MscLink* link) noexcept { msc_ = link; }
  // OS-USBDISK U2a: the endpoint 0 trace (ep0_trace.h), recorded while it is armed. Called once before the interrupt is enabled; none bound is no cost but a pointer test.
  void bind_trace(Ep0Trace* trace) noexcept { trace_ = trace; }

  void set_event_hook(UsbEventHook hook, void* context) noexcept {
    hook_ = hook;
    hook_context_ = context;
  }

  // Puts the controller in the state a bus reset leaves it in: endpoint 0 armed to receive, address 0, only endpoint 0 enabled,
  // flags clear, interrupts enabled. Called once before the D+ pull-up is switched on (so the first thing the host sees is a
  // device that is ready), and again by every bus reset.
  void prepare() noexcept {
    reset_bus();
    c_.bus_resets = 0;  // the count is the host's resets, not this one
  }

  // The USB interrupt.
  void isr() noexcept {
    const std::uint32_t entered = hw_.cycles();
    ++c_.isr_count;
    for (unsigned pass = 0;; ++pass) {
      const std::uint8_t flags = static_cast<std::uint8_t>(hw_.istat() & sie::kIntenWorkingSet);
      if (flags == 0u) {
        break;
      }
      if (pass >= kMaxIsrLoops) {
        ++c_.isr_loop_limit;
        break;
      }
      if ((flags & sie::kIstatUsbRst) != 0u) {
        reset_bus();  // clears every flag, including the ones noted above: they belong to the old bus state
        continue;
      }
      if ((flags & sie::kIstatTokDne) != 0u) {
        on_token(hw_.stat());
        hw_.istat_clear(sie::kIstatTokDne);  // pops the STAT FIFO
        continue;
      }
      if ((flags & sie::kIstatSofTok) != 0u) {
        on_sof();
        hw_.istat_clear(sie::kIstatSofTok);
      }
      if ((flags & sie::kIstatStall) != 0u) {
        // A stall was sent. The first handshake of the stall of endpoint 0 ends its EPSTALL (the one-shot); the stall itself lasts until the next SETUP as a BD-level stall of the transmit
        // descriptors (stall_ep0), so a retried token is answered STALL again. A halted bulk endpoint keeps its own EPSTALL until the host clears the halt. The flag does not say which endpoint it was.
        stall_seen();
        hw_.istat_clear(sie::kIstatStall);
      }
      if ((flags & sie::kIstatError) != 0u) {
        const std::uint8_t errors = hw_.errstat();
        hw_.errstat_clear(errors);
        trace(Ep0Trace::Tag::Error, false, false, false, 0, errors, 0);
        for (unsigned bit = 0; bit < 8u; ++bit) {
          if ((errors & (1u << bit)) != 0u) {
            ++c_.errors[bit];
          }
        }
        hw_.istat_clear(sie::kIstatError);
      }
      if ((flags & sie::kIstatSleep) != 0u) {
        ctl_.on_suspend();
        trace(Ep0Trace::Tag::Sleep, false, false, false, 0, 0, 0);
        notify(UsbEvent::Suspended, 0);
        hw_.istat_clear(sie::kIstatSleep);
      }
      if ((flags & sie::kIstatResume) != 0u) {
        ctl_.on_resume();
        notify(UsbEvent::Resumed, 0);
        hw_.istat_clear(sie::kIstatResume);
      }
    }
    if (msc_ != nullptr) {
      service_msc();  // also reached by the pump's kick (a pended interrupt): arms what the main loop published, whatever the flags said
    }
    const std::uint32_t spent = hw_.cycles() - entered;
    c_.isr_cycles_last = spent;
    c_.isr_cycles_sum += spent;
    if (spent > c_.isr_cycles_max) {
      c_.isr_cycles_max = spent;
    }
  }

  const DeviceCounters& counters() const noexcept { return c_; }
  void reset_counters() noexcept { c_ = DeviceCounters{}; }
  // The bench's window (OS-USBDISK U1d): forgets the worst entry so far. One word stored from the main loop; the interrupt preempts the main loop and never the other way round, so its compare-and-store is not split.
  void reset_isr_peak() noexcept { c_.isr_cycles_max = 0; }
  const ControlPipe& control() const noexcept { return ctl_; }

 private:
  static constexpr unsigned kCdcNumber = layout::kEpCdcIn & 0x0Fu;
  static constexpr unsigned kMidiNumber = layout::kEpMidiIn & 0x0Fu;
  static constexpr unsigned kAudioNumber = layout::kEpAudioIn & 0x0Fu;        // stems IN and return OUT share the number
  static constexpr unsigned kFeedbackNumber = layout::kEpAudioFeedback & 0x0Fu;
  static constexpr unsigned kMscOutNumber = layout::kEpMscOut & 0x0Fu;      // DISK mode: the same numbers as the AUDIO return / feedback pair (4 and 5)
  static constexpr unsigned kMscInNumber = layout::kEpMscIn & 0x0Fu;
  static_assert(kMscOutNumber == kAudioNumber && kMscInNumber == kFeedbackNumber, "DISK reuses the AUDIO endpoint numbers");
  static constexpr std::uint32_t kIsoOutRoom = kReturnMaxChunk * 4u;          // room armed for an OUT packet: 48 stereo frames (the endpoint's maximum is 180 bytes)

  BdtEntry& entry(unsigned endpoint, bool transmit, bool odd) noexcept { return bdt_.entry[bdt_index(endpoint, transmit, odd)]; }

  void notify(UsbEvent event, std::uint32_t arg) noexcept {
    if (hook_ != nullptr) {
      hook_(hook_context_, event, arg);
    }
  }

  void trace(Ep0Trace::Tag tag, bool odd, bool tx, bool data1, std::uint32_t pid, std::uint32_t w0, std::uint32_t bytes, std::uint32_t extra = 0) noexcept {
    if (trace_ != nullptr && trace_->armed()) {
      trace_->record(tag, hw_.cycles(), Ep0Trace::make_flags(odd, tx, data1, pid), w0, bytes, extra);
    }
  }

  // A descriptor is filled first, then handed over by the write that sets OWN: the address and the count must be in place
  // when the controller can see OWN.
  void give(BdtEntry& b, const std::uint8_t* buffer, std::uint32_t bytes, bool data1) noexcept {
    b.addr = reinterpret_cast<std::uintptr_t>(buffer);
    hw_.dmb();
    b.desc = bd::arm(bytes, data1);
  }
  void take_back_direction(unsigned endpoint, bool transmit) noexcept {
    for (unsigned odd = 0; odd < 2u; ++odd) {
      BdtEntry& b = entry(endpoint, transmit, odd != 0u);
      b.desc = 0;
      b.addr = 0;
    }
  }
  // An isochronous descriptor: handed over like give(), without the data toggle.
  void give_iso(BdtEntry& b, const void* buffer, std::uint32_t bytes) noexcept {
    b.addr = reinterpret_cast<std::uintptr_t>(buffer);
    hw_.dmb();
    b.desc = bd::arm_iso(bytes);
  }
  void take_back(unsigned endpoint) noexcept {
    for (unsigned dir = 0; dir < 2u; ++dir) {
      for (unsigned odd = 0; odd < 2u; ++odd) {
        BdtEntry& b = entry(endpoint, dir != 0u, odd != 0u);
        b.desc = 0;
        b.addr = 0;
      }
    }
  }

  // ---- Bus reset ----------------------------------------------------------------------------------------------------------
  void reset_bus() noexcept {
    hw_.ctl(sie::kCtlOddRst);  // held while the table is rebuilt: every pointer is even again
    for (unsigned n = 0; n < kBdtEndpoints; ++n) {
      take_back(n);
      if (n != 0u) {
        hw_.endpt(n, 0);  // only endpoint 0 exists until a configuration is selected
      }
    }
    ctl_.bus_reset();
    rx_holding_ = false;
    ep0_tx_.reset();
    ep0_stall_ = Ep0Stall::None;  // the transmit descriptors of endpoint 0 were zeroed by take_back above
    cdc_in_.reset();
    cdc_out_.reset();
    midi_in_pp_.reset();
    midi_out_pp_.reset();
    iso_in_pp_.reset();
    iso_out_pp_.reset();
    iso_fb_pp_.reset();
    iso_in_on_ = false;
    iso_out_on_ = false;
    if (iso_ != nullptr) {
      iso_->reset_session(ctl_.config().speed);
    }
    msc_session_reset(true);
    // Endpoint 0 receive: both descriptors armed, DATA0 as at reset (the host's next token is a SETUP, whatever the toggle).
    for (unsigned odd = 0; odd < 2u; ++odd) {
      give(entry(0, false, odd != 0u), buf_.ep0_rx[odd], kEp0Size, false);
    }
    hw_.endpt(0, kEndpt0);
    hw_.errstat_clear(sie::kErrstatAll);
    hw_.istat_clear(0xFFu);
    hw_.set_address(0);
    hw_.set_erren(sie::kErrstatAll);
    hw_.set_inten(sie::kIntenWorkingSet);
    hw_.ctl(sie::kCtlUsbEnSofEn);
    ++c_.bus_resets;
    trace(Ep0Trace::Tag::Reset, false, false, false, 0, 0, 0);
    notify(UsbEvent::BusReset, 0);
  }

  // ---- Tokens -------------------------------------------------------------------------------------------------------------
  void on_token(std::uint8_t stat) noexcept {
    const unsigned endpoint = sie::stat_endpoint(stat);
    const bool transmit = sie::stat_transmit(stat);
    const bool odd = sie::stat_odd(stat);
    if (endpoint >= kBdtEndpoints) {
      ++c_.tokens_stray;  // no descriptors exist for it: never index past the table
      return;
    }
    const std::uint32_t desc = entry(endpoint, transmit, odd).desc;  // read once: the controller has let go of it
    if (endpoint == 0u) {
      ++c_.tokens_ep0;
      token_ep0(transmit, odd, desc);
      hw_.ctl(sie::kCtlUsbEnSofEn);  // clears TXSUSPENDTOKENBUSY: the controller froze after the SETUP until software is done
    } else if (endpoint == kCdcNumber) {
      if (transmit) {
        token_cdc_in(odd);
      } else {
        token_cdc_out(odd, desc);
      }
    } else if (endpoint == kMidiNumber) {
      if (transmit) {
        token_midi_in(odd);
      } else {
        token_midi_out(odd, desc);
      }
    } else if (msc_on() && (endpoint == kMscOutNumber || endpoint == kMscInNumber)) {
      if (endpoint == kMscOutNumber && !transmit) {
        token_msc_out(odd, desc);
      } else if (endpoint == kMscInNumber && transmit) {
        token_msc_in(odd);
      } else {
        ++c_.tokens_stray;
      }
    } else if (endpoint == kAudioNumber) {
      if (transmit) {
        token_iso_in(odd);
      } else {
        token_iso_out(odd, desc);
      }
    } else if (endpoint == kFeedbackNumber && transmit) {
      token_iso_fb(odd);
    } else {
      ++c_.tokens_stray;
    }
  }

  void rearm_ep0_rx(bool odd) noexcept { give(entry(0, false, odd), buf_.ep0_rx[odd ? 1 : 0], kEp0Size, true); }

  void token_ep0(bool transmit, bool odd, std::uint32_t desc) noexcept {
    if (transmit) {
      trace(Ep0Trace::Tag::InDone, odd, true, (desc & bd::kData1) != 0u, bd::pid(desc), static_cast<std::uint32_t>(entry(0, true, odd).addr), bd::bytes(desc));
      if (!ep0_tx_.complete(odd)) {
        ++c_.pingpong_resyncs;
      }
      apply(ctl_.on_in_done());
      return;
    }
    const std::uint8_t* packet = buf_.ep0_rx[odd ? 1 : 0];
    const std::uint32_t bytes = bd::bytes(desc);
    const std::uint32_t pid = bd::pid(desc);
    if (pid == bd::kPidSetup && bytes == 8u) {
      std::uint8_t raw[8];
      std::memcpy(raw, packet, sizeof raw);
      trace(Ep0Trace::Tag::Setup, odd, false, false, pid, static_cast<std::uint32_t>(raw[0] | (raw[1] << 8) | (raw[2] << 16) | (static_cast<std::uint32_t>(raw[3]) << 24)),
            static_cast<std::uint32_t>(raw[6] | (raw[7] << 8)), static_cast<std::uint32_t>(raw[4] | (raw[5] << 8)));
      rearm_ep0_rx(odd);
      // A SETUP ends whatever transfer was in progress: take back the transmit descriptors (which also ends the BD-level stall of stall_ep0), and clear an EPSTALL no handshake has consumed.
      for (unsigned t = 0; t < 2u; ++t) {
        BdtEntry& b = entry(0, true, t != 0u);
        b.desc = 0;
      }
      ep0_tx_.flush();
      if (ep0_stall_ != Ep0Stall::None) {
        ep0_stall_ = Ep0Stall::None;
        trace(Ep0Trace::Tag::StallCleared, false, false, false, 0, 0, 0);
      }
      hw_.endpt(0, kEndpt0);
      apply(ctl_.on_setup(raw));
    } else if (pid == bd::kPidOut) {
      trace(Ep0Trace::Tag::OutDone, odd, false, (desc & bd::kData1) != 0u, pid, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(packet)), bytes);
      const ControlStep step = ctl_.on_out_done(packet, bytes);
      rearm_ep0_rx(odd);
      apply(step);
    } else {
      rearm_ep0_rx(odd);
      ++c_.tokens_stray;
    }
  }

  // Carries out a step of the control pipe: its effect first (so that a configuration is in place before the status
  // packet that confirms it), then the packet.
  void apply(const ControlStep& step) noexcept {
    switch (step.effect) {
      case Effect::None:
        break;
      case Effect::SetAddress:
        hw_.set_address(step.arg);
        trace(Ep0Trace::Tag::Address, false, false, false, 0, step.arg, 0);
        notify(UsbEvent::Addressed, step.arg);
        break;
      case Effect::Configure:
        configure(step.arg != 0u);
        break;
      case Effect::SetInterface:
        audio_interface_changed(step.arg);
        break;
      case Effect::Halt:
        set_halt(step.arg, true);
        break;
      case Effect::ClearHalt:
        set_halt(step.arg, false);
        break;
      case Effect::MscReset:
        if (msc_ != nullptr) {
          msc_->note(MscLink::kBot);  // the descriptors stay armed: the reset recovery's clear-halt takes them back (spec 3.4)
        }
        break;
    }
    switch (step.kind) {
      case ControlStep::Kind::None:
        break;
      case ControlStep::Kind::In:
        send_ep0(step.data, step.len, step.data1);
        break;
      case ControlStep::Kind::Stall:
        stall_ep0();
        break;
    }
  }

  void send_ep0(const std::uint8_t* data, std::uint32_t bytes, bool data1) noexcept {
    if (!ep0_tx_.can_arm()) {
      ++c_.tokens_stray;  // cannot happen: one packet is in flight at a time
      return;
    }
    const PingPong::Slot slot = ep0_tx_.arm();
    trace(Ep0Trace::Tag::ArmIn, slot.odd, true, data1, 0, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)), bytes);
    give(entry(0, true, slot.odd), data, bytes, data1);  // the control pipe chooses the toggle (DATA1 first, then alternating)
  }

  // The protocol stall of endpoint 0 (a request the control pipe does not serve). Two parts. EPSTALL is the one-shot of the proven path: the host's first token gets the STALL handshake and stall_seen() clears it
  // (every known stack for this controller does the same). The BD-level stall stays: both transmit descriptors are handed to the controller with OWN and STALL (count 0), which the controller answers with
  // STALL without consuming them, so a host that retries the IN after the one-shot is gone (a lost handshake, a retrying hub: OS-USBDISK int.14 usbmon, a device-qualifier IN with no completion for 5 s
  // because the retried IN was NAKed) is stalled again, not NAKed for ever (USB 2.0 9.2.7). Nothing here assumes the controller takes a SETUP while EPSTALL is set. The receive descriptors stay armed.
  // The next SETUP (token_ep0) and reset_bus() zero the transmit descriptors, which ends the BD stall. Bulk-halt stalls are separate (set_halt).
  // The STALL flag of a bulk endpoint's halt that the interrupt has not looked at yet is dropped first (its handler has nothing to do for a bulk endpoint): left set, it would be taken for the handshake
  // of THIS stall and end the one-shot before the host has seen it. The flag is read by the interrupt only (stall_seen), and a halt handshake that lands between this clear and the EPSTALL write
  // below is the one remaining gap (microseconds, the controller being frozen after the SETUP that brought us here).
  void stall_ep0() noexcept {
    ++c_.ep0_stalls;
    trace(Ep0Trace::Tag::StallSet, false, false, false, 0, 0, 0);
    ep0_tx_.flush();
    for (unsigned t = 0; t < 2u; ++t) {
      entry(0, true, t != 0u).desc = bd::kOwn | bd::kStall;  // the controller is frozen here: a descriptor it held is simply replaced
    }
    hw_.dmb();
    ep0_stall_ = Ep0Stall::Pending;
    hw_.istat_clear(sie::kIstatStall);
    hw_.endpt(0, static_cast<std::uint8_t>(kEndpt0 | kEndptStall));
  }

  // The controller sent a STALL handshake (ISTAT STALL). The FIRST one of an endpoint 0 stall is the proven path: EPSTALL off, the stall endpoint 0 asked for ends a request with NO finished token (a stalled data
  // stage raises no TOKDNE), so the CTL write that ends the freeze after a SETUP (on_token) is the only release there is, and a handshake that froze the controller anew (hypothesis, never seen on the unit:
  // int.14 read TXSUSPENDTOKENBUSY clear) would leave it deaf to the next SETUP (OS-USBDISK int.13): the freeze is released here, after the receive descriptors are read (never rewritten: a descriptor the controller has
  // finished and TOKDNE has not delivered yet holds a SETUP, and an arm would overwrite it), and not while a TOKDNE is pending: that token's own handling ends the freeze after its SETUP is processed.
  // Every later handshake of the same stall is the host's retry, answered by the BD stall: it is only counted (Ep0Trace::stall_retries; the flag is one bit, so two retries before the interrupt count once, and a
  // bulk halt's handshake inside the stall window counts too: a lower bound, which is what the unit needs). Not traced, so a retrying host cannot fill the 32 entries. A handshake with no endpoint 0 stall in
  // force is a bulk endpoint's: nothing to do.
  void stall_seen() noexcept {
    if (ep0_stall_ == Ep0Stall::Latched) {
      if (trace_ != nullptr) {
        trace_->note_stall_retry();
      }
      return;
    }
    if (ep0_stall_ != Ep0Stall::Pending) {
      return;
    }
    ep0_stall_ = Ep0Stall::Latched;
    hw_.endpt(0, kEndpt0);
    trace(Ep0Trace::Tag::StallSeen, false, false, false, 0, 0, 0);
    const std::uint32_t owned = (bd::owned(entry(0, false, false).desc) ? 1u : 0u) | (bd::owned(entry(0, false, true).desc) ? 2u : 0u);
    const bool released = (hw_.istat() & sie::kIstatTokDne) == 0u;  // the descriptor loads come first: nothing between this read and the CTL write
    if (released) {
      hw_.ctl(sie::kCtlUsbEnSofEn);  // clears TXSUSPENDTOKENBUSY
    }
    const bool busy = (hw_.ctl() & sie::kCtlTxSuspendTokenBusy) != 0u;  // the readback for the trace: set again means the stall handshake froze the controller anew
    trace(Ep0Trace::Tag::SetupArmed, false, false, false, 0, owned, 0, (released ? 1u : 0u) | (busy ? 2u : 0u));
  }

  // ---- Configuration ------------------------------------------------------------------------------------------------------
  void arm_rx(PingPong& pp, unsigned number, std::uint8_t (*buffers)[kCdcPacketBytes]) noexcept {
    while (pp.can_arm()) {
      const PingPong::Slot slot = pp.arm();
      give(entry(number, false, slot.odd), buffers[slot.odd ? 1 : 0], kCdcPacketBytes, slot.data1);
    }
  }

  // A configuration event: every endpoint number the mode uses goes back to its defaults (USB 2.0 9.1.1.5): enabled as the
  // table says (or disabled for configuration 0), no descriptor armed, toggles DATA0. The controller is frozen after the
  // SETUP token that brought us here, so the table can be rewritten without a transaction racing it.
  void configure(bool on) noexcept {
    const Mode mode = ctl_.config().mode;
    const bool usb_return = ctl_.config().usb_return;
    for (std::uint8_t n = 1; n <= last_endpoint(mode, usb_return); ++n) {
      take_back(n);
      hw_.endpt(n, on ? endpoint_config(mode, n, usb_return) : std::uint8_t{0});
    }
    cdc_in_.flush();
    cdc_out_.flush();
    midi_in_pp_.flush();
    midi_out_pp_.flush();
    iso_in_pp_.flush();
    iso_out_pp_.flush();
    iso_fb_pp_.flush();
    iso_in_on_ = false;  // every AudioStreaming interface is back at alt 0 (the control pipe says so too)
    iso_out_on_ = false;
    if (iso_ != nullptr) {
      iso_->reset_session(ctl_.config().speed);
    }
    msc_session_reset(false);
    if (on) {
      arm_rx(cdc_out_, kCdcNumber, buf_.cdc_out);
      arm_rx(midi_out_pp_, kMidiNumber, buf_.midi_out);
      service_in();
    }
    notify(on ? UsbEvent::Configured : UsbEvent::Deconfigured, on ? 1u : 0u);
  }

  void set_halt(std::uint8_t address, bool halt) noexcept {
    const unsigned number = address & 0x0Fu;
    const std::uint8_t now = hw_.endpt(number);
    if (halt) {
      hw_.endpt(number, static_cast<std::uint8_t>(now | kEndptStall));
      notify(UsbEvent::Halted, address);
      return;
    }
    // ClearFeature(ENDPOINT_HALT): the data toggle goes back to DATA0 on both host and device (USB 2.0 9.4.5), so the
    // descriptors of the number are taken back and re-armed from DATA0. (The chip has one EPSTALL per endpoint number, so
    // the other direction of the number is un-halted too.)
    hw_.endpt(number, static_cast<std::uint8_t>(now & ~kEndptStall));
    if (number == kCdcNumber) {
      take_back(number);
      cdc_in_.flush();
      cdc_out_.flush();
      arm_rx(cdc_out_, kCdcNumber, buf_.cdc_out);
    } else if (number == kMidiNumber) {
      take_back(number);
      midi_in_pp_.flush();
      midi_out_pp_.flush();
      arm_rx(midi_out_pp_, kMidiNumber, buf_.midi_out);
    } else if (msc_on() && number == kMscOutNumber) {
      // A stall request the pump posted before this clear and the ISR has not applied yet is older than the clear: the pump handles the clear event next and posts again if the engine is still
      // halted. Applying it after the clear would halt an engine that went back to idle (cases 9/11/13 end at the clear) and the host would find OUT stalled for ever.
      msc_out_stall_seen_ = static_cast<std::uint8_t>(msc_->out_stall_word() & 0xFFu);
      if ((now & kEndptStall) != 0u) {
        msc_->out_mark_drop();  // a packet ACKed just before EPSTALL is published after the stall's mark: everything published up to this clear is older than the stall (spec 3.4, amendment 1); before note() so the pump sees it first
      }
      take_back(number);
      msc_out_pp_.flush();  // armed again by service_in() below; the packets already published stay (CLEAR_FEATURE alone drops nothing)
      msc_->note(MscLink::kClrOut);
    } else if (msc_on() && number == kMscInNumber) {
      take_back(number);
      msc_in_pp_.flush();
      msc_arm_in_ = msc_->in_tail();  // the descriptors taken back are armed again from the same data
      msc_->note(MscLink::kClrIn);
    }
    notify(UsbEvent::HaltCleared, address);
    service_in();
  }

  // ---- Bulk endpoints -----------------------------------------------------------------------------------------------------
  void token_cdc_in(bool odd) noexcept {
    ++c_.tokens_cdc_in;
    if (!cdc_in_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    service_cdc_in();
  }

  void token_cdc_out(bool odd, std::uint32_t desc) noexcept {
    ++c_.tokens_cdc_out;
    if (!cdc_out_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    const std::uint32_t bytes = bd::bytes(desc) > kCdcPacketBytes ? kCdcPacketBytes : bd::bytes(desc);
    if (bytes != 0u) {
      ByteChunk chunk{};
      chunk.len = bytes;
      std::memcpy(chunk.data, buf_.cdc_out[odd ? 1 : 0], bytes);
      if (cdc_rx_.size_from_producer() >= CdcRxRing::capacity()) {
        ++c_.cdc_rx_dropped_chunks;  // this write overwrites the oldest chunk
      }
      cdc_rx_.write(&chunk, 1);
      c_.cdc_rx_bytes += bytes;
    }
    rearm_cdc_out();
  }

  // Back-pressure: the descriptors are armed again only while the receive ring is below kCdcRxHoldChunks; otherwise the host is NAKed until the
  // main loop has read enough. The next start of frame (1 ms) re-arms them, so a drained ring is noticed within a millisecond.
  void rearm_cdc_out() noexcept {
    if (!ctl_.configured() || ctl_.halted(layout::kEpCdcOut)) {
      return;
    }
    if (cdc_rx_.size_from_producer() >= kCdcRxHoldChunks) {
      if (!rx_holding_ && cdc_out_.can_arm()) {
        ++c_.cdc_rx_holds;  // counted once per hold, not once per millisecond of it
        rx_holding_ = true;
      }
      return;
    }
    rx_holding_ = false;
    arm_rx(cdc_out_, kCdcNumber, buf_.cdc_out);
  }

  void service_cdc_in() noexcept {
    if (!ctl_.configured() || ctl_.halted(layout::kEpCdcIn)) {
      return;  // a halted endpoint keeps its data in the ring: clear-halt takes the descriptors back, so nothing may sit in them
    }
    while (cdc_in_.can_arm()) {
      ByteChunk chunk;
      if (cdc_tx_.read(&chunk, 1) == 0u) {
        break;
      }
      const std::uint32_t bytes = chunk.len > kCdcPacketBytes ? kCdcPacketBytes : chunk.len;
      if (bytes == 0u) {
        continue;
      }
      const PingPong::Slot slot = cdc_in_.arm();
      std::uint8_t* packet = buf_.cdc_in[slot.odd ? 1 : 0];
      std::memcpy(packet, chunk.data, bytes);
      give(entry(kCdcNumber, true, slot.odd), packet, bytes, slot.data1);
      c_.cdc_tx_bytes += bytes;
      ++c_.cdc_tx_packets;
    }
  }

  void token_midi_in(bool odd) noexcept {
    ++c_.tokens_midi_in;
    if (!midi_in_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    service_midi_in();
  }

  void token_midi_out(bool odd, std::uint32_t desc) noexcept {
    ++c_.tokens_midi_out;
    if (!midi_out_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    const std::uint32_t bytes = bd::bytes(desc) > kCdcPacketBytes ? kCdcPacketBytes : bd::bytes(desc);
    const MidiPacketResult r = midi_packet_to_ring(midi_out_, buf_.midi_out[odd ? 1 : 0], bytes);
    ++c_.midi_out_packets;
    c_.midi_out_events += r.accepted;
    c_.midi_out_skipped += r.skipped;
    c_.midi_out_bad_bytes += r.bad_bytes;
    c_.midi_out_dropped = midi_out_.dropped();
    if (ctl_.configured()) {
      arm_rx(midi_out_pp_, kMidiNumber, buf_.midi_out);
    }
  }

  void service_midi_in() noexcept {
    if (!ctl_.configured() || ctl_.halted(layout::kEpMidiIn)) {
      return;
    }
    while (midi_in_pp_.can_arm()) {
      alignas(4) std::uint8_t packet[kCdcPacketBytes];
      const std::uint32_t bytes = midi_ring_to_packet(midi_in_, packet);
      if (bytes == 0u) {
        break;
      }
      const PingPong::Slot slot = midi_in_pp_.arm();
      std::uint8_t* target = buf_.midi_in[slot.odd ? 1 : 0];
      std::memcpy(target, packet, bytes);
      give(entry(kMidiNumber, true, slot.odd), target, bytes, slot.data1);
      ++c_.midi_in_packets;
    }
  }

  // ---- Isochronous endpoints (AUDIO mode) -------------------------------------------------------------------------------------------------
  // SET_INTERFACE on an AudioStreaming interface: the control pipe has stored the new alternate setting; start (alt 1) or stop (alt 0) the stream.
  // Both ends of a restart go through stop first, so that a host's "set alt 1 again" flushes the stream.
  void audio_interface_changed(std::uint8_t interface) noexcept {
    if (iso_ == nullptr || ctl_.config().mode != Mode::Audio) {
      ++c_.unsupported_interface_sets;
      return;
    }
    const AudioStreamState& a = ctl_.audio();
    if (interface == layout::kIfAudioIn) {
      stop_iso_in();
      if (a.in_active()) {
        start_iso_in();
      }
    } else if (interface == layout::kIfAudioOut && ctl_.config().usb_return) {  // (with USB RETURN off number 4 is the MIDI AudioControl: the control pipe never sends it here)
      stop_iso_out();
      if (a.out_active()) {
        start_iso_out();
      }
    }
  }

  void stop_iso_in() noexcept {
    take_back_direction(kAudioNumber, true);
    iso_in_pp_.flush();
    iso_in_on_ = false;
    iso_->stop_in();
  }
  void start_iso_in() noexcept {
    iso_in_pp_.flush();
    iso_->start_in(profile_channels(ctl_.config().profile));
    iso_in_on_ = true;
    iso_in_seen_token_ = true;  // the first start of frame is not a miss
    ++c_.iso_in_starts;
    arm_iso_in();  // the first token finds a packet (silence while the stem ring primes)
  }
  void stop_iso_out() noexcept {
    take_back_direction(kAudioNumber, false);
    take_back_direction(kFeedbackNumber, true);
    iso_out_pp_.flush();
    iso_fb_pp_.flush();
    iso_out_on_ = false;
    iso_->stop_out();
  }
  void start_iso_out() noexcept {
    iso_out_pp_.flush();
    iso_fb_pp_.flush();
    iso_->start_out();
    iso_out_on_ = true;
    ++c_.iso_out_starts;
    arm_iso_out();
    arm_iso_fb();
  }

  // One IN packet in flight: built from the stem ring into the free buffer of the pair and handed over. The count is frames x channels x 2.
  void arm_iso_in() noexcept {
    if (!iso_in_on_ || iso_in_pp_.armed() != 0u) {
      return;
    }
    const PingPong::Slot slot = iso_in_pp_.arm();
    InPacketBuffer& packet = buf_.iso_in;  // nothing was armed, so the controller is not reading it
    const std::uint32_t bytes = iso_->build_in(packet.samples);
    give_iso(entry(kAudioNumber, true, slot.odd), packet.samples, bytes);
    ++c_.iso_in_packets;
  }
  void token_iso_in(bool odd) noexcept {
    if (!iso_in_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    iso_in_seen_token_ = true;
    arm_iso_in();
  }

  // Both OUT descriptors armed with room for the largest packet; each is re-armed as its token completes, after its frames are in the ring.
  void arm_iso_out() noexcept {
    while (iso_out_on_ && iso_out_pp_.can_arm()) {
      const PingPong::Slot slot = iso_out_pp_.arm();
      give_iso(entry(kAudioNumber, false, slot.odd), buf_.iso_out[slot.odd ? 1 : 0].frames, kIsoOutRoom);
    }
  }
  void token_iso_out(bool odd, std::uint32_t desc) noexcept {
    if (!iso_out_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    if (iso_out_on_) {
      const std::uint32_t bytes = bd::bytes(desc) > kIsoOutRoom ? kIsoOutRoom : bd::bytes(desc);
      if (bytes == 0u) {
        ++c_.iso_out_empty;
      } else {
        iso_->on_out_packet(buf_.iso_out[odd ? 1 : 0].frames, bytes);
      }
      arm_iso_out();
    }
  }

  // The feedback value, one packet pending at a time: armed at a start of frame when the previous one has been read.
  void arm_iso_fb() noexcept {
    if (!iso_out_on_ || iso_fb_pp_.armed() != 0u) {
      return;
    }
    const PingPong::Slot slot = iso_fb_pp_.arm();
    FeedbackPacketBuffer& packet = buf_.iso_fb;  // nothing was armed, so the controller is not reading it
    const std::uint32_t bytes = iso_->feedback_packet(packet.bytes);
    give_iso(entry(kFeedbackNumber, true, slot.odd), packet.bytes, bytes);
    ++c_.iso_fb_packets;
  }
  void token_iso_fb(bool odd) noexcept {
    if (!iso_fb_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
  }

  // The start of frame of an AUDIO configuration: the rate, then the packets that depend on it.
  void iso_sof() noexcept {
    iso_->on_sof(hw_.cycles(), hw_frame(hw_, 0));
    if (iso_in_on_) {
      if (!iso_in_seen_token_) {
        ++c_.iso_in_missed;
      }
      iso_in_seen_token_ = false;
      arm_iso_in();  // a no-op while the packet built at the last token is still waiting for this frame's token
    }
    arm_iso_fb();
  }

  void service_in() noexcept {
    service_cdc_in();
    service_midi_in();
    service_msc();
  }

  // ---- Mass Storage (DISK mode) ----------------------------------------------------------------------------------------------------
  // The interrupt only moves packets between the descriptors and the rings of msc_link.h; every engine call is the pump's.
  bool msc_on() const noexcept { return msc_ != nullptr && ctl_.config().mode == Mode::Disk; }

  // A bus reset or a configuration event: every MSC descriptor was taken back; the engine is reset by the pump, which sees the counter.
  // `bus` is a bus reset (CTL[ODDRST]: the controller's ping-pong pointers went back to even); a configuration event leaves them where they are (flush).
  void msc_session_reset(bool bus) noexcept {
    if (msc_ == nullptr) {
      return;
    }
    if (bus) {
      msc_out_pp_.reset();
      msc_in_pp_.reset();
    } else {
      msc_out_pp_.flush();
      msc_in_pp_.flush();
    }
    msc_arm_in_ = msc_->in_tail();
    msc_->note(MscLink::kAll);
  }

  void service_msc() noexcept {
    if (!msc_on() || !ctl_.configured()) {
      return;
    }
    apply_msc_out_stall();
    arm_msc_out();
    service_msc_in();
  }

  void apply_msc_out_stall() noexcept {
    const std::uint32_t word = msc_->out_stall_word();
    if ((word & 0xFFu) == msc_out_stall_seen_) {
      return;
    }
    msc_out_stall_seen_ = static_cast<std::uint8_t>(word & 0xFFu);
    if (static_cast<std::uint8_t>(word >> 8) == msc_->live_epoch()) {  // a request from before a reset is dropped
      halt_msc(layout::kEpMscOut);
      msc_->out_mark_drop();  // what the controller ACKed before this stall never reaches the engine (spec 3.4, amendment 1)
    }
  }

  void halt_msc(std::uint8_t address) noexcept {
    const unsigned number = address & 0x0Fu;
    hw_.endpt(number, static_cast<std::uint8_t>(hw_.endpt(number) | kEndptStall));
    ctl_.mark_halt(address);  // GET_STATUS agrees
    ++c_.msc_stalls;
    notify(UsbEvent::Halted, address);
  }

  // Zero copy: the slot at out_head + armed is the descriptor's buffer; the token completion publishes it. A full ring leaves the descriptors unarmed (the host is NAKed).
  void arm_msc_out() noexcept {
    if (ctl_.halted(layout::kEpMscOut)) {
      return;
    }
    while (msc_out_pp_.can_arm()) {
      const std::uint32_t head = msc_->out_head();
      if (head + msc_out_pp_.armed() - msc_->out_tail() >= MscLink::kSlots) {
        break;
      }
      const PingPong::Slot slot = msc_out_pp_.arm();
      give(entry(kMscOutNumber, false, slot.odd), msc_->out_data(head + msc_out_pp_.armed() - 1u), MscLink::kPacket, slot.data1);
    }
  }

  void token_msc_out(bool odd, std::uint32_t desc) noexcept {
    if (!msc_out_pp_.complete(odd)) {
      ++c_.pingpong_resyncs;
    }
    const std::uint32_t bytes = bd::bytes(desc) > MscLink::kPacket ? MscLink::kPacket : bd::bytes(desc);
    msc_->out_publish(bytes, msc_->live_epoch());
    ++c_.msc_out_packets;
    service_msc();
  }

  void token_msc_in(bool odd) noexcept {
    const unsigned armed = msc_in_pp_.armed();
    if (!msc_in_pp_.complete(odd) || armed == 0u) {
      ++c_.pingpong_resyncs;
    }
    if (armed != 0u) {
      msc_->in_set_tail(msc_in_idx_[0] + 1u);  // the slot is the controller's no more
      msc_in_idx_[0] = msc_in_idx_[1];
    }
    service_msc();
  }

  // Arms the published IN slots in order, at most two descriptors. A slot of another epoch is skipped (stale); a stall MARKER is applied only when nothing is armed, so the stall
  // reaches the wire after the last data packet. Bounded: at most one pass over the ring.
  void service_msc_in() noexcept {
    if (ctl_.halted(layout::kEpMscIn)) {
      return;  // a halted endpoint keeps its data in the ring: CLEAR_FEATURE takes the descriptors back and re-arms from the tail
    }
    const std::uint32_t events = msc_->events();
    const std::uint8_t epoch = MscLink::epoch_of(events);
    for (unsigned n = 0; n < MscLink::kSlots + 2u && msc_arm_in_ != msc_->in_head(); ++n) {
      const std::uint32_t index = msc_arm_in_;
      const bool stale = msc_->in_epoch(index) != epoch;
      if (msc_->in_kind(index) == MscLink::Kind::Stall) {
        if (msc_in_pp_.armed() != 0u) {
          break;
        }
        ++msc_arm_in_;
        if (stale || msc_->in_gen(index) != MscLink::count(events, MscLink::kClrIn)) {
          ++c_.msc_in_stale;
          continue;
        }
        halt_msc(layout::kEpMscIn);
        break;
      }
      if (stale) {
        ++msc_arm_in_;
        ++c_.msc_in_stale;
        continue;
      }
      if (!msc_in_pp_.can_arm()) {
        break;
      }
      const PingPong::Slot slot = msc_in_pp_.arm();
      msc_in_idx_[msc_in_pp_.armed() - 1u] = index;
      ++msc_arm_in_;
      give(entry(kMscInNumber, true, slot.odd), msc_->in_data(index), msc_->in_len(index), slot.data1);
      ++c_.msc_in_packets;
    }
    if (msc_in_pp_.armed() == 0u) {
      msc_->in_set_tail(msc_arm_in_);  // everything below is delivered or dropped
    }
  }

  void on_sof() noexcept {
    audio_.note_sof();
    if (ctl_.suspended()) {
      ctl_.on_resume();  // frames are flowing: the bus is awake, whether or not the resume flag was seen
      notify(UsbEvent::Resumed, 1);
    }
    if (iso_ != nullptr && ctl_.configured() && ctl_.config().mode == Mode::Audio) {
      iso_sof();
    }
    service_in();  // starts an idle IN endpoint within a millisecond of the first byte being written
    rearm_cdc_out();  // and takes the host off hold once the main loop has read the receive ring down
  }

  Hw& hw_;
  BdtTable& bdt_;
  ControlPipe& ctl_;
  Counters& audio_;
  CdcTxRing& cdc_tx_;
  CdcRxRing& cdc_rx_;
  MidiRing& midi_in_;
  MidiRing& midi_out_;
  UsbBuffers buf_{};
  PingPong ep0_tx_;
  // ISR only. The protocol stall of endpoint 0: Pending = EPSTALL is set and no handshake has been seen (stall_ep0), Latched = the first handshake cleared EPSTALL and the transmit descriptors still carry
  // OWN|STALL, so retried tokens are answered STALL (stall_seen); None again at the next SETUP (token_ep0) or a bus reset.
  enum class Ep0Stall : std::uint8_t { None, Pending, Latched };
  Ep0Stall ep0_stall_ = Ep0Stall::None;
  PingPong cdc_in_;
  PingPong cdc_out_;
  PingPong midi_in_pp_;
  PingPong midi_out_pp_;
  DeviceCounters c_;
  PingPong iso_in_pp_;
  PingPong iso_out_pp_;
  PingPong iso_fb_pp_;
  IsoAudio* iso_ = nullptr;
  MscLink* msc_ = nullptr;
  Ep0Trace* trace_ = nullptr;  // ISR only once the interrupt line is on (ep0_trace.h)
  PingPong msc_out_pp_;
  PingPong msc_in_pp_;
  std::uint32_t msc_arm_in_ = 0;         // ISR only: the next IN slot to arm (tail + armed, plus the stale ones skipped)
  std::uint32_t msc_in_idx_[2] = {};     // ISR only: the slots of the descriptors in flight, oldest first
  std::uint8_t msc_out_stall_seen_ = 0;  // ISR only: the last OUT stall request sequence number looked at
  bool iso_in_on_ = false;           // the stems stream runs (alt 1 on kIfAudioIn)
  bool iso_out_on_ = false;          // the return stream and its feedback run (alt 1 on kIfAudioOut)
  bool iso_in_seen_token_ = false;   // an IN token completed since the last start of frame
  bool rx_holding_ = false;  // the bulk OUT descriptors are being held back (rearm_cdc_out)
  UsbEventHook hook_ = nullptr;
  void* hook_context_ = nullptr;
};

}  // namespace o3sdk::usb
