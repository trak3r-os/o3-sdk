// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB Mass Storage Bulk-Only Transport and SCSI transparent command engine (OS-USBDISK U1a). See msc.h. Written from the public "Universal
// Serial Bus Mass Storage Class, Bulk-Only Transport" 1.0 (CBW, CSW, the thirteen cases of 6.7, the reset recovery of 5.3.4) and the SCSI
// SPC/SBC command formats; no third-party code is used.
#include <o3sdk/usb/msc.h>

#include <cstring>

namespace o3sdk::usb::msc {
namespace {

constexpr std::uint32_t le32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
constexpr std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}
constexpr std::uint32_t be16(const std::uint8_t* p) noexcept { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }
void put_le32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}
void put_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}
constexpr std::uint32_t min32(std::uint32_t a, std::uint32_t b) noexcept { return a < b ? a : b; }

// SCSI operation codes.
constexpr std::uint8_t kTestUnitReady = 0x00;
constexpr std::uint8_t kRequestSense = 0x03;
constexpr std::uint8_t kInquiry = 0x12;
constexpr std::uint8_t kModeSense6 = 0x1A;
constexpr std::uint8_t kStartStopUnit = 0x1B;
constexpr std::uint8_t kPreventAllow = 0x1E;
constexpr std::uint8_t kReadFormatCapacities = 0x23;
constexpr std::uint8_t kReadCapacity10 = 0x25;
constexpr std::uint8_t kRead10 = 0x28;
constexpr std::uint8_t kWrite10 = 0x2A;
constexpr std::uint8_t kVerify10 = 0x2F;
constexpr std::uint8_t kSynchronizeCache10 = 0x35;

constexpr std::uint32_t kInquiryLen = 36;
constexpr std::uint32_t kSenseLen = 18;
constexpr std::uint32_t kReadCapacityLen = 8;
constexpr std::uint32_t kModeSenseLen = 4;
constexpr std::uint32_t kFormatCapLen = 12;

}  // namespace

Engine::Engine(BlockStore& store) noexcept : store_(store) {
  seen_present_ = store_.present() && store_.block_count() != 0;
  seen_blocks_ = seen_present_ ? store_.block_count() : 0;
}

// ---- Transport side -----------------------------------------------------------------------------------------------------------

bool Engine::can_accept_out(std::uint32_t max_len) const noexcept {
  if (halt_out_) {
    return false;
  }
  switch (phase_) {
    case Phase::ExpectCbw:
      return max_len >= kCbwSize;
    case Phase::DataOut: {
      if (moved_ >= xfer_) {
        return false;
      }
      const std::uint32_t room = (2u - fill_) * kBlockSize - pos_;
      return room >= min32(max_len, xfer_ - moved_);
    }
    default:
      return false;
  }
}

void Engine::go_dead() noexcept {
  phase_ = Phase::Dead;
  dead_ = true;
  halt_in_ = true;
  halt_out_ = true;
  clear_ring();
}

void Engine::out_packet(const std::uint8_t* data, std::uint32_t len) noexcept {
  if (halt_out_) {
    return;  // a halted endpoint answers STALL: nothing legitimate gets here
  }
  switch (phase_) {
    case Phase::ExpectCbw:
      if (len == kCbwSize && le32(data) == kCbwSignature) {
        std::memcpy(cbw_, data, kCbwSize);
        phase_ = Phase::CbwReceived;
      } else {
        go_dead();  // an invalid CBW: stall both pipes until the reset recovery
      }
      return;
    case Phase::DataOut: {
      if (moved_ >= xfer_) {
        return;  // the host sent more than the data stage takes; the stall ends it
      }
      const std::uint32_t room = (2u - fill_) * kBlockSize - pos_;
      std::uint32_t take = min32(len, xfer_ - moved_);
      take = min32(take, room);  // can_accept_out() keeps this from clipping
      while (take != 0) {
        const std::uint32_t chunk = min32(take, kBlockSize - pos_);
        std::memcpy(&buf_[wr_][pos_], data, chunk);
        pos_ += chunk;
        data += chunk;
        take -= chunk;
        moved_ += chunk;
        if (pos_ == kBlockSize) {
          pos_ = 0;
          wr_ ^= 1u;
          ++fill_;
        }
      }
      return;
    }
    default:
      go_dead();  // a packet where no CBW or data is expected: the host is out of step
      return;
  }
}

