// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app's recorder (kit/app recorder.h, docs/M2C-PLAN.md section 6, slice RC): Context::recorder() over a hal::record::Recorder in a Platform. With no recorder every call is refused and
// says so; with one, each call reaches it as made; clip() is the take's audio (a Level take settled, without its leading ring) and only when the take is Done; save() refuses what is not a
// take. The real recorder is record_service_test.cpp (the render side) and sim_record_test.cpp (the whole path through the simulator, and the save that reads back equal).
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/recorder.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/hal/record.h>
#include <o3sdk/platform/host/fakes.h>

#include <cstdint>
#include <vector>

using namespace o3sdk;
namespace hr = o3sdk::hal::record;

namespace {

class Blank final : public app::Screen {
 public:
  const char* title() const noexcept override { return "REC TEST"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class PlainApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"RECTEST", "0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }

 private:
  Blank screen_;
};

// A recorder that keeps what it was asked and answers as told.
class FakeRecorder final : public hr::Recorder {
 public:
  bool arm(const hr::Take& t) noexcept override {
    ++arms;
    last = t;
    if (refuse) {
      return false;
    }
    st = t.align == hr::Align::Free ? hr::State::Recording : (t.align == hr::Align::Level ? hr::State::Listening : hr::State::Waiting);
    return true;
  }
  void stop_at(hr::Align a) noexcept override { stopped_at = a; }
  void disarm() noexcept override { ++disarms; }
  hr::State state() const noexcept override { return st; }
  std::uint32_t frames() const noexcept override { return nframes; }
  std::uint32_t overrun() const noexcept override { return lost; }
  std::uint32_t settle() noexcept override {
    ++settles;
    return lead;
  }
  void release() noexcept override {
    ++releases;
    st = hr::State::Idle;
  }
  void set_tap(hr::Tap t) noexcept override { tap_ = t; }
  hr::Tap tap() const noexcept override { return tap_; }
  void set_pattern_steps(std::uint32_t n) noexcept override { pattern = n; }
  std::uint32_t pattern_steps() const noexcept override { return pattern; }

  bool refuse = false;
  hr::State st = hr::State::Idle;
  hr::Take last;
  hr::Align stopped_at = hr::Align::Free;
  std::uint32_t arms = 0, disarms = 0, settles = 0, releases = 0;
  std::uint32_t nframes = 0, lost = 0, lead = 0, pattern = 16;
  hr::Tap tap_ = hr::Tap::Input;
};

struct Rig {
  platform::host::FakeDisplay display;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  PlainApp app;
  FakeRecorder fake;
  app::Platform platform;
  app::Runtime runtime;

