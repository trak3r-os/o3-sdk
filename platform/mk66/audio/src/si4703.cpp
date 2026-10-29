// SPDX-License-Identifier: GPL-3.0-or-later
// The Si4703 driver (OS-011c): see si4703.h for the sources, the wire protocol and the sequences.
#include <o3sdk/platform/mk66/si4703.h>

#include <o3sdk/platform/mk66/audio/diag.h>

namespace o3sdk::platform::mk66::si4703 {

namespace cap = o3sdk::hal::capture;

// ---- Pure helpers -----------------------------------------------------------------------------------------------------------------------

void decode_read(const std::uint8_t (&bytes)[kReadBytes], Registers& out) noexcept {
  // Register 0x0A comes first, then 0x0B..0x0F, then 0x00..0x09.
  for (unsigned i = 0; i < kReadBytes / 2u; ++i) {
    const unsigned reg = (kStatusRssi + i) & (kRegisters - 1u);
    out.r[reg] = static_cast<std::uint16_t>((static_cast<unsigned>(bytes[2u * i]) << 8) | bytes[2u * i + 1u]);
  }
}

void encode_write(const Registers& regs, std::uint8_t (&bytes)[kWriteBytes]) noexcept {
  for (unsigned i = 0; i < kWriteBytes / 2u; ++i) {
    const std::uint16_t v = regs.r[kPowerCfg + i];
    bytes[2u * i] = static_cast<std::uint8_t>(v >> 8);
    bytes[2u * i + 1u] = static_cast<std::uint8_t>(v & 0xFFu);
  }
}

void apply_region(Registers& regs, cap::Region region) noexcept {
  const cap::RegionInfo& info = cap::region_info(region);
  // SYSCONFIG2: SPACE is bits 5:4 (0: 200 kHz, 1: 100 kHz, 2: 50 kHz), BAND bits 7:6 (0: 87.5-108, 1: 76-108); cleared, then set.
  std::uint16_t s2 = regs.r[kSysConfig2] & static_cast<std::uint16_t>(~0x00F0u);
  s2 = static_cast<std::uint16_t>(s2 | ((info.spacing == 10u ? 1u : 0u) << 4) | ((info.japan_wide ? 1u : 0u) << 6));
  regs.r[kSysConfig2] = s2;
  regs.r[kSysConfig1] = info.deemphasis_50us ? static_cast<std::uint16_t>(regs.r[kSysConfig1] | kDeEmphasis)
                                             : static_cast<std::uint16_t>(regs.r[kSysConfig1] & ~kDeEmphasis);
}

std::uint16_t tune_word(std::uint16_t channel_reg, unsigned channel) noexcept {
  return static_cast<std::uint16_t>((channel_reg & ~kChannelMask) | (channel & kChannelMask) | kTune);
}

unsigned rssi_of(const Registers& regs) noexcept { return regs.r[kStatusRssi] & 0xFFu; }
bool stereo_of(const Registers& regs) noexcept { return (regs.r[kStatusRssi] & kStereo) != 0u; }
bool stc_of(const Registers& regs) noexcept { return (regs.r[kStatusRssi] & kSeekTuneComplete) != 0u; }
bool band_limit_of(const Registers& regs) noexcept { return (regs.r[kStatusRssi] & kSeekFailBandLimit) != 0u; }
unsigned read_channel_of(const Registers& regs) noexcept { return regs.r[kReadChan] & kChannelMask; }

// ---- The driver -----------------------------------------------------------------------------------------------------------------------

namespace {

// The two-letter code of a step, shown on the RADIO page and by `diag radio` with the cause: "OW/N" = the oscillator write was not acknowledged. Same order as Radio::Step.
constexpr char kStepCodes[][3] = {"--", "RS", "PB", "OW", "OX", "OR", "CW", "PW", "PR", "RW", "TW", "TS", "TC", "TL", "KW", "KS", "KC", "KL", "PL"};
static_assert(sizeof(kStepCodes) / sizeof(kStepCodes[0]) == static_cast<unsigned>(Radio::Step::Poll) + 1u, "one code per step");
// The cause of a failed transfer, one letter: N not acknowledged, T timeout, A arbitration lost, B bus stuck. The other causes (S: a tune or seek never completed, R: the reset left
// the bus stuck, I: the chip answered with an id no radio has) are named where they happen.
char cause_of(I2cResult r) noexcept {
  switch (r) {
    case I2cResult::Nak: return 'N';
    case I2cResult::Timeout: return 'T';
    case I2cResult::ArbitrationLost: return 'A';
    case I2cResult::BusStuck: return 'B';
    case I2cResult::Ok: break;
  }
  return '?';
}
const char* result_name(I2cResult r) noexcept {
  switch (r) {
    case I2cResult::Ok: return "Ok";
    case I2cResult::Timeout: return "Timeout";
    case I2cResult::Nak: return "Nak";
    case I2cResult::ArbitrationLost: return "ArbLost";
    case I2cResult::BusStuck: return "BusStuck";
  }
  return "?";
}
const char* state_name(hal::capture::RadioStatus::State s) noexcept {
  using St = hal::capture::RadioStatus::State;
  switch (s) {
    case St::Off: return "Off";
    case St::Starting: return "Starting";
    case St::Ready: return "Ready";
    case St::Tuning: return "Tuning";
    case St::Seeking: return "Seeking";
    case St::Absent: return "Absent";
    case St::Failed: return "Failed";
  }
  return "?";
}

}  // namespace

void Radio::set_state(Status::State s) noexcept {
  if (status_.state != s) {
    status_.state = s;
    touch();
  }
}

bool Radio::read(Registers& into) noexcept {
  std::uint8_t bytes[kReadBytes];
  const I2cResult r = bus_.read(kAddress, bytes, kReadBytes);
  last_result_ = r;
  if (r != I2cResult::Ok) {
    fail_bus(r);
    return false;
  }
  failures_ = 0;
  decode_read(bytes, into);
  seen_ = into;
  return true;
}

bool Radio::write(const Registers& r) noexcept {
  std::uint8_t bytes[kWriteBytes];
  encode_write(r, bytes);
  const I2cResult result = bus_.transfer(kAddress, bytes, kWriteBytes, nullptr, 0);
  last_result_ = result;
  if (result != I2cResult::Ok) {
    fail_bus(result);
    return false;
  }
  failures_ = 0;
  return true;
}

// A failed transfer: the step is tried again after a short wait, up to kMaxBusFailures in a row; then the chip is Failed (a stuck bus is let go once). A failure that is not a
// refusal (a timeout, a stuck bus, a lost arbitration) leaves the slave possibly holding SDA, and then every later transfer would find the bus busy: the bus is recovered at once
// (nine clocks and a STOP) so that the retry meets a free bus.
void Radio::fail_bus(I2cResult result) noexcept {
  wait(kStcPollMs);
  ++errors_;
  if (result != I2cResult::Nak) {
    (void)bus_.recover();
  }
  if (++failures_ >= kMaxBusFailures) {
    failures_ = 0;
    if (result == I2cResult::Nak) {
      (void)bus_.recover();
    }
    fail(cause_of(result));
  }
}

void Radio::remember(char cause) noexcept {
  fail_step_ = step_;
  status_.fail[0] = kStepCodes[static_cast<unsigned>(step_)][0];
  status_.fail[1] = kStepCodes[static_cast<unsigned>(step_)][1];
  status_.fail[2] = '/';
  status_.fail[3] = cause;
  status_.fail[4] = '\0';
  touch();
}

// Failed, remembering where and why. A power-up that fails is started again from the reset (RST# held low for kRetryWaitMs) until kBringUpTries tries have been made; a tune or
// a seek that fails, or the last try, is Failed.
void Radio::fail(char cause) noexcept {
  remember(cause);
  if (status_.state == Status::State::Starting && tries_ < kBringUpTries) {
    ++tries_;
    pins_.hold_reset();
    step_ = Step::Reset;
    wait(kRetryWaitMs);
    return;
  }
  if (status_.state == Status::State::Starting) {
    pins_.hold_reset();  // the bring-up gave up: the chip is off and silent, as after Absent (power(true) runs the reset again)
  }  // a tune, seek or poll that fails once it is Ready leaves RST# released: the chip keeps playing what it was tuned to, the failure is only reported
  step_ = Step::Idle;
  set_state(Status::State::Failed);
}

void Radio::absent(char cause) noexcept {
  remember(cause);
  step_ = Step::Idle;
  pins_.hold_reset();
  set_state(Status::State::Absent);
}

void Radio::power(bool on) noexcept {
  if (on) {
    power_wanted_ = true;
    if (status_.state == Status::State::Off || status_.state == Status::State::Absent || status_.state == Status::State::Failed) {
      failures_ = 0;
      tries_ = 1;
      errors_ = 0;
      due_ = 0;  // no wait left from before: the reset is the next pass
      status_.fail[0] = '\0';
      want_tune_ = false;
      want_seek_ = false;
      want_region_ = false;
      status_.freq = static_cast<std::uint16_t>(cap::snap_freq(status_.region, status_.freq));
      step_ = Step::Reset;
      touch();
      set_state(Status::State::Starting);
    }
    return;
  }
  power_wanted_ = false;
  if (status_.state != Status::State::Off) {
    pins_.hold_reset();  // the chip off and silent: a reset is the power-down of the data sheet's "RST# low"
    step_ = Step::Idle;
    want_tune_ = false;
    want_seek_ = false;
    status_.stereo = false;
    status_.rssi = 0;
    set_state(Status::State::Off);
    touch();
  }
}

void Radio::restart() noexcept {
  power(false);
  power(true);
}

void Radio::set_region(Region region) noexcept {
  if (region == status_.region || static_cast<unsigned>(region) >= cap::kRegionCount) {
    return;
  }
  status_.region = region;
  status_.freq = static_cast<std::uint16_t>(cap::snap_freq(region, status_.freq));
  want_freq_ = status_.freq;
  touch();
  if (status_.state != Status::State::Off && status_.state != Status::State::Absent && status_.state != Status::State::Starting) {
    want_region_ = true;  // written and tuned when the chip is Ready
    want_tune_ = true;
  }
}

void Radio::tune(std::uint16_t freq) noexcept {
  const std::uint16_t f = static_cast<std::uint16_t>(cap::snap_freq(status_.region, freq));
  want_freq_ = f;
  status_.freq = f;  // the target, shown at once; the chip's own answer replaces it when the tune is done
  touch();
  want_seek_ = false;
  if (status_.state != Status::State::Off && status_.state != Status::State::Absent) {
    want_tune_ = true;
  }
}

void Radio::seek(bool up) noexcept {
  if (status_.state == Status::State::Off || status_.state == Status::State::Absent) {
    return;
  }
  want_seek_ = true;
  want_seek_up_ = up;
  want_tune_ = false;
}

void Radio::decode_status(const Registers& r) noexcept {
  const std::uint8_t rssi = static_cast<std::uint8_t>(rssi_of(r));
  const bool stereo = stereo_of(r);
  const std::uint16_t freq = static_cast<std::uint16_t>(cap::freq_of(status_.region, read_channel_of(r)));
  if (rssi != status_.rssi || stereo != status_.stereo || freq != status_.freq) {
    status_.rssi = rssi;
    status_.stereo = stereo;
    status_.freq = freq;
    touch();
  }
}

void Radio::next_request() noexcept {
  if (want_region_) {
    want_region_ = false;
    step_ = Step::RegionWrite;
  } else if (want_seek_) {
    step_ = Step::SeekWrite;
  } else if (want_tune_) {
    step_ = Step::TuneWrite;
  } else if (now_ - polled_ >= kStatusPollMs) {
    step_ = Step::Poll;
  }
}

void Radio::service(std::uint64_t now_ms) noexcept {
  now_ = now_ms;
  if (status_.state == Status::State::Off || status_.state == Status::State::Absent || status_.state == Status::State::Failed) {
    return;
  }
  if (step_ != Step::Idle && !due()) {
    return;  // a wait (the oscillator, the power-up, a poll interval, the pause after a failed transfer or between two tries)
  }
  if (step_ == Step::Idle) {
    if (status_.state != Status::State::Ready || !due()) {
      return;
    }
    next_request();
    if (step_ == Step::Idle) {
      return;
    }
  }
  switch (step_) {
    case Step::Idle:
      break;
    case Step::Reset:
      if (!pins_.reset()) {
        fail('R');  // the bus could not be restored after the reset
        break;
      }
      step_ = Step::Probe;
      break;
    case Step::Probe: {
      std::uint8_t bytes[kReadBytes];
      const I2cResult r = bus_.read(kAddress, bytes, kReadBytes);  // the first read after the reset must answer: no answer is no radio
      last_result_ = r;
      if (r != I2cResult::Ok) {
        ++errors_;
        if (++failures_ >= kMaxBusFailures) {  // asked a few times (a bus that timed out is let go and asked again): the chip is not there
          failures_ = 0;
          absent(cause_of(r));
        } else {
          if (r != I2cResult::Nak) {
            (void)bus_.recover();
          }
          wait(kStcPollMs);
        }
        break;
      }
      failures_ = 0;
      decode_read(bytes, regs_);
      seen_ = regs_;
      status_.chip_id = regs_.r[kDeviceId];
      touch();
      if (regs_.r[kDeviceId] == 0u || regs_.r[kDeviceId] == 0xFFFFu) {  // a bus with nothing on it can read all ones or all zeros
        absent('I');
        break;
      }
      step_ = Step::OscStart;
      break;
    }
    case Step::OscStart:
      regs_.r[kTest1] = kTest1Start;
      if (write(regs_)) {
        wait(kOscillatorMs);
        step_ = Step::OscWait;
      }
      break;
    case Step::OscWait:
      step_ = Step::OscRead;
      break;
    case Step::OscRead:  // the registers are read again before the configuration is written
      if (read(regs_)) {
        step_ = Step::ConfigWrite;
      }
      break;
    case Step::ConfigWrite:
      regs_.r[kPowerCfg] = kPowerCfgRun;
      regs_.r[kSysConfig2] = static_cast<std::uint16_t>((regs_.r[kSysConfig2] & 0x00F0u) | (kSeekThreshold << 8) | 1u);  // volume at its lowest until the region is set
      regs_.r[kSysConfig3] = static_cast<std::uint16_t>(regs_.r[kSysConfig3] | kSeekSkipCount | (kSeekSnr << 4));
      if (write(regs_)) {
        wait(kPowerUpMs);
        step_ = Step::PowerWait;
      }
      break;
    case Step::PowerWait:
      step_ = Step::PowerRead;
      break;
    case Step::PowerRead:  // and again after the power-up wait, before the region
      if (read(regs_)) {
        step_ = Step::RegionWrite;
      }
      break;
    case Step::RegionWrite:
      apply_region(regs_, status_.region);
      regs_.r[kSysConfig2] = static_cast<std::uint16_t>((regs_.r[kSysConfig2] & ~kVolumeMask) | kVolume);
      if (write(regs_)) {
        if (!want_seek_) {
          want_tune_ = true;  // the chip comes up on the frequency asked for (a seek asked for meanwhile wins)
          want_freq_ = status_.freq;
        }
        polled_ = now_;
        step_ = Step::Idle;
        wait(0);
        set_state(Status::State::Ready);
      }
      break;
    case Step::TuneWrite:
      regs_.r[kChannel] = tune_word(regs_.r[kChannel], cap::channel_of(status_.region, want_freq_));
      if (write(regs_)) {
        want_tune_ = false;
        started_ = now_;
        set_state(Status::State::Tuning);
        wait(kStcPollMs);
        step_ = Step::TuneWaitStc;
      }
      break;
    case Step::TuneWaitStc:
      if (read(regs_)) {
        if (stc_of(regs_)) {
          step_ = Step::TuneClear;
        } else if (now_ - started_ > kTuneTimeoutMs) {
          fail('S');  // it never completed
        } else {
          wait(kStcPollMs);
        }
      }
      break;
    case Step::TuneClear:
      regs_.r[kChannel] = static_cast<std::uint16_t>(regs_.r[kChannel] & ~kTune);
      if (write(regs_)) {
        wait(kStcPollMs);
        step_ = Step::TuneWaitClear;
      }
      break;
    case Step::TuneWaitClear:
      if (read(regs_)) {
        if (!stc_of(regs_)) {
          decode_status(regs_);
          polled_ = now_;
          step_ = Step::Idle;
          wait(0);
          set_state(Status::State::Ready);
        } else if (now_ - started_ > kTuneTimeoutMs) {
          fail('S');  // it never completed
        } else {
          wait(kStcPollMs);
        }
      }
      break;
    case Step::SeekWrite: {
      std::uint16_t p = static_cast<std::uint16_t>(regs_.r[kPowerCfg] & ~kSkMode);  // wrap at the band limit (SKMODE clear)
      p = want_seek_up_ ? static_cast<std::uint16_t>(p | kSeekUp) : static_cast<std::uint16_t>(p & ~kSeekUp);
      regs_.r[kPowerCfg] = static_cast<std::uint16_t>(p | kSeek);
      if (write(regs_)) {
        want_seek_ = false;
        started_ = now_;
        status_.band_limit = false;
        set_state(Status::State::Seeking);
        wait(kStcPollMs);
        step_ = Step::SeekWaitStc;
      }
      break;
    }
    case Step::SeekWaitStc:
      if (read(regs_)) {
        if (stc_of(regs_)) {
          if (band_limit_of(regs_) != status_.band_limit) {
            status_.band_limit = band_limit_of(regs_);
            touch();
          }
          step_ = Step::SeekClear;
        } else if (now_ - started_ > kSeekTimeoutMs) {
          fail('S');  // it never completed
        } else {
          wait(kStcPollMs);
        }
      }
      break;
    case Step::SeekClear:
      regs_.r[kPowerCfg] = static_cast<std::uint16_t>(regs_.r[kPowerCfg] & ~kSeek);
      if (write(regs_)) {
        wait(kStcPollMs);
        step_ = Step::SeekWaitClear;
      }
      break;
    case Step::SeekWaitClear:
      if (read(regs_)) {
        if (!stc_of(regs_)) {
          decode_status(regs_);
          polled_ = now_;
          step_ = Step::Idle;
          wait(0);
          set_state(Status::State::Ready);
        } else if (now_ - started_ > kSeekTimeoutMs) {
          fail('S');  // it never completed
        } else {
          wait(kStcPollMs);
        }
      }
      break;
    case Step::Poll:
      if (read(regs_)) {
        decode_status(regs_);
        polled_ = now_;
        step_ = Step::Idle;
        wait(0);
      }
      break;
  }
}

// ---- `diag radio` ---------------------------------------------------------------------------------------------------------------------

namespace {
void hex16(audio::diag::Text& t, std::uint16_t v) noexcept {
  for (int shift = 12; shift >= 0; shift -= 4) {
    t.chr("0123456789ABCDEF"[(v >> shift) & 0xFu]);
  }
}
}  // namespace

std::size_t Radio::describe(char* out, std::size_t cap) const noexcept {
  audio::diag::Text t(out, cap);
  t.str("radio: ").str(state_name(status_.state)).str(", step ").chr(kStepCodes[static_cast<unsigned>(step_)][0]).chr(kStepCodes[static_cast<unsigned>(step_)][1]);
  t.str(", try ").dec(tries_).chr('/').dec(kBringUpTries).str(", chip id 0x");
  hex16(t, status_.chip_id);
  t.str(", ").dec(status_.freq / 100u).chr('.').dec((status_.freq / 10u) % 10u).dec(status_.freq % 10u).str(" MHz region ").dec(static_cast<unsigned>(status_.region)).nl();
  t.str("last failure: ").str(status_.fail[0] == '\0' ? "none" : status_.fail).str(" (step/cause)").nl();
  t.str("bus: last transfer ").str(result_name(last_result_)).str(", ").dec(failures_).str(" in a row, ").dec(errors_).str(" failed since the power-up").nl();
  t.str("registers as last read (0A-0F, then 00-09):").nl();
  for (unsigned i = 0; i < kRegisters; ++i) {
    const unsigned r = (kStatusRssi + i) & (kRegisters - 1u);
    t.chr('0').chr("0123456789ABCDEF"[r]).chr(':');
    hex16(t, seen_.r[r]);
    t.chr((i & 7u) == 7u ? '\n' : ' ');
  }
  t.str("steps: RS reset, PB probe, OW/OX/OR oscillator write/wait/read, CW config, PW/PR power-up wait/read, RW region, TW/TS/TC/TL tune, KW/KS/KC/KL seek, PL poll").nl();
  t.str("causes: N not acknowledged, T timeout, B bus stuck, A arbitration lost, S never completed, R reset left the bus stuck, I bad chip id").nl();
  return t.size();
}

}  // namespace o3sdk::platform::mk66::si4703
