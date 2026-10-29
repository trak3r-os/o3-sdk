// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Si4703 FM receiver (OS-011c): the register facts, the two-wire protocol, the power-up / tune / seek sequences as a state machine the main loop steps, and the status
// decoded from the registers. Pure: it talks to an I2cBus and a Si4703Pins (the reset line and the SDIO level during the reset) and is tested against a model of the chip
// (tests/mk66_si4703_test.cpp); src/capture.cpp is the board's part.
//
// Sources.
//   * Behaviour: Silicon
//     Labs AN230 "Si4700/01/02/03 Programming Guide". The register map and bit names are the data sheet's (Si4702/03-C19, "Register description"); the constants below are checked against it by the test.
//   * The board: the radio shares the control bus I2C0 with the SGTL5000 and the control-button scanner (board.h kCodecScl, kCodecSda); its reset is PTE27 (board.h
//     kRadioReset) and its SDIO is that bus's SDA (SDIO pin 48 = PTD9 = SDA).
//     The receiver's audio goes through the analogue switch PTE26 to the codec's line input (board.h kInputSourceSelect). The seven-bit address is 0x10.
//     NOTHING OF THE BOARD'S WIRING HAS BEEN SEEN ON THIS TREE'S HARDWARE: the driver answers "Absent" when the chip does not acknowledge, and the Record screen then says
//     there is no radio (README, first hardware session).
//
// The wire protocol (data sheet "2-wire control interface"): there is no register address. A write sends the registers 0x02..0x07
// as twelve bytes, high byte first (the chip's internal pointer starts at 0x02: a write is always the whole group); a read returns the registers 0x0A..0x0F and then 0x00..0x09,
// thirty-two bytes (the pointer starts at 0x0A and wraps). I2cBus::read gives the read without an address phase.
//
// Reset (data sheet "Bus Mode Selection", AN230 "Powerup"): the chip selects its 2-wire mode at the rising edge of RST# if SDIO is low (and SEN
// is high, which the board's pull-up does), so SDA is held low while RST# is lowered for a millisecond and raised again, and released a millisecond after. Si4703Pins::reset()
// does that and the board's part restores the bus.
//
// Power-up (AN230 "Powerup"): after the reset the first read must answer; then TEST1 = 0x8100 (XOSCEN: the crystal oscillator) and a 500 ms wait for the
// oscillator; then POWERCFG = ENABLE | DMUTE | DSMUTE | RDSM | MONO (forced mono), SEEKTH 0x19, SKCNT 8, SKSNR 4 (the seek's strength, impulse and SNR thresholds) and a
// 110 ms wait (the data sheet's maximum power-up time); then the region (SYSCONFIG1 DE, SYSCONFIG2 SPACE and BAND) and VOLUME 10. RDS and its interrupt (GPIO2) are not used (the station name is a follow-up), so SYSCONFIG1's RDS and GPIO2 stay off.
//
// OS-RAD1 (the first run on the unit said Failed): the registers are read again after the oscillator wait and after the power-up wait; a
// transfer that times out or finds the bus stuck lets go of the bus at once instead of failing the next two
// transfers too; the probe is asked three times; a power-up that fails is started again from the reset (kBringUpTries). Every Failed and Absent keeps its step and cause
// (RadioStatus::fail, "OW/N", shown on the RADIO page and by `diag radio`).
//
// Tune: CHANNEL = the channel number with TUNE set; wait for STC (seek/tune complete); clear TUNE; wait for STC to clear. Seek
// Seek: SKMODE clear (wrap at the band limit), SEEKUP as asked, SEEK set; wait for STC; clear SEEK; wait for STC to clear; the tuned channel is READCHAN. The waits are bounded:
// this driver gives up after kTuneTimeoutMs / kSeekTimeoutMs (a station search of the whole band takes a few seconds) and says Failed.
//
// Contexts: the main loop only (the bus's contract, i2c_bus.h). service() does at most one bus transfer, so a pass of the main loop is never held for more than that.

#include <o3sdk/hal/capture.h>
#include <o3sdk/platform/mk66/i2c_bus.h>

#include <cstdint>

