// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/job.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "support/rng.h"

using namespace o3sdk;
using core::Job;
using core::JobRunner;
using core::JobStatus;
using core::SlotState;
using core::Tick;

namespace {

// A clock that moves only when a job spends time: the jobs below "work" by advancing it.
struct FakeClock {
  mutable Tick t = 0;
  Tick now() const noexcept { return t; }
};

// A job with `work_us` of work. It does what it is asked: min(budget, left) microseconds per step, which it spends on the
// clock. `hog` makes it take `hog` times its slice instead (a job that ignores its budget).
class WorkJob final : public Job {
 public:
  WorkJob(FakeClock& clock, std::uint32_t work_us) : clock_(clock), total_(work_us), left_(work_us) {}

  JobStatus step(std::uint32_t budget_us) noexcept override {
    ++steps;
    last_budget = budget_us;
    max_budget = budget_us > max_budget ? budget_us : max_budget;
    if (fail_after != 0 && steps >= fail_after) {
      return JobStatus::Failed;
    }
    std::uint32_t spend = budget_us < left_ ? budget_us : left_;
    if (hog != 0) {
      spend = budget_us * hog;
      spend = spend < left_ ? spend : left_;
    }
    clock_.t += spend;
    left_ -= spend;
    done_us += spend;
    return left_ == 0 ? JobStatus::Done : JobStatus::Running;
  }
  unsigned progress() const noexcept override {
    return total_ == 0 ? core::kJobProgressMax : (total_ - left_) * 1000u / total_;  // total_ stays below 4e6 here
  }
  void cancel() noexcept override { ++cancels; }
  const char* name() const noexcept override { return "WORK"; }
  const char* detail() const noexcept override { return fail_after != 0 ? "BROKEN" : ""; }
  bool cancellable() const noexcept override { return !stubborn; }

  std::uint32_t steps = 0;
  std::uint32_t cancels = 0;
  std::uint32_t last_budget = 0;
  std::uint32_t max_budget = 0;
  std::uint32_t done_us = 0;
  std::uint32_t hog = 0;
  std::uint32_t fail_after = 0;
  bool stubborn = false;  // a job that cannot be abandoned

 private:
  FakeClock& clock_;
  std::uint32_t total_;
  std::uint32_t left_;
};

}  // namespace

TEST_CASE("jobs: four slots, a finished slot stays occupied until released") {
  FakeClock clock;
  JobRunner runner;
  WorkJob a(clock, 100), b(clock, 100), c(clock, 100), d(clock, 100), e(clock, 100);
  CHECK(runner.start(a) == 0);
  CHECK(runner.start(b) == 1);
  CHECK(runner.start(c) == 2);
  CHECK(runner.start(d) == 3);
  CHECK(runner.start(e) == -1);  // all four in use
  CHECK(runner.running() == 4u);
  CHECK(runner.occupied() == 4u);
  CHECK(runner.job(2) == &c);
  CHECK(runner.job(9) == nullptr);
  CHECK(runner.state(9) == SlotState::Free);

  const core::PassResult r = runner.run(clock, 100000);
  CHECK(r.stepped == 4u);
  CHECK(r.skipped == 0u);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(runner.state(i) == SlotState::Done);
  }
  CHECK(runner.running() == 0u);
  CHECK(runner.occupied() == 4u);
  CHECK(runner.start(e) == -1);  // still occupied by the finished ones: the maintainer has not read the results yet
  runner.release(1);
  CHECK(runner.state(1) == SlotState::Free);
  CHECK(runner.job(1) == nullptr);
  CHECK(runner.start(e) == 1);
  CHECK(runner.state(1) == SlotState::Running);
  runner.release(1);  // a Running slot is not released: cancel it first
  CHECK(runner.state(1) == SlotState::Running);
  runner.release(77);  // out of range: nothing
}

