// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/input/pad_bus.h>

#include <o3sdk/platform/mk66/input/pins.h>

namespace o3sdk::platform::mk66::input {

bool PadBus::ready() noexcept {
  if (open_) {
    return true;
  }
  if (!armed_ || !audio_done_) {
    return false;
  }
  // (a 32-bit difference: wrap-safe, and the gate is long open by the time the low word of the uptime wraps, after 49 days)
  if (static_cast<std::uint32_t>(clock_.uptime_ms()) - rail_up_ms_ < kSettleMs) {
    return false;
  }
  open_ = true;
  return true;
}

I2cResult PadBus::transfer(std::uint8_t address7, const std::uint8_t* tx, std::uint8_t tx_len, std::uint8_t* rx, std::uint8_t rx_len) noexcept {
  if (!ready()) {
    return I2cResult::Nak;  // refused, not recorded: nothing touched the bus
  }
  if (!started_) {
    (void)start_master();
  }
  const I2cResult r = master_.transfer(address7, tx, tx_len, rx, rx_len);
  note(address7, r);
  return r;
}

I2cResult PadBus::read(std::uint8_t address7, std::uint8_t* rx, std::uint8_t rx_len) noexcept {
  if (!ready()) {
    return I2cResult::Nak;
  }
  if (!started_) {
    (void)start_master();
  }
  const I2cResult r = master_.read(address7, rx, rx_len);
  note(address7, r);
  return r;
}

I2cResult PadBus::recover() noexcept {
  if (!ready()) {
    return I2cResult::Nak;
  }
  if (!started_) {
    return start_master();  // the first initialisation is the clear
  }
  if (!needs_clear()) {
    return I2cResult::Ok;  // a NAK is the bus working: no clock gate, no remux, no nine pulses
  }
  const I2cResult r = master_.recover();
  note_clear(r);
  last_ = r;
  return r;
}

hal::PadBusStats PadBus::stats() const noexcept {
  hal::PadBusStats s = record_;
  if (started_) {  // before the master has taken PTA12 / PTA13 their mux is 0 and they read 0 whatever the line does: nothing to report
    s.lines_now = lines_.read();
  }
  return s;
}

// Takes the levels, then initialises the master (its bus clear runs inside) and records the result. Once, at the first call after the gate.
I2cResult PadBus::start_master() noexcept {
  started_ = true;
  record_.lines_first = lines_.read_before_init();
  const I2cResult r = master_.recover();
  note_clear(r);
  last_ = r;
  return r;
}

// The bus needs its module re-initialised and its clock pulsed when the latest transaction ended in a timeout, an arbitration loss or a stuck bus
// (a slave may hold a line), or when a line reads low now. An Ok or a NAK as the latest result with both lines high needs nothing.
bool PadBus::needs_clear() noexcept {
  if (last_ == I2cResult::Timeout || last_ == I2cResult::ArbitrationLost || last_ == I2cResult::BusStuck) {
    return true;
  }
  constexpr std::uint8_t kBoth = hal::PadBusStats::kLineScl | hal::PadBusStats::kLineSda;
  return (lines_.read() & kBoth) != kBoth;
}

void PadBus::note(std::uint8_t address7, I2cResult result) noexcept {
  last_ = result;
  hal::BusProbe* probe = nullptr;
  if (address7 == pins::kKeyScannerAddress) {
    probe = &record_.scanner;
  } else if (address7 == pins::kLedDriverAddress) {
    probe = &record_.leds;
  } else {
    return;  // no other chip is on this bus
  }
  const auto code = static_cast<std::uint8_t>(result);
  if (probe->count == 0u) {
    probe->first = code;
  }
  probe->last = code;
  if (probe->count != 0xFFFFFFFFu) {
    ++probe->count;
  }
}

void PadBus::note_clear(I2cResult result) noexcept {
  record_.clear = result == I2cResult::Ok ? hal::PadBusStats::kClearReleased : hal::PadBusStats::kClearHeld;
  if (record_.clears != 0xFFu) {
    ++record_.clears;
  }
}

}  // namespace o3sdk::platform::mk66::input
