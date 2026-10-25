// SPDX-License-Identifier: GPL-3.0-or-later
//
// When the outputs open (output_gate.h, track3r-os OS-007g, OS-OUTON): the audio path has to have run clean for a number of blocks, and what
// happened before that window does not count (an underrun anywhere in the uptime used to keep the outputs shut for good). OpenRetry: a failed
// unmute (the codec's I2C bus is shared with the button scanners) costs a backoff, never the loop. The cases at the end run main.cpp's own steps
// as audio.h describes them (a reading into the gate, the gate's verdict, the retry's, the unmute) over a timeline of boot, a fault and a mute.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/platform/mk66/output_gate.h>

#include <cstdint>

using o3sdk::platform::mk66::open_wanted;
using o3sdk::platform::mk66::OpenRetry;
using o3sdk::platform::mk66::OutputGate;

TEST_CASE("output gate: the default window is the proven 400 blocks (about 0.6 s)") {
  const OutputGate g;
  CHECK(g.clean_blocks() == 400u);
  CHECK(OutputGate::kCleanBlocks == 400u);
}

TEST_CASE("output gate: a clean run opens at the settle count, as the cumulative rule did") {
  OutputGate g(400);
  g.observe(70, 0, 0);
  CHECK_FALSE(g.ready(0));
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
  g.observe(140, 0, 0);
  g.observe(210, 0, 0);
  CHECK(g.ready(400));
  CHECK(g.ready(100000));
}

TEST_CASE("output gate: no reading ever taken is the same as a clean run (the first look of the loop)") {
  const OutputGate g(400);
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
}

TEST_CASE("output gate: an underrun moves the window, a clean stretch after it opens the gate again") {
  OutputGate g(400);
  g.observe(70, 0, 0);
  g.observe(140, 0, 0);
  g.observe(210, 3, 0);  // three late blocks somewhere in the last poll period: the clean run starts here
  CHECK_FALSE(g.ready(210));
  CHECK_FALSE(g.ready(609));
  CHECK(g.ready(610));
  // The cumulative rule would have refused for ever: the counter is still 3 and stays 3.
  g.observe(280, 3, 0);
  g.observe(1000, 3, 0);
  CHECK(g.ready(1000));
}

TEST_CASE("output gate: a hardware fault moves the window the same way, and a new one moves it again") {
  OutputGate g(400);
  g.observe(100, 0, 1);
  CHECK_FALSE(g.ready(499));
  CHECK(g.ready(500));
  g.observe(450, 0, 2);
  CHECK_FALSE(g.ready(500));
  CHECK_FALSE(g.ready(849));
  CHECK(g.ready(850));
  g.observe(900, 1, 2);
  CHECK_FALSE(g.ready(1299));
  CHECK(g.ready(1300));
}

TEST_CASE("output gate: faults that keep coming keep it shut") {
  OutputGate g(400);
  std::uint32_t faults = 0;
  for (std::uint32_t b = 70; b < 20000; b += 70) {
    g.observe(b, 0, ++faults);  // one at every poll: the path is never clean for 400 blocks
    CHECK_FALSE(g.ready(b));
  }
}

TEST_CASE("output gate: a reading with a smaller boundary count is a restart of the audio path") {
  OutputGate g(400);
  g.observe(5000, 7, 2);
  g.observe(5070, 7, 2);
  CHECK(g.ready(5470));
  g.observe(10, 0, 0);  // a new start(): the status counters began again
  CHECK_FALSE(g.ready(10));
  CHECK_FALSE(g.ready(399));
  CHECK(g.ready(400));
  g.observe(80, 1, 0);  // and a fault after the restart counts
  CHECK_FALSE(g.ready(479));
  CHECK(g.ready(480));
}

TEST_CASE("open retry: a first attempt is always allowed; a failure holds the next one back for the backoff") {
  OpenRetry r;
  CHECK(r.ready(false, 0));
  CHECK(r.ready(false, 123456));
  r.failed(10000);
  CHECK(r.failures() == 1);
  CHECK_FALSE(r.ready(false, 10000));
  CHECK_FALSE(r.ready(false, 11999));
  CHECK(r.ready(false, 12000));
  CHECK_FALSE(r.ready(true, 11999));  // an explicit request waits for the backoff as well: the unmute blocks for up to 0.2 s, not every pass
  CHECK(r.ready(true, 12000));
}