InPacket Engine::in_packet(std::uint8_t* out, std::uint32_t max) noexcept {
  using K = InPacket::Kind;
  if (halt_in_) {
    return {K::Stall, 0};
  }
  switch (phase_) {
    case Phase::SendCsw: {
      if (max < kCswSize) {
        return {K::Nak, 0};
      }
      put_le32(out, kCswSignature);
      put_le32(out + 4, tag_);
      put_le32(out + 8, csw_residue_);
      out[12] = static_cast<std::uint8_t>(csw_status_);
      phase_ = Phase::ExpectCbw;
      return {K::Packet, kCswSize};
    }
    case Phase::DataIn: {
      const std::uint32_t want = min32(max, xfer_ - moved_);
      if (op_ == Op::Small) {
        std::memcpy(out, small_ + moved_, want);
      } else {
        // A packet is full unless it is the last: wait until the whole of it is on hand.
        if (static_cast<std::uint32_t>(fill_) * kBlockSize - pos_ < want) {
          return {K::Nak, 0};
        }
        std::uint32_t left = want;
        std::uint8_t* dst = out;
        while (left != 0) {
          const std::uint32_t chunk = min32(left, kBlockSize - pos_);
          std::memcpy(dst, &buf_[rd_][pos_], chunk);
          pos_ += chunk;
          dst += chunk;
          left -= chunk;
          if (pos_ == kBlockSize) {
            pos_ = 0;
            rd_ ^= 1u;
            --fill_;
          }
        }
      }
      moved_ += want;
      if (moved_ == xfer_) {
        finish_in();
      }
      return {K::Packet, want};
    }
    default:
      return {K::Nak, 0};
  }
}

void Engine::clear_halt_in() noexcept {
  if (!dead_) {
    halt_in_ = false;
  }
}

void Engine::clear_halt_out() noexcept {
  if (!dead_) {
    halt_out_ = false;
  }
}

void Engine::bot_reset() noexcept {
  phase_ = Phase::ExpectCbw;
  dead_ = false;
  op_ = Op::None;
  clear_ring();
}

// A bus reset or SET_CONFIGURATION: a new session with the computer. The command in flight is dropped, both halts end, and everything the previous session set up
// (PREVENT, the unit attention nobody was told, the stale sense) is forgotten. The medium belief (seen_*) stays: it follows the store. The EJECT LATCH stays too (ejected_,
// eject_request_): the card went back to the unit, and a new session must not see it again; only medium_changed() (a fresh hand-over) or a new medium lifts it.
void Engine::reset_all() noexcept {
  bot_reset();
  halt_in_ = false;
  halt_out_ = false;
  prevent_ = false;
  ua_ = false;
  sense_ = Sense{};
}

void Engine::clear_ring() noexcept {
  rd_ = 0;
  wr_ = 0;
  fill_ = 0;
  pos_ = 0;
}

// ---- The owner's side ---------------------------------------------------------------------------------------------------------

void Engine::medium_changed() noexcept {
  // A READ or WRITE in its data stage was planned (LBA, length, capacity check) on the medium that is gone: it ends here, before the new medium can be touched. Without this a WRITE(10) whose
  // host data is still arriving would go on, block after block, onto the card that was put in meanwhile (OS-USBDISK, the writable step).
  if ((phase_ == Phase::DataIn || phase_ == Phase::DataOut) && (op_ == Op::Read || op_ == Op::Write)) {
    abort_data(sense::kNoMedium);
  }
  seen_present_ = store_.present() && store_.block_count() != 0;
  seen_blocks_ = seen_present_ ? store_.block_count() : 0;
  ua_ = true;
  ejected_ = false;
  eject_request_ = false;
}