  explicit Rig(bool with_recorder) : platform{display, input, nullptr, clock, false}, runtime((platform.recorder = with_recorder ? &fake : nullptr, app), engine, platform) {}
  app::Recorder& rec() { return runtime.context().recorder(); }
};

}  // namespace

TEST_CASE("app recorder: with no recorder every call is refused and says so") {
  Rig rig(false);
  app::Recorder& r = rig.rec();
  alignas(4) std::int16_t buf[64] = {};
  hr::Take t;
  t.buf = buf;
  t.capacity_frames = 32;
  CHECK_FALSE(r.available());
  CHECK_FALSE(r.arm(t));
  CHECK(r.state() == hr::State::Idle);
  CHECK(r.frames() == 0u);
  CHECK(r.overrun() == 0u);
  CHECK(r.settle() == 0u);
  r.stop_at(hr::Align::Bar);
  r.disarm();
  r.release();
  r.set_tap(hr::Tap::Master);
  CHECK(r.tap() == hr::Tap::Input);
  r.set_pattern_steps(32);
  CHECK(r.clip().frames == 0u);
  CHECK(r.clip().pcm == nullptr);
  CHECK_FALSE(r.save(rig.runtime.context().files(), "", "take"));
}

TEST_CASE("app recorder: each call reaches the recorder as made") {
  Rig rig(true);
  app::Recorder& r = rig.rec();
  const app::Recorder& cr = rig.runtime.context().recorder();
  CHECK(r.available());
  CHECK(cr.available());
  alignas(4) std::int16_t buf[256] = {};
  hr::Take t;
  t.buf = buf;
  t.capacity_frames = 100;
  t.channels = hr::Channels::Stereo;
  t.align = hr::Align::Bar;
  t.auto_steps = 32;
  CHECK(r.arm(t));
  CHECK(rig.fake.arms == 1u);
  CHECK(rig.fake.last.buf == buf);
  CHECK(rig.fake.last.capacity_frames == 100u);
  CHECK(rig.fake.last.channels == hr::Channels::Stereo);
  CHECK(rig.fake.last.align == hr::Align::Bar);
  CHECK(rig.fake.last.auto_steps == 32u);
  CHECK(r.state() == hr::State::Waiting);
  rig.fake.refuse = true;
  CHECK_FALSE(r.arm(t));
  rig.fake.refuse = false;
  r.stop_at(hr::Align::Pattern);
  CHECK(rig.fake.stopped_at == hr::Align::Pattern);
  r.disarm();
  CHECK(rig.fake.disarms == 1u);
  r.set_tap(hr::Tap::Master);
  CHECK(r.tap() == hr::Tap::Master);
  r.set_pattern_steps(12);
  CHECK(rig.fake.pattern == 12u);
  rig.fake.nframes = 77;
  rig.fake.lost = 5;
  CHECK(r.frames() == 77u);
  CHECK(r.overrun() == 5u);
  r.release();
  CHECK(rig.fake.releases == 1u);
  CHECK(r.state() == hr::State::Idle);
}

TEST_CASE("app recorder: clip is the take's audio, only when it is Done, without a Level take's leading ring") {
  Rig rig(true);
  app::Recorder& r = rig.rec();
  alignas(4) std::int16_t buf[2 * 200] = {};
  hr::Take t;
  t.buf = buf;
  t.capacity_frames = 200;
  t.channels = hr::Channels::Mix;
  REQUIRE(r.arm(t));
  rig.fake.nframes = 120;
  CHECK(r.clip().frames == 0u);  // Recording: not a take yet
  CHECK(rig.fake.settles == 0u);
  rig.fake.st = hr::State::Done;
  app::Recorder::Clip c = r.clip();
  CHECK(c.pcm == buf);
  CHECK(c.frames == 120u);
  CHECK(c.channels == 1u);
  // A Level take: the first 30 frames are the ring's zeros.
  rig.fake.st = hr::State::Idle;
  hr::Take level = t;
  level.align = hr::Align::Level;
  level.threshold = 1000;
  level.channels = hr::Channels::Stereo;
  REQUIRE(r.arm(level));
  rig.fake.st = hr::State::Done;
  rig.fake.nframes = 150;
  rig.fake.lead = 30;
  c = r.clip();
  CHECK(rig.fake.settles >= 1u);
  CHECK(c.pcm == buf + 2 * 30);  // stereo: two samples a frame
  CHECK(c.frames == 120u);
  CHECK(c.channels == 2u);
  // A take that was stopped before it began, or whose audio is all ring: nothing.
  rig.fake.nframes = 0;
  rig.fake.lead = 0;
  CHECK(r.clip().frames == 0u);
  rig.fake.nframes = 30;
  rig.fake.lead = 30;
  CHECK(r.clip().frames == 0u);
  CHECK(r.clip().pcm == nullptr);
}

TEST_CASE("app recorder: save refuses what is not a Done take, and a card that is not there") {
  Rig rig(true);
  app::Recorder& r = rig.rec();
  app::Files& files = rig.runtime.context().files();
  alignas(4) std::int16_t buf[128] = {};
  hr::Take t;
  t.buf = buf;
  t.capacity_frames = 64;
  CHECK_FALSE(r.save(files, "", "take"));  // nothing armed
  REQUIRE(r.arm(t));
  CHECK_FALSE(r.save(files, "", "take"));  // recording
  rig.fake.st = hr::State::Done;
  rig.fake.nframes = 64;
  CHECK_FALSE(r.save(files, "", "take"));  // a platform with no card: the files service says NO CARD (this rig has none)
  CHECK(files.status().phase == app::Files::Status::Failed);
  rig.fake.nframes = 0;
  CHECK_FALSE(r.save(files, "", "take"));  // an empty take is no file
}
