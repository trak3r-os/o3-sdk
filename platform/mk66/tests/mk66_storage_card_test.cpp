// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SD card logic (storage/sd_card.h) against a software card (mk66_storage_fakes.h): the identification sequence in the spec's order, SDHC and
// SDSC, the high-speed step and its failure, a card that never gets ready or is not there, every error path, the block reads (command choice,
// chunking, retries, byte addressing), and the property the whole slice is about: no command outside the read-side allowlist is ever sent. Pure;
// runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3sdk/platform/mk66/storage/sd_card.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sd = o3sdk::platform::mk66::storage::sd;
namespace st = o3sdk::platform::mk66::storage;
using fakes::FakeCardSpec;
using fakes::FakeSdBus;
using fakes::Medium;
using sd::Cmd;
using sd::Result;

namespace {

constexpr std::uint32_t kEightGb = 15'523'840u;

// A byte pattern that tells which block it is in and where: sector `lba`, byte `i`.
std::uint8_t pattern(std::uint32_t lba, unsigned i) { return static_cast<std::uint8_t>((lba * 7u + i * 13u + (lba >> 8)) & 0xFFu); }

void fill_sector(Medium& m, std::uint32_t lba) {
  std::uint8_t* s = m.sector(lba);
  for (unsigned i = 0; i < 512; ++i) {
    s[i] = pattern(lba, i);
  }
}

bool sector_is(const std::uint8_t* got, std::uint32_t lba) {
  for (unsigned i = 0; i < 512; ++i) {
    if (got[i] != pattern(lba, i)) {
      return false;
    }
  }
  return true;
}

// The commands the fake saw, as "wire index" numbers with 100 added for an application command: "0 8 155 141 ..." in the tests below.
std::vector<unsigned> sequence(const FakeSdBus& bus) {
  std::vector<unsigned> out;
  for (const FakeSdBus::Seen& s : bus.log) {
    out.push_back(sd::wire_index(s.cmd) + (sd::is_app(s.cmd) ? 100u : 0u));
  }
  return out;
}

}  // namespace

TEST_CASE("sd card: the identification sequence of an SDHC card, in the spec's order, at the spec's speeds") {
  Medium medium(kEightGb);
  FakeCardSpec spec;  // v2, high capacity, ACMD41 busy twice, high speed supported
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);

  // CMD0, CMD8, then CMD55 + ACMD41 until ready (busy twice: three rounds), CMD2, CMD3, CMD9, CMD7, CMD55 + ACMD6, CMD6 check, CMD6 switch.
  const std::vector<unsigned> expected = {0, 8, 55, 141, 55, 141, 55, 141, 2, 3, 9, 7, 55, 106, 6, 6};
  CHECK(sequence(bus) == expected);
  CHECK(bus.power_ups == 1u);
  CHECK(bus.log[1].arg == 0x1AAu);
  CHECK(bus.log[3].arg == 0x40FF8000u);  // HCS and the 3.3 V window
  CHECK(bus.log[11].arg == sd::rca_arg(FakeSdBus::kRca));  // CMD7 selects the RCA CMD3 handed out
  CHECK(bus.log[13].arg == 2u);                            // ACMD6: four lines
  CHECK(bus.log[14].arg == 0x00FFFFF1u);                   // the check
  CHECK(bus.log[15].arg == 0x80FFFFF1u);                   // the switch
  CHECK(bus.slept_ms == 20u);                               // two ACMD41 polls apart: 10 ms each

  // Bus: four lines on the host and the card, 400 kHz identification (375 kHz made), 25 MHz (22.5), then 50 MHz (45).
  CHECK(bus.host_width == 4u);
  CHECK(bus.card_width == 4u);
  CHECK(bus.clocks_asked.size() == 2u);  // the 400 kHz of power_up() is the bus's own business, not asked of it here
  CHECK(bus.clocks_asked[0] == 25'000'000u);
  CHECK(bus.clocks_asked[1] == 50'000'000u);
  CHECK(bus.hs_active);

  const sd::CardInfo& info = card.info();
  CHECK(info.ready);
  CHECK(card.ready());
  CHECK(info.version2);
  CHECK(info.block_addressed);
  CHECK(info.high_speed);
  CHECK(info.bus_width == 4);
  CHECK(info.clock_hz == 45'000'000u);
  CHECK(info.rca == FakeSdBus::kRca);
  CHECK(info.blocks == kEightGb);
  CHECK(info.capacity_mb == 7580u);
  CHECK(info.csd_version == 2);
  CHECK(card.block_count() == kEightGb);
  CHECK(card.stage() == sd::InitStage::Done);
  CHECK((info.ocr & sd::kOcrCcs) != 0u);
  CHECK(info.cid.manufacturer == 0x03);
  CHECK(std::strcmp(info.cid.oem, "SD") == 0);
  CHECK(std::strcmp(info.cid.product, "SC16G") == 0);
  CHECK(info.cid.serial == 0x1234ABCDu);
  CHECK(info.cid.year == 2024);
  CHECK(info.cid.month == 7);
  CHECK(card.counters().inits == 1u);
  CHECK(card.counters().commands == 16u);
  CHECK(card.counters().last_error == Result::Ok);
}