void Engine::unit_eject() noexcept {
  ejected_ = true;
  ua_ = true;
}

bool Engine::take_eject_request() noexcept {
  const bool r = eject_request_;
  eject_request_ = false;
  return r;
}

// What the host was last told about the medium against what the store says now: an insert, a removal or a different capacity is a medium
// change for the host (UNIT ATTENTION on its next command).
void Engine::refresh_medium() noexcept {
  const bool present = store_.present() && store_.block_count() != 0;
  const std::uint32_t blocks = present ? store_.block_count() : 0;
  if (present != seen_present_) {
    seen_present_ = present;
    seen_blocks_ = blocks;
    ua_ = true;
    ejected_ = false;
  } else if (present && blocks != seen_blocks_) {
    seen_blocks_ = blocks;
    ua_ = true;
  }
}

// ---- The main loop ------------------------------------------------------------------------------------------------------------

void Engine::service() noexcept {
  switch (phase_) {
    case Phase::CbwReceived:
      decode();
      return;
    case Phase::DataIn: {
      if (op_ != Op::Read) {
        return;
      }
      if (ejected_) {
        abort_data(sense::kNoMedium);  // after unit_eject() the store is not touched again and the transfer ends now
        return;
      }
      const std::uint32_t need = (xfer_ + kBlockSize - 1u) / kBlockSize;
      if (io_blocks_ >= need || fill_ >= 2) {
        return;
      }
      const IoResult r = store_.read(lba_ + io_blocks_, 1, buf_[wr_]);
      if (r == IoResult::Ok) {
        ++io_blocks_;
        wr_ ^= 1u;
        ++fill_;
      } else {
        abort_data(r == IoResult::NoMedium ? sense::kNoMedium : sense::kReadError);
      }
      return;
    }
    case Phase::DataOut: {
      if (ejected_) {
        abort_data(sense::kNoMedium);
        return;
      }
      if (fill_ != 0) {
        const IoResult r = store_.write(lba_ + io_blocks_, 1, buf_[rd_]);
        if (r == IoResult::Ok) {
          ++io_blocks_;
          rd_ ^= 1u;
          --fill_;
        } else {
          abort_data(r == IoResult::NoMedium ? sense::kNoMedium : (r == IoResult::WriteProtected ? sense::kDataProtect : sense::kWriteError));
          return;
        }
      }
      if (moved_ == xfer_ && fill_ == 0) {
        finish_out();  // a partial last block (host sent less than the device meant to take) is dropped, never written
      }
      return;
    }
    default:
      return;
  }
}

// ---- Command decoding ---------------------------------------------------------------------------------------------------------

void Engine::fail(const Sense& s) noexcept {
  sense_ = s;
  scsi_status_ = CswStatus::Failed;
  op_ = Op::None;
  dev_dir_ = Dir::None;
  dev_len_ = 0;
}

void Engine::end_with(CswStatus status, std::uint32_t residue) noexcept {
  csw_status_ = status;
  csw_residue_ = residue;
  phase_ = Phase::SendCsw;
}

void Engine::finish_in() noexcept {
  CswStatus st = scsi_status_;
  std::uint32_t residue = 0;
  if (hlen_ > dev_len_) {  // the host expected more than the device had: short transfer, stall IN (cases 5)
    halt_in_ = true;
    residue = hlen_ - dev_len_;
  } else if (hlen_ < dev_len_) {  // the device had more than the host asked for: stall IN, phase error (case 7)
    halt_in_ = true;
    st = CswStatus::PhaseError;
  }
  clear_ring();
  end_with(st, residue);
}

void Engine::finish_out() noexcept {
  CswStatus st = scsi_status_;
  std::uint32_t residue = 0;
  if (hlen_ > dev_len_) {  // the host sends more than the device takes: stall OUT (case 11)
    halt_out_ = true;
    residue = hlen_ - dev_len_;
  } else if (hlen_ < dev_len_) {  // the host sends less than the device wanted: stall OUT, phase error (case 13)
    halt_out_ = true;
    st = CswStatus::PhaseError;
  }
  clear_ring();
  end_with(st, residue);
}

