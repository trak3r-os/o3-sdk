// SPDX-License-Identifier: GPL-3.0-or-later
//
// The image's USB audio service (docs/M2C-PLAN.md section 7): audio::UsbAudioService over a Link to usb0, and the card's hand-over to the computer (DiskShare, ported from track3r-os
// platform/mk66/src/app_main.cpp "The running mode", its port over storage::device and usb0, with the Config page's request and the strip text moved to the kit). Main loop except
// where a line says otherwise; the USB interrupt reaches only the service's streams_changed() (usb0.cpp on_streams).
#include "../../src/internal.h"

#include <o3sdk/platform/mk66/audio/usb_audio_service.h>
#include <o3sdk/platform/mk66/usb/disk_share.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/storage/device.h>
#include <o3sdk/platform/mk66/usb/console.h>
#include <o3sdk/platform/mk66/usb/usb0.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>

namespace o3sdk::platform::mk66::usb_audio {

namespace {

namespace hu = hal::usb_audio;

bool g_power_latched = false;  // the shutdown has the card: the machine is frozen where it is (DiskShare::pass(latched))
std::uint32_t g_strip_at = 0;

// The card goes to the computer while the unit plays on: DiskShare is the state machine, this is its port over storage::device and usb0.
class DiskPort final : public DiskShare::Port {
 public:
  storage::CardOwner owner() const noexcept override { return storage::device::owner(); }
  bool card_busy() const noexcept override { return storage::device::card_busy(); }
  // o3-sdk: the files service's settings write is held by the card claim instead (disk_holds_card), and a write that is running is a claim that refuses the share at once (Busy).
  bool save_pending() const noexcept override { return false; }
  DiskShare::Layout layout() const noexcept override { return DiskShare::Layout{usb0::mode(), usb0::profile(), usb0::return_enabled()}; }
  bool layout_changing() const noexcept override { return usb0::change_phase() != 0u; }
  bool enter_disk_layout(const DiskShare::Layout& to) noexcept override {
    const bool ok = usb0::disk_layout_enter(to.profile, to.usb_return);
    if (ok) {
      console::note_usb_layout_requested();
    }
    return ok;
  }
  bool leave_disk_layout(const DiskShare::Layout& to) noexcept override {
    const bool ok = usb0::disk_layout_leave(to.mode, to.profile, to.usb_return);
    if (ok) {
      console::note_usb_layout_requested();
    }
    return ok;
  }
  bool change_layout(const DiskShare::Layout& to) noexcept override { return usb0::request_layout(to.mode, to.profile, to.usb_return); }  // (DISK is not presented: the attach fell back to STANDARD)
  bool configured() const noexcept override { return usb0::disk_state().configured; }
  storage::CardSwap::HandOver hand_to_host() noexcept override {
    appimg::service_while_blocked(true);  // (reads the volume's dirty flag before the card goes: three sectors)
    const storage::CardSwap::HandOver r = storage::device::hand_to_host();
    appimg::service_while_blocked(false);
    return r;
  }
  bool disk_open(bool writable) noexcept override {
    appimg::service_while_blocked(true);  // (the card's initialisation blocks: the display stays serviced)
    const bool opened = usb0::disk_open(writable);
    appimg::service_while_blocked(false);
    return opened;
  }
  void disk_close() noexcept override { usb0::disk_close(); }
  void pump(std::uint32_t now_ms) noexcept override { usb0::disk_pass(now_ms); }  // the SCSI state machine (and the card slot) for the computer
  // o3-sdk: the files service has no hold. A write that starts between the request and the flip is a claim, and the flip refuses it (the share ends with Busy and the user asks again); once the card
  // is the computer's the writer's gate says Host and the kit's settings stay dirty until the eject.
  void hold_background(bool) noexcept override {}
  hal::storage::Status take_back() noexcept override {
    appimg::service_while_blocked(true);
    const hal::storage::Status s = storage::device::take_back();
    appimg::service_while_blocked(false);
    return s;
  }
  bool engine_busy() const noexcept override { return usb0::disk_state().busy; }
  bool take_eject_request() noexcept override { return usb0::disk_take_eject_request(); }
  void unit_eject() noexcept override { usb0::disk_unit_eject(); }
  void log(const char* line) noexcept override { console::log(line); }
};
DiskPort g_disk_port;
DiskShare g_disk_share(g_disk_port);

// The USB driver, as the service sees it. The set presented, or the one a change is on its way to.
class Usb0Link final : public audio::UsbAudioService::Link {
 public:
  bool ready() const noexcept override { return usb0::iso() != nullptr; }
  bool audio() const noexcept override { return (usb0::change_phase() != 0u ? usb0::pending_mode() : usb0::mode()) == o3sdk::usb::Mode::Audio; }
  hu::Profile profile() const noexcept override { return audio::from_usb(usb0::change_phase() != 0u ? usb0::pending_profile() : usb0::profile()); }
  bool duplex() const noexcept override { return usb0::change_phase() != 0u ? usb0::pending_return() : usb0::return_enabled(); }
  bool changing() const noexcept override { return usb0::change_phase() != 0u || !g_disk_share.idle(); }
  bool request(bool audio, hu::Profile profile, bool duplex) noexcept override {
    if (!g_disk_share.idle() || g_power_latched) {
      return false;  // the card is being handed over or is out: the layout is the disk's until the eject
    }
    const bool was_changing = usb0::change_phase() != 0u;
    if (!usb0::request_layout(audio ? o3sdk::usb::Mode::Audio : o3sdk::usb::Mode::Standard, audio::to_usb(profile), duplex)) {
      return false;
    }
    if (!was_changing && usb0::change_phase() != 0u) {
      console::note_usb_layout_requested();  // the guard under the AUDIO set watches the set the change ends in
    }
    return true;
  }
  hu::DiskStatus disk() const noexcept override {
    hu::DiskStatus d;
    d.available = ready();
    d.phase = g_disk_share.state() == DiskShare::State::Out ? hu::DiskPhase::Out : (g_disk_share.idle() ? hu::DiskPhase::Idle : hu::DiskPhase::Working);
    d.card_present = storage::device::owner() != storage::CardOwner::NoCard;
    d.busy = storage::device::card_busy();
    d.writable = usb0::disk_state().writable;
    d.audio = usb0::mode() == o3sdk::usb::Mode::Audio;
    return d;
  }
  hu::DiskRefusal disk_share(bool read_only) noexcept override {
    if (!ready() || g_power_latched) {
      return hu::DiskRefusal::Unavailable;
    }
    if (usb0::change_phase() != 0u) {
      return hu::DiskRefusal::Layout;
    }
    switch (g_disk_share.share(static_cast<std::uint32_t>(system_clock().uptime_ms()), !read_only)) {
      case DiskShare::Refusal::None: return hu::DiskRefusal::None;
      case DiskShare::Refusal::NotIdle: return hu::DiskRefusal::NotIdle;
      case DiskShare::Refusal::NoCard: return hu::DiskRefusal::NoCard;
      case DiskShare::Refusal::Busy: return hu::DiskRefusal::Busy;
      case DiskShare::Refusal::SaveBusy: return hu::DiskRefusal::SaveBusy;
    }
    return hu::DiskRefusal::Unavailable;
  }
  bool disk_eject() noexcept override { return g_disk_share.eject(static_cast<std::uint32_t>(system_clock().uptime_ms())) == DiskShare::Refusal::None; }
};
Usb0Link g_link;

audio::UsbAudioService g_service(tap(), usb0::stem_map(), g_link);

}  // namespace

audio::UsbAudioService& service() noexcept { return g_service; }
hal::usb_audio::Device& device() noexcept { return g_service; }

hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, dsp::Engine& engine) noexcept { return g_service.wrap(inner, engine); }

