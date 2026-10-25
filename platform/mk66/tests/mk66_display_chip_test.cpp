// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FT812 bring-up and access (display/chip.h) against a model of the chip (mk66_display_fakes.h): the order of the init
// as a recorded transcript, the warm-start probe, every failure path with its time bound, the speed classes, the register
// access framing, the command FIFO and its fault recovery. Runs on the host.
#include <doctest/doctest.h>

#include "mk66_display_fakes.h"

#include <o3sdk/platform/mk66/display/chip.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace display = o3sdk::platform::mk66::display;
namespace ft = o3sdk::platform::mk66::display::ft812;
using o3sdk::platform::mk66::display::fake::FakeChip;
using Write = std::pair<std::uint32_t, std::uint32_t>;

namespace {

// The init transcript of a cold chip with this panel, as recorded from the model and then read against the Guide and
// Riedel's EVE_init (the differences from his order are listed in track3r-os tests/eve_riedel_test.cpp). Reads and the 1 ms
// poll delays are left out; "wN addr value" is an N-byte write.
const char* const kColdTranscript[] = {
    "speed init",
    "pd low",
    "delay 20",
    "pd high",
    "delay 40",
    "host 68 00",     // RST_PULSE
    "host 00 00",     // ACTIVE
    "delay 60",
    "w1 3020D4 00",   // backlight off
    "w1 302070 00",   // pixel clock stopped before the timing is written
    "w2 302034 0320", // HSIZE 800
    "w2 30202C 0420", // HCYCLE 1056
    "w2 302030 002E", // HOFFSET 46
    "w2 302038 0000", // HSYNC0 0
    "w2 30203C 000A", // HSYNC1 10
    "w2 302048 01E0", // VSIZE 480
    "w2 302040 020D", // VCYCLE 525
    "w2 302044 0017", // VOFFSET 23
    "w2 30204C 0000", // VSYNC0 0
    "w2 302050 000A", // VSYNC1 10
    "w1 302064 00",   // SWIZZLE
    "w1 30206C 01",   // PCLK_POL
    "w1 302068 00",   // CSPREAD
    "w1 302058 00",   // ROTATE
    "w1 302060 01",   // DITHER
    "w2 30205C 01B6", // OUTBITS
    "w1 302080 00",   // audio playback volume
    "w1 302084 00",   // synthesiser volume
    "w2 302088 0060", // synthesiser: mute
    "w4 300000 02000000",  // first list: clear colour black
    "w4 300004 26000007",  // clear colour, stencil, tag
    "w4 300008 00000000",  // display
    "w4 302054 00000002",  // swap at the next frame (a word write, as the Guide)
    "w2 30209C 8000", // DISP
    "w2 302098 8000", // DISP is an output
    "w1 302070 02",   // pixel clock: last
    "speed data",
};

std::vector<std::string> host_commands(const FakeChip& chip) {
  std::vector<std::string> out;
  for (const std::string& line : chip.transcript()) {
    if (line.compare(0, 5, "host ") == 0) {
      out.push_back(line.substr(5, 2));
    }
  }
  return out;
}

std::size_t line_of(const std::vector<std::string>& t, const std::string& s) {
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t[i] == s) {
      return i;
    }
  }
  return t.size();
}

}  // namespace

TEST_CASE("display chip: a cold chip's init matches the recorded transcript, line for line") {
  FakeChip chip;
  display::Chip driver(chip);
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::Ok);
  CHECK_FALSE(report.warm_start);
  CHECK(report.entry_id != ft::kChipId);
  CHECK_FALSE(report.recovered_coprocessor);
  CHECK_FALSE(report.bus_error);
  CHECK(report.id_polls >= 1);
  CHECK(report.id_polls < 100);

  const std::vector<std::string>& t = chip.transcript();
  const std::size_t n = sizeof(kColdTranscript) / sizeof(kColdTranscript[0]);
  CHECK(t.size() == n);
  for (std::size_t i = 0; i < n && i < t.size(); ++i) {
    CHECK(t[i] == kColdTranscript[i]);
  }
  CHECK(chip.speed() == display::SpiSpeed::Data);
  CHECK(chip.reg(ft::reg::kPwmDuty) == 0);  // the backlight is the caller's, after the first real frame
}