TEST_CASE("jobs: a pass never runs past its budget when the jobs keep to their slice, and splits what is left") {
  FakeClock clock;
  JobRunner runner;
  WorkJob a(clock, 10000), b(clock, 10000), c(clock, 10000);
  runner.start(a);
  runner.start(b);
  runner.start(c);

  core::PassResult r = runner.run(clock, 3000);
  CHECK(r.stepped == 3u);
  CHECK(r.used_us == 3000u);  // each got a third, and used all of it
  CHECK(a.last_budget == 1000u);
  CHECK(b.last_budget == 1000u);
  CHECK(c.last_budget == 1000u);

  // A job with little left leaves its share to the ones after it.
  FakeClock clock2;
  JobRunner runner2;
  WorkJob small(clock2, 100), big1(clock2, 10000), big2(clock2, 10000);
  runner2.start(small);
  runner2.start(big1);
  runner2.start(big2);
  r = runner2.run(clock2, 3000);
  CHECK(small.last_budget == 1000u);
  CHECK(big1.last_budget == (3000u - 100u) / 2u);                  // 1450: what was left, shared by two
  CHECK(big2.last_budget == (3000u - 100u - 1450u) / 1u);          // and the last one gets the rest
  CHECK(r.used_us == 3000u);
  CHECK(runner2.state(0) == SlotState::Done);

  // Every pass of a long run: the time used never exceeds the budget.
  for (int i = 0; i < 20; ++i) {
    r = runner.run(clock, 2500);
    CHECK(r.used_us <= 2500u);
    CHECK(r.skipped == 0u);
  }
  CHECK(runner.stats(0).overruns == 0u);
  CHECK(runner.running() == 0u);  // 30,000 us of work in all, at 2,500 a pass: done in 20 passes
}

TEST_CASE("jobs: a budget below the minimum slice runs nothing and says how many were left for the next pass") {
  FakeClock clock;
  JobRunner runner;
  WorkJob a(clock, 1000), b(clock, 1000);
  runner.start(a);
  runner.start(b);
  core::PassResult r = runner.run(clock, 0);
  CHECK(r.stepped == 0u);
  CHECK(r.skipped == 2u);
  CHECK(r.used_us == 0u);
  r = runner.run(clock, JobRunner::kMinSliceUs - 1);
  CHECK(r.stepped == 0u);
  CHECK(r.skipped == 2u);
  CHECK(a.steps == 0u);
  r = runner.run(clock, JobRunner::kMinSliceUs * 2);  // two jobs: the first gets 100, the second 50: both start
  CHECK(r.stepped == 2u);
  CHECK(a.last_budget == JobRunner::kMinSliceUs);
  CHECK(b.last_budget == JobRunner::kMinSliceUs);

  // No running job at all: an empty pass.
  JobRunner idle;
  r = idle.run(clock, 5000);
  CHECK(r.stepped == 0u);
  CHECK(r.skipped == 0u);
}

TEST_CASE("jobs: a job that ignores its slice is counted, the jobs after it find less left, and the next pass starts after it") {
  FakeClock clock;
  JobRunner runner;
  WorkJob rogue(clock, 100000), a(clock, 100000), b(clock, 100000);
  rogue.hog = 5;  // takes five times its slice
  runner.start(rogue);
  runner.start(a);
  runner.start(b);

  // budget 3000: the rogue is given 1000 and takes 5000. Nothing is left for the others in this pass.
  core::PassResult r = runner.run(clock, 3000);
  CHECK(r.stepped == 1u);
  CHECK(r.skipped == 2u);
  CHECK(r.used_us == 5000u);
  CHECK(runner.stats(0).overruns == 1u);
  CHECK(runner.stats(0).max_step_us == 5000u);
  CHECK(a.steps == 0u);

  // The next pass starts with the job that was skipped: the hog cannot keep the others out for ever.
  r = runner.run(clock, 3000);
  CHECK(a.steps == 1u);
  CHECK(b.steps == 1u);
  CHECK(rogue.steps == 2u);  // it was last in the order and still ran, but after the others had their go
  CHECK(runner.stats(1).overruns == 0u);
  CHECK(runner.stats(2).overruns == 0u);
}

