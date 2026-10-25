// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U1c part 2: the ISR-to-main link of the Mass Storage function (docs/specs/OS-USBDISK.md 3.4, shape (a)). msc::Engine is single-context (the main loop); the USB interrupt
// only moves packets between the controller and the two rings below, and MscPump (main loop, once a pass) is the only caller of the engine.
//
// Who writes what (single writer per field; the index words are release/acquire atomics, the slot payloads are plain bytes published by them):
//   OUT ring  (host -> device): the ISR writes out_head (at a token completion, after len/epoch/data) and the descriptor of the slot it armed; the pump writes out_tail.
//   IN ring   (device -> host): the pump writes in_head (after the slot's data/len/epoch/kind); the ISR writes in_tail, and only when a descriptor completes (or nothing is armed).
//             A slot of kind Stall is the IN stall MARKER: the ISR applies EPSTALL when it reaches it with nothing armed, which is the "after the last IN packet completes" rule.
//   events_   four 8-bit counters in ONE word: all (bus reset / SET_CONFIGURATION), bot (Bulk-Only Reset), clr_in, clr_out (CLEAR_FEATURE ENDPOINT_HALT). The ISR rewrites it with a
//             single store; the pump reads it once per pass.
//   out_drop_to_ the OUT head when the ISR applied an OUT stall (ISR writes): slots below it are older than the stall and never reach the engine.
//   out_stall_ the pump's request to halt the OUT endpoint: (epoch << 8) | sequence number; the ISR applies a new sequence number whose epoch is the live one.
// Stale data is dropped by the epoch ((all + bot) mod 256): the pump stamps IN slots with its snapshot's epoch and the ISR skips slots whose stamp is not the live one; the ISR
// stamps OUT packets with the live epoch and the pump drops older stamps and waits on newer ones. CLEAR_FEATURE alone never drops queued data.
// Memory: 2 x 8 x 64 = 1024 payload bytes plus about 60 bytes of bookkeeping. Worst-case latency of a packet: one ISR entry (the ISR arms on every token, SOF and kick) plus one main-loop pass.

#include <o3sdk/usb/msc.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::usb {

class MscLink {
 public:
  static constexpr std::uint32_t kSlots = 8;     // per ring, a power of two
  static constexpr std::uint32_t kPacket = 64;   // full speed bulk
  static_assert((kSlots & (kSlots - 1u)) == 0u, "slot indices wrap by masking");

  enum class Kind : std::uint8_t { Data, Stall };

  // ---- events (ISR writes, pump reads) ----
  static constexpr std::uint32_t kAll = 0, kBot = 8, kClrIn = 16, kClrOut = 24;  // bit offsets of the four counters
  std::uint32_t events() const noexcept { return events_.load(std::memory_order_acquire); }
  static constexpr std::uint8_t count(std::uint32_t word, std::uint32_t which) noexcept { return static_cast<std::uint8_t>(word >> which); }
  static constexpr std::uint8_t epoch_of(std::uint32_t word) noexcept { return static_cast<std::uint8_t>(count(word, kAll) + count(word, kBot)); }
  std::uint8_t live_epoch() const noexcept { return epoch_of(events()); }
  void note(std::uint32_t which) noexcept {  // ISR only
    const std::uint32_t w = events_.load(std::memory_order_relaxed);
    const std::uint32_t c = (w >> which) & 0xFFu;
    events_.store((w & ~(0xFFu << which)) | (((c + 1u) & 0xFFu) << which), std::memory_order_release);
  }

  // ---- OUT ring ----
  std::uint8_t* out_data(std::uint32_t index) noexcept { return out_data_[index % kSlots]; }
  std::uint32_t out_len(std::uint32_t index) const noexcept { return out_len_[index % kSlots]; }
  std::uint8_t out_epoch(std::uint32_t index) const noexcept { return out_epoch_[index % kSlots]; }
  std::uint32_t out_head() const noexcept { return out_head_.load(std::memory_order_acquire); }
  std::uint32_t out_tail() const noexcept { return out_tail_.load(std::memory_order_acquire); }
  void out_publish(std::uint32_t len, std::uint8_t epoch) noexcept {  // ISR: the slot at out_head
    const std::uint32_t h = out_head_.load(std::memory_order_relaxed);
    out_len_[h % kSlots] = static_cast<std::uint8_t>(len);
    out_epoch_[h % kSlots] = epoch;
    out_head_.store(h + 1u, std::memory_order_release);
  }
  // The ISR applied an OUT stall: every slot below `out_head` at that moment is older than the stall (spec 3.4, amendment 1); the pump drops them. ISR writes, pump reads.
  std::uint32_t out_drop_to() const noexcept { return out_drop_to_.load(std::memory_order_acquire); }
  void out_mark_drop() noexcept { out_drop_to_.store(out_head_.load(std::memory_order_relaxed), std::memory_order_release); }
  void out_release() noexcept { out_tail_.store(out_tail_.load(std::memory_order_relaxed) + 1u, std::memory_order_release); }  // pump

