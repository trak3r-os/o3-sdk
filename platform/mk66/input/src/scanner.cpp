// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/input/scanner.h>

#include <o3sdk/hal/leds.h>
#include <o3sdk/platform/mk66/input/pins.h>

namespace o3sdk::platform::mk66::input {

Scanner::Scanner(I2cBus& pad_bus, I2cBus& control_bus, ScanHardware& hardware, const hal::Clock& clock) noexcept
    : hw_(hardware),
      clock_(clock),
      pads_buttons_{Source(pad_bus, pins::kKeyScannerAddress, core::InputKind::Pad, pad_of_key, hal::InputStats::kPadsOnline),
                    Source(control_bus, pins::kKeyScannerAddress, core::InputKind::Button, button_of_key,
                           hal::InputStats::kButtonsOnline)},
      detents_(0) {}

void Scanner::begin() noexcept {
  const core::Tick now = clock_.now();
  for (Source& s : pads_buttons_) {
    for (unsigned attempt = 0; attempt < scan::kConfigureAttempts && !s.configured; ++attempt) {
      s.configured = s.chip.configure() == I2cResult::Ok;
      if (!s.configured) {
        ++errors_;
        (void)s.bus.recover();
      }
    }
    s.next_poll = now;
    s.health_at = now + scan::kHealthUs;
    if (s.configured) {
      s.more = true;  // a key held at power-up is already in the FIFO
    } else {
      s.lost = true;
      s.failures = scan::kConfigureAttempts;
      s.backoff_us = scan::kBackoffMinUs;
      s.retry_at = now + s.backoff_us;
    }
  }
  detents_.restart(hw_.encoder_count());
}

void Scanner::service() noexcept {
  const core::Tick now = clock_.now();
  refresh_stamp(now);
  // The control buttons first: when a modifier (Shift) and a pad arrive in the same millisecond there is no telling which came first (two
  // chips, no time stamps), and a pad that comes first would lose its Shift. The modifier first is the one reading that makes sense.
  service_source(pads_buttons_[kButtons], hw_.buttons_int_asserted(), now);
  service_source(pads_buttons_[kPads], hw_.pads_int_asserted(), now);
  publish_encoder(now);
}

void Scanner::service_source(Source& s, bool int_asserted, core::Tick now) noexcept {
  if (!s.configured) {
    if (core::tick_reached(now, s.retry_at)) {
      configure(s, now);
    }
  } else if (s.more || int_asserted || core::tick_reached(now, s.next_poll)) {
    drain(s, now);  // a failure clears `configured`, so the backoff gate above is the only one a failed chip needs
  }
  if (s.configured && core::tick_reached(now, s.health_at)) {
    check_config(s, now);
  }
  s.filter.service(now);
  if (s.resync_pending && s.configured && !s.more) {
    resync_lost_events(s);
  }
  publish(s);
}

void Scanner::configure(Source& s, core::Tick now) noexcept {
  const I2cResult r = s.chip.configure();
  if (r != I2cResult::Ok) {
    fail(s, now);
    return;
  }
  if (s.lost && s.failures >= scan::kRecoverAfterFailures) {
    resync_all(s);  // the chip was away for a while: whatever it held or released meanwhile is unknown
  }
  s.configured = true;
  s.lost = false;
  s.failures = 0;
  s.backoff_us = 0;
  s.more = true;  // read the FIFO next pass whatever INT says
  s.next_poll = now + scan::kSafetyPollUs;
  s.health_at = now + scan::kHealthUs;
}

void Scanner::drain(Source& s, core::Tick now) noexcept {
  RawKey events[scan::kMaxEventsPerDrain];
  Tca8418::Drain result;
  const I2cResult r = s.chip.drain(events, scan::kMaxEventsPerDrain, result);
  for (unsigned i = 0; i < result.stored; ++i) {  // what was read before a failure is real
    const std::uint8_t id = s.map(events[i].key);
    if (id == kNoKey) {
      ++unmapped_;  // a matrix position with no switch: never expected, counted with the filtered edges so it shows
      continue;
    }
    s.filter.edge(id, events[i].down);
    s.touched |= std::uint64_t{1} << id;  // meaningful only while a resync is pending: these keys' latest state came from the chip
  }
  // The overflow flag was read (and cleared) before the transfer that may have failed, and an event read that failed may have popped its
  // event: both are carried to the resync whether or not the drain went through, or the lost events would be forgotten with the failure.
  s.resync_pending = s.resync_pending || result.overflow || result.maybe_lost;
  if (!s.resync_pending) {
    s.touched = 0;
  }
  if (r != I2cResult::Ok) {
    fail(s, now);
    return;
  }
  s.lost = false;
  s.failures = 0;
  s.backoff_us = 0;
  s.more = result.more;
  s.next_poll = now + scan::kSafetyPollUs;
}

// Asks the chip whether it still holds its configuration. A chip back at its reset value lost its FIFO with its power: it is
// configured again and the keys believed down are released after it (the resync in configure()); a chip in any other state is only
// configured again.
void Scanner::check_config(Source& s, core::Tick now) noexcept {
  s.health_at = now + scan::kHealthUs;
  std::uint8_t cfg = 0;
  const I2cResult r = s.chip.read_config(cfg);
  if (r != I2cResult::Ok) {
    fail(s, now);
    return;
  }
  if (cfg == (tca8418::kCfgValue & tca8418::kCfgVerifyMask)) {
    return;
  }
  s.configured = false;
  s.lost = cfg == 0u;
  s.failures = s.lost ? scan::kRecoverAfterFailures : 0u;
  s.retry_at = now;
}

// A transfer failed: the chip may have been reset, so it is configured again before it is trusted; the bus is recovered from the
// second failure in a row; the next attempt waits for the backoff.
void Scanner::fail(Source& s, core::Tick now) noexcept {
  ++errors_;
  ++s.failures;
  s.lost = true;
  s.configured = false;
  s.more = false;
  if (s.failures >= scan::kRecoverAfterFailures) {
    (void)s.bus.recover();
  }
  s.backoff_us = s.backoff_us == 0u ? scan::kBackoffMinUs
                                    : (s.backoff_us * 2u < scan::kBackoffMaxUs ? s.backoff_us * 2u : scan::kBackoffMaxUs);
  s.retry_at = now + s.backoff_us;
}

// The chip's FIFO overflowed, or an event read failed: events were lost. With the overflow mode that is set (tca8418.h kCfgOvrFlowM) a
// full FIFO pushes its OLDEST event out, so what the chip kept is the newest ten: for the keys they name, the last of them is the
// truth, and it has been applied by now (the FIFO is empty and the edges accepted). Any other key believed down may have lost its
// release, and a release that never arrives would leave the key, Shift above all, down for ever: those are let go, through the
// filter's normal path.
void Scanner::resync_lost_events(Source& s) noexcept {
  s.resync_pending = false;
  ++resyncs_;
  (void)s.filter.release_except(s.touched);
  s.touched = 0;
}

// The chip was lost (a run of failed transfers, or a power glitch found by the read-back): its FIFO and the keys in it are gone, and
// nothing that was believed down can be trusted.
void Scanner::resync_all(Source& s) noexcept {
  s.resync_pending = false;
  ++resyncs_;
  (void)s.filter.release_all();
  s.touched = 0;
}

// last_stamp_ is at most `now` and normally within a pass of it. After a silence its distance from `now` can pass 2^31 us, where
// tick_reached() reads it as being in the future and keeps it: every event would then carry a stamp ahead of the clock.
void Scanner::refresh_stamp(core::Tick now) noexcept {
  if (core::tick_elapsed(now, last_stamp_) >= scan::kStampStaleUs) {
    last_stamp_ = now;  // queued events carry stamps no later than the old one, so the order is kept
  }
}

core::Tick Scanner::stamp(core::Tick time) noexcept {
  if (core::tick_reached(time, last_stamp_)) {
    last_stamp_ = time;
  }
  return last_stamp_;
}

void Scanner::publish(Source& s) noexcept {
  unsigned key = 0;
  bool down = false;
  core::Tick time = 0;
  while (s.filter.next_unpublished(key, down, time)) {
    const auto id = static_cast<std::uint8_t>(key);
    const core::Tick when = stamp(time);
    const core::InputEvent event = s.kind == core::InputKind::Pad
                                       ? core::make_pad_event(when, id, down ? kPadVelocity : std::int16_t{0})
                                       : core::make_button_event(when, id, down);
    if (!queue_.push(event)) {
      ++dropped_;  // drop the newest and count: the edge stays unpublished and is offered again next pass
      return;
    }
    s.filter.mark_published(key);
  }
}

void Scanner::publish_encoder(core::Tick now) noexcept {
  const int counted = detents_.feed(hw_.encoder_count());
  pending_detents_ += kEncoderCountUpIsClockwise ? counted : -counted;
  if (pending_detents_ == 0) {
    return;
  }
  const int chunk = pending_detents_ > 32767 ? 32767 : (pending_detents_ < -32767 ? -32767 : pending_detents_);
  if (queue_.push(core::make_encoder_event(stamp(now), kEncoderId, static_cast<std::int16_t>(chunk)))) {
    pending_detents_ -= chunk;
  } else {
    ++dropped_;  // the turns are kept and offered again: a full queue costs latency, not detents
  }
}

bool Scanner::inject(core::InputKind kind, std::uint8_t id, std::int16_t value) noexcept {
  core::InputEvent event;
  refresh_stamp(clock_.now());
  switch (kind) {
    case core::InputKind::Button:
      if (id >= kButtonCount || value < 0 || value > 1) {
        return false;
      }
      event = core::make_button_event(stamp(clock_.now()), id, value != 0);
      break;
    case core::InputKind::Pad:
      if (id >= hal::kPadLedCount || value < 0 || value > 127) {
        return false;
      }
      event = core::make_pad_event(stamp(clock_.now()), id, value);
      break;
    case core::InputKind::Encoder:
      if (id != 0u || value == 0) {
        return false;
      }
      event = core::make_encoder_event(stamp(clock_.now()), id, value);
      break;
    default:
      return false;
  }
  if (!queue_.push(event)) {
    ++dropped_;
    return false;
  }
  if (kind == core::InputKind::Pad) {
    const std::uint64_t bit = std::uint64_t{1} << id;
    injected_pads_ = value != 0 ? (injected_pads_ | bit) : (injected_pads_ & ~bit);
  }
  return true;
}

hal::InputStats Scanner::stats() const noexcept {
  hal::InputStats s;
  s.dropped = dropped_;
  s.errors = errors_;
  s.filtered = pads_buttons_[kPads].filter.filtered() + pads_buttons_[kButtons].filter.filtered() + unmapped_;
  s.resyncs = resyncs_;
  for (const Source& source : pads_buttons_) {
    if (source.configured) {
      s.online = static_cast<std::uint8_t>(s.online | source.online_bit);
    }
  }
  return s;
}

}  // namespace o3sdk::platform::mk66::input
