// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U2a-1: the running mode's hand-over and eject (platform/mk66/usb/disk_share.h) over the real owner (CardSwap), the real bridge and the real engine on a FAT32 image, with a simulated layout driver.
// The rules under test (spec 4.2, 6.1, 7, hazards 1, 4, 10, 11): the owner flips LAST and only into a confirmed Disk layout; every failure before the flip leaves the owner Tracker and puts the previous
// layout back; the Tracker never writes the card while the computer has it and never takes it back without an eject (the bus going away is not one); an eject stops the MSC first, then
// remounts, then restores the layout; an eject in the middle of a transfer cuts no block and leaves a mountable card. Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_fakes.h"
#include "mk66_storage_need.h"

#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/usb/disk_share.h>
#include <o3sdk/platform/mk66/save/fat_storage_writer.h>
#include <o3sdk/platform/mk66/usb/msc_bridge.h>
#include <o3sdk/usb/msc.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace st = o3sdk::platform::mk66::storage;
namespace msc = o3sdk::usb::msc;
namespace ou = o3sdk::usb;
using o3sdk::platform::mk66::DiskShare;
using o3sdk::platform::mk66::usb::MscBridge;
using fakes::Fat32Builder;
using fakes::Medium;
using hs = o3sdk::hal::storage::Status;
using Kind = DiskShare::Event::Kind;