// ---- The hand-over's pass, once a pass of the main loop -------------------------------------------------------------------------------------------------------------------------------

DiskNote disk_pass(std::uint32_t now, bool power_latched, unsigned render_load_percent) noexcept {
  DiskNote note;
  g_power_latched = power_latched;
  usb0::set_render_load(power_latched ? 0u : render_load_percent);  // Q11: the pump is held above 85 % of the block budget (not by a render that the shutdown mutes)
  g_disk_share.pass(now, power_latched);  // the pump, the step and the files service's hold; under the power latch the pump of a card that is out goes on (the shutdown waits for the command in flight)
  if (power_latched) {
    return note;
  }
  const DiskShare::Event e = g_disk_share.poll_event();
  switch (e.kind) {
    case DiskShare::Event::Kind::None:
      break;
    case DiskShare::Event::Kind::Shared:
      g_strip_at = now - 1000u;  // the persistent strip, below
      note.card_changed = true;  // the files service sees the card go: a list the app holds is stale
      break;
    case DiskShare::Event::Kind::Ejected:
      note.card_changed = true;  // hazard 13: the remount ends with the event that revalidates what the OS believes about the card
      if (storage::device::swap().remount_newly_dirty()) {  // 6.3 (F5): a warning after a Host session only, and only when the session left the flag set
        note.text = "CARD NOT EJECTED PROPERLY: CHECK THE COMPUTER";
        note.hold_ms = 8000;
      }
      break;
    case DiskShare::Event::Kind::EjectedNotReadable:
      note.card_changed = true;
      note.text = "THE CARD CAME BACK UNREADABLE";
      note.hold_ms = 4000;
      break;
    case DiskShare::Event::Kind::Failed:
      note.text = DiskShare::fail_text(e.fail);
      note.hold_ms = 4000;
      if (e.fail == DiskShare::Fail::Open) {  // the card did not start after the flip: it was given back, so it was remounted
        note.card_changed = true;
      }
      break;
  }
  if (g_disk_share.out() && now - g_strip_at >= 1000u) {
    g_strip_at = now;
    note.text = "CARD ON COMPUTER - EJECT FIRST";
    note.hold_ms = 2500;
  }
  return note;
}

