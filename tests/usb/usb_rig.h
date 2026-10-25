// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b test rig: the whole device side of the USB driver (descriptors, control pipe, rings, UsbDevice) wired to the fake
// controller of usb_fake_controller.h, plus a host. Used by the control, device and diag tests; it also runs on QEMU, so it
// uses no heap: a rig is constructed in place in static storage.
#pragma once

#include <o3sdk/usb/cdc.h>
#include <o3sdk/usb/control.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/device.h>
#include <o3sdk/usb/validate.h>

#include "usb_fake_controller.h"

#include <cstddef>
#include <cstdint>
#include <new>

namespace usb_rig {

using namespace o3sdk::usb;
using usb_fake::FakeController;
using usb_fake::Host;
using usb_fake::make_setup;
using usb_fake::Reply;

// Everything the driver needs, in the order the platform builds it.
struct alignas(kBdtAlign) Rig {
  explicit Rig(Config config = Config{})
      : cfg(config),
        dev_len(build_device_descriptor(cfg, dev_desc, sizeof dev_desc)),
        cfg_len(build_config_descriptor(cfg, cfg_desc, sizeof cfg_desc)),
        pipe(cfg, dev_desc, dev_len, cfg_desc, cfg_len),
        hw(bdt),
        dev(hw, bdt, pipe, audio, tx_ring, rx_ring, midi_in, midi_out),
        host(hw),
        writer(tx_ring) {
    hw.bind(&Rig::isr_thunk, this);
    dev.prepare();  // the state the platform leaves the controller in before the pull-up
  }

  static void isr_thunk(void* self) { static_cast<Rig*>(self)->dev.isr(); }

  BdtTable bdt{};
  Config cfg;
  std::uint8_t dev_desc[kDeviceDescriptorSize] = {};
  std::uint8_t cfg_desc[kMaxConfigDescriptorSize] = {};
  std::size_t dev_len;
  std::size_t cfg_len;
  ControlPipe pipe;
  Counters audio;
  CdcTxRing tx_ring;
  CdcRxRing rx_ring;
  MidiRing midi_in;
  MidiRing midi_out;
  FakeController hw;
  UsbDevice<FakeController> dev;
  Host host;
  CdcTxWriter writer;

  // ---- host-side conveniences ----
  Host::Result get_descriptor(std::uint8_t type, std::uint8_t index, std::uint16_t lang, std::uint16_t length, std::uint8_t* out,
                              std::uint32_t cap) {
    std::uint8_t raw[8];
    make_setup(raw, 0x80, 0x06, static_cast<std::uint16_t>((type << 8) | index), lang, length);
    return host.control_in(raw, out, cap);
  }
  Host::Result set_address(std::uint8_t address) {
    std::uint8_t raw[8];
    make_setup(raw, 0x00, 0x05, address, 0, 0);
    return host.control_none(raw);
  }
  Host::Result set_configuration(std::uint8_t value) {
    std::uint8_t raw[8];
    make_setup(raw, 0x00, 0x09, value, 0, 0);
    const Host::Result r = host.control_none(raw);
    host.reset_toggles();  // a configuration event resets the host's toggles too
    return r;
  }
  Host::Result set_feature_halt(bool set, std::uint8_t endpoint) {
    std::uint8_t raw[8];
    make_setup(raw, 0x02, set ? 0x03 : 0x01, 0, endpoint, 0);
    const Host::Result r = host.control_none(raw);
    if (!set) {
      host.reset_toggles();
    }
    return r;
  }

  // A real host's sequence up to a configured device: reset, device descriptor, address, descriptors, configuration.
  bool enumerate(std::uint8_t address = 7) {
    hw.bus_reset();
    std::uint8_t buf[512];
    Host::Result r = get_descriptor(1, 0, 0, 8, buf, sizeof buf);  // the first 8 bytes, as hosts do before they know bMaxPacketSize0
    if (!r.ok || r.length != 8u) {
      return false;
    }
    if (!set_address(address).ok) {
      return false;
    }
    r = get_descriptor(1, 0, 0, 18, buf, sizeof buf);
    if (!r.ok || r.length != 18u) {
      return false;
    }
    r = get_descriptor(2, 0, 0, 9, buf, sizeof buf);
    if (!r.ok || r.length != 9u) {
      return false;
    }
    r = get_descriptor(2, 0, 0, static_cast<std::uint16_t>(buf[2] | (buf[3] << 8)), buf, sizeof buf);
    if (!r.ok || r.length != cfg_len) {
      return false;
    }
    return set_configuration(1).ok;
  }
};

// Static storage for one rig at a time; each test case builds a fresh one.
inline Rig& fresh(Config config = Config{}) {
  alignas(kBdtAlign) static unsigned char storage[sizeof(Rig)];
  static Rig* current = nullptr;
  if (current != nullptr) {
    current->~Rig();
  }
  current = new (storage) Rig(config);
  return *current;
}

}  // namespace usb_rig
