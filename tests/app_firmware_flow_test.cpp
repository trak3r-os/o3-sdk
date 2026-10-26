// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware switcher's flow (kit/app firmware_flow.h, OS-FWCONFIG), on the fake sink (platform/host/fake_firmware.h). Class N of
// docs/M2B2-PLAN.md: the cases of track3r-os's tests/firmware_update_test.cpp that concern the flow (the list copied in one pass; question 1 and
// question 2, each answered by F1 alone and taken back by any other control or by the 30 s; the check's progress; every refusal; cancel() on every
// way out; the install reaching the port exactly once; nothing asked of the sink before the second question is answered), against the kit's
// FirmwareView and free functions. The tracker's App harness is not ported: the screen around the flow is app_system_config_test.cpp.
#include <doctest/doctest.h>

#include <o3sdk/app/firmware_flow.h>
#include <o3sdk/app/firmware_view.h>
#include <o3sdk/app/input.h>
#include <o3sdk/platform/host/fake_firmware.h>

#include <cstring>
#include <string>

using namespace o3sdk;
using app::Button;
using app::FirmwareView;
using app::Input;
using hal::firmware::Refusal;
using platform::host::FakeFirmwareSink;
using Phase = FirmwareView::Phase;
using State = hal::firmware::CheckStatus::State;

namespace {

Input press(Button b, bool shift = false) { return Input{Input::Type::Press, core::InputKind::Button, static_cast<std::uint8_t>(b), 1, shift, 0}; }
Input release(Button b) { return Input{Input::Type::Release, core::InputKind::Button, static_cast<std::uint8_t>(b), 0, false, 0}; }
Input pad_press(unsigned pad) { return Input{Input::Type::Press, core::InputKind::Pad, static_cast<std::uint8_t>(pad), 127, false, 0}; }
Input pad_release(unsigned pad) { return Input{Input::Type::Release, core::InputKind::Pad, static_cast<std::uint8_t>(pad), 0, false, 0}; }
Input turn(int detents) { return Input{Input::Type::Turn, core::InputKind::Encoder, 0, static_cast<std::int16_t>(detents), false, 0}; }

struct Rig {
  FakeFirmwareSink sink;
  FirmwareView v;
  core::Tick now = 5000;
  const char* said = nullptr;

  Rig() {
    sink.add("TRACKER_1.PTF", 812345);
    sink.add("TRACKER_2.PTF", 799000);
    sink.add("OLD.PTF", 640000);
  }
  bool open(const char* blocker = nullptr) { return app::firmware_open(v, &sink, blocker, said); }
  bool go(const char* blocker = nullptr) { return app::firmware_go(v, &sink, blocker); }
  bool back() { return app::firmware_back(v, &sink); }
  // One pass of the main loop: `ms` later, the page shown.
  bool pass(unsigned ms = 16, bool shown = true) {
    now += core::ticks_from_ms(ms);
    return app::firmware_poll(v, sink, now, shown);
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      (void)pass();
    }
  }
  // The path to question 2: open (unless the list is up), F1 (question 1), F1 (the check), passes until the verdict.
  void to_question_two() {
    if (!v.open()) {
      REQUIRE(open());
    }
    REQUIRE(go());
    REQUIRE(go());
    passes(FakeFirmwareSink::kDefaultSlices + 1);
  }
};

}  // namespace

TEST_CASE("firmware flow: the list is read in one pass, in order, with the reset target; a second open and a missing path open nothing") {
  Rig r;
  CHECK(r.open());
  CHECK(r.said == nullptr);
  CHECK(r.v.phase == Phase::List);
  CHECK(r.v.n == 3);
  CHECK(std::string(r.v.files[0].name) == "TRACKER_1.PTF");
  CHECK(r.v.files[0].reset_target);
  CHECK_FALSE(r.v.files[1].reset_target);
  CHECK(r.v.files[2].size == 640000u);
  CHECK(r.v.cursor == 0);
  CHECK_FALSE(r.v.more);
  CHECK(r.sink.count("list_begin") == 1u);  // one pass: begin, the entries, end
  CHECK(r.sink.count("list_end") == 1u);
  CHECK(r.sink.count("begin_check TRACKER_1.PTF") == 0u);  // nothing was asked of the check
  CHECK_FALSE(r.open());                                   // already open: nothing changes
  CHECK(r.sink.count("list_begin") == 1u);

  FirmwareView none;
  CHECK_FALSE(app::firmware_open(none, nullptr, nullptr, r.said));  // a build with no port
  CHECK(r.said == nullptr);
  r.sink.present = false;
  CHECK_FALSE(app::firmware_open(none, &r.sink, nullptr, r.said));  // a port that says it is not there
  CHECK(r.said == nullptr);
  CHECK(r.sink.count("list_begin") == 1u);
}