TEST_CASE("jobs: permille_of scales without overflowing") {
  CHECK(core::permille_of(0, 0) == 0u);
  CHECK(core::permille_of(5, 0) == 0u);
  CHECK(core::permille_of(0, 100) == 0u);
  CHECK(core::permille_of(50, 100) == 500u);
  CHECK(core::permille_of(100, 100) == 1000u);
  CHECK(core::permille_of(500, 100) == 1000u);  // more than the whole is the whole
  CHECK(core::permille_of(4294967295u, 4294967295u) == 1000u);
  const unsigned half = core::permille_of(std::uint64_t{1} << 40, std::uint64_t{2} << 40);
  CHECK((half >= 499u && half <= 500u));
  const unsigned tenth = core::permille_of(1000000000u, 10000000000ull);
  CHECK((tenth >= 99u && tenth <= 100u));
}

TEST_CASE("jobs: overran() tolerates the clock granularity and a quarter of the slice") {
  CHECK_FALSE(JobRunner::overran(1000, 1000));
  CHECK_FALSE(JobRunner::overran(1270, 1000));  // 1000 + 250 + 20
  CHECK(JobRunner::overran(1271, 1000));
  CHECK_FALSE(JobRunner::overran(20, 0));
  CHECK(JobRunner::overran(21, 0));
}

TEST_CASE("jobs: cancel calls the job once, never steps it again, and leaves finished slots alone") {
  FakeClock clock;
  JobRunner runner;
  WorkJob a(clock, 100000), b(clock, 100000), done(clock, 10);
  const int sa = runner.start(a);
  const int sb = runner.start(b);
  const int sd = runner.start(done);
  runner.run(clock, 3000);
  CHECK(runner.state(static_cast<unsigned>(sd)) == SlotState::Done);
  const std::uint32_t before = a.steps;

  runner.cancel(static_cast<unsigned>(sa));
  CHECK(runner.state(static_cast<unsigned>(sa)) == SlotState::Cancelled);
  CHECK(a.cancels == 1u);
  runner.cancel(static_cast<unsigned>(sa));  // again: nothing
  CHECK(a.cancels == 1u);
  runner.cancel(static_cast<unsigned>(sd));  // already done: nothing
  CHECK(done.cancels == 0u);
  CHECK(runner.state(static_cast<unsigned>(sd)) == SlotState::Done);
  runner.cancel(31);  // no such slot

  runner.run(clock, 3000);
  CHECK(a.steps == before);  // not stepped again
  CHECK(b.steps >= 2u);

  runner.release(static_cast<unsigned>(sa));
  CHECK(runner.state(static_cast<unsigned>(sa)) == SlotState::Free);
  runner.cancel_all();
  CHECK(b.cancels == 1u);
  CHECK(runner.running() == 0u);
  CHECK(runner.state(static_cast<unsigned>(sb)) == SlotState::Cancelled);
}

TEST_CASE("jobs: progress, Failed and the detail text") {
  FakeClock clock;
  JobRunner runner;
  WorkJob a(clock, 4000), bad(clock, 4000);
  bad.fail_after = 2;
  const int sa = runner.start(a);
  const int sb = runner.start(bad);
  CHECK(runner.job(static_cast<unsigned>(sa))->progress() == 0u);
  runner.run(clock, 2000);  // each gets 1000 of its 4000
  CHECK(runner.job(static_cast<unsigned>(sa))->progress() == 250u);
  CHECK(runner.state(static_cast<unsigned>(sb)) == SlotState::Running);
  runner.run(clock, 2000);
  CHECK(runner.state(static_cast<unsigned>(sb)) == SlotState::Failed);
  CHECK(std::strcmp(runner.job(static_cast<unsigned>(sb))->detail(), "BROKEN") == 0);
  CHECK(runner.job(static_cast<unsigned>(sa))->progress() == 500u);
  runner.run(clock, 2000);
  runner.run(clock, 2000);
  CHECK(runner.state(static_cast<unsigned>(sa)) == SlotState::Done);
  CHECK(runner.job(static_cast<unsigned>(sa))->progress() == core::kJobProgressMax);
  CHECK(runner.stats(static_cast<unsigned>(sb)).steps == 2u);  // not stepped after Failed
}