TEST_CASE("open retry: the automatic attempts stop after three failures; an explicit request keeps its retries") {
  OpenRetry r;
  r.failed(0);
  r.failed(2000);
  CHECK_FALSE(r.gave_up());
  CHECK(r.ready(false, 4000));
  r.failed(4000);
  CHECK(r.gave_up());
  CHECK_FALSE(r.ready(false, 6000));       // the cap: nobody asked, so the outputs stay closed
  CHECK_FALSE(r.ready(false, 1000000));
  CHECK_FALSE(r.ready(true, 5999));        // a request (track3r-os: a Play that found them closed): still behind the backoff ...
  CHECK(r.ready(true, 6000));              // ... and then it retries, as often as it asks
  r.failed(6000);
  CHECK(r.failures() == 4);
  CHECK_FALSE(r.ready(true, 7999));
  CHECK(r.ready(true, 8000));
  r.succeeded();                           // a success starts the count again
  CHECK(r.failures() == 0);
  CHECK_FALSE(r.gave_up());
  CHECK(r.ready(false, 8000));
}

TEST_CASE("open retry: the backoff survives the millisecond counter wrapping, and the failure count saturates") {
  OpenRetry r;
  r.failed(0xFFFFFFFFu - 500u);            // retry_at wraps to 1499
  CHECK_FALSE(r.ready(false, 0xFFFFFFFFu));
  CHECK_FALSE(r.ready(false, 1498));
  CHECK(r.ready(false, 1499));
  for (int i = 0; i < 400; ++i) {
    r.failed(5);
  }
  CHECK(r.failures() == 255);
}

// ---- OS-OUTON: when the main loop makes the attempt (open_wanted, restored from track3r-os with its cases) -------------------------------------------------------------

TEST_CASE("open_wanted: at boot, with nobody asking, the attempt waits for the audio path and for the project load, and then it is made") {
  CHECK_FALSE(open_wanted(/*audio_up=*/false, /*opened=*/false, /*asked=*/false, /*loading=*/false, /*fw_busy=*/false, /*powering_off=*/false));  // the audio path did not come up: nothing to open
  CHECK_FALSE(open_wanted(true, false, false, /*loading=*/true, false, false));                                                  // the boot project load: the unmute must not sit on its slices
  CHECK(open_wanted(true, false, false, false, false, false));                                                            // loaded, or nothing to load (no card, no Workspace)
}

TEST_CASE("open_wanted: a Play, a Song Play or a live note that found the outputs closed does not wait for the load (the fallback)") {
  CHECK(open_wanted(true, false, /*asked=*/true, /*loading=*/true, false, false));
  CHECK(open_wanted(true, false, true, false, false, false));
  CHECK_FALSE(open_wanted(/*audio_up=*/false, false, true, false, false, false));  // no audio path: the request waits for a start, it opens nothing
  CHECK_FALSE(open_wanted(false, false, true, true, false, false));
}

TEST_CASE("open_wanted: once opened, not again until whatever muted them clears the flag") {
  for (int asked = 0; asked < 2; ++asked) {
    for (int loading = 0; loading < 2; ++loading) {
      CHECK_FALSE(open_wanted(true, /*opened=*/true, asked != 0, loading != 0, false, false));
    }
  }
}

TEST_CASE("open_wanted: never while the firmware update path holds the card or waits for the bootloader, asked or not") {
  CHECK_FALSE(open_wanted(true, false, false, false, /*fw_busy=*/true, false));
  CHECK_FALSE(open_wanted(true, false, true, false, true, false));
  CHECK_FALSE(open_wanted(true, false, true, true, true, false));
  CHECK(open_wanted(true, false, false, false, false, false));  // and it comes back the moment the update path is idle
}

