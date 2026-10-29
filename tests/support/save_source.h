// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers the save tests share (OS-007n): a SaveSource over a vector that produces in the chunks the job asks for and can be told to fail, and a pump that runs a job to its end.
#include <o3sdk/app/atomic_save.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace o3sdk::test {

namespace app = o3sdk::app;
using Bytes = std::vector<std::uint8_t>;

// A source over a vector, produced in the chunks the job asks for; can be told to fail after some bytes.
class VecSource final : public app::SaveSource {
 public:
  explicit VecSource(Bytes data) : data_(std::move(data)) {}
  std::size_t fail_after = ~std::size_t{0};
  std::size_t steps = 0;
  void begin(o3sdk::core::ByteSink& sink) noexcept override {
    sink_ = &sink;
    pos_ = 0;
  }
  o3sdk::formats::Progress step(std::size_t budget) noexcept override {
    ++steps;
    if (pos_ >= fail_after) {
      return o3sdk::formats::Progress::Failed;
    }
    while (budget != 0 && pos_ < data_.size()) {
      std::size_t n = data_.size() - pos_ < budget ? data_.size() - pos_ : budget;
      if (fail_after != ~std::size_t{0} && pos_ + n > fail_after) {
        n = fail_after - pos_;
        if (n == 0) {
          return o3sdk::formats::Progress::Failed;
        }
      }
      const o3sdk::core::IoResult r = sink_->write(data_.data() + pos_, n);
      if (r.status != o3sdk::core::IoStatus::Ok) {
        return o3sdk::formats::Progress::Failed;
      }
      pos_ += r.count;
      budget -= r.count;
    }
    return pos_ >= data_.size() ? o3sdk::formats::Progress::Done : o3sdk::formats::Progress::Running;
  }
  const char* failure() const noexcept override { return "TEST SOURCE FAILED"; }
  std::uint32_t size_hint() const noexcept override { return static_cast<std::uint32_t>(data_.size()); }

 private:
  Bytes data_;
  o3sdk::core::ByteSink* sink_ = nullptr;
  std::size_t pos_ = 0;
};

// Runs a job to its end (the runner would give it slices): returns the number of steps, or 0 when it never ended.
inline unsigned run(app::AtomicSave& job, std::uint32_t budget_us = 2000) {
  for (unsigned i = 1; i < 100000; ++i) {
    if (job.step(budget_us) != o3sdk::core::JobStatus::Running) {
      return i;
    }
  }
  return 0;
}

}  // namespace o3sdk::test
