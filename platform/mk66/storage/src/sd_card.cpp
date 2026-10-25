// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/sd_card.h>

#include <cstring>

#ifndef O3SDK_SD_HIGH_SPEED
#define O3SDK_SD_HIGH_SPEED 1
#endif

namespace o3sdk::platform::mk66::storage::sd {
namespace {

constexpr unsigned kRcaTries = 3;  // the spec: a card that answers CMD3 with an RCA of 0 is asked again
constexpr std::uint32_t kDefaultSpeedHz = 25'000'000;
constexpr std::uint32_t kHighSpeedHz = 50'000'000;

}  // namespace

void Card::note(Result r) noexcept {
  switch (r) {
    case Result::NoResponse: ++counters_.no_response; break;
    case Result::CommandError: ++counters_.command_errors; break;
    case Result::CardError: ++counters_.card_errors; break;
    case Result::DataTimeout: ++counters_.data_timeouts; break;
    case Result::DataError: ++counters_.data_errors; break;
    default: break;
  }
  if (r != Result::Ok) {
    counters_.last_error = r;
  }
}

Result Card::fail(Result r) noexcept {
  counters_.last_error = r;
  counters_.last_error_stage = stage_;
  info_.ready = false;
  return r;
}

Result Card::send(Cmd cmd, std::uint32_t arg, Response& out) noexcept {
  ++counters_.commands;
  Result r = bus_.command(cmd, arg, out);
  if (r == Result::Ok) {
    const Resp type = response_of(cmd);
    if ((type == Resp::R1 || type == Resp::R1b) && r1_errors(out.word[0]) != 0u) {
      r = Result::CardError;
    }
  }
  note(r);
  return r;
}

Result Card::send_app(Cmd cmd, std::uint32_t arg, Response& out) noexcept {
  Response announce;
  Result r = send(Cmd::AppCmd, rca_arg(info_.rca), announce);
  if (r != Result::Ok) {
    return r;
  }
  if ((announce.word[0] & kR1AppCmd) == 0u) {
    note(Result::BadResponse);  // the card did not say it expects an application command
    return Result::BadResponse;
  }
  return send(cmd, arg, out);
}

Result Card::init() noexcept {
  ++counters_.inits;
  info_ = CardInfo{};
  stage_ = InitStage::PowerUp;
  if (!bus_.card_present()) {
    return fail(Result::NoCard);
  }
  Result r = bus_.power_up();
  if (r != Result::Ok) {
    return fail(r);
  }
  Response resp;

  stage_ = InitStage::Idle;
  r = send(Cmd::GoIdle, 0, resp);
  if (r != Result::Ok) {
    return fail(r);
  }

  stage_ = InitStage::IfCond;
  r = send(Cmd::SendIfCond, kIfCondArg, resp);
  if (r == Result::Ok) {
    if (!r7_ok(resp.word[0])) {
      return fail(Result::BadResponse);  // a card that answers but does not echo the voltage and pattern is not to be used
    }
    info_.version2 = true;
  } else if (r != Result::NoResponse) {
    return fail(r);
  }  // no answer: a version 1 card (ACMD41 finds out whether there is a card at all)

  stage_ = InitStage::OpCond;
  const policy::Deadline ready{bus_.micros(), policy::kAcmd41TotalUs};
  for (;;) {
    r = send_app(Cmd::AcmdSdSendOpCond, acmd41_arg(info_.version2), resp);
    if (r != Result::Ok) {
      return fail(r == Result::NoResponse ? Result::NoCard : r);  // nothing answers CMD55: no card, or one that is not an SD memory card
    }
    if ((resp.word[0] & kOcrBusy) != 0u) {
      break;
    }
    if (ready.expired(bus_.micros())) {
      return fail(Result::NotReady);
    }
    bus_.delay_ms(policy::kAcmd41PollMs);
  }
  info_.ocr = resp.word[0];
  info_.block_addressed = info_.version2 && (info_.ocr & kOcrCcs) != 0u;

  std::uint8_t reg[16];
  stage_ = InitStage::Cid;
  r = send(Cmd::AllSendCid, 0, resp);
  if (r != Result::Ok) {
    return fail(r);
  }
  unpack_r2(resp.word, reg);
  info_.cid = parse_cid(reg);
  std::memcpy(info_.cid_raw, reg, sizeof reg);

  stage_ = InitStage::Rca;
  for (unsigned attempt = 0; attempt < kRcaTries && info_.rca == 0u; ++attempt) {
    r = send(Cmd::SendRelativeAddr, 0, resp);
    if (r != Result::Ok) {
      return fail(r);
    }
    info_.rca = r6_error(resp.word[0]) ? std::uint16_t{0} : r6_rca(resp.word[0]);
  }
  if (info_.rca == 0u) {
    return fail(Result::BadResponse);
  }

  stage_ = InitStage::Csd;
  r = send(Cmd::SendCsd, rca_arg(info_.rca), resp);
  if (r != Result::Ok) {
    return fail(r);
  }
  unpack_r2(resp.word, reg);
  std::memcpy(info_.csd_raw, reg, sizeof reg);
  const Csd csd = parse_csd(reg);
  info_.csd_version = csd.version;
  info_.csd_write_protected = csd.write_protected;
  if (!csd.valid) {
    return fail(Result::Unsupported);
  }
  info_.blocks = csd.blocks;
  info_.capacity_mb = capacity_mb(csd.blocks);

  stage_ = InitStage::Select;
  r = send(Cmd::SelectCard, rca_arg(info_.rca), resp);
  if (r != Result::Ok) {
    return fail(r);
  }

  if (!info_.block_addressed) {
    stage_ = InitStage::BlockLen;
    r = send(Cmd::SetBlockLen, kBlockSize, resp);  // an SDSC card reads in blocks of the length it is told; 512 is the one the file system needs
    if (r != Result::Ok) {
      return fail(r);
    }
  }

  stage_ = InitStage::BusWidth;
  r = send_app(Cmd::AcmdSetBusWidth, kBusWidthArg4, resp);
  if (r != Result::Ok) {
    return fail(r);
  }
  bus_.set_bus_width(4);
  info_.bus_width = 4;

  stage_ = InitStage::Speed;
  info_.clock_hz = bus_.set_clock(kDefaultSpeedHz);
  if (info_.clock_hz == 0u) {
    return fail(Result::BadArgument);
  }
#if O3SDK_SD_HIGH_SPEED
  if ((csd.ccc & kCccSwitch) != 0u) {  // CMD6 exists from class 10 (spec 1.10) on
    try_high_speed();
  }
#endif

  stage_ = InitStage::Done;
  info_.ready = true;
  return Result::Ok;
}

// The check and the switch are two CMD6 with a 64-byte status each. Anything that goes wrong leaves the card at the default speed: the
// card is still good, the extra speed is the only thing lost.
void Card::try_high_speed() noexcept {
  std::uint8_t status[kSwitchStatusBytes];
  Response resp;
  ++counters_.commands;
  Result r = bus_.read_data(Cmd::SwitchFunc, kSwitchCheckHighSpeed, status, kSwitchStatusBytes, 1, resp);
  note(r);
  if (r != Result::Ok || r1_errors(resp.word[0]) != 0u || !switch_status_high_speed_supported(status)) {
    return;
  }
  ++counters_.commands;
  r = bus_.read_data(Cmd::SwitchFunc, kSwitchSetHighSpeed, status, kSwitchStatusBytes, 1, resp);
  note(r);
  if (r != Result::Ok || r1_errors(resp.word[0]) != 0u || switch_status_group1_selected(status) != 1u) {
    return;
  }
  const std::uint32_t hz = bus_.set_clock(kHighSpeedHz);
  if (hz != 0u) {
    info_.clock_hz = hz;
  }
  info_.high_speed = true;
}

Result Card::read_chunk(std::uint32_t lba, std::uint32_t count, std::uint8_t* dst) noexcept {
  const Cmd cmd = count == 1u ? Cmd::ReadSingleBlock : Cmd::ReadMultipleBlock;
  const std::uint32_t arg = info_.block_addressed ? lba : lba * kBlockSize;  // an SDSC card is under 4 GiB: the byte address fits
  Result r = Result::Ok;
  for (unsigned attempt = 0; attempt <= policy::kReadRetries; ++attempt) {
    Response resp;
    ++counters_.commands;
    r = bus_.read_data(cmd, arg, dst, kBlockSize, count, resp);
    if (r == Result::Ok && r1_errors(resp.word[0]) != 0u) {
      r = Result::CardError;
    }
    note(r);
    if (r == Result::DataError && attempt < policy::kReadRetries) {
      ++counters_.retries;  // a CRC or end-bit error: the bus has reset itself and stopped the card, the same chunk is asked for again
      continue;
    }
    break;
  }
  return r;
}

Result Card::read_blocks(std::uint32_t lba, std::uint32_t count, void* dst) noexcept {
  ++counters_.reads;
  if (!info_.ready) {
    return Result::Uninitialised;
  }
  if (count == 0u) {
    return Result::Ok;
  }
  if (dst == nullptr || lba >= info_.blocks || count > info_.blocks - lba) {
    return Result::BadArgument;
  }
  if (!bus_.card_present()) {
    info_.ready = false;  // taken out: the next mount starts from the card's own initialisation
    ++counters_.failures;
    counters_.last_error = Result::NoCard;
    return Result::NoCard;
  }
  std::uint8_t* out = static_cast<std::uint8_t*>(dst);
  while (count != 0u) {
    const std::uint32_t n = count < policy::kMaxBlocksPerCommand ? count : policy::kMaxBlocksPerCommand;
    const Result r = read_chunk(lba, n, out);
    if (r != Result::Ok) {
      ++counters_.failures;
      return r;
    }
    counters_.blocks += n;
    lba += n;
    count -= n;
    out += static_cast<std::size_t>(n) * kBlockSize;
  }
  return Result::Ok;
}

// One block: CMD24 and its data phase (the bus waits for the CRC status and the card's busy), then CMD13 until READY_FOR_DATA in the transfer state, at most
// kWriteStatusUs. Any R1 error bit (WP_VIOLATION included) fails the block.
Result Card::write_one(std::uint32_t lba, const std::uint8_t* src) noexcept {
  const std::uint32_t arg = info_.block_addressed ? lba : lba * kBlockSize;
  Response resp;
  ++counters_.commands;
  Result r = bus_.write_data(Cmd::WriteBlock, arg, src, kBlockSize, 1, resp);
  if (r == Result::Ok && r1_errors(resp.word[0]) != 0u) {
    r = Result::CardError;
  }
  note(r);
  if (r != Result::Ok) {
    return r;
  }
  const policy::Deadline status{bus_.micros(), policy::kWriteStatusUs};
  for (;;) {
    r = send(Cmd::SendStatus, rca_arg(info_.rca), resp);
    if (r != Result::Ok) {
      return r;
    }
    if ((resp.word[0] & kR1ReadyForData) != 0u && r1_state(resp.word[0]) == kStateTran) {
      return Result::Ok;
    }
    if (status.expired(bus_.micros())) {
      note(Result::NotReady);
      return Result::NotReady;
    }
  }
}

Result Card::write_blocks(std::uint32_t lba, std::uint32_t count, const void* src) noexcept {
  ++counters_.writes;
  if (!info_.ready) {
    return Result::Uninitialised;
  }
  if (count == 0u) {
    return Result::Ok;
  }
  if (src == nullptr || lba >= info_.blocks || count > info_.blocks - lba) {
    return Result::BadArgument;
  }
  if (!bus_.card_present()) {
    info_.ready = false;
    ++counters_.write_failures;
    counters_.last_error = Result::NoCard;
    return Result::NoCard;
  }
  if (write_protected()) {
    ++counters_.write_failures;
    counters_.last_error = Result::WriteProtected;
    return Result::WriteProtected;  // nothing was sent
  }
  const std::uint8_t* in = static_cast<const std::uint8_t*>(src);
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result r = write_one(lba + i, in + static_cast<std::size_t>(i) * kBlockSize);
    if (r != Result::Ok) {
      ++counters_.write_failures;
      return r;
    }
    ++counters_.blocks_written;
  }
  return Result::Ok;
}

BlockResult Card::start() noexcept {
  switch (init()) {
    case Result::Ok: return BlockResult::Ok;
    case Result::NoCard: return BlockResult::NoMedium;
    case Result::Unsupported: return BlockResult::Unsupported;
    default: return BlockResult::Failed;
  }
}

BlockResult Card::read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept {
  switch (read_blocks(lba, count, dst)) {
    case Result::Ok: return BlockResult::Ok;
    case Result::Uninitialised:
    case Result::NoCard: return BlockResult::NotReady;
    case Result::BadArgument: return BlockResult::OutOfRange;
    default: return BlockResult::Failed;
  }
}

BlockResult Card::write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept {
  switch (write_blocks(lba, count, src)) {
    case Result::Ok: return BlockResult::Ok;
    case Result::Uninitialised:
    case Result::NoCard: return BlockResult::NotReady;
    case Result::BadArgument: return BlockResult::OutOfRange;
    case Result::WriteProtected: return BlockResult::WriteProtected;
    default: return BlockResult::Failed;
  }
}

}  // namespace o3sdk::platform::mk66::storage::sd