// The store failed (or the medium went) in the middle of a data stage: stall the data pipe, report the command failed, and give the host the
// residue of what was really moved.
void Engine::abort_data(const Sense& s) noexcept {
  sense_ = s;
  std::uint32_t done = 0;
  if (op_ == Op::Write) {
    halt_out_ = moved_ < hlen_;  // the host still has data to send: stall OUT; when it has sent it all it waits for the CSW and a stall would be a phase error
    done = io_blocks_ * kBlockSize;  // only whole blocks the store took count
  } else {
    halt_in_ = true;
    done = moved_;
  }
  clear_ring();
  end_with(CswStatus::Failed, hlen_ > done ? hlen_ - done : 0u);
}

void Engine::make_small(std::uint32_t natural, std::uint32_t alloc) noexcept {
  const std::uint32_t n = min32(natural, alloc);
  op_ = n != 0 ? Op::Small : Op::None;
  dev_dir_ = n != 0 ? Dir::In : Dir::None;
  dev_len_ = n;
}

// UNIT ATTENTION first, then the medium. INQUIRY and REQUEST SENSE never come here.
bool Engine::gate_medium(bool needs_medium) noexcept {
  refresh_medium();
  if (ua_) {
    ua_ = false;
    fail(sense::kMediumChanged);
    return false;
  }
  if (needs_medium && (!seen_present_ || ejected_)) {
    fail(sense::kNoMedium);
    return false;
  }
  return true;
}

// READ(10) and WRITE(10): the gate has passed. The device's intention is the whole block run; the data stage moves min(host, device) of it.
void Engine::cmd_rw(bool write) noexcept {
  const std::uint8_t* cdb = cbw_ + 15;
  if ((cdb[1] >> 5) != 0) {
    fail(sense::kBadField);  // RDPROTECT / WRPROTECT: no protection information
    return;
  }
  if (write && store_.write_protected()) {
    fail(sense::kDataProtect);
    return;
  }
  const std::uint32_t lba = be32(cdb + 2);
  const std::uint32_t n = be16(cdb + 7);
  if (static_cast<std::uint64_t>(lba) + n > seen_blocks_) {
    fail(sense::kLbaRange);
    return;
  }
  if (n == 0) {
    return;  // nothing to move, not an error
  }
  lba_ = lba;
  op_ = write ? Op::Write : Op::Read;
  dev_dir_ = write ? Dir::Out : Dir::In;
  dev_len_ = n * kBlockSize;
}