namespace o3sdk::platform::mk66::si4703 {

inline constexpr std::uint8_t kAddress = 0x10;  // the radio's 7-bit address

// ---- Registers (the chip's register map) ---------------------------------------------------------------------------------------------------------
inline constexpr unsigned kDeviceId = 0x00;
inline constexpr unsigned kPowerCfg = 0x02;
inline constexpr unsigned kChannel = 0x03;
inline constexpr unsigned kSysConfig1 = 0x04;
inline constexpr unsigned kSysConfig2 = 0x05;
inline constexpr unsigned kSysConfig3 = 0x06;
inline constexpr unsigned kTest1 = 0x07;
inline constexpr unsigned kStatusRssi = 0x0A;
inline constexpr unsigned kReadChan = 0x0B;
inline constexpr unsigned kRegisters = 16;
inline constexpr unsigned kReadBytes = 32;   // 0x0A..0x0F then 0x00..0x09
inline constexpr unsigned kWriteBytes = 12;  // 0x02..0x07

// POWERCFG
inline constexpr std::uint16_t kEnable = 0x0001;
inline constexpr std::uint16_t kDisable = 0x0040;
inline constexpr std::uint16_t kSeek = 0x0100;
inline constexpr std::uint16_t kSeekUp = 0x0200;
inline constexpr std::uint16_t kSkMode = 0x0400;
inline constexpr std::uint16_t kRdsMode = 0x0800;
inline constexpr std::uint16_t kMono = 0x2000;
inline constexpr std::uint16_t kDMute = 0x4000;
inline constexpr std::uint16_t kDSMute = 0x8000;
// CHANNEL
inline constexpr std::uint16_t kChannelMask = 0x03FF;
inline constexpr std::uint16_t kTune = 0x8000;
// SYSCONFIG1
inline constexpr std::uint16_t kDeEmphasis = 0x0800;  // DE: 1 = 50 us
// SYSCONFIG2
inline constexpr std::uint16_t kVolumeMask = 0x000F;
// TEST1
inline constexpr std::uint16_t kXoscEnable = 0x8000;
// STATUSRSSI
inline constexpr std::uint16_t kStereo = 0x0100;  // ST
inline constexpr std::uint16_t kSeekFailBandLimit = 0x2000;  // SF/BL
inline constexpr std::uint16_t kSeekTuneComplete = 0x4000;   // STC

inline constexpr std::uint16_t kTest1Start = 0x8100;  // TEST1 XOSCEN (AN230 "Powerup")
inline constexpr std::uint16_t kPowerCfgRun = kEnable | kDMute | kDSMute | kRdsMode | kMono;
inline constexpr unsigned kSeekThreshold = 0x19;  // SEEKTH: the least RSSI a station needs for a seek to stop
inline constexpr unsigned kSeekSkipCount = 8;     // SKCNT (impulse detection threshold)
inline constexpr unsigned kSeekSnr = 4;           // SKSNR
inline constexpr unsigned kVolume = 10;           // VOLUME (0..15)
inline constexpr std::uint16_t kDeviceMfg = 0x0242;  // DEVICEID[11:0]: Silicon Labs; logged, not required (only the read has to answer)

// Waits and bounds (milliseconds).
inline constexpr std::uint32_t kResetLowMs = 1;       // RST# low time
inline constexpr std::uint32_t kOscillatorMs = 500;   // AN230 page 9
inline constexpr std::uint32_t kPowerUpMs = 110;      // the data sheet's maximum power-up time
inline constexpr std::uint32_t kStcPollMs = 10;       // between reads while waiting for STC
inline constexpr std::uint32_t kStatusPollMs = 250;   // between status reads while idle
inline constexpr std::uint32_t kTuneTimeoutMs = 2000;
inline constexpr std::uint32_t kSeekTimeoutMs = 12000;
inline constexpr unsigned kMaxBusFailures = 3;        // in a row, then Failed (a probe: then Absent)
inline constexpr unsigned kBringUpTries = 3;          // a power-up that fails (a transfer, the reset) is started again from the reset this many times in all, then Failed
inline constexpr std::uint32_t kRetryWaitMs = 200;    // RST# low for this long between two tries

// ---- Register image and wire format (pure) ------------------------------------------------------------------------------------------------
struct Registers {
  std::uint16_t r[kRegisters] = {};
};
// The 32 bytes a read returns, into the register image in register order (0x0A..0x0F first, then 0x00..0x09).
void decode_read(const std::uint8_t (&bytes)[kReadBytes], Registers& out) noexcept;
// Registers 0x02..0x07 of the image as the 12 bytes of a write.
void encode_write(const Registers& regs, std::uint8_t (&bytes)[kWriteBytes]) noexcept;
// The region's bits in SYSCONFIG1 (DE) and SYSCONFIG2 (SPACE, BAND), the rest of both registers kept.
void apply_region(Registers& regs, hal::capture::Region region) noexcept;
// The CHANNEL register for a tune: the channel number with TUNE set (the other bits of the register are reserved and kept as read).
std::uint16_t tune_word(std::uint16_t channel_reg, unsigned channel) noexcept;
// What the status registers say.
unsigned rssi_of(const Registers& regs) noexcept;
bool stereo_of(const Registers& regs) noexcept;
bool stc_of(const Registers& regs) noexcept;
bool band_limit_of(const Registers& regs) noexcept;
unsigned read_channel_of(const Registers& regs) noexcept;  // READCHAN[9:0]

// ---- The board's two lines ---------------------------------------------------------------------------------------------------------------
class Pins {
 public:
  // The 2-wire select reset: SDIO (the bus's SDA) low, RST# low for kResetLowMs, RST# high, kResetLowMs, SDIO released and the bus restored. Blocks about 2 ms (the main loop).
  // False: the bus could not be restored (stuck).
  virtual bool reset() noexcept = 0;
  // RST# low: the chip off and silent (power-down by reset).
  virtual void hold_reset() noexcept = 0;