TEST_CASE("open_wanted: never while the shutdown has the audio path (the power-off latch), whatever the other arguments are") {
  for (int audio_up = 0; audio_up < 2; ++audio_up) {
    for (int opened = 0; opened < 2; ++opened) {
      for (int asked = 0; asked < 2; ++asked) {
        for (int loading = 0; loading < 2; ++loading) {
          for (int fw_busy = 0; fw_busy < 2; ++fw_busy) {
            CHECK_FALSE(open_wanted(audio_up != 0, opened != 0, asked != 0, loading != 0, fw_busy != 0, /*powering_off=*/true));
          }
        }
      }
    }
  }
  // and without the latch the same inputs open as before
  CHECK(open_wanted(true, false, false, false, false, /*powering_off=*/false));
  CHECK(open_wanted(true, false, true, true, false, false));
}

namespace {

// main.cpp's loop body for the open (audio.h, "HOW main.cpp USES IT", step 3): a fresh reading into the gate, then, while the path runs and the outputs
// are closed, the gate's verdict and the retry's; then the unmute: a success opens, a failure is shown and retried later; nothing in it ever stops the loop.
struct BootLoop {
  OutputGate gate;
  OpenRetry retry;
  bool started = true;      // audio_device().start() returned Ok
  bool opened = false;
  bool unmute_ok = true;    // false: a codec write that is not acknowledged
  bool fw_busy = false;     // appimg::firmware_update_active(): the update path holds the card or waits for the bootloader
  bool led_fault = false;   // the audio LED code for a failed unmute
  int unmutes = 0;
  int failures_seen = 0;
  int passes = 0;           // the rest of the loop (display, input, LEDs) ran this many times: a failed open does not stop it
  // One pass with one status reading; true when the unmute ran on it.
  bool pass(std::uint32_t ms, std::uint32_t boundaries, std::uint32_t underruns = 0, std::uint32_t faults = 0) {
    ++passes;
    gate.observe(boundaries, underruns, faults);
    if (!open_wanted(started, opened, /*asked=*/false, /*loading=*/false, fw_busy, /*powering_off=*/false) || !gate.ready(boundaries) || !retry.ready(false, ms)) {
      return false;
    }
    ++unmutes;
    if (unmute_ok) {
      opened = true;
      retry.succeeded();
      led_fault = false;
    } else {
      opened = false;
      retry.failed(ms);
      led_fault = true;
      ++failures_seen;
    }
    return true;
  }
  // audio_device().mute() from elsewhere in the image: the loop's flag follows.
  void muted() { opened = false; }
};

}  // namespace

TEST_CASE("boot timeline: a clean start opens on the first pass that sees 400 boundaries, and once") {
  BootLoop loop;
  CHECK_FALSE(loop.pass(10, 7));
  CHECK_FALSE(loop.pass(300, 206));
  CHECK_FALSE(loop.pass(580, 399));
  CHECK(loop.pass(581, 400));
  CHECK(loop.opened);
  CHECK_FALSE(loop.pass(700, 482));
  CHECK_FALSE(loop.pass(100000, 68900));
  CHECK(loop.unmutes == 1);
}

TEST_CASE("boot timeline: the display bring-up took seconds: the first pass of the loop opens at once") {
  BootLoop loop;
  CHECK(loop.pass(2500, 1700));
  CHECK(loop.opened);
  CHECK(loop.unmutes == 1);
}

TEST_CASE("boot timeline: an underrun moves the gate's window, and the open waits for 400 clean blocks after it") {
  BootLoop loop;
  CHECK_FALSE(loop.pass(100, 70));
  CHECK_FALSE(loop.pass(200, 140, /*underruns=*/2));  // two late blocks somewhere in the last poll period: the clean run starts at 140
  CHECK_FALSE(loop.pass(500, 350, 2));
  CHECK_FALSE(loop.pass(780, 539, 2));
  CHECK(loop.pass(781, 540, 2));  // 400 blocks after the boundary the fault was seen at
  CHECK(loop.opened);
}

TEST_CASE("boot timeline: a start that failed opens nothing; a later start that works goes through the same gate") {
  BootLoop loop;
  loop.started = false;
  for (std::uint32_t ms = 0; ms < 2000; ms += 100) {
    CHECK_FALSE(loop.pass(ms, 0));
  }
  CHECK(loop.unmutes == 0);
  loop.started = true;  // start() retried and returned Ok: the counters begin again at 0
  CHECK_FALSE(loop.pass(2100, 1));
  CHECK_FALSE(loop.pass(2500, 399));
  CHECK(loop.pass(2501, 400));  // the same 400 clean blocks
  CHECK(loop.opened);
}

