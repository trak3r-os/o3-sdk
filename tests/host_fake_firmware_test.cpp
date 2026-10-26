// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fake firmware sink (platform/host/fake_firmware.h, OS-FWCONFIG): the list, the script of a check, the refusals, install() that only records, and an install that the
// bootloader never takes. The App's tests and the simulator's dialogs stand on it, so what it says is pinned here.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/hal/firmware.h>
#include <o3sdk/platform/host/fake_firmware.h>

#include <cstring>
#include <string>
#include <vector>

using namespace o3sdk;
using namespace o3sdk::platform::host;
using hal::firmware::CheckStatus;
using hal::firmware::Refusal;
using State = CheckStatus::State;

namespace {

CheckStatus run_until_done(hal::firmware::FirmwareSink& sink, unsigned* polls = nullptr) {
  CheckStatus s;
  unsigned n = 0;
  do {
    s = sink.poll_check();
    ++n;
  } while (s.state == State::Running && n < 100u);
  if (polls != nullptr) {
    *polls = n;
  }
  return s;
}

}  // namespace

TEST_CASE("fake firmware sink: the list is the fixture in order, the first file is the reset target") {
  FakeFirmwareSink fake;
  hal::firmware::FirmwareSink& sink = fake;
  CHECK(sink.available());
  fake.present = false;
  CHECK(!sink.available());
  fake.add("PolyendTracker_1.9.0.ptf", 1048576);
  fake.add("community-0.9.ptf", 800000);
  {
    CHECK(sink.list_begin() == Refusal::None);
    hal::firmware::File f{};
    std::vector<std::string> names;
    std::vector<bool> firsts;
    while (sink.list_next(f)) {
      names.push_back(f.name);
      firsts.push_back(f.reset_target);
    }
    sink.list_end();
    CHECK(names == std::vector<std::string>{"PolyendTracker_1.9.0.ptf", "community-0.9.ptf"});
    CHECK(firsts == std::vector<bool>{true, false});
  }
  fake.clear_files();
  CHECK(sink.list_begin() == Refusal::None);
  hal::firmware::File f{};
  CHECK(!sink.list_next(f));  // an empty folder
  sink.list_end();
  fake.list_refusal = Refusal::OnComputer;
  CHECK(sink.list_begin() == Refusal::OnComputer);
  CHECK(fake.count("list_begin") == 3u);
}

TEST_CASE("fake firmware sink: a scripted check runs in slices with progress and ends as the script says") {
  FakeFirmwareSink fake;
  hal::firmware::FirmwareSink& sink = fake;
  fake.add("good.ptf", 900);
  fake.add("bad.ptf", 600);
  FakeFirmwareSink::Script bad;
  bad.slices = 2;
  bad.verdict = State::Failed;
  bad.offset = 0x40D;
  bad.fault = "flash configuration field is not the required one";
  fake.script("bad.ptf", bad);
  // the default script
  CHECK(sink.begin_check("good.ptf") == Refusal::None);
  CHECK(sink.install() == Refusal::Busy);  // not before the verdict: the check is running, and install() changes nothing
  CHECK(sink.begin_check("bad.ptf") == Refusal::Busy);  // one job at a time, like the service
  unsigned polls = 0;
  std::uint32_t last = 0;
  CheckStatus s = sink.poll_check();
  CHECK(s.state == State::Running);
  CHECK(s.total == 900u);
  CHECK(s.done > 0u);
  CHECK(!s.installing);
  last = s.done;
  s = run_until_done(sink, &polls);
  CHECK(polls == FakeFirmwareSink::kDefaultSlices - 1u);
  CHECK(s.state == State::Checked);
  CHECK(s.image == 0x3F2A0u);
  CHECK(s.crc == 0x5EA1B0C4u);
  CHECK(s.fault == nullptr);
  CHECK(last < 900u);
  CHECK(sink.poll_check().state == State::Checked);  // stays until something is done with it
  // a failing one
  CHECK(sink.begin_check("bad.ptf") == Refusal::None);
  s = run_until_done(sink);
  CHECK(s.state == State::Failed);
  CHECK(s.offset == 0x40Du);
  NEED(s.fault != nullptr);
  CHECK(std::strcmp(s.fault, "flash configuration field is not the required one") == 0);
  CHECK(sink.install() == Refusal::NotChecked);
  CHECK(fake.installs == 0u);
  // refusals, a missing file
  CHECK(sink.begin_check("missing.ptf") == Refusal::NotFound);
  fake.check_refusal = Refusal::StorageBusy;
  CHECK(sink.begin_check("good.ptf") == Refusal::StorageBusy);
  fake.check_refusal = Refusal::None;
  // cancel
  CHECK(sink.begin_check("good.ptf") == Refusal::None);
  (void)sink.poll_check();
  sink.cancel();
  CHECK(sink.poll_check().state == State::Idle);
  CHECK(fake.last_name == "good.ptf");
}