  // ---- IN ring ----
  std::uint8_t* in_data(std::uint32_t index) noexcept { return in_data_[index % kSlots]; }
  std::uint32_t in_len(std::uint32_t index) const noexcept { return in_len_[index % kSlots]; }
  std::uint8_t in_epoch(std::uint32_t index) const noexcept { return in_epoch_[index % kSlots]; }
  Kind in_kind(std::uint32_t index) const noexcept { return in_kind_[index % kSlots]; }
  std::uint8_t in_gen(std::uint32_t index) const noexcept { return in_gen_[index % kSlots]; }
  std::uint32_t in_head() const noexcept { return in_head_.load(std::memory_order_acquire); }
  std::uint32_t in_tail() const noexcept { return in_tail_.load(std::memory_order_acquire); }
  void in_set_tail(std::uint32_t t) noexcept { in_tail_.store(t, std::memory_order_release); }  // ISR
  // Pump: publishes the slot at in_head. `gen` is the clr_in counter of the pump's snapshot (a marker only applies while it is still the live one).
  void in_publish(Kind kind, std::uint32_t len, std::uint8_t epoch, std::uint8_t gen) noexcept {
    const std::uint32_t h = in_head_.load(std::memory_order_relaxed);
    in_kind_[h % kSlots] = kind;
    in_len_[h % kSlots] = static_cast<std::uint8_t>(len);
    in_epoch_[h % kSlots] = epoch;
    in_gen_[h % kSlots] = gen;
    in_head_.store(h + 1u, std::memory_order_release);
  }

  // ---- OUT stall request (pump writes, ISR reads) ----
  void post_out_stall(std::uint8_t epoch) noexcept {
    out_stall_seq_ = static_cast<std::uint8_t>(out_stall_seq_ + 1u);
    out_stall_.store((static_cast<std::uint32_t>(epoch) << 8) | out_stall_seq_, std::memory_order_release);
  }
  std::uint32_t out_stall_word() const noexcept { return out_stall_.load(std::memory_order_acquire); }

 private:
  std::atomic<std::uint32_t> events_{0};
  std::atomic<std::uint32_t> out_head_{0}, out_tail_{0}, in_head_{0}, in_tail_{0}, out_stall_{0}, out_drop_to_{0};
  std::uint8_t out_stall_seq_ = 0;  // pump only
  std::uint8_t out_len_[kSlots] = {}, out_epoch_[kSlots] = {};
  std::uint8_t in_len_[kSlots] = {}, in_epoch_[kSlots] = {}, in_gen_[kSlots] = {};
  Kind in_kind_[kSlots] = {};
  alignas(4) std::uint8_t out_data_[kSlots][kPacket] = {};  // the controller's DMA writes here (zero copy)
  alignas(4) std::uint8_t in_data_[kSlots][kPacket] = {};   // and reads here
};
static_assert(alignof(MscLink) >= 4, "the packet buffers are word aligned by declaration");

// The main-loop half: runs the engine. Once per pass while the interface is configured in DISK mode. `kick` (optional) pends the USB interrupt so a published IN packet
// does not wait for the next start of frame.
class MscPump {
 public:
  using Kick = void (*)();
  MscPump(MscLink& link, msc::Engine& engine, Kick kick = nullptr) noexcept : link_(link), engine_(engine), kick_(kick) {}
  // One pass: the events, then up to kMaxRounds rounds of {OUT packets into the engine, engine.service() (at most one block of card I/O), the OUT stall, IN packets out of the
  // engine} while a round makes progress.
  void step() noexcept;
  static constexpr unsigned kMaxRounds = 4;
  std::uint8_t epoch() const noexcept { return MscLink::epoch_of(snap_); }
  std::uint32_t dropped_out() const noexcept { return dropped_out_; }

 private:
  MscLink& link_;
  msc::Engine& engine_;
  Kick kick_;
  std::uint32_t snap_ = 0;
  bool in_stall_posted_ = false;
  bool out_stall_posted_ = false;
  std::uint32_t dropped_out_ = 0;

  bool round(std::uint8_t epoch) noexcept;
};

}  // namespace o3sdk::usb