void Engine::plan_scsi() noexcept {
  // The CDB bytes past bCBWCBLength are not part of the command: zero them.
  std::uint8_t cdb[16];
  std::memcpy(cdb, cbw_ + 15, sizeof cdb);
  const std::uint32_t cblen = cbw_[14] & 0x1Fu;
  for (std::uint32_t i = cblen; i < sizeof cdb; ++i) {
    cdb[i] = 0;
  }
  std::memcpy(cbw_ + 15, cdb, sizeof cdb);

  const std::uint8_t opcode = cdb[0];
  std::uint32_t need = 0;  // CDB length of the opcode
  switch (opcode) {
    case kTestUnitReady:
    case kRequestSense:
    case kInquiry:
    case kModeSense6:
    case kStartStopUnit:
    case kPreventAllow:
      need = 6;
      break;
    case kReadFormatCapacities:
    case kReadCapacity10:
    case kRead10:
    case kWrite10:
    case kVerify10:
    case kSynchronizeCache10:
      need = 10;
      break;
    default:
      break;
  }
  // INQUIRY and REQUEST SENSE keep the sense data of the command before them; every other command starts from none.
  if (opcode != kInquiry && opcode != kRequestSense) {
    sense_ = sense::kNone;
  }
  if (need == 0) {
    fail(sense::kBadOpcode);
    return;
  }
  if (cblen < need) {
    fail(sense::kBadField);
    return;
  }

  switch (opcode) {
    case kTestUnitReady:
      (void)gate_medium(true);
      return;

    case kRequestSense: {
      if ((cdb[1] & 0x01u) != 0) {
        fail(sense::kBadField);  // descriptor format sense is not offered
        return;
      }
      refresh_medium();
      Sense s = sense_;
      if (s.key == sense::kKeyNone && ua_) {  // a unit attention nobody has been told about yet is reported here
        s = sense::kMediumChanged;
        ua_ = false;
      }
      std::memset(small_, 0, kSenseLen);
      small_[0] = 0x70;
      small_[2] = s.key;
      small_[7] = 10;
      small_[12] = s.asc;
      small_[13] = s.ascq;
      sense_ = sense::kNone;
      make_small(kSenseLen, cdb[4]);
      return;
    }

    case kInquiry: {
      if ((cdb[1] & 0x03u) != 0 || cdb[2] != 0) {
        fail(sense::kBadField);  // no VPD pages, no command support data
        return;
      }
      std::memset(small_, 0, kInquiryLen);
      small_[1] = 0x80;  // removable
      small_[2] = 0x04;  // SPC-2
      small_[3] = 0x02;  // response data format
      small_[4] = static_cast<std::uint8_t>(kInquiryLen - 5u);
      std::memcpy(small_ + 8, "Track3r ", 8);
      std::memcpy(small_ + 16, "SD CARD         ", 16);
      std::memcpy(small_ + 32, "0001", 4);
      make_small(kInquiryLen, be16(cdb + 3));
      return;
    }

    case kModeSense6: {
      if (!gate_medium(true)) {
        return;
      }
      const std::uint8_t page = cdb[2] & 0x3Fu;
      if ((cdb[2] >> 6) == 3u || page != 0x3Fu) {
        fail(sense::kBadField);  // all pages only, and there are none: no caching page
        return;
      }
      std::memset(small_, 0, kModeSenseLen);
      small_[0] = kModeSenseLen - 1u;
      small_[2] = store_.write_protected() ? 0x80 : 0x00;
      make_small(kModeSenseLen, cdb[4]);
      return;
    }

    case kStartStopUnit: {
      if (!gate_medium(false)) {
        return;
      }
      const bool start = (cdb[4] & 0x01u) != 0;
      const bool loej = (cdb[4] & 0x02u) != 0;
      if ((cdb[4] >> 4) != 0) {
        fail(sense::kBadField);  // no power conditions
        return;
      }
      if (loej && !start) {  // eject
        if (seen_present_ && !ejected_) {
          if (prevent_) {
            fail(sense::kRemovalPrevented);
            return;
          }
          ejected_ = true;
          eject_request_ = true;
        }
        return;
      }
      if (start && (!seen_present_ || ejected_)) {
        fail(sense::kNoMedium);  // the host cannot load the medium back: the unit's owner does that
      }
      return;
    }

    case kPreventAllow:
      if (!gate_medium(false)) {
        return;
      }
      if ((cdb[4] & 0xFEu) != 0) {
        fail(sense::kBadField);
        return;
      }
      prevent_ = (cdb[4] & 0x01u) != 0;
      return;

    case kReadFormatCapacities: {
      if (!gate_medium(false)) {
        return;
      }
      std::memset(small_, 0, kFormatCapLen);
      small_[3] = 8;
      const bool formatted = seen_present_ && !ejected_;  // an ejected medium is "no media" here as it is for every other command
      put_be32(small_ + 4, formatted ? seen_blocks_ : 0u);
      small_[8] = formatted ? 2 : 3;  // formatted media / no media
      small_[9] = 0;
      small_[10] = static_cast<std::uint8_t>(kBlockSize >> 8);
      small_[11] = static_cast<std::uint8_t>(kBlockSize);
      make_small(kFormatCapLen, be16(cdb + 7));
      return;
    }

    case kReadCapacity10: {
      if (!gate_medium(true)) {
        return;
      }
      if ((cdb[8] & 0x01u) == 0 && be32(cdb + 2) != 0) {
        fail(sense::kBadField);
        return;
      }
      put_be32(small_, seen_blocks_ - 1u);
      put_be32(small_ + 4, kBlockSize);
      make_small(kReadCapacityLen, kReadCapacityLen);
      return;
    }

    case kRead10:
    case kWrite10:
      if (gate_medium(true)) {
        cmd_rw(opcode == kWrite10);
      }
      return;

    case kVerify10:
    case kSynchronizeCache10: {
      if (!gate_medium(true)) {
        return;
      }
      if (opcode == kVerify10 && (cdb[1] & 0x02u) != 0) {
        fail(sense::kBadField);  // BYTCHK: the host would send data to compare; not offered
        return;
      }
      if (static_cast<std::uint64_t>(be32(cdb + 2)) + be16(cdb + 7) > seen_blocks_) {
        fail(sense::kLbaRange);
      }
      return;  // nothing is cached (section 6.2) and a verify of blocks that are all readable is the range check
    }

    default:
      fail(sense::kBadOpcode);
      return;
  }
}