TEST_CASE("fake firmware sink: install() only records the call, and what follows is scripted") {
  FakeFirmwareSink fake;
  hal::firmware::FirmwareSink& sink = fake;
  fake.add("good.ptf", 900);
  fake.add("stuck.ptf", 900);
  fake.add("late.ptf", 900);
  FakeFirmwareSink::Script late;
  late.install_end = State::DidNotStart;
  late.install_slices = 2;
  fake.script("late.ptf", late);
  FakeFirmwareSink::Script broke;
  broke.install_end = State::Failed;
  broke.install_slices = 2;
  broke.install_fault = "i/o error";
  fake.script("stuck.ptf", broke);

  CHECK(sink.begin_check("late.ptf") == Refusal::None);
  CHECK(run_until_done(sink).state == State::Checked);
  CHECK(sink.install() == Refusal::None);
  CHECK(sink.install() == Refusal::Busy);  // the job runs: nothing changes
  CHECK(fake.installs == 1u);
  CheckStatus s = sink.poll_check();
  CHECK(s.state == State::Running);
  CHECK(s.installing);
  s = run_until_done(sink);
  CHECK(s.state == State::DidNotStart);
  CHECK(s.installing);
  CHECK(sink.begin_check("good.ptf") == Refusal::None);  // free again

  CHECK(run_until_done(sink).state == State::Checked);
  CHECK(sink.begin_check("stuck.ptf") == Refusal::None);  // a waiting result is replaced, as the service does
  CHECK(run_until_done(sink).state == State::Checked);
  CHECK(sink.install() == Refusal::None);
  s = run_until_done(sink);
  CHECK(s.state == State::Failed);
  NEED(s.fault != nullptr);
  CHECK(std::strcmp(s.fault, "i/o error") == 0);
  CHECK(fake.installs == 2u);

  // a refusal that is scripted, then an install that is taken and stays in the wait for the bootloader, as on the unit where the chip has reset; it cannot be cancelled
  CHECK(sink.begin_check("good.ptf") == Refusal::None);
  CHECK(run_until_done(sink).state == State::Checked);
  fake.install_refusal = Refusal::OnComputer;
  CHECK(sink.install() == Refusal::OnComputer);
  CHECK(fake.installs == 2u);
  fake.install_refusal = Refusal::None;
  CHECK(sink.install() == Refusal::None);
  CHECK(fake.installs == 3u);
  for (unsigned i = 0; i < 10; ++i) {
    s = sink.poll_check();
    CHECK(s.state == State::Running);
    CHECK(s.installing);
    CHECK(sink.install() == Refusal::Busy);  // a second INSTALL never turns the running job into a failure
    CHECK(sink.begin_check("late.ptf") == Refusal::Busy);
  }
  sink.cancel();
  s = sink.poll_check();
  CHECK(s.state == State::Running);
  CHECK(s.installing);
  CHECK(fake.installs == 3u);
}

TEST_CASE("fake firmware sink: a check that lapses says Failed and cannot be installed") {
  FakeFirmwareSink fake;
  hal::firmware::FirmwareSink& sink = fake;
  fake.add("good.ptf", 900);
  CHECK(sink.begin_check("good.ptf") == Refusal::None);
  CHECK(run_until_done(sink).state == State::Checked);
  fake.lapse();
  const CheckStatus s = sink.poll_check();
  CHECK(s.state == State::Failed);
  NEED(s.fault != nullptr);
  CHECK(sink.install() == Refusal::NotChecked);
  CHECK(fake.installs == 0u);
}
