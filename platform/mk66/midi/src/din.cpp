// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DIN emitter (midi/din.h): which byte goes to the UART next, and when. Everything here is bounded: poll() makes at most one decision per
// call and pops at most a few stale items, next_wake() peeks the two queue heads.
#include <o3sdk/platform/mk66/midi/din.h>

namespace o3sdk::platform::mk66::midi {

namespace {
constexpr std::uint8_t kChunk = 3;
}

// GCC 13 reads the slot store of core::SpscRing::push, inlined into the three push functions below, as a write past the atomic beside the ring ("writing 1 byte into
// a region of size 0"): a false positive, the index is `head & (N - 1)` into an array of N. The diagnostic is silenced for these functions only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"

bool DinEmitter::push_rt(std::uint64_t due, std::uint8_t byte) noexcept {
  if (!is_realtime(byte)) {
    return false;
  }
  OutItem it = out_item(due, msg1(byte));
  it.epoch = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  return rt_.push(it);
}

bool DinEmitter::push_msg(std::uint64_t due, const Msg& m) noexcept {
  if (m.len < 1u || m.len > 3u || is_realtime(m.b[0]) || m.b[0] == kSysExStart || m.b[0] == kSysExEnd) {
    return false;
  }
  OutItem it = out_item(due, m);
  it.epoch = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  return msg_.push(it);
}

bool DinEmitter::push_sysex(std::uint64_t due, const std::uint8_t* data, std::uint32_t n) noexcept {
  const std::uint32_t bytes = n + 2u;  // F0 ... F7
  const std::uint32_t chunks = (bytes + kChunk - 1u) / kChunk;
  if (chunks > 255u || chunks > msg_room() || (n != 0u && data == nullptr)) {
    return false;
  }
  const std::uint8_t ep = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  std::uint32_t k = 0;  // index in the stream F0, data..., F7
  for (std::uint32_t c = 0; c < chunks; ++c) {
    OutItem it{};
    it.due = due;
    it.run = c == 0u ? static_cast<std::uint8_t>(chunks) : std::uint8_t{0};
    it.epoch = ep;
    std::uint8_t len = 0;
    for (std::uint32_t j = 0; j < kChunk && k < bytes; ++j, ++k) {
      std::uint8_t v;
      if (k == 0u) {
        v = kSysExStart;
      } else if (k == bytes - 1u) {
        v = kSysExEnd;
      } else {
        v = static_cast<std::uint8_t>(data[k - 1u] & 0x7Fu);
      }
      it.b[len++] = v;
    }
    it.len = len;
    if (!msg_.push(it)) {  // cannot happen: the room was checked and the ring only drains meanwhile
      return false;
    }
  }
  return true;
}

#pragma GCC diagnostic pop

std::uint64_t DinEmitter::free_at() const noexcept {
  const std::uint32_t depth = cfg_.commit_depth < 1u ? 1u : (cfg_.commit_depth > 2u ? 2u : cfg_.commit_depth);
  const std::uint64_t held = din::kByteCycles * (depth - 1u);
  return busy_until_ > held ? busy_until_ - held : 0u;
}

// When something due at `due` can be started, given the earliest the UART takes a byte (`fa`): a byte committed ahead starts when the one before
// it ends (busy_until_), so an item already due by then is ready at `fa`; a later one at its own time.
std::uint64_t DinEmitter::ready_time(std::uint64_t due, std::uint64_t fa) const noexcept { return due <= busy_until_ ? fa : due; }

void DinEmitter::sync_epoch() noexcept {
  const std::uint8_t ep = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  if (message_open() && cur_epoch_ != ep) {
    // The producer flushed: this message is cut. A SysEx is closed with F7 so that the receiver does not wait for it forever.
    cur_pos_ = 0;
    cur_len_ = 0;
    chunks_left_ = 0;
    if (cur_sysex_) {
      cur_[0] = kSysExEnd;
      cur_len_ = 1;
      cur_sysex_ = false;
      cur_epoch_ = ep;
    }
  }
}

void DinEmitter::fill_rt_head(std::uint8_t ep) noexcept {
  if (rt_head_valid_ && rt_head_.epoch != ep) {
    rt_head_valid_ = false;
  }
  while (!rt_head_valid_ && rt_.pop(rt_head_)) {
    rt_head_valid_ = rt_head_.epoch == ep;
  }
}

void DinEmitter::fill_msg_head(std::uint8_t ep) noexcept {
  if (msg_head_valid_ && msg_head_.epoch != ep) {
    msg_head_valid_ = false;
  }
  while (!msg_head_valid_ && msg_.pop(msg_head_)) {
    // A stale item (flushed) or a continuation chunk with no start (the start was flushed) is dropped.
    msg_head_valid_ = msg_head_.epoch == ep && msg_head_.run != 0u;
  }
}

bool DinEmitter::ordinary_held_by_guard(std::uint64_t start) noexcept {
  if (!cfg_.guard_window || !rt_head_valid_) {
    return false;
  }
  // An ordinary byte starting at `start` would still be on the wire when the real-time byte falls due.
  if (rt_head_.due > start && start + din::kByteCycles > rt_head_.due) {
    ++stats_.guard_holds;
    return true;
  }
  return false;
}

namespace {
// Field by field: the aggregate `Tx{false, false, 0, 0, 0}` is a memset call at -Os, and this function runs from SRAM_L, which may not call flash (tools/check_ramfunc.py).
O3SDK_AUDIO_INLINE DinEmitter::Tx no_byte() noexcept {
  DinEmitter::Tx t;
  t.valid = false;
  t.realtime = false;
  t.byte = 0;
  t.start = 0;
  t.due = 0;
  return t;
}
}  // namespace

DinEmitter::Tx DinEmitter::poll(std::uint64_t now) noexcept {
  const Tx none = no_byte();
  if (now < free_at()) {
    return none;  // the UART's holding register is full: the transmit-complete interrupt will come
  }
  const std::uint64_t start = now > busy_until_ ? now : busy_until_;
  sync_epoch();
  const std::uint8_t ep = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  fill_rt_head(ep);

  // 1. A real-time byte whose time has come goes first, in the middle of a message if need be.
  if (rt_head_valid_ && rt_head_.due <= start) {
    const Tx t{true, true, rt_head_.b[0], start, rt_head_.due};
    rt_head_valid_ = false;
    const std::uint64_t late = start - t.due;
    ++stats_.rt_sent;
    if (message_open()) {
      ++stats_.rt_inserted;
    }
    if (late > stats_.rt_lateness_max) {
      stats_.rt_lateness_max = late;
    }
    stats_.rt_lateness_sum += late;
    if (late > din::kByteCycles) {
      ++stats_.rt_late_over_byte;
    }
    busy_until_ = start + din::kByteCycles;
    return t;
  }

  // 2. The message in progress sends its next byte, back to back.
  if (message_open()) {
    if (cur_pos_ >= cur_len_) {
      // The next chunk of a run: the run was whole in the ring when it started, so it is there; if it is not (a flush took the rest) the
      // message ends here.
      OutItem next;
      if (chunks_left_ == 0u || !msg_.pop(next) || next.epoch != ep) {
        chunks_left_ = 0;
        cur_pos_ = cur_len_ = 0;
        if (cur_sysex_) {
          cur_[0] = kSysExEnd;
          cur_len_ = 1;
          cur_sysex_ = false;
          cur_epoch_ = ep;
        } else {
          return none;
        }
      } else {
        --chunks_left_;
        cur_len_ = next.len;
        cur_pos_ = 0;
        for (std::uint32_t i = 0; i < 3u; ++i) {
          cur_[i] = next.b[i];
        }
      }
    }
    if (ordinary_held_by_guard(start)) {
      return none;
    }
    const std::uint8_t byte = cur_[cur_pos_++];
    if (byte == kSysExEnd) {
      cur_sysex_ = false;
    }
    ++stats_.msg_bytes;
    busy_until_ = start + din::kByteCycles;
    return Tx{true, false, byte, start, cur_due_};
  }

  // 3. A message whose time has come starts (when the whole run is in the ring).
  fill_msg_head(ep);
  if (msg_head_valid_ && msg_head_.due <= start && msg_.size() + 1u >= msg_head_.run) {
    if (ordinary_held_by_guard(start)) {
      return none;
    }
    for (std::uint32_t i = 0; i < 3u; ++i) {
      cur_[i] = msg_head_.b[i];
    }
    cur_len_ = msg_head_.len;
    cur_pos_ = 0;
    chunks_left_ = msg_head_.run > 1u ? msg_head_.run - 1u : 0u;
    cur_due_ = msg_head_.due;
    cur_epoch_ = msg_head_.epoch;
    cur_sysex_ = msg_head_.b[0] == kSysExStart;
    msg_head_valid_ = false;
    const std::uint8_t byte = cur_[cur_pos_++];
    if (byte == kSysExEnd) {
      cur_sysex_ = false;
    }
    ++stats_.msg_bytes;
    busy_until_ = start + din::kByteCycles;
    return Tx{true, false, byte, start, cur_due_};
  }
  return none;
}

std::uint64_t DinEmitter::next_wake(std::uint64_t now) noexcept {
  sync_epoch();
  const std::uint8_t ep = static_cast<std::uint8_t>(epoch_.load(std::memory_order_acquire));
  fill_rt_head(ep);
  const std::uint64_t fa = free_at();
  std::uint64_t best = kNever;
  const auto consider = [&best, now](std::uint64_t t) {
    const std::uint64_t at = t > now ? t : now;
    if (at < best) {
      best = at;
    }
  };
  if (rt_head_valid_) {
    consider(ready_time(rt_head_.due, fa));
  }
  std::uint64_t ordinary = kNever;
  if (message_open()) {
    ordinary = fa;
  } else {
    fill_msg_head(ep);
    if (msg_head_valid_ && msg_.size() + 1u >= msg_head_.run) {
      ordinary = ready_time(msg_head_.due, fa);
    }
  }
  if (ordinary != kNever) {
    const std::uint64_t at = ordinary > now ? ordinary : now;
    const std::uint64_t start = at > busy_until_ ? at : busy_until_;
    if (cfg_.guard_window && rt_head_valid_ && rt_head_.due > start && start + din::kByteCycles > rt_head_.due) {
      // Held until the real-time byte has gone: it is the real-time candidate above that wakes the emitter, and the ordinary byte follows it.
      consider(rt_head_.due);
    } else {
      consider(ordinary);
    }
  }
  return best;
}

}  // namespace o3sdk::platform::mk66::midi