namespace {

bool g_job = false;  // a registered claim: a load, an export or a firmware upload holds the card
bool job_claim() noexcept { return g_job; }

constexpr std::uint32_t kLba = 2048;
constexpr std::uint32_t kLayoutChangeMs = 820;  // the detach (120 + 700 ms) of usb0::service()

class Rig final : public DiskShare::Port {
 public:
  Medium medium{20'000};
  st::FatStorage fat;
  st::CardSwap swap;
  MscBridge bridge;
  msc::Engine eng;
  DiskShare share;
  std::vector<std::string> order;  // the calls that matter, in the order they were made
  DiskShare::Layout cur{ou::Mode::Audio, ou::Profile::P10, true};
  bool changing = false;
  bool host_configures = true;     // a computer is plugged in and configures the Disk device
  bool attach_falls_back = false;  // the Disk descriptors did not build: the driver re-attaches as STANDARD
  bool refuse_layout = false;      // request_layout() says no (a change in progress, the driver stopped)
  bool refuse_return = false;      // ... only for the way back
  bool save_due = false;
  bool latched = false;            // the power latch: the shutdown has the card (the image's g_power_latched)
  bool held = false;               // the save service's hold, as the machine last set it
  unsigned take_backs = 0;
  unsigned layout_requests = 0;
  std::uint32_t now = 0;

  Rig() : fat(medium), swap(fat, medium), bridge(medium, swap), eng(bridge), share(*this) {
    Fat32Builder b(medium);
    b.add_file(Fat32Builder::kRootCluster, "README.TXT", std::vector<std::uint8_t>(700, 0x5A));
    g_job = false;
    (void)swap.claims().add(&job_claim);
    (void)swap.reacquire();
    swap.begin();
  }

  // Port
  st::CardOwner owner() const noexcept override { return swap.owner(); }
  bool card_busy() const noexcept override { return swap.card_busy(); }
  bool save_pending() const noexcept override { return save_due; }
  DiskShare::Layout layout() const noexcept override { return cur; }
  bool layout_changing() const noexcept override { return changing; }
  // The three usb0 calls with their real acceptance rules (usb0.cpp): disk_layout_enter refuses when DISK is presented, disk_layout_leave unless it is, request_layout (an ordinary change) when it is.
  bool enter_disk_layout(const DiskShare::Layout& to) noexcept override { return queue(to, cur.mode != ou::Mode::Disk && !refuse_layout); }
  bool leave_disk_layout(const DiskShare::Layout& to) noexcept override { return queue(to, cur.mode == ou::Mode::Disk && !refuse_return); }
  bool change_layout(const DiskShare::Layout& to) noexcept override { return queue(to, cur.mode != ou::Mode::Disk && !refuse_return); }
  bool configured() const noexcept override { return host_configures && !changing && cur.mode == ou::Mode::Disk; }
  st::CardSwap::HandOver hand_to_host() noexcept override {
    const st::CardSwap::HandOver r = swap.hand_to_host();
    order.push_back(r == st::CardSwap::HandOver::Ok ? "hand" : "hand:refused");
    return r;
  }
  bool disk_open(bool writable) noexcept override {
    order.push_back("open");
    bridge.set_writable(writable);
    const bool ok = bridge.open();
    eng.medium_changed();
    return ok;
  }
  void disk_close() noexcept override {
    order.push_back("close");
    bridge.close();
  }
  hs take_back() noexcept override {
    order.push_back("take_back");
    ++take_backs;
    return swap.take_back();
  }
  bool engine_busy() const noexcept override { return !eng.idle(); }
  bool take_eject_request() noexcept override { return eng.take_eject_request(); }
  void unit_eject() noexcept override {
    order.push_back("unit_eject");
    eng.unit_eject();
  }
  void pump(std::uint32_t) noexcept override {
    if (cur.mode == ou::Mode::Disk && configured()) {
      eng.service();
    }
  }
  void hold_background(bool on) noexcept override { held = on; }
  void log(const char*) noexcept override {}

  // One millisecond of the main loop: the layout driver, the pump (only when the computer has the Disk configured, like usb0::disk_pass), then the machine.
  void tick() {
    ++now;
    if (changing && now >= done_at_) {
      cur = target_;
      changing = false;
    }
    share.pass(now, latched);
  }
  void run(std::uint32_t ms) {
    for (std::uint32_t i = 0; i < ms; ++i) {
      tick();
    }
  }
  bool run_until(DiskShare::State s, std::uint32_t max_ms) {
    for (std::uint32_t i = 0; i < max_ms; ++i) {
      if (share.state() == s) {
        return true;
      }
      tick();
    }
    return share.state() == s;
  }
  int index_of(const char* what) const {
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == what) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  bool readme_reads() {
    o3sdk::hal::storage::FileHandle file{};
    std::uint8_t b[16] = {};
    std::size_t got = 0;
    return fat.mounted() && fat.open("README.TXT", file) == hs::Ok && fat.read(file, b, sizeof b, got) == hs::Ok && got == sizeof b && b[0] == 0x5A;
  }

 private:
  bool queue(const DiskShare::Layout& to, bool accepted) noexcept {
    ++layout_requests;
    if (changing || !accepted) {
      return false;
    }
    order.push_back(to.mode == ou::Mode::Disk ? "layout:disk" : "layout:back");
    target_ = to;
    if (to.mode == ou::Mode::Disk && attach_falls_back) {
      target_.mode = ou::Mode::Standard;
    }
    changing = true;
    done_at_ = now + kLayoutChangeMs;
    return true;
  }
  DiskShare::Layout target_;
  std::uint32_t done_at_ = 0;
};

void cbw(msc::Engine& e, const std::vector<std::uint8_t>& cdb, std::uint32_t hlen, bool in) {
  std::uint8_t c[msc::kCbwSize] = {};
  std::memcpy(c, "USBC", 4);
  c[4] = 0x42;
  for (unsigned i = 0; i < 4; ++i) {
    c[8 + i] = static_cast<std::uint8_t>(hlen >> (8 * i));
  }
  c[12] = in ? 0x80 : 0x00;
  c[14] = static_cast<std::uint8_t>(cdb.size());
  std::memcpy(c + 15, cdb.data(), cdb.size());
  e.out_packet(c, msc::kCbwSize);
}

// The whole way out: asked, through the layout and the configuration, the flip, Out.
bool go_out(Rig& r, bool writable = true) {
  if (r.share.share(r.now, writable) != DiskShare::Refusal::None) {
    return false;
  }
  if (!r.run_until(DiskShare::State::Out, 8000)) {
    return false;
  }
  r.eng.reset_all();  // the computer's enumeration (a bus reset / SET_CONFIGURATION): the UNIT ATTENTION of the new medium is not what these tests drive
  return true;
}

}  // namespace

TEST_CASE("disk share: the way out is layout, then configuration, then the flip, then the bridge; the owner moves last") {
  Rig r;
  CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
  CHECK_FALSE(r.share.holding());  // Quiesce: the write it waits for has to start
  CHECK(r.owner() == st::CardOwner::Tracker);
  NEED(r.run_until(DiskShare::State::Layout, 100));
  CHECK(r.share.holding());
  CHECK(r.held);  // set by the pass, after the step that entered Layout
  NEED(r.run_until(DiskShare::State::Out, 8000));
  const std::vector<std::string> want{"layout:disk", "hand", "open"};
  CHECK(r.order == want);
  CHECK(r.owner() == st::CardOwner::Host);
  CHECK(r.cur.mode == ou::Mode::Disk);
  CHECK(r.bridge.present());
  CHECK_FALSE(r.bridge.write_protected());
  CHECK(r.share.out());
  CHECK_FALSE(r.share.holding());
  CHECK_FALSE(r.held);  // Out: the gate Host holds the save service
  CHECK(r.share.poll_event().kind == Kind::Shared);
  CHECK(r.share.poll_event().kind == Kind::None);  // one-shot
  CHECK(r.swap.host_sessions() == 1u);
}

TEST_CASE("disk share: the READ ONLY policy reaches the bridge, and every refusal touches nothing") {
  {
    Rig r;
    NEED(go_out(r, false));
    CHECK(r.bridge.write_protected());
  }
  {
    Rig r;  // no card: the owner is not Tracker
    r.swap.release();
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::NoCard);
    CHECK(r.share.idle());
    CHECK(r.layout_requests == 0u);
  }
  {
    Rig r;  // a job holds the card
    g_job = true;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::Busy);
    CHECK(r.layout_requests == 0u);
    g_job = false;
    NEED(go_out(r));
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::NotIdle);  // a card that is out cannot be shared again
    CHECK(r.layout_requests == 1u);
  }
}