TEST_CASE("firmware flow: an empty folder says so, F1 has nothing to choose, BACK closes it and cancels; more files than the list holds are flagged") {
  Rig r;
  r.sink.clear_files();
  REQUIRE(r.open());
  CHECK(r.v.n == 0);
  CHECK_FALSE(r.go());
  CHECK(r.v.phase == Phase::List);
  CHECK(r.sink.count("begin_check ") == 0u);
  const unsigned cancels = r.sink.count("cancel");
  CHECK(r.back());
  CHECK(r.v.phase == Phase::Closed);
  CHECK(r.sink.count("cancel") == cancels + 1u);
  CHECK_FALSE(r.back());  // closed: nothing to step back from

  for (unsigned i = 0; i < FirmwareView::kMax; ++i) {
    r.sink.add("F" + std::to_string(i) + ".PTF", 1000);
  }
  REQUIRE(r.open());
  CHECK(r.v.n == FirmwareView::kMax);
  CHECK_FALSE(r.v.more);  // exactly kMax files: none beyond
  r.v.close();
  r.sink.add("ONE_MORE.PTF", 1000);
  REQUIRE(r.open());
  CHECK(r.v.n == FirmwareView::kMax);
  CHECK(r.v.more);  // OS-BUG-FWLIST: the 13th file is counted, not listed
}

TEST_CASE("firmware flow: every refusal says why and starts nothing: the app's blocker, the port's own") {
  Rig r;
  CHECK_FALSE(r.open("A RECORDING IS RUNNING"));
  CHECK(std::string(r.said) == "A RECORDING IS RUNNING");
  CHECK(r.sink.count("list_begin") == 0u);  // the blocker is asked before the card is touched
  CHECK(r.v.phase == Phase::Closed);

  struct Row {
    Refusal why;
    const char* text;
  };
  const Row rows[] = {{Refusal::OnComputer, "THE CARD IS ON THE COMPUTER"},
                      {Refusal::StorageBusy, "THE CARD IS BUSY: TRY AGAIN"},
                      {Refusal::Busy, "ANOTHER UPDATE STEP IS RUNNING"},
                      {Refusal::NoCard, "NO CARD"},
                      {Refusal::NotWritable, "THE CARD CANNOT BE USED FOR AN UPDATE"},
                      {Refusal::NoFolder, "NO FIRMWARE FOLDER ON THE CARD"},
                      {Refusal::Io, "CARD READ ERROR"}};
  for (const Row& row : rows) {
    CAPTURE(row.text);
    r.sink.list_refusal = row.why;
    CHECK_FALSE(r.open());
    CHECK(std::string(r.said) == row.text);
    CHECK(r.v.phase == Phase::Closed);
    CHECK(r.v.n == 0);
  }
  r.sink.list_refusal = Refusal::None;
  REQUIRE(r.open());

  // A refused check starts nothing: the list again, with the reason on its line.
  REQUIRE(r.go());
  CHECK(r.v.phase == Phase::AskUpdate);
  r.sink.check_refusal = Refusal::NotFound;
  CHECK(r.go());
  CHECK(r.v.phase == Phase::List);
  CHECK(r.v.bad);
  CHECK(r.v.msg == "THE FILE IS NOT ON THE CARD ANY MORE");
  CHECK(r.sink.count("install") == 0u);
}

