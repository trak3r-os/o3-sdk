// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Si4703 driver (si4703.h, OS-011c) against a model of the chip: the wire format, the region and channel arithmetic, the power-up sequence with its waits, tune and seek,
// the refusals (a chip that does not answer, a tune that never completes, a bus that fails), and the requests made while the chip is busy. Host and Cortex-M4F. What only the
// board shows (the wiring, the reset, the audio level) is the first hardware session's.
#include <doctest/doctest.h>

#include <o3sdk/hal/capture.h>
#include <o3sdk/platform/mk66/si4703.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace cap = o3sdk::hal::capture;
namespace si = o3sdk::platform::mk66::si4703;
using o3sdk::platform::mk66::I2cBus;
using o3sdk::platform::mk66::I2cResult;

namespace {

// The chip: sixteen registers, a read that returns them from 0x0A on, a write that stores 0x02..0x07. TUNE or SEEK set starts an operation that completes (STC set, READCHAN
// updated) after `stc_polls` reads of the status; clearing the bit clears STC.
class Chip final : public I2cBus {
 public:
  Chip() {
    reg.r[si::kDeviceId] = 0x1242;
    reg.r[si::kStatusRssi] = 0x0000;
  }
  I2cResult read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept override {
    ++reads;
    ops.push_back('R');
    if (!answers || address7 != si::kAddress || rx_len != si::kReadBytes || fail_reads > 0) {
      if (fail_reads > 0) {
        --fail_reads;
        return fail_with;
      }
      return I2cResult::Nak;
    }
    advance();
    reg.r[si::kStatusRssi] = static_cast<std::uint16_t>((reg.r[si::kStatusRssi] & ~(si::kStereo | 0xFFu)) | (stereo ? si::kStereo : 0u) | rssi);
    for (unsigned i = 0; i < si::kReadBytes / 2u; ++i) {
      const unsigned r = (si::kStatusRssi + i) & 15u;
      rx[2u * i] = static_cast<std::uint8_t>(reg.r[r] >> 8);
      rx[2u * i + 1u] = static_cast<std::uint8_t>(reg.r[r] & 0xFFu);
    }
    return I2cResult::Ok;
  }
  I2cResult transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t*, std::uint8_t rx_len) noexcept override {
    ++writes;
    ops.push_back('W');
    if (!answers || address7 != si::kAddress || tx_len != si::kWriteBytes || rx_len != 0u || fail_writes > 0) {
      if (fail_writes > 0) {
        --fail_writes;
        return fail_with;
      }
      return I2cResult::Nak;
    }
    for (unsigned i = 0; i < si::kWriteBytes / 2u; ++i) {
      reg.r[si::kPowerCfg + i] = static_cast<std::uint16_t>((static_cast<unsigned>(tx[2u * i]) << 8) | tx[2u * i + 1u]);
    }
    // TUNE / SEEK edges
    const bool tune = (reg.r[si::kChannel] & si::kTune) != 0u;
    const bool seek = (reg.r[si::kPowerCfg] & si::kSeek) != 0u;
    if ((tune && !tune_active) || (seek && !seek_active)) {
      pending = (tune && !tune_active) ? 1 : 2;
      countdown = stc_polls;
      reg.r[si::kStatusRssi] = static_cast<std::uint16_t>(reg.r[si::kStatusRssi] & ~si::kSeekTuneComplete);
    }
    if ((!tune && tune_active) || (!seek && seek_active)) {
      reg.r[si::kStatusRssi] = static_cast<std::uint16_t>(reg.r[si::kStatusRssi] & ~si::kSeekTuneComplete);
      pending = 0;
    }
    tune_active = tune;
    seek_active = seek;
    return I2cResult::Ok;
  }
  I2cResult recover() noexcept override {
    ++recovers;
    return I2cResult::Ok;
  }