TEST_CASE("disk share: a write that is due is done first (2 s), then the hand-over is refused; nothing was asked of the bus") {
  {
    Rig r;
    r.save_due = true;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(500);
    CHECK(r.share.state() == DiskShare::State::Quiesce);
    CHECK_FALSE(r.held);  // (a hold here would keep the write from ever starting: it would be refused for a save it prevented)
    CHECK(r.layout_requests == 0u);
    r.save_due = false;  // the settings write finished
    NEED(r.run_until(DiskShare::State::Out, 8000));
  }
  {
    Rig r;
    r.save_due = true;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kSaveWaitMs + 2);
    CHECK(r.share.idle());
    CHECK(r.share.fail() == DiskShare::Fail::SaveBusy);
    CHECK(r.share.poll_event().kind == Kind::Failed);
    CHECK(r.layout_requests == 0u);  // no layout change, so no rollback to make
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(std::string(DiskShare::fail_text(DiskShare::Fail::SaveBusy)) == "CARD BUSY: WAIT FOR THE SAVE");
  }
}

TEST_CASE("disk share, rollback (F4): a layout that is refused, one that falls back to STANDARD, and a computer that never configures leave the owner Tracker and the layout as it was") {
  {
    Rig r;
    r.refuse_layout = true;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kSaveWaitMs);
    CHECK(r.share.idle());
    CHECK(r.share.fail() == DiskShare::Fail::Layout);
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(r.swap.host_sessions() == 0u);
    CHECK(r.cur.mode == ou::Mode::Audio);
  }
  {
    Rig r;
    r.attach_falls_back = true;  // usb0::attach(): "descriptors did not build, back to STANDARD"
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kLayoutMs + kLayoutChangeMs + 200);
    CHECK(r.share.idle());
    CHECK(r.share.fail() == DiskShare::Fail::Layout);
    CHECK(r.share.poll_event().kind == Kind::Failed);
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(r.swap.host_sessions() == 0u);
    CHECK(r.index_of("hand") < 0);
    CHECK(r.index_of("layout:back") >= 0);
    CHECK((r.cur == r.share.previous_layout()));  // Audio, profile and return as they were (Q6)
    CHECK(r.cur.mode == ou::Mode::Audio);
    CHECK_FALSE(r.share.layout_stuck());  // (the way back from a STANDARD that fell back is an ordinary change: usb0's leave refuses unless DISK is presented)
    CHECK(std::string(DiskShare::fail_text(DiskShare::Fail::Layout)) == "COULD NOT SHARE THE CARD");
  }
  {
    Rig r;
    r.host_configures = false;  // nobody plugged in (there is no VBUS sensing: this is the only way to know)
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kLayoutMs + DiskShare::kConfigureMs + kLayoutChangeMs + 200);
    CHECK(r.share.idle());
    CHECK(r.share.fail() == DiskShare::Fail::NoHost);
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(r.swap.host_sessions() == 0u);
    CHECK(r.take_backs == 0u);
    CHECK(r.index_of("hand") < 0);
    CHECK(r.index_of("open") < 0);
    CHECK((r.cur == r.share.previous_layout()));
    CHECK(r.medium.blocks_written == 0u);
  }
}

