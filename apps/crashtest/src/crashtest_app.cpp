// SPDX-License-Identifier: GPL-3.0-or-later
//
// CRASH TEST: a session-only test app, never a release. Its one console command, `fault`, provokes each fault the MK66 platform's crash record and status-LED
// blink code are meant to catch (faults.h), so that a tester session can prove on the unit that the record survives the reset and that the LED shows it
// (docs/M2C-PLAN.md section 9, decision D15; apps/crashtest/CHECKLIST.md is the procedure). Nothing on the unit's own controls provokes anything: the
// pads and buttons do nothing here.
//
//   fault                    lists the faults
//   fault <kind>             says what the fault does and what `crash` must show afterwards; does nothing
//   fault <kind> confirm     arms it: the reply goes out, the screen counts down 1.5 s, then the fault is provoked and the unit resets itself
//   fault cancel             disarms an armed fault during that second
//
// `state` adds `armed=<kind|none> prev=<kind|none> nofault=<kind|none> seq=<n>`: what is armed now; the fault the run before this boot provoked (kept across the
// reset in a few words of .noinit RAM, read once and forgotten: after a power-up or a second reset it is `none`); the fault that came back without a fault;
// how many were provoked since the RAM last powered up. The platform's own `crash`, `stage` and `log` say what happened.

#include "faults.h"
#include "marker.h"

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/diag.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/dsp/engine.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

using namespace o3sdk;
using crashtest::Kind;
using ui::Color;
using ui::Face;
using ui::Rect;

constexpr std::uint32_t kFireDelayMs = 1500;  // from the arming pass to the fault: the reply and the screen go out first
constexpr char kConfirm[] = "confirm";

bool same(const char* a, const char* b) noexcept {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// ---- What survives the reset ------------------------------------------------------------------------------------------------------------
// marker.h; the object is here: in the .noinit section of the image (no initialiser, no constructor), an ordinary zeroed static on the host (the simulator, the tests).
#if defined(__arm__)
[[gnu::section(".bss.noinit")]] crashtest::Marker g_marker;
#else
crashtest::Marker g_marker;
#endif

// ---- The render-side fault: a clocked hook ----------------------------------------------------------------------------------------------
// `render` provokes the bus fault inside the engine's block, in the render interrupt, to show the record from handler mode. The main loop arms it, the render
// interrupt consumes it: idle -> armed (main) -> running -> returned (render, only if the core took no fault) -> idle (main).
class RenderFault final : public dsp::Clocked {
 public:
  void arm() noexcept { state_.store(kArmed, std::memory_order_release); }
  void reset() noexcept { state_.store(kIdle, std::memory_order_release); }
  bool returned() const noexcept { return state_.load(std::memory_order_acquire) == kReturned; }

  void block(dsp::BlockContext&) noexcept override {
    if (state_.load(std::memory_order_acquire) != kArmed) {
      return;
    }
    state_.store(kRunning, std::memory_order_relaxed);
    crashtest::provoke(Kind::Bus);
    state_.store(kReturned, std::memory_order_release);
  }

 private:
  static constexpr std::uint8_t kIdle = 0;
  static constexpr std::uint8_t kArmed = 1;
  static constexpr std::uint8_t kRunning = 2;
  static constexpr std::uint8_t kReturned = 3;
  std::atomic<std::uint8_t> state_{kIdle};
};

// ---- The screen ---------------------------------------------------------------------------------------------------------------------------

class CrashApp;
CrashApp& instance() noexcept;

class CrashScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "FAULTS"; }
  void draw(app::Context& ctx, ui::Renderer& r, const Rect& area) noexcept override;
};

// ---- The app ------------------------------------------------------------------------------------------------------------------------------

class CrashApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"CRASH TEST SESSION ONLY", "0.0.0 SESSION ONLY"};
    return kInfo;
  }

  void setup(app::Context& ctx) noexcept override {
    pending_ = false;
    fire_at_ = 0;
    waiting_render_ = false;
    nofault_ = false;
    render_.reset();
    std::uint32_t kind = 0;
    std::uint32_t seq = 0;
    prev_ = false;
    seq_ = 0;
    if (crashtest::marker_read(g_marker, kind, seq)) {
      seq_ = seq;
      if (kind != 0u && kind <= crashtest::kKindCount) {
        prev_ = true;
        prev_kind_ = static_cast<Kind>(kind - 1u);
      }
    }
    crashtest::marker_write(g_marker, 0, seq_);  // read once: a later reset that is not ours says `none`
    (void)ctx.engine().add_hook(render_);
    ctx.show(screen_);
  }

  void update(app::Context& ctx) noexcept override {
    if (waiting_render_ && render_.returned()) {
      waiting_render_ = false;
      nofault_ = true;
      nofault_kind_ = Kind::Render;
      crashtest::marker_write(g_marker, 0, seq_);
      render_.reset();
      ctx.redraw();
    }
    if (!pending_) {
      return;
    }
    const std::uint64_t now = ctx.uptime_ms();
    if (fire_at_ == 0u) {
      fire_at_ = now + kFireDelayMs;  // the first pass after the command: its reply has been queued, the screen shows the countdown
      ctx.redraw();
      return;
    }
    if (now < fire_at_) {
      return;  // (the screen redraws on the runtime's frame clock: the countdown moves by itself)
    }
    fire(ctx);
  }

  std::size_t diag_commands(const app::DiagCommand*& rows) const noexcept override;

  std::size_t describe_state(const app::Context&, char* out, std::size_t cap) const noexcept override {
    app::StateWriter w(out, cap);
    w.str("armed", pending_ ? crashtest::info(kind_).name : "none");
    w.str("prev", prev_ ? crashtest::info(prev_kind_).name : "none");
    w.str("nofault", nofault_ ? crashtest::info(nofault_kind_).name : "none");
    w.num("seq", seq_);
    return w.size();
  }

  // ---- For the command and the screen (main loop). ----
  bool pending() const noexcept { return pending_; }
  Kind pending_kind() const noexcept { return kind_; }
  std::uint64_t fire_at() const noexcept { return fire_at_; }
  bool has_prev() const noexcept { return prev_; }
  Kind prev_kind() const noexcept { return prev_kind_; }
  bool has_nofault() const noexcept { return nofault_; }
  Kind nofault_kind() const noexcept { return nofault_kind_; }

  void arm(Kind kind) noexcept {
    kind_ = kind;
    pending_ = true;
    fire_at_ = 0;
    nofault_ = false;
  }
  void cancel() noexcept {
    pending_ = false;
    fire_at_ = 0;
  }

 private:
  // The marker first (the fault may never come back), then the fault. When the core took none, this returns and says so.
  void fire(app::Context& ctx) noexcept {
    const Kind kind = kind_;
    pending_ = false;
    fire_at_ = 0;
    ++seq_;
    crashtest::marker_write(g_marker, static_cast<std::uint32_t>(kind) + 1u, seq_);
    if (kind == Kind::Render) {
      waiting_render_ = true;
      render_.arm();  // the next block provokes it; the main loop goes on until the reset
      return;
    }
    crashtest::provoke(kind);
    nofault_ = true;
    nofault_kind_ = kind;
    crashtest::marker_write(g_marker, 0, seq_);
    ctx.redraw();
  }

  CrashScreen screen_;
  RenderFault render_;
  bool pending_ = false;
  Kind kind_ = Kind::Nmi;
  std::uint64_t fire_at_ = 0;  // 0: not scheduled yet (the first update pass after arm() sets it)
  bool waiting_render_ = false;
  bool nofault_ = false;
  Kind nofault_kind_ = Kind::Nmi;
  bool prev_ = false;
  Kind prev_kind_ = Kind::Nmi;
  std::uint32_t seq_ = 0;
};

CrashApp& instance() noexcept {
  static CrashApp app;
  return app;
}

// ---- The console command --------------------------------------------------------------------------------------------------------------------

void pad_name(app::DiagOut& out, const char* name) noexcept {
  std::size_t n = 0;
  while (name[n] != '\0') {
    ++n;
  }
  out.text(name);
  for (; n < 8u; ++n) {
    out.chr(' ');
  }
}

void list_faults(app::DiagOut& out) noexcept {
  out.text("CRASH TEST (session only). fault <kind> confirm arms one; the unit resets 1.5 s later.\n");
  for (const crashtest::FaultInfo& f : crashtest::kFaults) {
    pad_name(out, f.name);
    out.text(f.what).nl();
  }
  out.text("fault <kind> says what `crash` must show; fault cancel disarms. Then: crash, stage, log.\n");
}