  // One status read of an operation in progress.
  void advance() {
    if (pending == 0) {
      return;
    }
    if (never_completes) {
      return;
    }
    if (countdown > 0) {
      --countdown;
      return;
    }
    if (pending == 1) {
      reg.r[si::kReadChan] = static_cast<std::uint16_t>(reg.r[si::kChannel] & si::kChannelMask);
    } else {
      const bool up = (reg.r[si::kPowerCfg] & si::kSeekUp) != 0u;
      if (station_channel < 0) {
        reg.r[si::kStatusRssi] = static_cast<std::uint16_t>(reg.r[si::kStatusRssi] | si::kSeekFailBandLimit);
      } else {
        reg.r[si::kStatusRssi] = static_cast<std::uint16_t>(reg.r[si::kStatusRssi] & ~si::kSeekFailBandLimit);
        reg.r[si::kReadChan] = static_cast<std::uint16_t>(station_channel);
        (void)up;
      }
    }
    reg.r[si::kStatusRssi] = static_cast<std::uint16_t>(reg.r[si::kStatusRssi] | si::kSeekTuneComplete);
    pending = 0;
  }

  // What the reset line does to the chip: everything back to its power-on state.
  void power_cycle() {
    reg = si::Registers{};
    reg.r[si::kDeviceId] = 0x1242;
    pending = 0;
    tune_active = false;
    seek_active = false;
  }

  si::Registers reg;
  bool answers = true;
  bool stereo = false;
  std::uint8_t rssi = 40;
  unsigned stc_polls = 2;
  bool never_completes = false;
  int station_channel = 77;
  unsigned fail_reads = 0;
  unsigned fail_writes = 0;
  I2cResult fail_with = I2cResult::Nak;  // what a failed transfer says
  std::string ops;                       // every transfer, 'R' or 'W', in order
  unsigned reads = 0;
  unsigned writes = 0;
  unsigned recovers = 0;
  bool tune_active = false;
  bool seek_active = false;
  int pending = 0;
  unsigned countdown = 0;
};

class FakePins final : public si::Pins {
 public:
  explicit FakePins(Chip* c = nullptr) : chip(c) {}
  bool reset() noexcept override {
    ++resets;
    if (chip != nullptr) {
      chip->power_cycle();
    }
    return reset_ok;
  }
  void hold_reset() noexcept override { ++holds; }
  unsigned resets = 0;
  unsigned holds = 0;
  bool reset_ok = true;
  Chip* chip;
};

// Runs the driver one pass per millisecond until `done` or `limit_ms`; returns the time it took.
template <typename Done>
std::uint64_t run(si::Radio& radio, std::uint64_t& now, std::uint64_t limit_ms, Done done) {
  const std::uint64_t t0 = now;
  while (now - t0 < limit_ms && !done()) {
    radio.service(now);
    ++now;
  }
  return now - t0;
}

using State = cap::RadioStatus::State;

}  // namespace

TEST_CASE("si4703: the wire format is 0x0A..0x0F then 0x00..0x09 on a read and 0x02..0x07 on a write") {
  std::uint8_t bytes[si::kReadBytes];
  for (unsigned i = 0; i < si::kReadBytes; ++i) {
    bytes[i] = static_cast<std::uint8_t>(0x10u + i);
  }
  si::Registers regs;
  si::decode_read(bytes, regs);
  CHECK(regs.r[0x0A] == 0x1011);
  CHECK(regs.r[0x0F] == 0x1A1B);
  CHECK(regs.r[0x00] == 0x1C1D);
  CHECK(regs.r[0x09] == 0x2E2F);
  std::uint8_t out[si::kWriteBytes];
  si::encode_write(regs, out);
  si::Registers r2;
  for (unsigned i = 2; i < 8; ++i) {
    r2.r[i] = static_cast<std::uint16_t>(0x0100u * i + i + 0x40u);
  }
  si::encode_write(r2, out);
  CHECK(out[0] == 0x02);
  CHECK(out[1] == 0x42);
  CHECK(out[10] == 0x07);
  CHECK(out[11] == 0x47);
}

TEST_CASE("si4703: the constants are the data sheet's register definitions") {
  CHECK(si::kEnable == 0x01);
  CHECK(si::kDisable == 0x40);
  CHECK(si::kSeek == 0x0100);
  CHECK(si::kSeekUp == 0x0200);
  CHECK(si::kSkMode == 0x0400);
  CHECK(si::kRdsMode == 0x0800);
  CHECK(si::kMono == 0x2000);
  CHECK(si::kDMute == 0x4000);
  CHECK(si::kDSMute == 0x8000);
  CHECK(si::kTune == 0x8000);
  CHECK(si::kDeEmphasis == 0x0800);
  CHECK(si::kStereo == 0x0100);
  CHECK(si::kSeekFailBandLimit == 0x2000);
  CHECK(si::kSeekTuneComplete == 0x4000);
  CHECK(si::kPowerCfgRun == 0xE801);  // ENABLE | DMUTE | DSMUTE | RDSM | MONO
  CHECK(si::kTest1Start == 0x8100);
}