TEST_CASE("firmware flow: question 1 names the file under the cursor and F1 YES starts the check on exactly that file") {
  Rig r;
  REQUIRE(r.open());
  r.v.cursor = 1;
  REQUIRE(r.go());
  CHECK(r.v.phase == Phase::AskUpdate);
  CHECK(std::string(r.v.chosen()) == "TRACKER_2.PTF");
  CHECK(r.sink.count("begin_check TRACKER_2.PTF") == 0u);  // asking started nothing
  CHECK(r.go());
  CHECK(r.v.phase == Phase::Checking);
  CHECK(r.sink.last_name == "TRACKER_2.PTF");
  CHECK(r.sink.count("begin_check TRACKER_2.PTF") == 1u);
  CHECK(r.sink.count("install") == 0u);
}

TEST_CASE("firmware flow: question 1 is taken back by any other control, not by F1 alone, Shift or F8") {
  Rig r;
  REQUIRE(r.open());
  CHECK_FALSE(app::firmware_question_taken_back(r.v, press(Button::Up)));  // no question is up
  REQUIRE(r.go());
  REQUIRE(r.v.asking());

  CHECK_FALSE(app::firmware_question_taken_back(r.v, press(Button::F1)));            // the answer
  CHECK_FALSE(app::firmware_question_taken_back(r.v, press(Button::F8)));            // F8's own binding steps back
  CHECK_FALSE(app::firmware_question_taken_back(r.v, press(Button::F8, true)));
  CHECK_FALSE(app::firmware_question_taken_back(r.v, press(Button::Shift)));          // Shift alone is not a control yet
  CHECK_FALSE(app::firmware_question_taken_back(r.v, release(Button::Params)));       // a release takes nothing back
  CHECK_FALSE(app::firmware_question_taken_back(r.v, pad_release(3)));
  CHECK(app::firmware_question_taken_back(r.v, press(Button::F1, true)));             // Shift + F1 is bound to nothing here
  CHECK(app::firmware_question_taken_back(r.v, press(Button::F2)));
  CHECK(app::firmware_question_taken_back(r.v, press(Button::Params)));
  CHECK(app::firmware_question_taken_back(r.v, press(Button::Down)));
  CHECK(app::firmware_question_taken_back(r.v, pad_press(3)));
  CHECK(app::firmware_question_taken_back(r.v, turn(1)));
  CHECK(app::firmware_question_taken_back(r.v, turn(-2)));

  const unsigned begins = r.sink.count("begin_check TRACKER_1.PTF");
  app::firmware_question_drop(r.v, &r.sink);
  CHECK(r.v.phase == Phase::List);
  CHECK(r.sink.count("begin_check TRACKER_1.PTF") == begins);  // nothing was started
  CHECK_FALSE(r.v.bad);

  // Question 2 too, and then the good check is let go.
  r.to_question_two();
  REQUIRE(r.v.phase == Phase::AskInstall);
  CHECK(app::firmware_question_taken_back(r.v, press(Button::Params)));
  const unsigned cancels = r.sink.count("cancel");
  app::firmware_question_drop(r.v, &r.sink);
  CHECK(r.v.phase == Phase::List);
  CHECK(r.sink.count("cancel") == cancels + 1u);
  CHECK(r.sink.count("install") == 0u);
}

TEST_CASE("firmware flow: the check runs a slice a pass with progress, BACK cancels it, a verdict of Checked asks question 2 with the length and the CRC") {
  Rig r;
  REQUIRE(r.open());
  REQUIRE(r.go());
  REQUIRE(r.go());
  REQUIRE(r.v.phase == Phase::Checking);
  CHECK(r.pass());  // slice 1 of 3
  CHECK(r.v.phase == Phase::Checking);
  CHECK(r.v.total == 812345u);
  CHECK(r.v.done > 0u);
  CHECK(r.v.done < r.v.total);
  const std::uint32_t first = r.v.done;
  CHECK(r.pass());
  CHECK(r.v.done > first);

  // BACK (CANCEL) drops it: the list again, the sink cancelled, no verdict.
  const unsigned cancels = r.sink.count("cancel");
  CHECK(r.back());
  CHECK(r.v.phase == Phase::List);
  CHECK(r.sink.count("cancel") == cancels + 1u);
  CHECK_FALSE(r.pass());  // the list asks nothing of the sink

  REQUIRE(r.go());
  REQUIRE(r.go());
  r.passes(FakeFirmwareSink::kDefaultSlices);
  CHECK(r.v.phase == Phase::AskInstall);
  CHECK(r.v.image == 0x3F2A0u);
  CHECK(r.v.crc == 0x5EA1B0C4u);
  CHECK(r.sink.count("install") == 0u);  // nothing is written before the second F1
}

