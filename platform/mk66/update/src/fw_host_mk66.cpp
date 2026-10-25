// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's side of the firmware update path (OS-006c): what `fw update` does to the board. THE ONLY SOURCE THAT NAMES board::kBootloaderRequest, the pin
// PTA1 (tools/check_layers.py rule 7), and the only place that drives it.
//
// The hand-over (_fwinfo with
// the bare image name written first, then the bootloader request pin driven low): PTA1, board pin
// 81, is configured as a GPIO output the way the Teensy core's pin setup does (PORT_PCR = MUX(1) | DSE | SRE; NXP MK66F18.h PORT_PCR_MUX_MASK 0x700,
// PORT_PCR_DSE_MASK 0x40, PORT_PCR_SRE_MASK 0x4), its data bit cleared, then its direction set to output, so it is driven low from the first instant it is an output.
// What reacts to that line (the hardware or the bootloader) is not known. This code waits 5 s (fw_service.cpp) and
// then lets the pin go: input, with its reset pad function restored (JTAG_TDI, pull-up), nothing driven. That this is the right resting state (the line was
// never touched before) is an assumption for the first hardware session (README, FW checklist).
#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/storage/device.h>
#include <o3sdk/platform/mk66/update/fat_firmware_store.h>
#include <o3sdk/platform/mk66/update/fw_device.h>
#include <o3sdk/platform/mk66/update/fw_sink.h>
#include <o3sdk/platform/mk66/usb/console.h>
#include <o3sdk/platform/mk66/usb/usb0.h>

#include <cstdint>
#include <new>

namespace o3sdk::platform::mk66::update {
namespace {

constexpr std::uint32_t kPcrMux1 = 1u << 8;  // PORT_PCR_MUX(1): GPIO
constexpr std::uint32_t kPcrSre = 1u << 2;   // slew rate: slow
constexpr std::uint32_t kPcrDse = 1u << 6;   // drive strength: high
// PTA1's reset state: ALT7 = JTAG_TDI with the pull enabled and selected up (K66 RM, PORTx_PCRn / signal multiplexing: the JTAG pins reset to their debug function
// with pulls; the RM was not machine-readable here, so 0x703 is from its table, not from MK66F18.h: checklist FW-10 reads the pin).
constexpr std::uint32_t kPcrResetPta1 = (7u << 8) | (1u << 1) | (1u << 0);

class Mk66FwHost final : public FwHost {
 public:
  std::uint32_t now_ms() noexcept override { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

  // The cycle counter, the SysTick millisecond count and a hash of the chip's unique id (the USB serial is made from it): not secret, but not a number
  // anyone can guess from outside either; the token is a guard against a stray line, not against an attacker with the cable.
  std::uint32_t entropy() noexcept override {
    std::uint32_t h = regs::dwt_cyccnt() ^ (static_cast<std::uint32_t>(system_clock().uptime_ms()) * 2246822519u);
    for (const char* p = usb0::serial(); p != nullptr && *p != '\0'; ++p) {
      h = (h ^ static_cast<std::uint8_t>(*p)) * 16777619u;
    }
    return h;
  }

  bool storage_busy() noexcept override { return appimg::storage_in_use(); }  // a project load or a running save (OS-007o)
  bool card_on_host() noexcept override { return storage::device::owner() == storage::CardOwner::Host; }
  std::uint32_t host_sessions() noexcept override { return storage::device::host_sessions(); }

  void quiesce() noexcept override {
    appimg::stop_transport();
    audio_device().mute();  // the amplifier key first, then the codec (audio.cpp Mk66Audio::mute)
    appimg::audio_closed();  // OS-OUTON: the app's "outputs open" follows the codec, so a hand-over that fails (or times out) leaves them to be opened again, not silent for good
  }

  void drain_output(std::uint32_t max_ms) noexcept override {
    usb0::cdc_writer().flush();
    const std::uint64_t start = system_clock().uptime_ms();
    while (system_clock().uptime_ms() - start < max_ms) {
      asm volatile("wfi");  // the next SysTick; the USB interrupt sends the packets meanwhile
    }
  }

  void bootloader_request(bool low) noexcept override {
    const Pin pin = board::kBootloaderRequest;
    if (low) {
      regs::sim_scgc5() = regs::sim_scgc5() | regs::kScgc5PortA;  // the port's clock gate (boot::early_pins turned them all on; this does not rely on it)
      regs::port_pcr(pin) = kPcrMux1 | kPcrDse | kPcrSre;
      regs::gpio_pcor(pin) = regs::mask_of(pin);                     // the data bit low before the pin becomes an output
      regs::gpio_pddr(pin) = regs::gpio_pddr(pin) | regs::mask_of(pin);
    } else {
      regs::gpio_pddr(pin) = regs::gpio_pddr(pin) & ~regs::mask_of(pin);  // an input again, before the pad function changes
      regs::port_pcr(pin) = kPcrResetPta1;                                // back to what the chip gave it at reset (JTAG_TDI, pulled up)
    }
  }

  void log(const char* text) noexcept override { console::log(text); }
};

}  // namespace

namespace device {

FwService& service() noexcept {
  static Mk66FwHost host;
  static FatFirmwareStore store(storage::device::fat(), storage::device::block());
  static FwService svc(store, host);
  return svc;
}

hal::firmware::FirmwareSink& sink() noexcept {
  static FwServiceSink adapter(service());
  return adapter;
}

}  // namespace device
}  // namespace o3sdk::platform::mk66::update