TEST_CASE("si4703: the regions set SPACE, BAND and DE and keep the other bits") {
  struct Row {
    cap::Region region;
    std::uint16_t sys1_de;
    unsigned space;
    unsigned band;
  };
  const Row rows[] = {
      {cap::Region::Europe, si::kDeEmphasis, 1, 0},
      {cap::Region::Usa, 0, 0, 0},
      {cap::Region::Australia, si::kDeEmphasis, 0, 0},
      {cap::Region::Japan, si::kDeEmphasis, 1, 1},
  };
  for (const Row& row : rows) {
    si::Registers regs;
    regs.r[si::kSysConfig1] = 0x1234 | si::kDeEmphasis;  // RDS and the rest, and DE set to start with
    regs.r[si::kSysConfig2] = 0xF30A;                    // SEEKTH, an unknown pair of SPACE/BAND bits, volume
    si::apply_region(regs, row.region);
    CHECK((regs.r[si::kSysConfig1] & si::kDeEmphasis) == row.sys1_de);
    CHECK((regs.r[si::kSysConfig1] & ~si::kDeEmphasis) == (0x1234 & ~si::kDeEmphasis));
    CHECK(((regs.r[si::kSysConfig2] >> 4) & 3u) == row.space);
    CHECK(((regs.r[si::kSysConfig2] >> 6) & 3u) == row.band);
    CHECK((regs.r[si::kSysConfig2] & 0xFF0Fu) == 0xF30Au);  // SEEKTH and the volume untouched
  }
}

TEST_CASE("capture: the bands, the channel arithmetic, snapping and stepping") {
  // Europe: 87.5 .. 108.0 MHz in 100 kHz steps: 206 channels.
  CHECK(cap::channel_count(cap::Region::Europe) == 206);
  CHECK(cap::channel_of(cap::Region::Europe, 8750) == 0);
  CHECK(cap::channel_of(cap::Region::Europe, 10800) == 205);
  CHECK(cap::freq_of(cap::Region::Europe, 205) == 10800);
  CHECK(cap::channel_of(cap::Region::Europe, 9990) == 124);  // rounded to the nearest channel
  CHECK(cap::channel_of(cap::Region::Europe, 9995) == 125);
  CHECK(cap::channel_of(cap::Region::Europe, 5000) == 0);
  CHECK(cap::channel_of(cap::Region::Europe, 12000) == 205);
  // USA: 200 kHz steps from 87.5: the last whole step below 108 is 107.9.
  CHECK(cap::channel_count(cap::Region::Usa) == 103);
  CHECK(cap::freq_of(cap::Region::Usa, 102) == 10790);
  CHECK(cap::snap_freq(cap::Region::Usa, 10800) == 10790);
  CHECK(cap::snap_freq(cap::Region::Usa, 9910) == 9910);
  CHECK(cap::snap_freq(cap::Region::Usa, 9930) == 9930);
  // Australia has the USA's grid, Europe's de-emphasis.
  CHECK(cap::channel_count(cap::Region::Australia) == 103);
  // Japan (PAR-085): 76.0 .. 108.0 in 100 kHz steps.
  CHECK(cap::channel_count(cap::Region::Japan) == 321);
  CHECK(cap::freq_of(cap::Region::Japan, 0) == 7600);
  CHECK(cap::channel_of(cap::Region::Japan, 9000) == 140);
  CHECK(cap::snap_freq(cap::Region::Japan, 7550) == 7600);
  // The encoder: a detent is a channel, clamped at both ends.
  CHECK(cap::step_freq(cap::Region::Europe, 8750, -3) == 8750);
  CHECK(cap::step_freq(cap::Region::Europe, 8750, 3) == 8780);
  CHECK(cap::step_freq(cap::Region::Europe, 10790, 5) == 10800);
  CHECK(cap::step_freq(cap::Region::Usa, 9910, 2) == 9950);
  // Every region's channel count fits the CHANNEL field.
  for (unsigned r = 0; r < cap::kRegionCount; ++r) {
    CHECK(cap::channel_count(static_cast<cap::Region>(r)) <= 1024u);
    for (unsigned c = 0; c < cap::channel_count(static_cast<cap::Region>(r)); ++c) {
      CHECK(cap::channel_of(static_cast<cap::Region>(r), cap::freq_of(static_cast<cap::Region>(r), c)) == c);
    }
  }
}