const char* disk_request(bool read_only) noexcept {
  if (usb0::mode() == o3sdk::usb::Mode::Disk && g_disk_share.idle()) {
    return "the boot disk is already out (restart to leave it)";
  }
  if (g_power_latched) {
    return "the unit is shutting down";
  }
  switch (g_link.disk_share(read_only)) {
    case hu::DiskRefusal::None: return nullptr;
    case hu::DiskRefusal::Unavailable: return "no USB disk on this unit, or the driver is down";
    case hu::DiskRefusal::NotIdle: return "a hand-over is already running or the disk is out";
    case hu::DiskRefusal::NoCard: return "no card for the unit to give";
    case hu::DiskRefusal::Busy: return "the card is in use (a load, a save, a listing, an upload)";
    case hu::DiskRefusal::SaveBusy: return "a save is running";
    case hu::DiskRefusal::Layout: return "a USB mode change is in progress";
  }
  return "refused";
}

const char* disk_eject_request() noexcept { return g_link.disk_eject() ? nullptr : "the disk is not out"; }

const char* disk_state_name() noexcept {
  switch (g_disk_share.state()) {
    case DiskShare::State::Idle: return usb0::mode() == o3sdk::usb::Mode::Disk ? "boot disk" : (g_disk_share.layout_stuck() ? "idle (layout not restored)" : "idle");
    case DiskShare::State::Quiesce: return "waiting for a save";
    case DiskShare::State::Layout: return "switching the USB set";
    case DiskShare::State::Configure: return "waiting for the computer";
    case DiskShare::State::Out: return "out";
    case DiskShare::State::Draining: return "ejecting";
    case DiskShare::State::Returning: return "restoring the USB set";
  }
  return "?";
}

}  // namespace o3sdk::platform::mk66::usb_audio
