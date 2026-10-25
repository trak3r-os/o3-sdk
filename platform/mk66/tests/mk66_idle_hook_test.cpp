// SPDX-License-Identifier: GPL-3.0-or-later
//
// IdleHook (idle_hook.h, OS-003f): the hook boot::delay_ms() calls once a millisecond. Its one rule is that it is never entered from
// itself (a hook that services the display may find the driver re-initialising the chip, which waits with delay_ms()). The two cases
// are track3r-os's mk66_boot_gate_test.cpp hook cases; the gate itself was the tracker's composition root's and is not in the kit.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/idle_hook.h>

using o3sdk::platform::mk66::IdleHook;

namespace {

int g_runs = 0;
IdleHook* g_hook = nullptr;
int g_depth = 0;
int g_max_depth = 0;

void count_run() noexcept { ++g_runs; }
void nest_run() noexcept {
  ++g_runs;
  ++g_depth;
  g_max_depth = g_depth > g_max_depth ? g_depth : g_max_depth;
  g_hook->run();  // a hook that waits, and the wait calls the hook again
  --g_depth;
}

}  // namespace

TEST_CASE("idle hook: runs once a call, nothing without one, and removing it stops it") {
  IdleHook h;
  CHECK_FALSE(h.installed());
  g_runs = 0;
  h.run();
  CHECK(g_runs == 0);
  h.set(&count_run);
  CHECK(h.installed());
  h.run();
  h.run();
  h.run();
  CHECK(g_runs == 3);
  h.set(nullptr);
  CHECK_FALSE(h.installed());
  h.run();
  CHECK(g_runs == 3);
}

TEST_CASE("idle hook: it is never entered from itself, and it is armed again when it returns") {
  IdleHook h;
  g_hook = &h;
  g_runs = 0;
  g_depth = 0;
  g_max_depth = 0;
  h.set(&nest_run);
  h.run();  // the hook calls run() from inside: that call does nothing
  CHECK(g_runs == 1);
  CHECK(g_max_depth == 1);
  h.run();  // and the next call, from outside, runs it again
  CHECK(g_runs == 2);
  CHECK(g_max_depth == 1);
  g_hook = nullptr;
}