TEST_CASE("sd card: an SDSC card is byte addressed, is set to 512-byte blocks and, without class 10, is not asked to switch") {
  Medium medium(131'072);  // 64 MB
  FakeCardSpec spec;
  spec.high_capacity = false;
  spec.csd_v1 = true;
  spec.switch_class = false;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  const std::vector<unsigned> expected = {0, 8, 55, 141, 55, 141, 55, 141, 2, 3, 9, 7, 16, 55, 106};
  CHECK(sequence(bus) == expected);
  CHECK(bus.block_len == 512u);
  CHECK(!card.info().block_addressed);
  CHECK(card.info().version2);        // it answered CMD8 and is still standard capacity
  CHECK(!card.info().high_speed);
  CHECK(card.info().csd_version == 1);
  CHECK(card.info().blocks == 131'072u);
  CHECK(card.info().clock_hz == 22'500'000u);
  CHECK(bus.clocks_asked.size() == 1u);
  CHECK(!bus.logged(Cmd::SwitchFunc));
  // A read of block 5 is a read at byte 5 * 512.
  fill_sector(medium, 5);
  std::uint8_t got[512];
  NEED(card.read_blocks(5, 1, got) == Result::Ok);
  CHECK(bus.last_read_arg == 5u * 512u);
  CHECK(sector_is(got, 5));
}

TEST_CASE("sd card: a version 1 card (no answer to CMD8) is asked for standard capacity only") {
  Medium medium(131'072);
  FakeCardSpec spec;
  spec.v2 = false;
  spec.high_capacity = true;  // would be, if it were a version 2 card
  spec.csd_v1 = true;
  spec.switch_class = false;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  CHECK(!card.info().version2);
  CHECK(!card.info().block_addressed);
  CHECK(bus.log[3].arg == 0x00FF8000u);  // no HCS: the host does not claim high-capacity support to a card that does not know the bit
  CHECK(card.counters().no_response == 1u);  // the CMD8 nobody answered
}

TEST_CASE("sd card: a failed high-speed switch leaves the card at the default speed and is not an error") {
  Medium medium(kEightGb);
  FakeCardSpec spec;
  spec.hs_switch_fails = true;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  CHECK(!card.info().high_speed);
  CHECK(card.info().clock_hz == 22'500'000u);
  CHECK(bus.clocks_asked.size() == 1u);  // the 50 MHz was never asked for
  CHECK(card.ready());

  FakeCardSpec no_hs;
  no_hs.hs_supported = false;
  FakeSdBus bus2(medium, no_hs);
  sd::Card card2(bus2);
  NEED(card2.init() == Result::Ok);
  CHECK(!card2.info().high_speed);
  CHECK(bus2.count(Cmd::SwitchFunc) == 1u);  // checked, found unsupported, not switched
}

TEST_CASE("sd card: no card, a card that does not answer, a card that never gets ready, an echo that is wrong") {
  Medium medium(kEightGb);

  FakeCardSpec absent;
  absent.present = false;
  FakeSdBus bus_absent(medium, absent);
  sd::Card card_absent(bus_absent);
  CHECK(card_absent.init() == Result::NoCard);
  CHECK(bus_absent.log.empty());  // the detect switch said so: not one command, not even the power-up
  CHECK(bus_absent.power_ups == 0u);
  CHECK(card_absent.counters().last_error == Result::NoCard);
  CHECK(card_absent.counters().last_error_stage == sd::InitStage::PowerUp);
  CHECK(!card_absent.ready());

  FakeCardSpec mute;
  mute.respond = false;  // something in the slot that never speaks
  FakeSdBus bus_mute(medium, mute);
  sd::Card card_mute(bus_mute);
  CHECK(card_mute.init() == Result::NoCard);
  CHECK(card_mute.counters().last_error_stage == sd::InitStage::OpCond);
  CHECK(bus_mute.count(Cmd::AppCmd) == 1u);  // one CMD55, no answer, no loop

  FakeCardSpec stuck;
  stuck.never_ready = true;
  FakeSdBus bus_stuck(medium, stuck);
  sd::Card card_stuck(bus_stuck);
  CHECK(card_stuck.init() == Result::NotReady);
  CHECK(card_stuck.counters().last_error_stage == sd::InitStage::OpCond);
  CHECK(bus_stuck.slept_ms >= 1000u - sd::policy::kAcmd41PollMs);  // it waited the spec's second ...
  CHECK(bus_stuck.slept_ms <= 1000u + 2u * sd::policy::kAcmd41PollMs);  // ... and not much more
  CHECK(bus_stuck.count(Cmd::AcmdSdSendOpCond) <= 102u);
  CHECK(!bus_stuck.logged(Cmd::AllSendCid));  // nothing after the stage that failed

  // Initialising again after a failure starts over (and can succeed).
  FakeCardSpec flaky;
  FakeSdBus bus_flaky(medium, flaky);
  sd::Card card_flaky(bus_flaky);
  bus_flaky.spec.present = false;
  CHECK(card_flaky.init() == Result::NoCard);
  bus_flaky.spec.present = true;
  CHECK(card_flaky.init() == Result::Ok);
  CHECK(card_flaky.counters().inits == 2u);
}

TEST_CASE("sd card: init stops at the stage that fails and says which") {
  Medium medium(kEightGb);
  // The card answers CMD8 with a wrong echo: not to be used.
  struct BadEcho final : public sd::SdBus {
    FakeSdBus inner;
    BadEcho(Medium& m, const FakeCardSpec& s) : inner(m, s) {}
    bool card_present() const noexcept override { return inner.card_present(); }
    sd::Result power_up() noexcept override { return inner.power_up(); }
    std::uint32_t set_clock(std::uint32_t hz) noexcept override { return inner.set_clock(hz); }
    void set_bus_width(unsigned b) noexcept override { inner.set_bus_width(b); }
    sd::Result command(Cmd c, std::uint32_t a, sd::Response& r) noexcept override {
      const sd::Result res = inner.command(c, a, r);
      if (c == Cmd::SendIfCond && res == sd::Result::Ok) {
        r.word[0] ^= 0x1u;
      }
      return res;
    }
    sd::Result read_data(Cmd c, std::uint32_t a, void* d, std::uint32_t bs, std::uint32_t n, sd::Response& r) noexcept override {
      return inner.read_data(c, a, d, bs, n, r);
    }
    sd::Result write_data(Cmd c, std::uint32_t a, const void* d, std::uint32_t bs, std::uint32_t n, sd::Response& r) noexcept override {
      return inner.write_data(c, a, d, bs, n, r);
    }
    bool write_protect_switch() const noexcept override { return inner.write_protect_switch(); }
    void delay_ms(std::uint32_t ms) noexcept override { inner.delay_ms(ms); }
    std::uint32_t micros() noexcept override { return inner.micros(); }
  } echo(medium, FakeCardSpec{});
  sd::Card card(echo);
  CHECK(card.init() == Result::BadResponse);
  CHECK(card.counters().last_error_stage == sd::InitStage::IfCond);
  CHECK(!echo.inner.logged(Cmd::AcmdSdSendOpCond));

  // An SDUC card (CSD version 3): refused at the CSD.
  struct Uc final : public sd::SdBus {
    FakeSdBus inner;
    Uc(Medium& m, const FakeCardSpec& s) : inner(m, s) {}
    bool card_present() const noexcept override { return inner.card_present(); }
    sd::Result power_up() noexcept override { return inner.power_up(); }
    std::uint32_t set_clock(std::uint32_t hz) noexcept override { return inner.set_clock(hz); }
    void set_bus_width(unsigned b) noexcept override { inner.set_bus_width(b); }
    sd::Result command(Cmd c, std::uint32_t a, sd::Response& r) noexcept override {
      const sd::Result res = inner.command(c, a, r);
      if (c == Cmd::SendCsd && res == sd::Result::Ok) {
        r.word[3] = (r.word[3] & 0x3FFFFFu) | (2u << 22);  // CSD_STRUCTURE = 2: bits 127:126 are the top two of word 3's 24 bits
      }
      return res;
    }
    sd::Result read_data(Cmd c, std::uint32_t a, void* d, std::uint32_t bs, std::uint32_t n, sd::Response& r) noexcept override {
      return inner.read_data(c, a, d, bs, n, r);
    }
    sd::Result write_data(Cmd c, std::uint32_t a, const void* d, std::uint32_t bs, std::uint32_t n, sd::Response& r) noexcept override {
      return inner.write_data(c, a, d, bs, n, r);
    }
    bool write_protect_switch() const noexcept override { return inner.write_protect_switch(); }
    void delay_ms(std::uint32_t ms) noexcept override { inner.delay_ms(ms); }
    std::uint32_t micros() noexcept override { return inner.micros(); }
  } uc(medium, FakeCardSpec{});
  sd::Card card_uc(uc);
  CHECK(card_uc.init() == Result::Unsupported);
  CHECK(card_uc.counters().last_error_stage == sd::InitStage::Csd);
  CHECK(!uc.inner.logged(Cmd::SelectCard));
  CHECK(st::BlockResult::Unsupported == card_uc.start());
}

TEST_CASE("sd card: reads choose CMD17 or CMD18, split at 32 blocks, and deliver every byte") {
  Medium medium(kEightGb);
  for (std::uint32_t lba = 1000; lba < 1000 + 80; ++lba) {
    fill_sector(medium, lba);
  }
  FakeCardSpec spec;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  const unsigned commands_after_init = bus.reads_single + bus.reads_multi;
  CHECK(commands_after_init == 0u);  // the CMD6 status reads are not block reads

  std::vector<std::uint8_t> got(80 * 512 + 3, 0xEE);
  // One block: CMD17, the argument is the block number (block addressed).
  NEED(card.read_blocks(1003, 1, got.data()) == Result::Ok);
  CHECK(bus.reads_single == 1u);
  CHECK(bus.reads_multi == 0u);
  CHECK(bus.last_read_arg == 1003u);
  CHECK(sector_is(got.data(), 1003));
  std::fill(got.begin(), got.end(), std::uint8_t{0xEE});
  // Seventy blocks: three CMD18, 32 + 32 + 6.
  NEED(card.read_blocks(1000, 70, got.data() + 3) == Result::Ok);  // an unaligned destination
  CHECK(bus.reads_single == 1u);
  CHECK(bus.reads_multi == 3u);
  for (std::uint32_t i = 0; i < 70; ++i) {
    CHECK(sector_is(got.data() + 3 + 512u * i, 1000 + i));
  }
  CHECK(got[0] == 0xEE);  // nothing before or after the range was touched
  CHECK(got[3 + 70 * 512] == 0xEE);
  CHECK(card.counters().blocks == 71u);
  CHECK(card.counters().reads == 2u);
  CHECK(card.counters().failures == 0u);
  // A read of exactly 33 blocks: 32 and a single.
  NEED(card.read_blocks(1000, 33, got.data()) == Result::Ok);
  CHECK(bus.reads_multi == 4u);
  CHECK(bus.reads_single == 2u);
  // The BlockDevice face of the same read.
  CHECK(card.read(1000, 2, got.data()) == st::BlockResult::Ok);
  CHECK(sector_is(got.data() + 512, 1001));
}

TEST_CASE("sd card: bad arguments, a read before init, and a card taken out") {
  Medium medium(kEightGb);
  FakeCardSpec spec;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  std::uint8_t buf[512];
  CHECK(card.read_blocks(0, 1, buf) == Result::Uninitialised);
  CHECK(card.read(0, 1, buf) == st::BlockResult::NotReady);
  NEED(card.init() == Result::Ok);
  CHECK(card.read_blocks(0, 0, buf) == Result::Ok);  // nothing asked, nothing done
  CHECK(card.read_blocks(kEightGb, 1, buf) == Result::BadArgument);
  CHECK(card.read_blocks(kEightGb - 1, 2, buf) == Result::BadArgument);
  CHECK(card.read_blocks(0xFFFFFFFFu, 2, buf) == Result::BadArgument);
  CHECK(card.read_blocks(0, 1, nullptr) == Result::BadArgument);
  CHECK(card.read(kEightGb, 1, buf) == st::BlockResult::OutOfRange);
  CHECK(card.read_blocks(kEightGb - 1, 1, buf) == Result::Ok);  // the last block is there
  const unsigned before = static_cast<unsigned>(bus.log.size());
  bus.spec.present = false;
  CHECK(card.read_blocks(10, 1, buf) == Result::NoCard);
  CHECK(bus.log.size() == before);  // the detect switch is checked first: no command is sent to a slot that is empty
  CHECK(!card.ready());
  CHECK(card.read_blocks(10, 1, buf) == Result::Uninitialised);
  bus.spec.present = true;
  CHECK(card.start() == st::BlockResult::Ok);  // back in: a new init
  CHECK(card.ready());
}

TEST_CASE("sd card: a data error is retried, a timeout is not, and the counts say what happened") {
  Medium medium(kEightGb);
  for (std::uint32_t lba = 50; lba < 60; ++lba) {
    fill_sector(medium, lba);
  }
  FakeCardSpec spec;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  std::uint8_t buf[512 * 4];

  bus.spec.data_errors = 2;  // two CRC errors, then the third try is good
  CHECK(card.read_blocks(50, 4, buf) == Result::Ok);
  CHECK(card.counters().retries == 2u);
  CHECK(card.counters().data_errors == 2u);
  CHECK(sector_is(buf + 512 * 3, 53));
  CHECK(bus.reads_multi == 1u);

  bus.spec.data_errors = 3;  // one more than the tries: the read fails
  CHECK(card.read_blocks(50, 4, buf) == Result::DataError);
  CHECK(card.counters().retries == 4u);
  CHECK(card.counters().failures == 1u);
  CHECK(bus.spec.data_errors == 0u);  // the three faults went to the three tries
  CHECK(card.read(50, 4, buf) == st::BlockResult::Ok);  // and the card is fine again

  const unsigned log_before = static_cast<unsigned>(bus.log.size());
  bus.spec.data_timeouts = 1;
  CHECK(card.read_blocks(50, 4, buf) == Result::DataTimeout);
  CHECK(bus.log.size() == log_before + 1u);  // one command: a card that did not answer in time is not asked again
  CHECK(card.counters().data_timeouts == 1u);
  CHECK(card.counters().last_error == Result::DataTimeout);
  CHECK(card.read(50, 4, buf) == st::BlockResult::Ok);  // the next read works

  CHECK(card.read(50, 1, buf) == st::BlockResult::Ok);
  bus.spec.data_timeouts = 1;
  CHECK(card.read(50, 1, buf) == st::BlockResult::Failed);
}

TEST_CASE("sd card: whatever happens, no command outside the read-side allowlist is ever sent") {
  // Run the whole life of a card (init, reads, errors, a second init) and check every command the bus was given. The fake refuses anything that is
  // not known() with Result::Refused, and the card would have failed on it: so the log is all there is, and it is all allowlisted.
  Medium medium(kEightGb);
  fill_sector(medium, 1);
  FakeCardSpec spec;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  std::uint8_t buf[512 * 40];
  NEED(card.init() == Result::Ok);
  for (unsigned i = 0; i < 10; ++i) {
    bus.spec.data_errors = i % 3u;
    (void)card.read_blocks(1, 1 + i * 4u, buf);
  }
  bus.spec.data_timeouts = 1;
  (void)card.read_blocks(1, 8, buf);
  NEED(card.init() == Result::Ok);
  unsigned writes = 0;
  for (const FakeSdBus::Seen& s : bus.log) {
    CHECK(sd::known(s.cmd));
    const unsigned index = sd::wire_index(s.cmd);
    const bool write_class = !sd::is_app(s.cmd) && (index == 24 || index == 25 || index == 27 || index == 28 || index == 29 || index == 32 ||
                                                    index == 33 || index == 38 || index == 42 || index == 56);  // CMD24 included: a read-only life never sends it
    writes += write_class ? 1u : 0u;
  }
  CHECK(writes == 0u);
  CHECK(!bus.log.empty());
  // And the bus layer says no to a value that is not a command (the image's SdhcBus does the same check before it touches a register).
  sd::Response r;
  CHECK(bus.command(static_cast<Cmd>(24), 0, r) == Result::Refused);
  CHECK(bus.command(static_cast<Cmd>(25), 0, r) == Result::Refused);
  CHECK(bus.command(static_cast<Cmd>(sd::kAppFlag | 23), 0, r) == Result::Refused);
}

// ---- OS-006c: the write path --------------------------------------------------------------------------------------------------------------------

TEST_CASE("sd card write: one CMD24 and one CMD13 per block, in order, the right address, the bytes on the medium") {
  Medium medium(kEightGb);
  FakeCardSpec spec;
  spec.write_busy_polls = 2;  // the card says "programming" twice after each block
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  const std::size_t before = bus.log.size();
  std::uint8_t data[512 * 3];
  for (unsigned b = 0; b < 3; ++b) {
    for (unsigned i = 0; i < 512; ++i) {
      data[b * 512 + i] = pattern(5000 + b, i);
    }
  }
  CHECK(card.write_blocks(5000, 3, data) == Result::Ok);
  const std::vector<unsigned> seq = sequence(bus);
  const std::vector<unsigned> tail(seq.begin() + static_cast<std::ptrdiff_t>(before), seq.end());
  // per block: CMD24, then CMD13 until ready (two "programming" answers, then ready)
  const std::vector<unsigned> expected = {24, 13, 13, 13, 24, 13, 13, 13, 24, 13, 13, 13};
  CHECK(tail == expected);
  CHECK(bus.writes == 3u);
  CHECK(medium.blocks_written == 3u);
  CHECK(bus.last_write_arg == 5002u);  // block addressed (SDHC): the argument is the block number
  for (unsigned b = 0; b < 3; ++b) {
    std::uint8_t got[512];
    medium.read_sector(5000 + b, got);
    CHECK(sector_is(got, 5000 + b));
  }
  CHECK(card.counters().writes == 1u);
  CHECK(card.counters().blocks_written == 3u);
  // Through the BlockDevice view, and a read of the same blocks gives the bytes back.
  std::uint8_t back[512 * 3];
  CHECK(card.write(6000, 3, data) == st::BlockResult::Ok);
  CHECK(card.read(6000, 3, back) == st::BlockResult::Ok);
  CHECK(std::memcmp(back, data, sizeof data) == 0);
  // Out of range and a missing buffer send nothing.
  const std::size_t n = bus.log.size();
  CHECK(card.write_blocks(kEightGb - 1u, 2, data) == Result::BadArgument);
  CHECK(card.write_blocks(10, 1, nullptr) == Result::BadArgument);
  CHECK(card.write_blocks(10, 0, data) == Result::Ok);
  CHECK(bus.log.size() == n);
}

TEST_CASE("sd card write: an SDSC card is written by byte address") {
  Medium medium(2'000'000u);
  FakeCardSpec spec;
  spec.v2 = false;
  spec.csd_v1 = true;
  FakeSdBus bus(medium, spec);
  sd::Card card(bus);
  NEED(card.init() == Result::Ok);
  std::uint8_t data[512] = {1, 2, 3};
  CHECK(card.write_blocks(40, 1, data) == Result::Ok);
  CHECK(bus.last_write_arg == 40u * 512u);
}

TEST_CASE("sd card write: a write-protected card is refused before anything is sent") {
  for (const unsigned which : {0u, 1u}) {
    Medium medium(kEightGb);
    FakeCardSpec spec;
    spec.csd_wp = which == 0u;
    spec.wp_switch = which == 1u;
    FakeSdBus bus(medium, spec);
    sd::Card card(bus);
    NEED(card.init() == Result::Ok);
    CHECK(card.write_protected());
    std::uint8_t data[512] = {};
    CHECK(card.write_blocks(100, 1, data) == Result::WriteProtected);
    CHECK(card.write(100, 1, data) == st::BlockResult::WriteProtected);
    CHECK(!bus.logged(Cmd::WriteBlock));
    CHECK(medium.blocks_written == 0u);
    // Reads work as before.
    CHECK(card.read(100, 1, data) == st::BlockResult::Ok);
  }
}

TEST_CASE("sd card write: a CRC status error, a timeout, a card that stays busy and a card pulled out end the call") {
  std::uint8_t data[512 * 2] = {};
  {  // the first block's CRC status says "rejected": the call ends there, the second block is never sent
    Medium medium(kEightGb);
    FakeCardSpec spec;
    spec.write_crc_errors = 1;
    FakeSdBus bus(medium, spec);
    sd::Card card(bus);
    NEED(card.init() == Result::Ok);
    CHECK(card.write_blocks(200, 2, data) == Result::DataError);
    CHECK(bus.count(Cmd::WriteBlock) == 1u);
    CHECK(card.counters().write_failures == 1u);
    CHECK(card.counters().data_errors == 1u);
    CHECK(card.write_blocks(200, 2, data) == Result::Ok);  // no retry inside a call, a new call works
  }
  {  // the data phase times out
    Medium medium(kEightGb);
    FakeCardSpec spec;
    spec.write_timeouts = 1;
    FakeSdBus bus(medium, spec);
    sd::Card card(bus);
    NEED(card.init() == Result::Ok);
    CHECK(card.write_blocks(200, 1, data) == Result::DataTimeout);
    CHECK(card.write(200, 1, data) == st::BlockResult::Ok);
  }
  {  // the card never leaves programming: CMD13 is polled for at most kWriteStatusUs, then NotReady
    Medium medium(kEightGb);
    FakeCardSpec spec;
    spec.write_stuck_busy = true;
    FakeSdBus bus(medium, spec);
    sd::Card card(bus);
    NEED(card.init() == Result::Ok);
    CHECK(card.write_blocks(200, 2, data) == Result::NotReady);
    CHECK(bus.count(Cmd::WriteBlock) == 1u);
    CHECK(bus.count(Cmd::SendStatus) <= sd::policy::kWriteStatusUs / 1000u + 2u);  // the fake clock runs 1 ms a call while it is stuck
    CHECK(bus.count(Cmd::SendStatus) >= 2u);
  }
  {  // the card taken out between calls
    Medium medium(kEightGb);
    FakeCardSpec spec;
    FakeSdBus bus(medium, spec);
    sd::Card card(bus);
    NEED(card.init() == Result::Ok);
    bus.spec.present = false;
    CHECK(card.write_blocks(200, 1, data) == Result::NoCard);
    CHECK(!card.ready());
    CHECK(card.write_blocks(200, 1, data) == Result::Uninitialised);
  }
}