void Engine::start_data() noexcept {
  xfer_ = min32(hlen_, dev_len_);
  moved_ = 0;
  io_blocks_ = 0;
  clear_ring();
  phase_ = dev_dir_ == Dir::In ? Phase::DataIn : Phase::DataOut;
}

// The thirteen cases of the BOT specification 6.7, by what the host expects (Hn none, Hi in, Ho out) against what the device intends.
void Engine::decode() noexcept {
  tag_ = le32(cbw_ + 4);
  hlen_ = le32(cbw_ + 8);
  h_in_ = (cbw_[12] & 0x80u) != 0;
  op_ = Op::None;
  dev_dir_ = Dir::None;
  dev_len_ = 0;
  scsi_status_ = CswStatus::Passed;
  xfer_ = 0;
  moved_ = 0;
  io_blocks_ = 0;

  // A valid CBW can still be not meaningful: reserved bits, a LUN we do not have, a CDB length outside 1..16. The command fails (the data
  // stage is stalled by the cases below), and the sense says why.
  const bool reserved = (cbw_[12] & 0x7Fu) != 0 || (cbw_[13] & 0xF0u) != 0 || (cbw_[14] & 0xE0u) != 0;
  const std::uint32_t cblen = cbw_[14] & 0x1Fu;
  const std::uint32_t lun = cbw_[13] & 0x0Fu;
  if (reserved || cblen == 0 || cblen > 16) {
    fail(sense::kBadField);
  } else if (lun > max_lun()) {
    fail(sense::kBadLun);
  } else {
    plan_scsi();
  }

  if (hlen_ == 0) {
    // Cases 1, 2, 3: the host expects no data.
    end_with(dev_len_ == 0 ? scsi_status_ : CswStatus::PhaseError, 0);
    return;
  }
  if (h_in_) {
    if (dev_len_ != 0 && dev_dir_ == Dir::Out) {  // case 8: the device wants to receive
      halt_in_ = true;
      end_with(CswStatus::PhaseError, hlen_);
    } else if (dev_len_ == 0) {  // case 4: the device has no data
      halt_in_ = true;
      end_with(scsi_status_, hlen_);
    } else {  // cases 5, 6, 7
      start_data();
    }
    return;
  }
  if (dev_len_ != 0 && dev_dir_ == Dir::In) {  // case 10: the device wants to send
    halt_out_ = true;
    end_with(CswStatus::PhaseError, hlen_);
  } else if (dev_len_ == 0) {  // case 9: the device wants no data
    halt_out_ = true;
    end_with(scsi_status_, hlen_);
  } else {  // cases 11, 12, 13
    start_data();
  }
}

}  // namespace o3sdk::usb::msc