TEST_CASE("si4703: the power-up sequence, in order, with its waits") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 1000;
  CHECK(radio.status().state == State::Off);
  radio.set_region(cap::Region::Japan);
  radio.tune(9000);
  radio.power(true);
  CHECK(radio.status().state == State::Starting);
  const std::uint64_t t = run(radio, now, 2000, [&] { return radio.status().state == State::Ready; });
  CHECK(radio.status().state == State::Ready);
  CHECK(t >= si::kOscillatorMs + si::kPowerUpMs);  // the two waits are honoured
  CHECK(t < 800);
  CHECK(pins.resets == 1);
  CHECK(radio.status().chip_id == 0x1242);
  CHECK(chip.reg.r[si::kTest1] == si::kTest1Start);
  CHECK((chip.reg.r[si::kPowerCfg] & (si::kEnable | si::kDMute | si::kDSMute | si::kMono)) == (si::kEnable | si::kDMute | si::kDSMute | si::kMono));
  CHECK((chip.reg.r[si::kSysConfig2] & si::kVolumeMask) == si::kVolume);
  CHECK(((chip.reg.r[si::kSysConfig2] >> 8) & 0xFFu) == si::kSeekThreshold);
  CHECK(((chip.reg.r[si::kSysConfig2] >> 6) & 3u) == 1u);  // Japan: BAND = 1
  CHECK((chip.reg.r[si::kSysConfig3] & 0xFFu) == (si::kSeekSkipCount | (si::kSeekSnr << 4)));
  // It then tunes to what was asked for before the power-up: Japan, 90.0 MHz is channel 140.
  run(radio, now, 500, [&] { return radio.status().state == State::Ready && radio.status().freq == 9000 && chip.reg.r[si::kReadChan] == 140; });
  CHECK(chip.reg.r[si::kReadChan] == 140);
  CHECK((chip.reg.r[si::kChannel] & si::kTune) == 0u);  // TUNE cleared after the tune
  CHECK(radio.status().freq == 9000);
}

TEST_CASE("si4703: a chip that does not answer is Absent, and held in reset") {
  Chip chip;
  chip.answers = false;
  FakePins pins;
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  radio.power(true);
  run(radio, now, 100, [&] { return radio.status().state != State::Starting; });
  CHECK(radio.status().state == State::Absent);
  CHECK(pins.holds >= 1);
  CHECK(radio.status().chip_id == 0);
  // Absent stays Absent: tune and seek do nothing, service does nothing.
  radio.tune(9000);
  radio.seek(true);
  const unsigned reads = chip.reads;
  run(radio, now, 100, [] { return false; });
  CHECK(chip.reads == reads);
  CHECK(radio.status().state == State::Absent);
  // Powering it on again tries again (a card inserted later, a loose wire fixed).
  chip.answers = true;
  radio.power(true);
  run(radio, now, 2000, [&] { return radio.status().state == State::Ready; });
  CHECK(radio.status().state == State::Ready);
}

TEST_CASE("si4703: a bus with nothing on it (all ones, all zeros) is Absent too") {
  for (std::uint16_t id : {static_cast<std::uint16_t>(0xFFFF), static_cast<std::uint16_t>(0x0000)}) {
    Chip chip;
    chip.reg.r[si::kDeviceId] = id;
    FakePins pins;
    si::Radio radio(chip, pins);
    std::uint64_t now = 0;
    radio.power(true);
    run(radio, now, 100, [&] { return radio.status().state != State::Starting; });
    CHECK(radio.status().state == State::Absent);
  }
}

TEST_CASE("si4703: a reset that leaves the bus stuck is tried kBringUpTries times, then Failed at the reset step") {
  Chip chip;
  FakePins pins;
  pins.reset_ok = false;
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  radio.power(true);
  run(radio, now, 3000, [&] { return radio.status().state != State::Starting; });
  CHECK(radio.status().state == State::Failed);
  CHECK(pins.resets == si::kBringUpTries);
  CHECK(std::strcmp(radio.status().fail, "RS/R") == 0);
  CHECK(pins.holds == si::kBringUpTries);  // RST# low between the tries (twice) and for good at the final Failed
}