TEST_CASE("firmware flow: a failed check returns to the list with the fault and the offset, upper case; nothing is written") {
  Rig r;
  FakeFirmwareSink::Script s;
  s.verdict = State::Failed;
  s.offset = 4096;
  s.fault = "flash configuration field is not the required one";
  r.sink.script("TRACKER_1.PTF", s);
  REQUIRE(r.open());
  REQUIRE(r.go());
  REQUIRE(r.go());
  r.passes(FakeFirmwareSink::kDefaultSlices);
  CHECK(r.v.phase == Phase::List);
  CHECK(r.v.bad);
  CHECK(r.v.msg == "NOT A VALID IMAGE: FLASH CONFIGURATION FIELD IS NOT THE REQUIRED ONE AT 4096");
  CHECK(r.sink.count("install") == 0u);
  CHECK(r.sink.count("cancel") >= 1u);  // the failed job is let go
}

TEST_CASE("firmware flow: question 2 is answered by F1 alone; INSTALL reaches the port exactly once and nothing leaves the hand-over") {
  Rig r;
  r.to_question_two();
  REQUIRE(r.v.phase == Phase::AskInstall);
  CHECK(r.go());
  CHECK(r.v.phase == Phase::Installing);
  CHECK(r.sink.installs == 1u);
  CHECK(r.sink.count("install") == 1u);
  CHECK_FALSE(r.go());   // F1 in the hand-over: nothing
  CHECK_FALSE(r.back());  // nothing cancels it
  CHECK(r.sink.installs == 1u);
  const unsigned cancels = r.sink.count("cancel");
  CHECK_FALSE(app::firmware_abandon(r.v, &r.sink));  // a card change does not touch it
  CHECK(r.sink.count("cancel") == cancels);
  CHECK(r.v.phase == Phase::Installing);
  r.passes(10);  // the fake never says the bootloader took over, as on the unit the chip has not reset yet
  CHECK(r.v.phase == Phase::Installing);

  // The page not shown past INSTALL: reported as changed, never closed or cancelled (the caller puts it back).
  CHECK(r.pass(16, false));
  CHECK(r.v.phase == Phase::Installing);
  CHECK(r.sink.count("cancel") == cancels);
}

TEST_CASE("firmware flow: an install the bootloader never takes, or that fails before the pin, comes back to the list with the reason") {
  {
    Rig r;
    FakeFirmwareSink::Script s;
    s.install_end = State::DidNotStart;
    r.sink.script("TRACKER_1.PTF", s);
    r.to_question_two();
    REQUIRE(r.go());
    r.passes(FakeFirmwareSink::kDefaultSlices + 1);
    CHECK(r.v.phase == Phase::List);
    CHECK(r.v.bad);
    CHECK(r.v.msg == "UPDATE DID NOT START");
  }
  {
    Rig r;
    FakeFirmwareSink::Script s;
    s.install_end = State::Failed;
    s.install_fault = "i/o error";
    r.sink.script("TRACKER_1.PTF", s);
    r.to_question_two();
    REQUIRE(r.go());
    r.passes(FakeFirmwareSink::kDefaultSlices + 1);
    CHECK(r.v.phase == Phase::List);
    CHECK(r.v.msg == "UPDATE FAILED: I/O ERROR");
  }
  {
    Rig r;  // install() itself refused: nothing was touched, the good check is let go
    r.to_question_two();
    r.sink.install_refusal = Refusal::OnComputer;
    CHECK(r.go());
    CHECK(r.v.phase == Phase::List);
    CHECK(r.v.msg == "THE CARD IS ON THE COMPUTER");
    CHECK(r.sink.installs == 0u);
  }
}