TEST_CASE("display chip: the order of the steps that matter to the chip's safety") {
  FakeChip chip;
  display::Chip driver(chip);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  const std::vector<std::string>& t = chip.transcript();
  // power-down pulse < reset < ACTIVE < settle < any register write < pixel clock last
  CHECK(line_of(t, "pd low") < line_of(t, "pd high"));
  CHECK(line_of(t, "pd high") < line_of(t, "host 68 00"));
  CHECK(line_of(t, "host 68 00") < line_of(t, "host 00 00"));
  CHECK(line_of(t, "host 00 00") < line_of(t, "w1 3020D4 00"));
  // backlight off before anything is configured; the pixel clock stopped before the timing is rewritten
  CHECK(line_of(t, "w1 3020D4 00") < line_of(t, "w2 302034 0320"));
  CHECK(line_of(t, "w1 302070 00") < line_of(t, "w2 302034 0320"));
  // the first list and its swap are in place before DISP and the clock start
  CHECK(line_of(t, "w4 300008 00000000") < line_of(t, "w4 302054 00000002"));
  CHECK(line_of(t, "w4 302054 00000002") < line_of(t, "w2 30209C 8000"));
  CHECK(line_of(t, "w2 30209C 8000") < line_of(t, "w1 302070 02"));
  CHECK(t.size() >= 2);
  CHECK(t[t.size() - 2] == "w1 302070 02");  // PCLK is the last register write
  // the data speed only after the scan-out proved itself
  CHECK(t.back() == "speed data");
  CHECK(chip.reg(ft::reg::kPclk) == 2);
  // the first list as swapped in: black, one clear, display
  const std::vector<std::uint8_t>& shown = chip.displayed();
  CHECK(shown.size() >= 12);  // a first list was swapped in
  CHECK(chip.ram_dl_word(0) == ft::kDlClearColorBlack);
  CHECK(chip.ram_dl_word(1) == ft::kDlClearAll);
  CHECK(chip.ram_dl_word(2) == ft::kDlDisplay);
}

TEST_CASE("display chip: the backlight goes off the moment the chip answers, before the engines' reset is even awaited") {
  using o3sdk::platform::mk66::display::fake::Txn;
  FakeChip chip;
  display::Chip driver(chip);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  std::size_t first_pwm_write = chip.txns().size();
  std::size_t first_cpureset_read = chip.txns().size();
  std::size_t last_id_read = 0;
  for (std::size_t i = 0; i < chip.txns().size(); ++i) {
    const Txn& t = chip.txns()[i];
    if (t.kind == Txn::Kind::Write && t.addr == ft::reg::kPwmDuty && first_pwm_write == chip.txns().size()) {
      first_pwm_write = i;
    }
    if (t.kind == Txn::Kind::Read && t.addr == ft::reg::kCpuReset && first_cpureset_read == chip.txns().size()) {
      first_cpureset_read = i;
    }
    if (t.kind == Txn::Kind::Read && t.addr == ft::reg::kId && t.value == ft::kChipId && last_id_read == 0) {
      last_id_read = i;  // the first read that saw 0x7C
    }
  }
  CHECK(first_pwm_write < chip.txns().size());
  CHECK(first_pwm_write > last_id_read);          // not before the chip can take a register write ...
  CHECK(first_pwm_write < first_cpureset_read);   // ... and before anything else is waited for
}

TEST_CASE("display chip: no SPI access sooner than 20 ms after boot: the init waits for the uptime") {
  static_assert(display::kBootSettleMs == 20, "the FT812 needs 20 ms from PD# high to its first access (Guide p.11)");
  {
    FakeChip chip;
    chip.set_time_ms(5);  // 5 ms after SysTick started
    display::Chip driver(chip);
    const std::uint64_t t0 = chip.now_us();
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    const std::vector<std::string>& t = chip.transcript();
    CHECK(t.size() > 2);
    if (t.size() > 2) {
      CHECK(t[0] == "delay 15");  // the rest of the 20 ms, before the speed is set or anything is sent
      CHECK(t[1] == "speed init");
    }
    // the first chip-select of the init happened no earlier than 20 ms of uptime
    CHECK(chip.first_access_ms() >= 20u);
    CHECK(chip.first_access_ms() < 30u);
    CHECK(t0 / 1000u == 5u);
  }
  {
    FakeChip chip;
    chip.set_time_ms(0);  // as early as it gets: the whole 20 ms
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(chip.transcript()[0] == "delay 20");
  }
  {
    FakeChip chip;
    chip.set_time_ms(20);  // exactly there: nothing to wait for
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(chip.transcript()[0] == "speed init");
  }
  {
    FakeChip chip;
    chip.set_time_ms(4'000'000);  // hours up (a retry, a re-init): never a wait
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(chip.transcript()[0] == "speed init");
  }
}