TEST_CASE("si4703: the power-up reads the registers again after the oscillator wait and after the power-up wait") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  radio.power(true);
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  // probe (R), TEST1 (W), re-read (R), configuration (W), re-read (R), region and volume (W)
  CHECK(chip.ops.substr(0, 6) == "RWRWRW");
}

TEST_CASE("si4703: a write nobody acknowledges starts the power-up again from the reset, and a transient fault is survived") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  chip.fail_writes = si::kMaxBusFailures;  // the first try's TEST1 write fails three times in a row
  radio.power(true);
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  CHECK(radio.status().state == State::Ready);
  CHECK(pins.resets == 2);  // the second try
  CHECK(std::strcmp(radio.status().fail, "OW/N") == 0);  // remembered
  radio.power(false);
  radio.power(true);
  CHECK(radio.status().fail[0] == '\0');  // a new power-up starts clean
}

TEST_CASE("si4703: a power-up that fails on every try is Failed, saying the step and the cause") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  chip.fail_writes = 1000;
  radio.power(true);
  run(radio, now, 5000, [&] { return radio.status().state == State::Failed; });
  CHECK(radio.status().state == State::Failed);
  CHECK(pins.resets == si::kBringUpTries);
  CHECK(std::strcmp(radio.status().fail, "OW/N") == 0);
  CHECK(pins.holds == si::kBringUpTries);  // RST# low between the tries (twice) and for good at the final Failed: the chip is off, not left running
  // restart(): the chip is held in reset and the whole power-up runs again; the fault is gone now.
  chip.fail_writes = 0;
  radio.restart();
  CHECK(radio.status().state == State::Starting);
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  CHECK(radio.status().state == State::Ready);
}

TEST_CASE("si4703: a transfer that times out lets go of the bus at once, and one timeout does not end the radio") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  chip.fail_with = I2cResult::Timeout;
  chip.fail_reads = 1;  // the probe read times out once
  radio.power(true);
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  CHECK(radio.status().state == State::Ready);
  CHECK(chip.recovers == 1);
  CHECK(radio.status().fail[0] == '\0');
  chip.fail_with = I2cResult::BusStuck;
  chip.fail_reads = 2;
  chip.recovers = 0;
  run(radio, now, 1000, [] { return false; });
  CHECK(radio.status().state == State::Ready);
  CHECK(chip.recovers == 2);  // each failure let go of the bus; two in a row is not three
}

TEST_CASE("si4703: a tune that never completes names its step and cause") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  chip.never_completes = true;
  radio.power(true);
  run(radio, now, 6000, [&] { return radio.status().state == State::Failed; });
  CHECK(radio.status().state == State::Failed);
  CHECK(std::strcmp(radio.status().fail, "TS/S") == 0);
  CHECK(pins.resets == 1);
  CHECK(pins.holds == 0);  // a tune that fails once the chip is Ready leaves RST# released: it keeps playing (a radio take is not silenced), only the failure is reported
}

TEST_CASE("si4703: an Absent chip says why, after asking a few times") {
  {
    Chip chip;
    chip.answers = false;
    FakePins pins;
    si::Radio radio(chip, pins);
    std::uint64_t now = 0;
    radio.power(true);
    run(radio, now, 500, [&] { return radio.status().state != State::Starting; });
    CHECK(radio.status().state == State::Absent);
    CHECK(chip.reads == si::kMaxBusFailures);
    CHECK(std::strcmp(radio.status().fail, "PB/N") == 0);
  }
  {
    Chip chip;
    chip.reg.r[si::kDeviceId] = 0xFFFF;
    FakePins pins;
    si::Radio radio(chip, pins);
    std::uint64_t now = 0;
    radio.power(true);
    run(radio, now, 500, [&] { return radio.status().state != State::Starting; });
    CHECK(radio.status().state == State::Absent);
    CHECK(std::strcmp(radio.status().fail, "PB/I") == 0);
  }
}