// OS-USBDISK int.14 (usbmon): a control URB of the enumeration can hang for the host's whole 5 s control timeout once (a NAK the host retries until it gives up) and the host then goes on. The wait for the
// computer must outlast that, or the unit rolls back and pulls the line under a host that was about to configure the device.
static_assert(DiskShare::kConfigureMs >= 8000u, "the wait for the computer is past a host's 5 s control timeout, with margin");
TEST_CASE("disk share: a computer that configures the Disk device 6 s into the wait (past a hung control URB) still gets the card, and one that never does ends at the new budget") {
  {
    Rig r;
    r.host_configures = false;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    NEED(r.run_until(DiskShare::State::Configure, 8000));
    r.run(6000);  // the hung URB: 5.0 to 5.5 s, then the configuration read works
    CHECK(r.share.state() == DiskShare::State::Configure);
    r.host_configures = true;
    r.run(100);
    CHECK(r.share.state() == DiskShare::State::Out);
    CHECK(r.owner() == st::CardOwner::Host);
    CHECK(r.share.fail() == DiskShare::Fail::None);
  }
  {
    Rig r;
    r.host_configures = false;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    NEED(r.run_until(DiskShare::State::Configure, 8000));
    r.run(DiskShare::kConfigureMs - 100);
    CHECK(r.share.state() == DiskShare::State::Configure);  // still waiting just before the budget
    r.run(200);
    CHECK(r.share.state() != DiskShare::State::Configure);  // rolled back at it
    r.run(kLayoutChangeMs + 100);
    CHECK(r.share.fail() == DiskShare::Fail::NoHost);
    CHECK(r.owner() == st::CardOwner::Tracker);
  }
}

TEST_CASE("disk share, rollback: a job that began while the computer was enumerating refuses the flip, and the owner stays Tracker") {
  Rig r;
  r.host_configures = false;
  CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
  NEED(r.run_until(DiskShare::State::Configure, 8000));
  g_job = true;  // an export began after the hand-over was accepted
  r.host_configures = true;
  r.run(10);
  g_job = false;
  r.run(kLayoutChangeMs + 100);
  CHECK(r.share.idle());
  CHECK(r.share.fail() == DiskShare::Fail::Busy);
  CHECK(r.owner() == st::CardOwner::Tracker);
  CHECK(r.swap.host_sessions() == 0u);
  CHECK(r.index_of("hand:refused") >= 0);
  CHECK(r.index_of("open") < 0);
  CHECK((r.cur == r.share.previous_layout()));
}

TEST_CASE("disk share, rollback: a card that does not start is given back at once, nothing was written, and the layout returns") {
  Rig r;
  r.medium.fail_start = true;
  CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
  NEED(r.run_until(DiskShare::State::Returning, 8000));
  r.medium.fail_start = false;
  r.run(kLayoutChangeMs + 100);
  CHECK(r.share.idle());
  CHECK(r.share.fail() == DiskShare::Fail::Open);
  CHECK(r.share.poll_event().kind == Kind::Failed);
  CHECK(r.owner() != st::CardOwner::Host);
  CHECK(r.take_backs == 1u);
  CHECK(r.medium.blocks_written == 0u);
  CHECK(r.index_of("close") < r.index_of("take_back"));
  CHECK((r.cur == r.share.previous_layout()));
}

