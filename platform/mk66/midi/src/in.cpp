// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI in, the timing side (midi/in.h): the byte parser and the drain of the midiRt ring.
#include <o3sdk/platform/mk66/midi/in.h>

namespace o3sdk::platform::mk66::midi {

InParser::Out InParser::feed(std::uint8_t b) noexcept {
  // A real-time byte may sit anywhere, between the bytes of a message too, and does not disturb it.
  if (is_realtime(b)) {
    switch (b) {
      case kClock:
        return Out{ByteKind::Clock, 0};
      case kStart:
        return Out{ByteKind::Start, 0};
      case kContinue:
        return Out{ByteKind::Continue, 0};
      case kStop:
        return Out{ByteKind::Stop, 0};
      default:
        return Out{ByteKind::Pass, 0};  // active sensing, reset, the undefined ones: not ours
    }
  }
  if (is_status(b)) {
    sysex_ = b == kSysExStart;
    have_ = 0;
    if (b == kSongPosition) {
      status_ = b;
      need_ = 2;
      return Out{ByteKind::Consumed, 0};
    }
    // Everything else, a message of someone else's: remember it for running status (channel voice) and let it through.
    if (b >= 0xF0u) {
      status_ = 0;  // system common and SysEx framing do not start a running status
      need_ = 0;
    } else {
      status_ = b;
      need_ = data_bytes_of(b);
    }
    return Out{ByteKind::Pass, 0};
  }
  // A data byte.
  if (sysex_) {
    return Out{ByteKind::Pass, 0};
  }
  if (status_ == kSongPosition) {
    d_[have_++] = b;
    if (have_ == need_) {
      have_ = 0;
      status_ = 0;
      return Out{ByteKind::Spp, static_cast<std::uint16_t>(d_[0] | (d_[1] << 7))};
    }
    return Out{ByteKind::Consumed, 0};
  }
  if (status_ != 0u && need_ != 0u) {
    if (++have_ == need_) {
      have_ = 0;  // running status: the next data byte starts another message of the same kind
    }
  }
  return Out{ByteKind::Pass, 0};
}

void MidiIn::reset() noexcept {
  for (InParser& p : parser_) {
    p.reset();
  }
  armed_ = Armed::None;
  have_held_ = false;
}

MidiIn::Drained MidiIn::drain(RtInRing& ring, const ClockMap& map, ClockDll& dll, std::uint32_t max_items, TransportEvent* ev,
                              std::uint32_t ev_cap, RtItem* pass, std::uint32_t pass_cap) noexcept {
  Drained d;
  while (d.items < max_items) {
    RtItem it;
    if (have_held_) {
      it = held_;
    } else if (!ring.pop(it)) {
      break;
    }
    const std::uint32_t port = it.port < kPortCount ? it.port : 0u;

    // Look at the byte without committing the parser, to know whether there is room for what it makes: a clock makes no output, a transport
    // byte one event, a byte that is not timing one `pass` entry. A byte that has no room waits (held) for the next block.
    InParser trial = parser_[port];
    const InParser::Out o = trial.feed(it.byte);
    const bool timing_port = cfg_.clock_enabled && port == static_cast<std::uint32_t>(cfg_.clock_port);
    const bool makes_event = timing_port && (o.kind == ByteKind::Stop || o.kind == ByteKind::Spp);
    const bool makes_pass = o.kind == ByteKind::Pass;
    if ((makes_event && d.events >= ev_cap) || (makes_pass && d.passed >= pass_cap)) {
      held_ = it;
      have_held_ = true;
      if (makes_pass) {
        ++stats_.pass_overflow;
      }
      break;
    }
    have_held_ = false;
    parser_[port] = trial;
    ++d.items;
    if (stats_.bytes[port] != 0xFFFFFFFFu) {
      ++stats_.bytes[port];
    }

    switch (o.kind) {
      case ByteKind::Pass:
        pass[d.passed++] = it;
        ++stats_.passed;
        break;
      case ByteKind::Consumed:
        break;
      case ByteKind::Clock:
      case ByteKind::Start:
      case ByteKind::Continue:
      case ByteKind::Stop:
      case ByteKind::Spp:
        if (!timing_port) {
          ++stats_.ignored_port;
          break;
        }
        if (o.kind == ByteKind::Clock) {
          const core::SQ32 t = map.to_rel(it.cycles - cfg_.latency_cycles[port], dll.origin());
          const ClockDll::Result r = dll.on_clock(t);
          ++stats_.clocks;
          if (armed_ != Armed::None && r != ClockDll::Result::Ignored && d.events < ev_cap) {
            // The transport begins on this clock (the standard: Start and Continue take effect on the next timing clock).
            std::uint64_t tick = dll.tick();
            if (cfg_.start_pulses > 0) {
              tick += static_cast<std::uint64_t>(cfg_.start_pulses);
            } else if (cfg_.start_pulses < 0) {
              const std::uint64_t back = static_cast<std::uint64_t>(-static_cast<std::int32_t>(cfg_.start_pulses));
              tick = tick > back ? tick - back : 0u;
            }
            ev[d.events++] = TransportEvent{armed_ == Armed::Start ? TransportEvent::Kind::Start : TransportEvent::Kind::Continue,
                                            armed_port_, 0, tick};
            ++stats_.transport;
            armed_ = Armed::None;
          }
        } else if (o.kind == ByteKind::Start || o.kind == ByteKind::Continue) {
          armed_ = o.kind == ByteKind::Start ? Armed::Start : Armed::Continue;
          armed_port_ = static_cast<std::uint8_t>(port);
        } else if (o.kind == ByteKind::Stop) {
          armed_ = Armed::None;
          ev[d.events++] = TransportEvent{TransportEvent::Kind::Stop, static_cast<std::uint8_t>(port), 0, 0};
          ++stats_.transport;
        } else {
          ev[d.events++] = TransportEvent{TransportEvent::Kind::Spp, static_cast<std::uint8_t>(port),
                                          static_cast<std::uint16_t>(o.value & 0x3FFFu), 0};
          ++stats_.transport;
        }
        break;
    }
  }
  return d;
}

}  // namespace o3sdk::platform::mk66::midi