TEST_CASE("display chip: a warm chip is told CLKINT explicitly, so a CLKEXT that survived an unwired PD# does not") {
  {
    FakeChip chip(/*warm=*/true);
    chip.pd_wired = false;
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(host_commands(chip) == std::vector<std::string>({"68", "48", "00"}));  // reset, internal clock, ACTIVE
  }
  {
    FakeChip chip(/*warm=*/true);  // PD# wired: the same, it costs nothing
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(host_commands(chip) == std::vector<std::string>({"68", "48", "00"}));
  }
  {
    FakeChip chip(/*warm=*/true);  // an explicit choice is honoured
    display::Chip driver(chip);
    display::InitConfig config;
    config.clock = display::HostClock::External;
    CHECK(driver.init(config).status == display::InitStatus::Ok);
    CHECK(host_commands(chip) == std::vector<std::string>({"68", "44", "00"}));
  }
  {
    FakeChip chip;  // cold: nothing (none is needed)
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(host_commands(chip) == std::vector<std::string>({"68", "00"}));
  }
}

TEST_CASE("display chip: the first register writes once the chip answers are the backlight off and its frequency") {
  FakeChip chip;
  display::Chip driver(chip);
  display::InitConfig config;
  config.backlight_hz = 1000;
  CHECK(driver.init(config).status == display::InitStatus::Ok);
  const auto& w = chip.writes();
  CHECK(w.size() > 3);
  if (w.size() > 3) {
    CHECK(w[0] == Write{ft::reg::kPwmDuty, 0u});
    CHECK(w[1] == Write{ft::reg::kPwmHz, 1000u});
    CHECK(w[2].first != ft::reg::kPwmDuty);  // and nothing else of the backlight afterwards
  }
}

TEST_CASE("display chip: the init takes the orientation setting and nothing else from it") {
  for (unsigned o : {0u, 1u, 2u, 7u}) {
    FakeChip chip;
    display::Chip driver(chip);
    display::InitConfig config;
    config.orientation = static_cast<display::Orientation>(o);
    CHECK(driver.init(config).status == display::InitStatus::Ok);
    CHECK(chip.reg(ft::reg::kRotate) == o);
  }
}

TEST_CASE("display chip: optional clock command and backlight frequency") {
  {
    FakeChip chip;
    display::Chip driver(chip);
    display::InitConfig config;
    config.clock = display::HostClock::Internal;
    CHECK(driver.init(config).status == display::InitStatus::Ok);
    CHECK(line_of(chip.transcript(), "host 48 00") < line_of(chip.transcript(), "host 00 00"));
  }
  {
    FakeChip chip;
    display::Chip driver(chip);
    display::InitConfig config;
    config.clock = display::HostClock::External;
    config.soft_reset = false;
    config.backlight_hz = 1000;
    CHECK(driver.init(config).status == display::InitStatus::Ok);
    CHECK(line_of(chip.transcript(), "host 44 00") < line_of(chip.transcript(), "host 00 00"));
    CHECK(line_of(chip.transcript(), "host 68 00") == chip.transcript().size());  // no reset command was asked for
    CHECK(chip.reg(ft::reg::kPwmHz) == 1000);
  }
  {
    FakeChip chip;  // the default: nothing sent, the chip's 250 Hz stays
    display::Chip driver(chip);
    CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
    CHECK(line_of(chip.transcript(), "host 48 00") == chip.transcript().size());
    CHECK(line_of(chip.transcript(), "host 44 00") == chip.transcript().size());
    CHECK(chip.first_write_index(ft::reg::kPwmHz) < 0);
  }
}

TEST_CASE("display chip: a panel timing the controller would mis-scan is refused before anything is done to the chip") {
  FakeChip chip;
  display::Chip driver(chip);
  display::InitConfig config;
  config.panel.hoffset = 400;  // the active line would run past the cycle
  const display::InitReport report = driver.init(config);
  CHECK(report.status == display::InitStatus::BadConfig);
  CHECK(chip.transcript().empty());  // no PD# pulse, no speed change, no register
  CHECK(chip.writes().empty());
  CHECK(chip.selects() == 0u);
  config = display::InitConfig{};
  config.panel.pclk = 0;  // a panel that never gets a clock
  CHECK(driver.init(config).status == display::InitStatus::BadConfig);
  CHECK(chip.selects() == 0u);
}

