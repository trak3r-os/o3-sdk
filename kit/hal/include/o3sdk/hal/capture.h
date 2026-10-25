// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The capture HAL (OS-011c): what the sample recorder needs from the board that is not the audio stream itself. The audio of every source reaches the render as
// the RX block (and the USB return) and is captured by the engine (audio/taps.h RecTap); this contract is the CONTROL side, in the main loop only:
//
//   * which analogue source the codec's ADC listens to and how much gain it has (the SGTL5000: line in or microphone; the line-in jack and the FM receiver share the
//     codec's line input behind an analogue switch on the board), and
//   * the FM receiver (Si4703): power, region, tune, seek and its status.
//
// Sources. `Source` lists what the Record screen offers; whether a source is analogue, the channel it records (mix, left, right) and whether it is the USB return are
// facts of the app's table (app/recorder.h), the hardware's part is `select_source`. The sources are Line in, Radio, Mic LG and Mic HG; the
// left, right and USB entries are new Community OS behaviour (product-ux section 4: "line L/R, mic, radio, USB").
//
// The radio's regions are four (Europe, USA, Australia, Japan; PAR-085) with
// their band and channel spacing. Frequencies are integers in units of 10 kHz (8750 is 87.50 MHz): no float in the control path.
//
// Contexts. Every call is for the main loop (it may do I2C, which waits). The hardware's services are polled: `service()` is called once a pass of the main loop and does
// one bounded step (an I2C transfer at most); nothing blocks for a settling time (the radio's 500 ms oscillator wait is a state the pass steps through, not a delay).

#include <cstdint>