TEST_CASE("jobs: the budget logic survives the wrap of the 32-bit tick") {
  FakeClock clock;
  clock.t = 0xFFFFFF00u;  // 256 us before the wrap
  JobRunner runner;
  WorkJob a(clock, 5000), b(clock, 5000);
  runner.start(a);
  runner.start(b);
  const core::PassResult r = runner.run(clock, 1000);
  CHECK(r.stepped == 2u);
  CHECK(r.used_us == 1000u);
  CHECK(clock.t == 0xFFFFFF00u + 1000u);  // wrapped past zero
  CHECK(a.last_budget == 500u);
}

TEST_CASE("jobs: random jobs and budgets, 2000 passes: compliant jobs never exceed a budget, everyone finishes, fairly") {
#if defined(__arm__)
  constexpr int kRounds = 40;
#else
  constexpr int kRounds = 400;
#endif
  test::Rng rng(0x0B5E55EDu);
  for (int round = 0; round < kRounds; ++round) {
    FakeClock clock;
    clock.t = rng.next();
    JobRunner runner;
    WorkJob j0(clock, 1 + rng.below(40000)), j1(clock, 1 + rng.below(40000)), j2(clock, 1 + rng.below(40000)),
        j3(clock, 1 + rng.below(40000));
    WorkJob* jobs[4] = {&j0, &j1, &j2, &j3};
    for (WorkJob* j : jobs) {
      CHECK(runner.start(*j) >= 0);
    }
    for (int pass = 0; pass < 5000 && runner.running() != 0; ++pass) {
      const std::uint32_t budget = rng.below(4000);
      const unsigned running = runner.running();
      const core::PassResult r = runner.run(clock, budget);
      CHECK(r.used_us <= budget);
      CHECK(r.stepped + r.skipped == running);  // every running job was either stepped or left for the next pass
    }
    CHECK(runner.running() == 0u);
    for (unsigned i = 0; i < 4; ++i) {
      CHECK(runner.state(i) == SlotState::Done);
      CHECK(runner.stats(i).overruns == 0u);
    }
  }
}

// ---- a step that cancels or releases another job ---------------------------------------------------------------------------

namespace {

// A job whose first step calls a hook with the runner (a job that cancels, releases or starts another).
class HookJob final : public Job {
 public:
  using Hook = void (*)(JobRunner&, void*);
  HookJob(FakeClock& clock, JobRunner& runner, Hook hook, void* ctx, JobStatus answer = JobStatus::Running)
      : clock_(clock), runner_(runner), hook_(hook), ctx_(ctx), answer_(answer) {}
  JobStatus step(std::uint32_t budget_us) noexcept override {
    ++steps;
    last_budget = budget_us;
    clock_.t += budget_us;  // uses its whole slice
    if (hook_ != nullptr && steps == 1) {
      hook_(runner_, ctx_);
    }
    return answer_;
  }
  unsigned progress() const noexcept override { return 0; }
  void cancel() noexcept override { ++cancels; }
  const char* name() const noexcept override { return "HOOK"; }
  std::uint32_t steps = 0;
  std::uint32_t cancels = 0;
  std::uint32_t last_budget = 0;

 private:
  FakeClock& clock_;
  JobRunner& runner_;
  Hook hook_;
  void* ctx_;
  JobStatus answer_;
};

struct CancelTarget {
  unsigned slot;
  bool release;
};
void cancel_it(JobRunner& r, void* ctx) {
  const CancelTarget* t = static_cast<const CancelTarget*>(ctx);
  r.cancel(t->slot);
  if (t->release) {
    r.release(t->slot);
  }
}

}  // namespace