TEST_CASE("si4703: describe() is bounded by its buffer and names the state, the step and the registers") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  radio.power(true);
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  char buf[1024];
  const std::size_t n = radio.describe(buf, sizeof buf);
  CHECK(n < sizeof buf);
  buf[n < sizeof buf ? n : sizeof buf - 1] = '\0';
  const std::string text(buf);
  CHECK(text.find("radio: Ready") == 0);
  CHECK(text.find("chip id 0x1242") != std::string::npos);
  CHECK(text.find("0A:") != std::string::npos);
  CHECK(text.find("00:1242") != std::string::npos);
  CHECK(text.find("last failure: none") != std::string::npos);
  for (std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{100}}) {
    char small[128];
    std::memset(small, 0x55, sizeof small);
    CHECK(radio.describe(small, cap) <= cap);
    CHECK(small[cap < sizeof small ? cap : sizeof small - 1] == 0x55);  // nothing written past the buffer
  }
}

namespace {
struct Ready {
  Chip chip;
  FakePins pins{&chip};
  si::Radio radio{chip, pins};
  std::uint64_t now = 0;
  Ready() {
    radio.power(true);
    run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
    run(radio, now, 300, [] { return false; });  // the initial tune (87.5 MHz, channel 0) settles
  }
};
}  // namespace

TEST_CASE("si4703: tune writes the channel with TUNE, waits for STC, clears TUNE and reports the chip's frequency") {
  Ready rig;
  CHECK(rig.radio.status().state == State::Ready);
  rig.radio.tune(9990);
  CHECK(rig.radio.status().freq == 9990);  // the target shows at once
  bool saw_tuning = false;
  run(rig.radio, rig.now, 1000, [&] {
    saw_tuning = saw_tuning || rig.radio.status().state == State::Tuning;
    return saw_tuning && rig.radio.status().state == State::Ready;
  });
  CHECK(saw_tuning);
  CHECK(rig.radio.status().state == State::Ready);
  CHECK(rig.chip.reg.r[si::kReadChan] == 124);
  CHECK((rig.chip.reg.r[si::kChannel] & si::kChannelMask) == 124);
  CHECK((rig.chip.reg.r[si::kChannel] & si::kTune) == 0u);
  CHECK(rig.radio.status().freq == 9990);
  // A frequency off the grid is snapped to it.
  rig.radio.tune(9996);
  CHECK(rig.radio.status().freq == 10000);
}

TEST_CASE("si4703: seek waits for STC, stops on a station and reads its channel; no station sets the band limit flag") {
  Ready rig;
  CHECK(rig.radio.status().state == State::Ready);
  rig.chip.station_channel = 77;
  rig.radio.seek(true);
  bool saw = false;
  run(rig.radio, rig.now, 3000, [&] {
    saw = saw || rig.radio.status().state == State::Seeking;
    return saw && rig.radio.status().state == State::Ready;
  });
  CHECK(saw);
  CHECK(rig.radio.status().freq == 8750 + 770);
  CHECK_FALSE(rig.radio.status().band_limit);
  CHECK((rig.chip.reg.r[si::kPowerCfg] & si::kSeek) == 0u);  // SEEK cleared
  CHECK((rig.chip.reg.r[si::kPowerCfg] & si::kSeekUp) != 0u);
  CHECK((rig.chip.reg.r[si::kPowerCfg] & si::kSkMode) == 0u);  // wrap

  rig.chip.station_channel = -1;
  rig.radio.seek(false);
  saw = false;
  run(rig.radio, rig.now, 3000, [&] {
    saw = saw || rig.radio.status().state == State::Seeking;
    return saw && rig.radio.status().state == State::Ready;
  });
  CHECK(rig.radio.status().band_limit);
  CHECK((rig.chip.reg.r[si::kPowerCfg] & si::kSeekUp) == 0u);
}

TEST_CASE("si4703: a tune or a seek that never completes ends Failed after its bound; power-on recovers") {
  Ready rig;
  rig.chip.never_completes = true;
  rig.radio.tune(9990);
  const std::uint64_t t = run(rig.radio, rig.now, 5000, [&] { return rig.radio.status().state == State::Failed; });
  CHECK(rig.radio.status().state == State::Failed);
  CHECK(t > si::kTuneTimeoutMs);
  CHECK(t < si::kTuneTimeoutMs + 200);
  CHECK(rig.pins.holds == 0);  // (the chip was never put in reset by the failure)
  rig.chip.never_completes = false;
  rig.radio.power(true);
  CHECK(rig.radio.status().state == State::Starting);
  run(rig.radio, rig.now, 3000, [&] { return rig.radio.status().state == State::Ready; });
  CHECK(rig.radio.status().state == State::Ready);
}