 protected:
  Pins() = default;
  ~Pins() = default;
  Pins(const Pins&) = delete;
  Pins& operator=(const Pins&) = delete;
};

// ---- The driver ---------------------------------------------------------------------------------------------------------------------------
class Radio {
 public:
  using Status = hal::capture::RadioStatus;
  using Region = hal::capture::Region;

  enum class Step : std::uint8_t {
    Idle,           // Off, Absent, Failed, or Ready with nothing to do
    Reset,          // the 2-wire select reset (Pins::reset, about 2 ms)
    Probe,          // read the registers after the reset: does the chip answer
    OscStart,       // write TEST1
    OscWait,        // 500 ms
    OscRead,        // read the registers again
    ConfigWrite,    // write POWERCFG, SEEKTH, SKCNT, SKSNR
    PowerWait,      // 110 ms
    PowerRead,      // read the registers again
    RegionWrite,    // write the region and VOLUME
    TuneWrite,      // write CHANNEL with TUNE
    TuneWaitStc,    // read until STC
    TuneClear,      // write CHANNEL without TUNE
    TuneWaitClear,  // read until STC is clear
    SeekWrite,      // write POWERCFG with SEEK
    SeekWaitStc,    // read until STC
    SeekClear,      // write POWERCFG without SEEK
    SeekWaitClear,  // read until STC is clear, then READCHAN is the station
    Poll            // read the status
  };

  Radio(I2cBus& bus, Pins& pins) noexcept : bus_(bus), pins_(pins) {}

  // Control (main loop). power(true) from Off, Absent or Failed starts the sequence; from any other state it does nothing. The region and frequency set before or during the
  // power-up are what the chip comes up on. A tune or seek while it is starting, tuning or seeking is held and runs when it is Ready (the last request wins).
  void power(bool on) noexcept;
  void set_region(Region region) noexcept;
  void tune(std::uint16_t freq) noexcept;
  void seek(bool up) noexcept;
  // `diag radio retry`: the chip back in reset and the whole power-up again, from any state.
  void restart() noexcept;
  // One step: the wait that is due, or one bus transfer. `now_ms` is the 64-bit uptime.
  void service(std::uint64_t now_ms) noexcept;
  const Status& status() const noexcept { return status_; }
  // `diag radio` (OS-RAD1): the state, the step, the try, the last failure, the last bus result and counters, and the registers as last read, as text (about 600 bytes). The board's
  // part (the pin levels) is added by the caller. Returns the length written; never more than `cap` (a truncated text ends at `cap`).
  std::size_t describe(char* out, std::size_t cap) const noexcept;

 private:
  bool read(Registers& into) noexcept;     // one bus read; counts failures
  bool write(const Registers& r) noexcept; // one bus write of 0x02..0x07; counts failures
  void fail_bus(I2cResult result) noexcept;
  void remember(char cause) noexcept;      // the step in progress and the cause, as the status' fail code
  void fail(char cause) noexcept;          // Failed (or, in a power-up with tries left, a new try), remembering the step and the cause
  void absent(char cause) noexcept;        // Absent: the chip is put back in reset
  void set_state(Status::State s) noexcept;
  void touch() noexcept { ++status_.generation; }
  void wait(std::uint32_t ms) noexcept { due_ = now_ + ms; }
  bool due() const noexcept { return now_ >= due_; }
  void next_request() noexcept;  // Ready: start a held tune / seek, or poll when it is time
  void decode_status(const Registers& r) noexcept;

  I2cBus& bus_;
  Pins& pins_;
  Status status_{};
  Registers regs_{};            // the last read: the shadow every write is made from (this keeps one transfer a step)
  Registers seen_{};            // the last read exactly as the chip answered (regs_ is changed to what is about to be written), for `diag radio`
  Step step_ = Step::Idle;
  std::uint64_t now_ = 0;
  std::uint64_t due_ = 0;       // the wait in progress ends at this uptime
  std::uint64_t started_ = 0;   // the tune or seek began at this uptime
  std::uint64_t polled_ = 0;    // the last status read
  unsigned failures_ = 0;
  unsigned tries_ = 1;                // the power-up try in progress (1..kBringUpTries)
  std::uint16_t errors_ = 0;          // failed transfers since the power-up was asked for (wraps)
  I2cResult last_result_ = I2cResult::Ok;
  Step fail_step_ = Step::Idle;       // where the last failure or refusal was
  bool power_wanted_ = false;
  bool want_tune_ = false;      // a held tune (want_freq_) or seek (want_seek_up_)
  bool want_seek_ = false;
  bool want_seek_up_ = false;
  bool want_region_ = false;    // the region changed since the chip was configured
  std::uint16_t want_freq_ = 8750;
};

}  // namespace o3sdk::platform::mk66::si4703