void fault_command(const app::DiagArgs& args, app::DiagOut& out) noexcept {
  CrashApp& a = instance();
  const char* const word = args.arg(1);
  if (word[0] == '\0') {
    list_faults(out);
    return;
  }
  if (same(word, "cancel")) {
    if (a.pending()) {
      a.cancel();
      out.text("cancelled\n");
    } else {
      out.text("nothing was armed\n");
    }
    return;
  }
  const crashtest::FaultInfo* found = nullptr;
  Kind kind = Kind::Nmi;
  for (std::size_t i = 0; i < crashtest::kKindCount; ++i) {
    if (same(word, crashtest::kFaults[i].name)) {
      found = &crashtest::kFaults[i];
      kind = static_cast<Kind>(i);
    }
  }
  if (found == nullptr) {
    out.text("unknown fault: ").text(word).text(" (fault lists them)\n");
    return;
  }
  if (!same(args.arg(2), kConfirm)) {
    out.text(found->name).text(": ").text(found->what).nl();
    out.text("after the reset, crash shows: ").text(found->expect).nl();
    out.text("nothing done: fault ").text(found->name).text(" confirm arms it\n");
    return;
  }
  if (a.pending()) {
    out.text("already armed: ").text(crashtest::info(a.pending_kind()).name).text(" (fault cancel disarms it)\n");
    return;
  }
  a.arm(kind);
  out.text("armed: ").text(found->name).text(". In 1.5 s the unit faults and resets; the console drops and returns after the reboot.\n");
  out.text("then: crash (expect: ").text(found->expect).text("), stage, log 20; the LED: 6 flashes, twice, then the heartbeat\n");
  out.text("if state shows nofault=").text(found->name).text(", the core took no fault\n");
}

constexpr app::DiagCommand kCommands[] = {
    {"fault", "fault [<kind> [confirm]|cancel]: a fault on purpose, SESSION ONLY", &fault_command},
};

std::size_t CrashApp::diag_commands(const app::DiagCommand*& rows) const noexcept {
  rows = kCommands;
  return sizeof kCommands / sizeof kCommands[0];
}

// ---- The screen's drawing ---------------------------------------------------------------------------------------------------------------

// `text` after `n` characters of `line` (at most 63 in all: the callers' strings are short and fixed).
void append(char (&line)[64], std::size_t& n, const char* text) noexcept {
  for (; *text != '\0' && n < sizeof line - 1u; ++text) {
    line[n++] = *text;
  }
}

void CrashScreen::draw(app::Context& ctx, ui::Renderer& r, const Rect& area) noexcept {
  const CrashApp& a = instance();
  const int x = 16;
  int y = area.y + 10;
  r.text(x, y, "CRASH TEST: SESSION ONLY", Color::Accent, Face::Display);
  y += 40;
  r.text(x, y, "Not a release: it provokes faults on purpose and the unit resets itself.", Color::Text, Face::Label);
  y += 20;
  r.text(x, y, "The pads and buttons do nothing. Over USB: fault <kind> confirm.", Color::TextDim, Face::Label);
  y += 20;
  r.text(x, y, "Then: crash, stage, log. The LED: 6 flashes twice, then the heartbeat.", Color::TextDim, Face::Label);
  y += 30;
  for (const crashtest::FaultInfo& f : crashtest::kFaults) {
    r.text(x, y, f.name, Color::Text, Face::Label);
    r.text(x + 90, y, f.what, Color::TextDim, Face::Label);
    y += 18;
  }
  y += 10;
  char line[64];
  if (a.pending()) {
    const std::uint64_t now = ctx.uptime_ms();
    const std::uint32_t left = a.fire_at() == 0u ? kFireDelayMs : (a.fire_at() > now ? static_cast<std::uint32_t>(a.fire_at() - now) : 0u);
    std::size_t n = 0;
    append(line, n, "ARMED: ");
    append(line, n, crashtest::info(a.pending_kind()).name);
    append(line, n, " FIRES IN ");
    line[n++] = static_cast<char>('0' + (left / 1000u) % 10u);
    line[n++] = '.';
    line[n++] = static_cast<char>('0' + (left / 100u) % 10u);
    line[n++] = 'S';
    line[n] = '\0';
    r.text(x, y, line, Color::Accent, Face::Label);
  } else if (a.has_nofault()) {
    r.text(x, y, "NO FAULT WAS TAKEN: ", Color::Accent, Face::Label);
    r.text(x + 180, y, crashtest::info(a.nofault_kind()).name, Color::Text, Face::Label);
  } else if (a.has_prev()) {
    r.text(x, y, "LAST RUN PROVOKED: ", Color::Text, Face::Label);
    r.text(x + 180, y, crashtest::info(a.prev_kind()).name, Color::Play, Face::Label);
  } else {
    r.text(x, y, "NOTHING ARMED", Color::TextDim, Face::Label);
  }
}

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept { return instance(); }
