// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI note bookkeeping, so that no note hangs (OS-005c; PAR-063, "MIDI note hangs fixed": a sample instrument that follows a MIDI note without
// an explicit Note OFF, a muted track, stuck notes of an external controller). For every channel and note it counts the notes that were sent on
// and not yet off, so that a stop, a mute, a panic or an instrument change can send exactly the note-offs that are owed, and no others. Two
// tracks may sound the same note on the same channel: each owes its own off, so it is a count and not a flag (a saturating byte: 255 owed
// offs of one note are a runaway sender, and the count stops there).
//
// Pure data, no I/O: the router (midi/out.h) feeds it every note it queues and asks it, a few at a time (the work per block is bounded), which
// notes are still owed. 16 channels x 128 notes = 2 KiB of counts and 256 bytes of "is any owed" bits, so that finding the next owed note
// looks at no more than 64 words.

#include <cstdint>

namespace o3sdk::platform::mk66::midi {

class HangBook {
 public:
  static constexpr std::uint32_t kChannels = 16;
  static constexpr std::uint32_t kNotes = 128;
  static constexpr std::uint32_t kCells = kChannels * kNotes;
  static constexpr std::uint32_t kWords = kCells / 32u;

  void clear() noexcept {
    for (std::uint32_t i = 0; i < kCells; ++i) {
      count_[i] = 0;
    }
    for (std::uint32_t i = 0; i < kWords; ++i) {
      bits_[i] = 0;
    }
    owed_ = 0;
    cursor_ = 0;
  }

  // A note-on went out: one more off is owed. Channel 0..15, note 0..127 (masked).
  void note_on(std::uint8_t ch, std::uint8_t note) noexcept {
    const std::uint32_t i = index(ch, note);
    if (count_[i] != 0xFFu) {
      ++count_[i];
      ++owed_;
      bits_[i / 32u] |= 1u << (i % 32u);
    }
  }
  // A note-off went out: an owed off is paid. False when none was owed (an off for a note that is not sounding: harmless, the caller may
  // still send it).
  bool note_off(std::uint8_t ch, std::uint8_t note) noexcept {
    const std::uint32_t i = index(ch, note);
    if (count_[i] == 0u) {
      return false;
    }
    --count_[i];
    --owed_;
    if (count_[i] == 0u) {
      bits_[i / 32u] &= ~(1u << (i % 32u));
    }
    return true;
  }

  std::uint32_t owed() const noexcept { return owed_; }
  std::uint8_t count(std::uint8_t ch, std::uint8_t note) const noexcept { return count_[index(ch, note)]; }

  // The next sounding note at or after the sweep cursor, in channel and note order, wrapping once; forgets all of its owed offs (the caller sends
  // ONE off, which silences the note however many note-ons it got: the receiver has one voice for it, and a panic is not the place to count).
  // False when nothing is owed. Only the channels whose bit is set in `channels` are considered (bit 0 is channel 0). Bounded: at most kWords + 1
  // words are looked at.
  bool take_next(std::uint8_t& ch, std::uint8_t& note, std::uint16_t channels = 0xFFFFu) noexcept {
    if (owed_ == 0u) {
      return false;
    }
    const std::uint32_t first = cursor_ / 32u;
    for (std::uint32_t n = 0; n <= kWords; ++n) {
      const std::uint32_t w = (first + n) % kWords;
      if (((channels >> (w / (kNotes / 32u))) & 1u) == 0u) {
        continue;  // a word holds 32 notes of one channel
      }
      std::uint32_t bits = bits_[w];
      if (n == 0u) {
        bits &= ~0u << (cursor_ % 32u);  // the cells before the cursor in its own word wait for the wrap
      } else if (n == kWords) {
        bits &= (cursor_ % 32u) == 0u ? 0u : ~(~0u << (cursor_ % 32u));
      }
      if (bits != 0u) {
        const std::uint32_t bit = static_cast<std::uint32_t>(__builtin_ctz(bits));
        const std::uint32_t i = w * 32u + bit;
        owed_ -= count_[i];
        count_[i] = 0;
        bits_[w] &= ~(1u << bit);
        cursor_ = (i + 1u) % kCells;
        ch = static_cast<std::uint8_t>(i / kNotes);
        note = static_cast<std::uint8_t>(i % kNotes);
        return true;
      }
    }
    return false;  // nothing owed on the wanted channels
  }

 private:
  static constexpr std::uint32_t index(std::uint8_t ch, std::uint8_t note) noexcept {
    return static_cast<std::uint32_t>(ch & 0x0Fu) * kNotes + (note & 0x7Fu);
  }
  std::uint8_t count_[kCells] = {};
  std::uint32_t bits_[kWords] = {};
  std::uint32_t owed_ = 0;
  std::uint32_t cursor_ = 0;
};

}  // namespace o3sdk::platform::mk66::midi
