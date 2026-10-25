// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/msc_link.h>

namespace o3sdk::usb {

void MscPump::step() noexcept {
  // 1. Events, once: the engine calls they stand for, in the order the reset recovery uses.
  const std::uint32_t ev = link_.events();
  if (ev != snap_) {
    const auto changed = [&](std::uint32_t which) { return MscLink::count(ev, which) != MscLink::count(snap_, which); };
    if (changed(MscLink::kAll)) {
      engine_.reset_all();
      in_stall_posted_ = false;
      out_stall_posted_ = false;
    } else if (changed(MscLink::kBot)) {
      engine_.bot_reset();
      in_stall_posted_ = false;  // the stall markers and requests before the reset are stale (epoch); the halts the engine keeps are posted again
      out_stall_posted_ = false;
    }
    if (changed(MscLink::kClrIn)) {
      engine_.clear_halt_in();
      in_stall_posted_ = false;
    }
    if (changed(MscLink::kClrOut)) {
      engine_.clear_halt_out();
      out_stall_posted_ = false;
    }
    snap_ = ev;
  }
  const std::uint8_t epoch = MscLink::epoch_of(snap_);
  bool any = false;
  for (unsigned r = 0; r < kMaxRounds; ++r) {
    if (!round(epoch)) {
      break;
    }
    any = true;
  }
  if (any && kick_ != nullptr) {
    kick_();
  }
}

// One round; true when a packet moved (so another round may find the engine ready for more).
bool MscPump::round(std::uint8_t epoch) noexcept {
  bool moved = false;
  // 2. OUT packets: stale ones dropped (an older epoch, or older than an OUT stall, or while the engine is halted); newer ones wait for the next pass (which reads the event word first);
  // the rest go in while the engine takes them.
  for (std::uint32_t n = 0; n < MscLink::kSlots; ++n) {
    const std::uint32_t tail = link_.out_tail();
    if (tail == link_.out_head()) {
      break;
    }
    const std::int8_t age = static_cast<std::int8_t>(link_.out_epoch(tail) - epoch);
    if (age > 0) {
      break;
    }
    const bool before_stall = static_cast<std::int32_t>(link_.out_drop_to() - tail) > 0;
    if (age == 0 && !before_stall && !engine_.out_stalled()) {
      if (!engine_.can_accept_out(link_.out_len(tail))) {
        break;
      }
      engine_.out_packet(link_.out_data(tail), link_.out_len(tail));
    } else {
      ++dropped_out_;
    }
    link_.out_release();
    moved = true;  // a freed slot: the interrupt can arm it
  }

  // 3. The card: at most one block.
  engine_.service();
  if (engine_.out_stalled() && !out_stall_posted_) {
    link_.post_out_stall(epoch);
    out_stall_posted_ = true;
  }

  // 4. IN packets (and the stall marker after the last one).
  for (std::uint32_t n = 0; n < MscLink::kSlots && !in_stall_posted_; ++n) {
    const std::uint32_t head = link_.in_head();
    if (head - link_.in_tail() >= MscLink::kSlots) {
      break;
    }
    const msc::InPacket p = engine_.in_packet(link_.in_data(head), MscLink::kPacket);
    if (p.kind == msc::InPacket::Kind::Nak) {
      break;
    }
    if (p.kind == msc::InPacket::Kind::Packet) {
      link_.in_publish(MscLink::Kind::Data, p.len, epoch, 0);
    } else {
      link_.in_publish(MscLink::Kind::Stall, 0, epoch, MscLink::count(snap_, MscLink::kClrIn));
      in_stall_posted_ = true;
    }
    moved = true;
  }
  return moved;
}

}  // namespace o3sdk::usb