TEST_CASE("jobs: a step that cancels a sibling: the sibling is not stepped this pass, the rest share what is left") {
  FakeClock clock;
  JobRunner runner;
  CancelTarget target{1, false};
  HookJob a(clock, runner, cancel_it, &target);
  WorkJob b(clock, 100000), c(clock, 100000);
  runner.start(a);
  runner.start(b);
  runner.start(c);
  const core::PassResult r = runner.run(clock, 3000);
  CHECK(a.last_budget == 1000u);
  CHECK(b.steps == 0u);  // cancelled by a's step before its turn
  CHECK(b.cancels == 1u);
  CHECK(runner.state(1) == SlotState::Cancelled);
  CHECK(c.steps == 1u);
  CHECK(c.last_budget == 2000u);  // b's share went to c, not into the void
  CHECK(r.stepped == 2u);
  CHECK(r.skipped == 0u);
  CHECK(r.used_us == 3000u);
  // And never again.
  runner.run(clock, 3000);
  CHECK(b.steps == 0u);
  CHECK(b.cancels == 1u);
}

TEST_CASE("jobs: a step that cancels and releases a sibling, or starts another in its slot, is safe") {
  FakeClock clock;
  JobRunner runner;
  CancelTarget target{1, true};
  HookJob a(clock, runner, cancel_it, &target);
  WorkJob b(clock, 100000), c(clock, 100000);
  runner.start(a);
  runner.start(b);
  runner.start(c);
  core::PassResult r = runner.run(clock, 3000);  // b's slot is Free now, with no job in it
  CHECK(b.steps == 0u);
  CHECK(runner.state(1) == SlotState::Free);
  CHECK(runner.job(1) == nullptr);
  CHECK(c.steps == 1u);
  CHECK(r.stepped == 2u);
  CHECK(runner.occupied() == 2u);

  // The freed slot is taken by a new job from inside a step: it is Running when its turn comes, so it runs in the same pass.
  FakeClock clock2;
  JobRunner runner2;
  WorkJob fresh(clock2, 100000);
  struct Swap {
    WorkJob* job;
    unsigned slot;
  } swap{&fresh, 1};
  HookJob a2(clock2, runner2, [](JobRunner& rr, void* ctx) {
    Swap* s = static_cast<Swap*>(ctx);
    rr.cancel(s->slot);
    rr.release(s->slot);
    CHECK(rr.start(*s->job) == static_cast<int>(s->slot));
  }, &swap);
  WorkJob b2(clock2, 100000);
  runner2.start(a2);
  runner2.start(b2);
  r = runner2.run(clock2, 2000);
  CHECK(b2.steps == 0u);
  CHECK(b2.cancels == 1u);
  CHECK(fresh.steps == 1u);  // the replacement ran
  CHECK(runner2.job(1) == &fresh);
  CHECK(runner2.state(1) == SlotState::Running);
}

TEST_CASE("jobs: a job that cancels itself in its step stays Cancelled, whatever it answers") {
  for (const JobStatus answer : {JobStatus::Running, JobStatus::Done, JobStatus::Failed}) {
    FakeClock clock;
    JobRunner runner;
    CancelTarget target{0, false};
    HookJob a(clock, runner, cancel_it, &target, answer);
    runner.start(a);
    runner.run(clock, 1000);
    CHECK(runner.state(0) == SlotState::Cancelled);
    CHECK(a.cancels == 1u);
    runner.run(clock, 1000);
    CHECK(a.steps == 1u);
    runner.release(0);
    CHECK(runner.state(0) == SlotState::Free);
  }
}

TEST_CASE("jobs: a job that cannot be cancelled keeps running when asked, and the others are cancelled") {
  FakeClock clock;
  JobRunner runner;
  WorkJob stubborn(clock, 5000), plain(clock, 100000);
  stubborn.stubborn = true;
  runner.start(stubborn);
  runner.start(plain);
  runner.cancel(0);
  CHECK(runner.state(0) == SlotState::Running);
  CHECK(stubborn.cancels == 0u);
  runner.cancel_all();
  CHECK(runner.state(0) == SlotState::Running);
  CHECK(runner.state(1) == SlotState::Cancelled);
  CHECK(plain.cancels == 1u);
  runner.run(clock, 3000);
  CHECK(stubborn.steps == 1u);
  for (int i = 0; i < 10; ++i) {
    runner.run(clock, 3000);
  }
  CHECK(runner.state(0) == SlotState::Done);  // it ran to its end
}
