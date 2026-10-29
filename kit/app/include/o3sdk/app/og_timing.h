// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the time of an OG export goes (OS-FILES-C T16, v0.6.8: the longest step of a file's job was 263-360 ms on the unit with writes of 8 KB, 6000+ overruns an export): the cost of each stage of the
// export, in CPU cycles read from a counter the platform hands in (the MK66's is hal::cycles(), the DWT counter; a test hands in a stub), kept per stage as the longest single piece, how many pieces and the sum,
// and the single worst job step with the stage that took most of it. Read by `diag ogexp status`. It measures and changes nothing: with no counter attached every call is one branch.
//
// Stages (one job step is one AtomicSave::step; a stage is the part of the main loop's work it names):
//   Survey   a slice of the question's survey (SaveService::og_survey_step); kept over an export start, cleared by the next survey
//   Plan     the plan and the next file's set-up (OgExportPlan::begin / next, the instrument decision, the checks before a file)
//   Convert  the source's own work in a step: the OG writers, the PCM resampler / converter, the CRC (a Write step less its f_write calls)
//   Write    one f_write of the card writer (FatFs: the data, the FAT update, the cluster allocation)
//   Sync     finish(): f_sync + f_close of the file
//   Rename   the renames and the removals (temp -> final, backup moves)
//   Dir      f_stat / open / size / mkdir / create / the gate: the directory side, and the folder name lookup
//   Verify   the read-back of the file (open, 2 KB reads, close)
//   Other    the save service's bookkeeping around a file: the job's start (the card pin, the guard's checks) and the file's end (reconcile)
// Main loop only; no allocation, no heap, nothing shared with an ISR.

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

class OgTiming {
 public:
  enum class Stage : std::uint8_t { Survey, Plan, Convert, Write, Sync, Rename, Dir, Verify, Other, Count };
  static constexpr unsigned kStages = static_cast<unsigned>(Stage::Count);
  static constexpr unsigned kLines = kStages + 1u;  // line(): the worst step, then one per stage
  using Counter = std::uint32_t (*)() noexcept;     // a free-running 32-bit cycle count (differences only)

  // `counter` null: off. `per_us`: counter ticks in a microsecond (180 on the MK66, 1 for a counter in microseconds).
  void attach(Counter counter, std::uint32_t per_us) noexcept {
    counter_ = counter;
    per_us_ = per_us != 0u ? per_us : 1u;
  }
  bool on() const noexcept { return counter_ != nullptr; }
  std::uint32_t now() const noexcept { return counter_(); }

  // The start of an export: everything but the survey's row is cleared. clear_survey(): the start of a survey.
  void reset() noexcept;
  void clear_survey() noexcept;

  // One timed piece of a stage, ended by the destructor. `unit`: the piece is a whole main-loop step of its own (the plan, a survey slice is not: its row is kept apart), so it competes for the worst step.
  // Inside a job step (begin_step .. end_step) the pieces are summed into the step and `unit` is false.
  class Scope {
   public:
    Scope(OgTiming* timing, Stage stage, bool unit = true) noexcept : timing_(timing != nullptr && timing->on() ? timing : nullptr), stage_(stage), unit_(unit) {
      if (timing_ != nullptr) {
        start_ = timing_->now();
      }
    }
    ~Scope() { end(); }
    // Ends the piece now (once; the destructor then does nothing).
    void end() noexcept {
      if (timing_ != nullptr) {
        const std::uint32_t used = timing_->now() - start_;
        timing_->add(stage_, used);
        if (unit_) {
          timing_->worst(used, stage_);
        }
        timing_ = nullptr;
      }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    OgTiming* timing_;
    Stage stage_;
    bool unit_;
    std::uint32_t start_ = 0;
  };

  // A job step: the pieces noted between begin_step() and end_step() belong to it; end_step() books the rest of the step (its time less the pieces) to `rest` and offers the whole to the worst step,
  // under the stage that took most of it.
  void begin_step() noexcept {
    for (std::uint32_t& c : step_) {
      c = 0;
    }
  }
  void end_step(Stage rest, std::uint32_t started) noexcept;

  // The pieces so far, in microseconds (max, sum) and the count.
  std::uint32_t max_us(Stage s) const noexcept { return rows_[idx(s)].max / per_us_; }
  std::uint32_t count(Stage s) const noexcept { return rows_[idx(s)].count; }
  std::uint32_t sum_us(Stage s) const noexcept { return static_cast<std::uint32_t>(rows_[idx(s)].sum / per_us_); }
  std::uint32_t worst_us() const noexcept { return worst_ / per_us_; }
  Stage worst_stage() const noexcept { return worst_stage_; }
  static const char* name(Stage s) noexcept;

  // Line i of `ogexp status`'s timing report into out[0..cap) (NUL-terminated): 0 the worst step ("time worst 263412 us in write"), 1..kStages a stage ("time write max 263412 us n 6110 sum 8123 ms").
  // The length; 0 when there is nothing to say (no piece yet) or i is out of range.
  std::size_t line(unsigned i, char* out, std::size_t cap) const noexcept;

  void add(Stage s, std::uint32_t used) noexcept {
    Row& r = rows_[idx(s)];
    r.max = used > r.max ? used : r.max;
    ++r.count;
    r.sum += used;
    step_[idx(s)] += used;
  }
  void worst(std::uint32_t used, Stage s) noexcept {
    if (used > worst_) {
      worst_ = used;
      worst_stage_ = s;
    }
  }

 private:
  struct Row {
    std::uint32_t max = 0;
    std::uint32_t count = 0;
    std::uint64_t sum = 0;
  };
  static constexpr unsigned idx(Stage s) noexcept { return static_cast<unsigned>(s) < kStages ? static_cast<unsigned>(s) : 0u; }

  Counter counter_ = nullptr;
  std::uint32_t per_us_ = 1;
  Row rows_[kStages];
  std::uint32_t step_[kStages] = {};  // the pieces of the job step in progress, by stage
  std::uint32_t worst_ = 0;
  Stage worst_stage_ = Stage::Other;
};

}  // namespace o3sdk::app