TEST_CASE("display chip: a warm chip (left running by the bootloader) is seen, silenced first and brought up clean") {
  FakeChip chip(/*warm=*/true);
  display::Chip driver(chip);
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::Ok);
  CHECK(report.warm_start);
  CHECK(report.entry_id == ft::kChipId);
  CHECK(report.entry_clock > 900'000'000u);  // REG_CLOCK says it had been running for ages
  CHECK(report.entry_frames >= 12345u);
  const std::vector<std::string>& t = chip.transcript();
  // the very first thing done to a warm chip is the backlight off, before PD# is touched
  CHECK(t.size() > 2);
  if (t.size() > 2) {
    CHECK(t[1] == "w1 3020D4 00");
  }
  CHECK(line_of(t, "w1 3020D4 00") < line_of(t, "pd low"));
  // and it was reset: its frame count started over
  CHECK(chip.reg(ft::reg::kPclk) == 2);
}

TEST_CASE("display chip: PD# not wired: the reset command is what resets a warm chip") {
  FakeChip chip(/*warm=*/true);
  chip.pd_wired = false;
  display::Chip driver(chip);
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::Ok);
  CHECK(report.warm_start);
  CHECK(chip.awake());
  CHECK(line_of(chip.transcript(), "host 68 00") < chip.transcript().size());
}

TEST_CASE("display chip: no chip: NoChip, after a bounded wait, with the backlight and the clock never touched") {
  FakeChip chip;
  chip.present = false;
  display::Chip driver(chip);
  const std::uint64_t t0 = chip.now_us();
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::NoChip);
  CHECK(report.id_polls == display::kIdPollLimit);
  const std::uint64_t spent_ms = (chip.now_us() - t0) / 1000u;
  CHECK(spent_ms >= display::kPdLowMs + display::kPdHighWaitMs + display::kActiveWaitMs + display::kIdPollLimit);
  CHECK(spent_ms < 1000u);  // a missing display costs under a second per attempt, never a hang
  CHECK(chip.writes().empty());
  CHECK(chip.speed() == display::SpiSpeed::Init);
}

TEST_CASE("display chip: an engine that never leaves reset is ResetTimeout, still bounded, nothing configured") {
  FakeChip chip;
  chip.cpureset_stuck = true;
  display::Chip driver(chip);
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::ResetTimeout);
  CHECK(report.reset_polls == display::kResetPollLimit);
  CHECK(chip.first_write_index(ft::reg::kPclk) < 0);
  CHECK(chip.first_write_index(ft::reg::kHsize) < 0);
}

TEST_CASE("display chip: stale coprocessor pointers are recovered before the first frame; a recovery that fails is reported") {
  {
    FakeChip chip(/*warm=*/true);
    chip.pd_wired = false;  // the model keeps its registers through the PD# pulse
    display::Chip driver(chip);
    // The model resets the pointers on RST_PULSE: plant stale ones by hand, as the bootloader's last frame would have left
    // them, and init the already-warm chip with the reset command off.
    driver.wr16(ft::reg::kCmdRead, 0x40);
    driver.wr16(ft::reg::kCmdWrite, 0x80);
    display::InitConfig config;
    config.soft_reset = false;
    const display::InitReport report = driver.init(config);
    CHECK(report.status == display::InitStatus::Ok);
    CHECK(report.recovered_coprocessor);
    CHECK(driver.fault_count() == 1u);
    CHECK(chip.reg(ft::reg::kCmdRead) == 0u);
    CHECK(chip.reg(ft::reg::kCmdWrite) == 0u);
  }
  {
    FakeChip chip(/*warm=*/true);
    chip.pd_wired = false;
    chip.recover_fails = true;
    display::Chip driver(chip);
    driver.wr16(ft::reg::kCmdWrite, 0x80);
    display::InitConfig config;
    config.soft_reset = false;
    const display::InitReport report = driver.init(config);
    CHECK(report.status == display::InitStatus::CoprocessorStuck);
    CHECK(chip.first_write_index(ft::reg::kPclk) < 0);  // never got as far as configuring the panel
  }
}