TEST_CASE("disk share, rollback: a previous layout the driver will not take is retried and then given up on; the owner is Tracker and the MSC answers NOT READY") {
  Rig r;
  r.refuse_return = true;
  r.host_configures = false;
  CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
  r.run(DiskShare::kLayoutMs + DiskShare::kConfigureMs + kLayoutChangeMs + DiskShare::kReturnMs + 200);
  CHECK(r.share.idle());
  CHECK(r.share.layout_stuck());
  CHECK(r.owner() == st::CardOwner::Tracker);
  CHECK(r.cur.mode == ou::Mode::Disk);  // stuck in the Disk layout with no owner behind it: harmless (NOT READY), never the other way round
  CHECK_FALSE(r.bridge.present());
}

TEST_CASE("disk share: the Tracker never writes the card while the computer has it, and never takes it back without an eject (unplug, bus reset, a long silence, a busy claim)") {
  Rig r;
  namespace sv = o3sdk::platform::mk66::save;
  sv::FatStorageWriter writer(r.fat, r.medium);
  NEED(go_out(r));
  const std::uint64_t written = r.medium.blocks_written;
  const std::uint64_t read = r.medium.blocks_read;
  r.host_configures = false;  // unplugged without an eject
  r.eng.reset_all();          // the bus reset a re-plug brings
  g_job = true;               // even a claim that comes and goes changes nothing
  r.run(60'000);
  g_job = false;
  r.run(540'000);  // ten minutes: Q7, no timeout
  CHECK(r.share.state() == DiskShare::State::Out);
  CHECK(r.owner() == st::CardOwner::Host);
  CHECK(r.take_backs == 0u);
  CHECK(writer.gate() == o3sdk::hal::storage::WriteStatus::NoCard);
  CHECK(writer.make_dir("Projects/X") == o3sdk::hal::storage::WriteStatus::NoCard);
  CHECK(r.fat.mounted() == false);
  CHECK(r.swap.reacquire() == hs::NotMounted);
  CHECK(r.medium.blocks_written == written);
  CHECK(r.medium.blocks_read == read);  // the OS side reads nothing either
  CHECK(r.share.share(r.now, true) == DiskShare::Refusal::NotIdle);
}

TEST_CASE("disk share: the computer's eject stops the MSC, remounts, then restores the previous layout (AUDIO comes back, Q6)") {
  Rig r;
  NEED(go_out(r));
  (void)r.share.poll_event();
  r.run(5);
  // START STOP UNIT, LoEj=1 Start=0 (PREVENT is not set: the eject is allowed).
  cbw(r.eng, {0x1B, 0x00, 0x00, 0x00, 0x02, 0x00}, 0, false);
  r.run(2);
  NEED(r.run_until(DiskShare::State::Returning, 5000));
  r.run(kLayoutChangeMs + 50);
  CHECK(r.share.idle());
  const DiskShare::Event e = r.share.poll_event();
  CHECK(e.kind == Kind::Ejected);
  CHECK(r.owner() == st::CardOwner::Tracker);
  CHECK(r.readme_reads());
  CHECK(r.cur.mode == ou::Mode::Audio);
  CHECK((r.cur == r.share.previous_layout()));
  const int close = r.index_of("close");
  const int back = r.index_of("take_back");
  const int layout = r.index_of("layout:back");
  CHECK(close >= 0);
  CHECK(close < back);
  CHECK(back < layout);
  CHECK(r.bridge.present() == false);
}

TEST_CASE("disk share: the unit's eject tells the engine first; a STANDARD unit goes back to STANDARD; the card can be shared again") {
  Rig r;
  r.cur.mode = ou::Mode::Standard;
  NEED(go_out(r));
  CHECK(r.share.eject(r.now) == DiskShare::Refusal::None);
  CHECK(r.order.back() == "unit_eject");
  NEED(r.run_until(DiskShare::State::Idle, 8000));
  CHECK(r.cur.mode == ou::Mode::Standard);
  CHECK(r.owner() == st::CardOwner::Tracker);
  CHECK(r.eng.ejected());  // the eject latch is the engine's; the next hand-over's disk_open() (medium_changed) clears it
  CHECK(r.share.eject(r.now) == DiskShare::Refusal::NotIdle);  // nothing to eject now
  (void)r.share.poll_event();
  CHECK(go_out(r));  // a second session
  CHECK(r.swap.host_sessions() == 2u);
  CHECK(r.bridge.present());
}

TEST_CASE("disk share: an eject in the middle of a WRITE cuts no block, writes nothing after it, and leaves a card FatFs mounts") {
  Rig r;
  NEED(go_out(r));
  (void)r.share.poll_event();
  r.run(5);
  // A WRITE(10) of 8 blocks, 1100 bytes in: the first block or two are on the card.
  cbw(r.eng, {0x2A, 0, 0, 0, static_cast<std::uint8_t>(kLba >> 8), static_cast<std::uint8_t>(kLba), 0, 0, 8, 0}, 8 * 512, false);
  r.tick();
  std::uint8_t buf[64];
  std::memset(buf, 0x66, sizeof buf);
  for (unsigned sent = 0; sent < 1100; sent += 64) {
    NEED(r.eng.can_accept_out(64));
    r.eng.out_packet(buf, 64);
    r.tick();
  }
  const std::uint64_t at_eject = r.medium.blocks_written;
  CHECK(at_eject >= 1u);
  CHECK(at_eject < 8u);
  CHECK(r.share.eject(r.now) == DiskShare::Refusal::None);
  // The computer is gone mid-command (it never reads the CSW): the drain ends at its bound, not before, not never.
  const std::uint32_t asked = r.now;
  NEED(r.run_until(DiskShare::State::Returning, DiskShare::kDrainMs + 100));
  CHECK(r.now - asked <= DiskShare::kDrainMs + 1u);
  CHECK(r.medium.blocks_written == at_eject);  // after unit_eject() the engine touches the store no more
  r.run(kLayoutChangeMs + 50);
  CHECK(r.share.idle());
  CHECK(r.share.poll_event().kind == Kind::Ejected);
  CHECK(r.owner() == st::CardOwner::Tracker);
  CHECK(r.readme_reads());  // the cut copy is on the computer's side to repair; the card itself mounts
  std::uint8_t out[512] = {};
  CHECK(r.bridge.write(kLba, 1, out) == msc::IoResult::NoMedium);  // the bridge is shut: a late command reaches nothing
  CHECK(r.medium.blocks_written == at_eject);
}

TEST_CASE("disk share: a computer that reads the last CSW ends the drain at once") {
  Rig r;
  NEED(go_out(r));
  r.run(5);
  cbw(r.eng, {0x2A, 0, 0, 0, static_cast<std::uint8_t>(kLba >> 8), static_cast<std::uint8_t>(kLba), 0, 0, 1, 0}, 512, false);
  r.tick();
  CHECK(r.share.eject(r.now) == DiskShare::Refusal::None);
  std::uint8_t pkt[64];
  for (int i = 0; i < 40 && !r.eng.idle(); ++i) {  // the host drains the stall and the CSW like a real one
    if (r.eng.out_stalled()) {
      r.eng.clear_halt_out();
    }
    if (r.eng.in_stalled()) {
      r.eng.clear_halt_in();
    }
    (void)r.eng.in_packet(pkt, 64);
    r.eng.service();
  }
  const std::uint32_t asked = r.now;
  NEED(r.run_until(DiskShare::State::Returning, 100));
  CHECK(r.now - asked < 100u);
  r.run(kLayoutChangeMs + 50);
  CHECK(r.owner() == st::CardOwner::Tracker);
}

TEST_CASE("disk share: a layout stuck in DISK is restored by an eject (Q6), and a share goes on from it keeping the layout it was entered from") {
  {
    Rig r;
    r.refuse_return = true;
    r.host_configures = false;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kLayoutMs + DiskShare::kConfigureMs + kLayoutChangeMs + DiskShare::kReturnMs + 200);
    NEED(r.share.idle());
    NEED(r.share.layout_stuck());
    CHECK(r.cur.mode == ou::Mode::Disk);
    r.refuse_return = false;  // the layout driver is free again
    CHECK(r.share.eject(r.now) == DiskShare::Refusal::None);
    CHECK(r.share.state() == DiskShare::State::Returning);
    r.run(kLayoutChangeMs + 100);
    CHECK(r.share.idle());
    CHECK_FALSE(r.share.layout_stuck());
    CHECK(r.cur.mode == ou::Mode::Audio);
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(r.share.eject(r.now) == DiskShare::Refusal::NotIdle);  // nothing stuck now
  }
  {
    Rig r;
    r.refuse_return = true;
    r.host_configures = false;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    r.run(DiskShare::kLayoutMs + DiskShare::kConfigureMs + kLayoutChangeMs + DiskShare::kReturnMs + 200);
    NEED(r.share.layout_stuck());
    r.refuse_return = false;
    r.host_configures = true;
    NEED(go_out(r));  // from the stuck DISK layout: no second layout change asked, the owner flips
    CHECK(r.share.previous_layout().mode == ou::Mode::Audio);  // (DISK is never recorded as the layout to go back to)
    CHECK(r.share.eject(r.now) == DiskShare::Refusal::None);
    NEED(r.run_until(DiskShare::State::Idle, 8000));
    CHECK(r.cur.mode == ou::Mode::Audio);
    CHECK_FALSE(r.share.layout_stuck());
  }
}

TEST_CASE("disk share, the power latch (7): a card that is out keeps being pumped so the command in flight ends; with the Tracker holding the card nothing flips and the save hold is lifted") {
  {
    Rig r;
    NEED(go_out(r));
    (void)r.share.poll_event();
    r.run(5);
    cbw(r.eng, {0x28, 0, 0, 0, static_cast<std::uint8_t>(kLba >> 8), static_cast<std::uint8_t>(kLba), 0, 0, 1, 0}, 512, true);  // READ(10), one block
    r.tick();
    CHECK_FALSE(r.eng.idle());
    r.latched = true;  // the shutdown's stage A lands with the command in flight
    std::uint8_t pkt[64];
    for (int i = 0; i < 100 && !r.eng.idle(); ++i) {  // the computer reads the data and the CSW
      (void)r.eng.in_packet(pkt, 64);
      r.tick();
    }
    CHECK(r.eng.idle());  // Mk66PowerHost::card_busy() clears: stage C does not wait out its 8 s cap
    CHECK(r.share.state() == DiskShare::State::Out);  // frozen: no eject path, no remount under the latch
    CHECK(r.owner() == st::CardOwner::Host);
    CHECK(r.take_backs == 0u);
    CHECK(r.held);
  }
  for (const DiskShare::State where : {DiskShare::State::Quiesce, DiskShare::State::Layout, DiskShare::State::Configure}) {
    Rig r;
    r.save_due = where == DiskShare::State::Quiesce;
    r.host_configures = where != DiskShare::State::Configure;
    r.refuse_layout = false;
    CHECK(r.share.share(r.now, true) == DiskShare::Refusal::None);
    if (where == DiskShare::State::Layout) {
      r.run_until(DiskShare::State::Layout, 100);
    } else if (where == DiskShare::State::Configure) {
      NEED(r.run_until(DiskShare::State::Configure, 8000));
    }
    NEED(r.share.state() == where);
    r.latched = true;
    r.run(DiskShare::kLayoutMs + DiskShare::kConfigureMs + 2 * kLayoutChangeMs);
    CHECK_FALSE(r.held);  // the shutdown's settings flush can be written
    CHECK(r.share.state() == where);
    CHECK(r.owner() == st::CardOwner::Tracker);
    CHECK(r.index_of("hand") < 0);
    CHECK(r.take_backs == 0u);
  }
}