namespace o3sdk::hal::capture {

enum class Source : std::uint8_t {
  LineMix = 0,   // line in, left and right mixed
  LineLeft,      // line in, the left channel only
  LineRight,     // line in, the right channel only
  MicLow,        // microphone, the low gain range (0..25 dB)
  MicHigh,       // microphone, the high gain range (0..44 dB, "Mic HG")
  Radio,         // the FM receiver, through the codec's line input
  Usb,           // the host's stream (USB return): no analogue part, the codec is left as it is
  Count
};
inline constexpr unsigned kSourceCount = static_cast<unsigned>(Source::Count);

// ---- The radio's regions --------------------------------------------------------------------------------------------------------------
enum class Region : std::uint8_t { Europe = 0, Usa, Australia, Japan, Count };
inline constexpr unsigned kRegionCount = static_cast<unsigned>(Region::Count);

struct RegionInfo {
  const char* name;
  std::uint16_t bottom;    // the lowest frequency, 10 kHz units
  std::uint8_t spacing;    // channel spacing, 10 kHz units (10: 100 kHz, 20: 200 kHz)
  bool japan_wide;         // Si4703 BAND = 1 (76..108 MHz) instead of 0 (87.5..108 MHz)
  bool deemphasis_50us;    // Si4703 DE: 50 us (Europe, Australia, Japan) instead of 75 us (USA)
};
// (bottom 87.5 / 76.0 MHz, spacing 0.1 / 0.2 MHz, DE and BAND as listed here).
inline constexpr RegionInfo kRegions[kRegionCount] = {
    {"EUROPE", 8750, 10, false, true},
    {"USA", 8750, 20, false, false},
    {"AUSTRALIA", 8750, 20, false, true},
    {"JAPAN", 7600, 10, true, true},
};
// The top of every band: 108 MHz. The highest channel a region reaches is the last whole spacing below it.
inline constexpr std::uint16_t kBandTop = 10800;

constexpr const RegionInfo& region_info(Region r) noexcept {
  return kRegions[static_cast<unsigned>(r) < kRegionCount ? static_cast<unsigned>(r) : 0u];
}
// How many channels a region has (the Si4703's CHANNEL field is 10 bits: 1023 at most).
constexpr unsigned channel_count(Region r) noexcept {
  const RegionInfo& i = region_info(r);
  return static_cast<unsigned>((kBandTop - i.bottom) / i.spacing) + 1u;
}
// The Si4703's channel number of a frequency: the nearest channel of the region ((freq - bottom) / spacing + 0.5, rounded to nearest), clamped to the band.
constexpr unsigned channel_of(Region r, unsigned freq) noexcept {
  const RegionInfo& i = region_info(r);
  if (freq <= i.bottom) {
    return 0;
  }
  const unsigned c = (freq - i.bottom + i.spacing / 2u) / i.spacing;
  const unsigned last = channel_count(r) - 1u;
  return c < last ? c : last;
}
// The frequency of a channel (channel * spacing + bottom); a channel past the band is the top channel.
constexpr unsigned freq_of(Region r, unsigned channel) noexcept {
  const RegionInfo& i = region_info(r);
  const unsigned last = channel_count(r) - 1u;
  return i.bottom + (channel < last ? channel : last) * i.spacing;
}
// A frequency made valid: the nearest channel's.
constexpr unsigned snap_freq(Region r, unsigned freq) noexcept { return freq_of(r, channel_of(r, freq)); }
// The encoder's step: `detents` channels from `freq`, clamped to the band (freq + detents * spacing, clamped to bottom and 108).
constexpr unsigned step_freq(Region r, unsigned freq, int detents) noexcept {
  const int c = static_cast<int>(channel_of(r, freq)) + detents;
  const int last = static_cast<int>(channel_count(r)) - 1;
  return freq_of(r, static_cast<unsigned>(c < 0 ? 0 : (c > last ? last : c)));
}

// ---- The radio's status --------------------------------------------------------------------------------------------------------------
struct RadioStatus {
  enum class State : std::uint8_t {
    Off,       // never powered, or powered down; the reset line holds the chip silent
    Starting,  // the power-up sequence is running (about 0.7 s)
    Ready,     // listening
    Tuning,    // a tune is in progress
    Seeking,   // a seek is in progress
    Absent,    // the chip did not answer (the board has no radio, or its wiring is not what the driver expects): nothing else works
    Failed     // it answered and then stopped: the bus failed three times in a row, or a tune or seek never completed
  };
  State state = State::Off;
  Region region = Region::Europe;
  std::uint16_t freq = 8750;     // the tuned frequency (10 kHz units): what the chip reports once it is ready, the target while it tunes
  std::uint8_t rssi = 0;         // the received signal strength, 0..75 dBuV (the Si4703's RSSI field)
  bool stereo = false;           // a stereo pilot is received (this OS forces the chip to mono: the flag is informational)
  bool band_limit = false;       // the last seek stopped at the band limit without finding a station (SF/BL)
  std::uint16_t chip_id = 0;     // DEVICEID as read, for the log and `diag` (0 until the chip answered)
  char fail[5] = {};             // where the last Failed or Absent came from, "OW/N": the step (two letters) and the cause (one letter), "" if none (si4703.h, `diag radio`)
  std::uint32_t generation = 0;  // moves whenever any field above changed: the screen redraws on it
};

// ---- The control seam --------------------------------------------------------------------------------------------------------------------
class CaptureHw {
 public:
  // Points the codec's ADC at `source` with a gain of `gain_percent` (0..100, mapped per source by the platform: line 0..15 of the ADC's own steps,
  // microphone 0..25 or 0..44 dB) and the analogue switch to the line jack or the receiver. A source with no analogue part (Usb) leaves the codec as it is and returns true.
  // False: a register write was not acknowledged. The caller holds the monitor path silent around a change (the ADC's input switch clicks).
  virtual bool select_source(Source source, std::uint8_t gain_percent) noexcept = 0;
  // The gain only (the encoder while recording): the same mapping, no switch. False: not acknowledged.
  virtual bool set_gain(Source source, std::uint8_t gain_percent) noexcept = 0;

  // The radio. `radio_power(true)` starts the power-up sequence (a no-op when it is on or starting) and `radio_power(false)` puts the chip back in reset. The region and the
  // frequency given with the power-up are what it comes up on; a tune, a seek or a region change while it is starting are applied when it is ready (the last one wins).
  virtual void radio_power(bool on) noexcept = 0;
  virtual void radio_region(Region region) noexcept = 0;
  virtual void radio_tune(std::uint16_t freq) noexcept = 0;
  virtual void radio_seek(bool up) noexcept = 0;
  // One bounded step of the radio's state machine and its status poll; `now_ms` is the 64-bit uptime. Called once a pass of the main loop, whatever the screen.
  virtual void service(std::uint64_t now_ms) noexcept = 0;
  virtual RadioStatus radio_status() const noexcept = 0;

 protected:
  CaptureHw() = default;
  ~CaptureHw() = default;  // never deleted through the interface: static instances, no heap
  CaptureHw(const CaptureHw&) = delete;
  CaptureHw& operator=(const CaptureHw&) = delete;
};

}  // namespace o3sdk::hal::capture
