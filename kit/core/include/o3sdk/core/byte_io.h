// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3sdk::core {

// Byte-stream contracts for the resumable readers and writers (formats/native today, formats/og next).
// They do no file I/O themselves: the platform wraps an SD file, a USB pipe or a test buffer behind them.
// Both are non-blocking by contract, so a cooperative job (OS-002c) can drive a reader or writer in small
// steps and never stalls the main loop.

enum class IoStatus : std::uint8_t {
  Ok,          // `count` > 0 bytes moved (possibly fewer than asked)
  WouldBlock,  // nothing moved now, `count` == 0: ask again on a later step
  End,         // sources only: the stream is over, `count` == 0, and every later read says End again
  Failed,      // device or medium error: the stream is unusable, `count` is ignored
};

struct IoResult {
  std::size_t count;
  IoStatus status;
};

// Callers never trust an implementation: a count above what was asked, or Ok with a zero count, is treated
// as Failed.
class ByteSource {
 public:
  // Reads at most `max` (> 0) bytes into `dst`.
  virtual IoResult read(void* dst, std::size_t max) noexcept = 0;

 protected:
  ByteSource() = default;
  ~ByteSource() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;
};

class ByteSink {
 public:
  // Accepts at most `n` (> 0) bytes from `src`; Ok reports how many were taken (a short count is normal).
  // `End` is not a valid answer for a sink.
  virtual IoResult write(const void* src, std::size_t n) noexcept = 0;

 protected:
  ByteSink() = default;
  ~ByteSink() = default;
  ByteSink(const ByteSink&) = delete;
  ByteSink& operator=(const ByteSink&) = delete;
};

}  // namespace o3sdk::core