TEST_CASE("si4703: three failed transfers in a row are Failed (the bus is recovered once); an isolated failure is retried") {
  Ready rig;
  rig.chip.fail_reads = 2;  // two in a row, then it answers: the poll or the tune goes on
  rig.radio.tune(9990);
  run(rig.radio, rig.now, 1000, [&] { return rig.radio.status().state == State::Ready && rig.chip.reg.r[si::kReadChan] == 124; });
  CHECK(rig.radio.status().state == State::Ready);
  CHECK(rig.chip.recovers == 0);
  rig.chip.fail_reads = 1000;
  rig.radio.tune(9890);
  run(rig.radio, rig.now, 1000, [&] { return rig.radio.status().state == State::Failed; });
  CHECK(rig.radio.status().state == State::Failed);
  CHECK(rig.chip.recovers == 1);
  CHECK(rig.pins.holds == 0);  // three failed transfers of a running chip: Failed, RST# still released
}

TEST_CASE("si4703: requests made while the chip is busy are held and the last one wins") {
  Chip chip;
  FakePins pins(&chip);
  si::Radio radio(chip, pins);
  std::uint64_t now = 0;
  radio.power(true);
  radio.tune(9000);
  radio.tune(9990);  // while Starting: the last tune wins
  run(radio, now, 3000, [&] { return radio.status().state == State::Ready; });
  run(radio, now, 300, [] { return false; });
  CHECK(chip.reg.r[si::kReadChan] == 124);
  CHECK(radio.status().freq == 9990);
  // A seek then a tune: the tune cancels the seek.
  radio.seek(true);
  radio.tune(9100);
  run(radio, now, 500, [] { return false; });
  CHECK(chip.reg.r[si::kReadChan] == 35);
  CHECK(radio.status().freq == 9100);
}

TEST_CASE("si4703: a region change while ready rewrites the band and tunes to the nearest channel of the new grid") {
  Ready rig;
  rig.radio.tune(9910);
  run(rig.radio, rig.now, 500, [] { return false; });
  CHECK(rig.radio.status().freq == 9910);
  rig.radio.set_region(cap::Region::Japan);
  CHECK(rig.radio.status().region == cap::Region::Japan);
  run(rig.radio, rig.now, 800, [] { return false; });
  CHECK(((rig.chip.reg.r[si::kSysConfig2] >> 6) & 3u) == 1u);
  CHECK(rig.chip.reg.r[si::kReadChan] == (9910 - 7600) / 10);
  CHECK(rig.radio.status().freq == 9910);
  // USA's 200 kHz grid snaps 99.1 to 99.1 (87.5 + 58 * 0.2) and 99.0 to 99.1.
  rig.radio.set_region(cap::Region::Usa);
  run(rig.radio, rig.now, 800, [] { return false; });
  CHECK(rig.radio.status().freq == 9910);
  rig.radio.tune(9900);
  run(rig.radio, rig.now, 500, [] { return false; });
  CHECK(rig.radio.status().freq == 9910);
}

TEST_CASE("si4703: the status poll reads the signal strength and the stereo flag; power-off holds the chip in reset and clears them") {
  Ready rig;
  rig.chip.rssi = 55;
  rig.chip.stereo = true;
  const std::uint32_t gen = rig.radio.status().generation;
  run(rig.radio, rig.now, 2 * si::kStatusPollMs + 50, [] { return false; });
  CHECK(rig.radio.status().rssi == 55);
  CHECK(rig.radio.status().stereo);
  CHECK(rig.radio.status().generation != gen);
  const unsigned holds = rig.pins.holds;
  rig.radio.power(false);
  CHECK(rig.radio.status().state == State::Off);
  CHECK(rig.radio.status().rssi == 0);
  CHECK_FALSE(rig.radio.status().stereo);
  CHECK(rig.pins.holds == holds + 1);
  const unsigned reads = rig.chip.reads;
  run(rig.radio, rig.now, 500, [] { return false; });
  CHECK(rig.chip.reads == reads);  // nothing is polled while it is off
}

TEST_CASE("si4703: service does at most one bus transfer a call") {
  Ready rig;
  rig.radio.tune(9990);
  for (int i = 0; i < 2000; ++i) {
    const unsigned before = rig.chip.reads + rig.chip.writes;
    rig.radio.service(rig.now++);
    CHECK(rig.chip.reads + rig.chip.writes - before <= 1u);
  }
}