TEST_CASE("display chip: a scan-out that never starts is NoFrames and the SPI stays at the init speed") {
  FakeChip chip;
  chip.scan_dead = true;
  display::Chip driver(chip);
  const std::uint64_t t0 = chip.now_us();
  const display::InitReport report = driver.init(display::InitConfig{});
  CHECK(report.status == display::InitStatus::NoFrames);
  CHECK(chip.speed() == display::SpiSpeed::Init);
  CHECK((chip.now_us() - t0) / 1000u < 1000u);
}

TEST_CASE("display chip: register access frames: sizes, byte order, burst") {
  FakeChip chip;
  display::Chip driver(chip);
  driver.host_command(ft::host::kActive);
  chip.advance_ms(100);  // the chip answers
  driver.wr8(0x000100u, 0xAB);
  driver.wr16(0x000102u, 0xBEEF);
  driver.wr32(0x000104u, 0xDEADC0DEu);
  CHECK(chip.ram_g(0x100) == 0xAB);
  CHECK(chip.ram_g(0x102) == 0xEF);
  CHECK(chip.ram_g(0x103) == 0xBE);
  CHECK(driver.rd8(0x000100u) == 0xAB);
  CHECK(driver.rd16(0x000102u) == 0xBEEF);
  CHECK(driver.rd32(0x000104u) == 0xDEADC0DEu);
  const std::uint8_t block[6] = {1, 2, 3, 4, 5, 6};
  driver.write_block(0x000200u, block, sizeof block);
  std::uint8_t back[6] = {};
  driver.read_block(0x000200u, back, sizeof back);
  for (std::size_t i = 0; i < sizeof block; ++i) {
    CHECK(back[i] == block[i]);
  }
  CHECK(driver.rd8(ft::reg::kId) == ft::kChipId);
  // empty and null blocks do nothing (no chip-select cycle)
  const std::uint32_t before = chip.selects();
  driver.write_block(0, nullptr, 4);
  driver.write_block(0, block, 0);
  driver.read_block(0, nullptr, 4);
  CHECK(chip.selects() == before);
  // backlight is clamped to the register's range
  driver.set_backlight(200);
  CHECK(chip.reg(ft::reg::kPwmDuty) == ft::kPwmDutyMax);
  driver.set_backlight(0);
  CHECK(chip.reg(ft::reg::kPwmDuty) == 0u);
}

TEST_CASE("display chip: the command FIFO: flow-controlled writes, idle wait, fault found and recovered") {
  FakeChip chip;
  display::Chip driver(chip);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  const std::uint32_t words[5] = {0xFFFFFF00u, 1, 2, 3, 4};
  CHECK(driver.cmd_write(words, 5, 50));
  CHECK(driver.cmd_wait_idle(50));
  CHECK_FALSE(driver.cmd_check());
  CHECK(driver.fault_count() == 0u);
  CHECK(driver.cmd_write(words, 0, 50));  // nothing to write is fine

  chip.inject_fault();  // the coprocessor stops: REG_CMD_READ = 0xFFF
  CHECK(driver.cmd_check());
  CHECK(driver.fault_count() == 1u);
  CHECK_FALSE(driver.cmd_check());  // recovered: the pointers read normally again
  CHECK(chip.reg(ft::reg::kCmdRead) == 0u);
  CHECK(chip.reg(ft::reg::kCmdWrite) == 0u);

  chip.inject_fault();
  CHECK_FALSE(driver.cmd_write(words, 5, 50));  // a fault in the middle: reported, recovered
  CHECK(driver.fault_count() == 2u);
  chip.inject_fault();
  CHECK_FALSE(driver.cmd_wait_idle(50));
  CHECK(driver.fault_count() == 3u);
  CHECK(driver.cmd_wait_idle(50));  // and the next wait is clean
}

TEST_CASE("display chip: the recovery order is the Guide's: reset held, pointers zeroed, reset released") {
  FakeChip chip;
  display::Chip driver(chip);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  const std::size_t before = chip.writes().size();
  driver.cmd_recover();
  const auto& w = chip.writes();
  CHECK(w.size() == before + 5);
  if (w.size() != before + 5) {
    return;
  }
  CHECK(w[before + 0] == Write{ft::reg::kCpuReset, 1u});
  CHECK(w[before + 1].first == ft::reg::kCmdRead);
  CHECK(w[before + 2].first == ft::reg::kCmdWrite);
  CHECK(w[before + 3].first == ft::reg::kCmdDl);  // Riedel's extra
  CHECK(w[before + 4] == Write{ft::reg::kCpuReset, 0u});
}
