// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/og_timing.h>

namespace o3sdk::app {

namespace {

// Appends `s` / the decimal of `v` at out[n], never past cap - 1.
std::size_t put(char* out, std::size_t cap, std::size_t n, const char* s) noexcept {
  for (; *s != '\0' && n + 1u < cap; ++s) {
    out[n++] = *s;
  }
  return n;
}

std::size_t put_dec(char* out, std::size_t cap, std::size_t n, std::uint32_t v) noexcept {
  char digits[10];
  unsigned d = 0;
  do {
    digits[d++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (d != 0u && n + 1u < cap) {
    out[n++] = digits[--d];
  }
  return n;
}

}  // namespace

void OgTiming::reset() noexcept {
  const Row survey = rows_[static_cast<unsigned>(Stage::Survey)];  // the question was asked before the export: its row stays
  for (Row& r : rows_) {
    r = Row{};
  }
  rows_[static_cast<unsigned>(Stage::Survey)] = survey;
  begin_step();
  worst_ = 0;
  worst_stage_ = Stage::Other;
}

void OgTiming::clear_survey() noexcept {
  rows_[static_cast<unsigned>(Stage::Survey)] = Row{};
}

void OgTiming::end_step(Stage rest, std::uint32_t started) noexcept {
  const std::uint32_t total = now() - started;
  std::uint32_t pieces = 0;
  for (const std::uint32_t c : step_) {
    pieces += c;
  }
  add(rest, total > pieces ? total - pieces : 0u);
  unsigned top = 0;  // the stage with most of the step
  for (unsigned i = 1; i < kStages; ++i) {
    top = step_[i] > step_[top] ? i : top;
  }
  worst(total, static_cast<Stage>(top));
}

const char* OgTiming::name(Stage s) noexcept {
  switch (s) {
    case Stage::Survey: return "survey";
    case Stage::Plan: return "plan";
    case Stage::Convert: return "convert";
    case Stage::Write: return "write";
    case Stage::Sync: return "sync";
    case Stage::Rename: return "rename";
    case Stage::Dir: return "dir";
    case Stage::Verify: return "verify";
    case Stage::Other:
    case Stage::Count: break;
  }
  return "other";
}

std::size_t OgTiming::line(unsigned i, char* out, std::size_t cap) const noexcept {
  if (out == nullptr || cap == 0u) {
    return 0;
  }
  out[0] = '\0';
  std::size_t n = 0;
  if (i == 0u) {
    if (worst_ == 0u) {
      return 0;
    }
    n = put(out, cap, n, "time worst ");
    n = put_dec(out, cap, n, worst_us());
    n = put(out, cap, n, " us in ");
    n = put(out, cap, n, name(worst_stage_));
  } else if (i <= kStages) {
    const Stage s = static_cast<Stage>(i - 1u);
    if (count(s) == 0u) {
      return 0;
    }
    n = put(out, cap, n, "time ");
    n = put(out, cap, n, name(s));
    n = put(out, cap, n, " max ");
    n = put_dec(out, cap, n, max_us(s));
    n = put(out, cap, n, " us n ");
    n = put_dec(out, cap, n, count(s));
    n = put(out, cap, n, " sum ");
    n = put_dec(out, cap, n, sum_us(s) / 1000u);
    n = put(out, cap, n, " ms");
  } else {
    return 0;
  }
  out[n] = '\0';
  return n;
}

}  // namespace o3sdk::app
