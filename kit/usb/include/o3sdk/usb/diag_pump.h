// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/usb/cdc.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::usb::diag {

// The main loop's side of the `diag` console (OS-006b): moves what the host typed from the CDC receive ring into the shell, with
// back-pressure so that no reply is ever lost.
//
// The chain is: host -> bulk OUT descriptors -> CdcRxRing (interrupt writes) -> this pump -> Shell -> CdcTxWriter -> CdcTxRing -> bulk IN -> host.
// A paste of several commands used to be able to start more replies than the transmit ring holds (3.9 KB, and the main loop writes a whole
// reply in microseconds while the host takes a packet a millisecond), which dropped the heads of replies. Now each link says "not yet":
//   * the shell takes input only while the transmit ring has room for a reply (kConsoleFeedRoomBytes, the largest a command makes) and while
//     no pager owns the terminal; the pump keeps what it was not given and offers it again on the next pass;
//   * the receive ring then fills, and at kCdcRxHoldChunks of its 15 the interrupt stops arming the bulk OUT descriptors, so the host's
//     packets are NAKed and retried by the host itself (usb/device.h);
//   * a long reply is a pager (a non-interruptible stream) that writes only what fits.
// Nothing blocks and nothing is dropped; the price is that a paste is served at the speed the host reads.
inline constexpr std::size_t kConsoleFeedRoomBytes = 1600;

class ConsolePump {
 public:
  ConsolePump(Shell& shell, CdcRxRing& rx) noexcept : shell_(shell), rx_(rx) {}

  // One pass of the main loop. Bounded: at most twice the ring's depth of chunks.
  void service() noexcept {
    for (unsigned i = 0; i < 2u * CdcRxRing::capacity(); ++i) {
      if (off_ >= chunk_.len) {
        if (rx_.read(&chunk_, 1) != 1u) {
          chunk_.len = 0;
          off_ = 0;
          return;
        }
        off_ = 0;
      }
      off_ += static_cast<std::uint32_t>(shell_.feed(chunk_.data + off_, chunk_.len - off_, kConsoleFeedRoomBytes));
      if (off_ < chunk_.len) {
        return;  // held back: no room for a reply, or a pager owns the terminal
      }
    }
  }

  // Bytes read from the ring and not yet given to the shell.
  std::uint32_t held_bytes() const noexcept { return chunk_.len > off_ ? chunk_.len - off_ : 0u; }

 private:
  Shell& shell_;
  CdcRxRing& rx_;
  ByteChunk chunk_{};  // the chunk being fed: the shell may take only part of it
  std::uint32_t off_ = 0;
};

}  // namespace o3sdk::usb::diag