TEST_CASE("failed unmute at boot: the loop goes on, the fault is shown, the retry waits for the backoff, and the attempts end at three") {
  BootLoop loop;
  loop.unmute_ok = false;  // the codec's I2C bus NACKs (it is shared with the button scanners)
  CHECK(loop.pass(3000, 3000));  // attempt 1 at 3000
  CHECK_FALSE(loop.opened);
  CHECK(loop.led_fault);
  CHECK(loop.failures_seen == 1);
  const int passes_before = loop.passes;
  for (std::uint32_t ms = 3001; ms < 5000; ++ms) {  // 2 s of ordinary passes: no attempt, no 0.2 s block on every pass
    CHECK_FALSE(loop.pass(ms, ms));
  }
  CHECK(loop.passes == passes_before + 1999);       // the loop ran every one of them: nothing halted
  CHECK(loop.unmutes == 1);
  CHECK(loop.pass(5000, 5000));                     // attempt 2 after the backoff
  CHECK(loop.failures_seen == 2);
  CHECK_FALSE(loop.pass(6999, 6999));
  CHECK(loop.pass(7000, 7000));                     // attempt 3
  CHECK(loop.failures_seen == 3);
  CHECK(loop.retry.gave_up());
  for (std::uint32_t ms = 7001; ms < 60000; ms += 250) {  // after the cap the outputs stay closed and the loop never blocks again
    CHECK_FALSE(loop.pass(ms, ms));
  }
  CHECK(loop.unmutes == 3);
  CHECK_FALSE(loop.opened);
  CHECK(loop.led_fault);
}

TEST_CASE("failed unmute: a second attempt that works opens the outputs and takes the fault code back") {
  BootLoop loop;
  loop.unmute_ok = false;
  CHECK(loop.pass(3000, 3000));
  loop.unmute_ok = true;  // the bus is fine again
  CHECK_FALSE(loop.pass(4999, 4999));
  CHECK(loop.pass(5000, 5000));
  CHECK(loop.opened);
  CHECK_FALSE(loop.led_fault);
  CHECK(loop.retry.failures() == 0);
  CHECK(loop.unmutes == 2);
}

TEST_CASE("a mute from elsewhere clears the flag, and the loop opens them again through the gate") {
  BootLoop loop;
  CHECK(loop.pass(3000, 3000));
  NEED(loop.opened);
  loop.muted();
  CHECK_FALSE(loop.opened);
  CHECK(loop.pass(3100, 3070));  // the path has been clean all along: the next pass opens again
  CHECK(loop.unmutes == 2);
  loop.muted();
  CHECK_FALSE(loop.pass(3200, 3140, 1));  // a fault since: the window starts over
  CHECK_FALSE(loop.pass(3480, 3539, 1));
  CHECK(loop.pass(3481, 3540, 1));
  CHECK(loop.unmutes == 3);
}

TEST_CASE("fw update hand-over: quiesce clears the open; nothing raises the amplifier during the wait; a failed or timed-out hand-over opens them again") {
  BootLoop loop;
  CHECK(loop.pass(3000, 3000));
  CHECK(loop.opened);
  // `fw update`: the upload and the re-check hold the card, then quiesce() mutes the outputs and appimg::audio_closed() clears the flag.
  loop.fw_busy = true;
  CHECK_FALSE(loop.pass(10000, 10000));
  loop.muted();
  CHECK_FALSE(loop.opened);
  for (std::uint32_t ms = 10001; ms < 15000; ms += 100) {  // the 5 s wait for the bootloader: the update path is busy, the gate would be ready, the amplifier stays down
    CHECK_FALSE(loop.pass(ms, ms));
  }
  CHECK(loop.unmutes == 1);
  loop.fw_busy = false;  // "the bootloader did not take over": the phase is idle again
  CHECK(loop.pass(15100, 15100));
  CHECK(loop.opened);
  CHECK(loop.unmutes == 2);
}