TEST_CASE("firmware flow: question 2 lapses after 30 s, when the service lets the check go, or on BACK; install() is never called") {
  {
    Rig r;
    r.to_question_two();
    REQUIRE(r.v.phase == Phase::AskInstall);
    CHECK_FALSE(r.pass(10000));  // 10 s later the good check is still good
    CHECK(r.v.phase == Phase::AskInstall);
    CHECK(r.pass(21000));  // past 30 s
    CHECK(r.v.phase == Phase::List);
    CHECK(r.v.bad);
    CHECK(r.sink.count("install") == 0u);
  }
  {
    Rig r;
    r.to_question_two();
    r.sink.lapse();  // the card went to a computer: the service says Failed
    CHECK(r.pass());
    CHECK(r.v.phase == Phase::List);
    CHECK(r.v.msg == "CHECK AGAIN");
    CHECK(r.sink.count("install") == 0u);
  }
  {
    Rig r;
    r.to_question_two();
    const unsigned cancels = r.sink.count("cancel");
    CHECK(r.back());
    CHECK(r.v.phase == Phase::List);
    CHECK(r.sink.count("cancel") == cancels + 1u);
  }
}

TEST_CASE("firmware flow: the refusals of the opening hold at INSTALL: a blocker that came while the question waited cancels the check") {
  Rig r;
  r.to_question_two();
  REQUIRE(r.v.phase == Phase::AskInstall);
  const unsigned cancels = r.sink.count("cancel");
  CHECK(r.go("A RECORDING IS RUNNING"));
  CHECK(r.v.phase == Phase::List);
  CHECK(r.v.bad);
  CHECK(r.v.msg == "A RECORDING IS RUNNING");
  CHECK(r.sink.count("cancel") == cancels + 1u);
  CHECK(r.sink.count("install") == 0u);
}

TEST_CASE("firmware flow: cancel() on every way out: abandon in each phase, the page lost before INSTALL; an install in the hand-over is not touched") {
  {
    Rig r;
    REQUIRE(r.open());
    CHECK(app::firmware_abandon(r.v, &r.sink));  // the list
    CHECK(r.v.phase == Phase::Closed);
    CHECK(r.sink.count("cancel") == 1u);
    CHECK_FALSE(app::firmware_abandon(r.v, &r.sink));  // closed: nothing
    CHECK(r.sink.count("cancel") == 1u);
  }
  {
    Rig r;  // a running check holds the card
    REQUIRE(r.open());
    REQUIRE(r.go());
    REQUIRE(r.go());
    r.passes(1);
    CHECK(app::firmware_abandon(r.v, &r.sink));
    CHECK(r.v.phase == Phase::Closed);
    CHECK(r.sink.poll_check().state == State::Idle);
  }
  {
    Rig r;  // a good check that waits keeps the key injection refused until it lapses
    r.to_question_two();
    CHECK(app::firmware_abandon(r.v, &r.sink));
    CHECK(r.v.phase == Phase::Closed);
    CHECK(r.v.n == 0);
    CHECK(r.sink.poll_check().state == State::Idle);
  }
  {
    Rig r;  // the page is lost to a path that is not the keys: the pass cancels and closes
    r.to_question_two();
    const unsigned cancels = r.sink.count("cancel");
    CHECK(r.pass(16, false));
    CHECK(r.v.phase == Phase::Closed);
    CHECK(r.sink.count("cancel") == cancels + 1u);
  }
}

TEST_CASE("firmware flow: the phase names the console's state line uses") {
  CHECK(std::string(app::firmware_phase_name(Phase::Closed)) == "closed");
  CHECK(std::string(app::firmware_phase_name(Phase::List)) == "list");
  CHECK(std::string(app::firmware_phase_name(Phase::AskUpdate)) == "ask_update");
  CHECK(std::string(app::firmware_phase_name(Phase::Checking)) == "checking");
  CHECK(std::string(app::firmware_phase_name(Phase::AskInstall)) == "ask_install");
  CHECK(std::string(app::firmware_phase_name(Phase::Installing)) == "installing");
}
