// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U2a-1, the running mode (a): the hand-over of the card to the computer and the eject, as ONE state machine over a small port (docs/specs/OS-USBDISK.md 4.2, 6.1, 7, 12 hazards 1, 4, 10).
// Pure logic, main loop only, no allocation, no clock of its own (every call takes the millisecond clock), no USB or card code: the image binds the port to storage::device and usb0
// (src/disk_share_port.cpp), the tests to fakes.
//
//   share()   Quiesce (the save service may still hold a write that is due: up to kSaveWaitMs) -> Layout (the Disk layout is requested; the previous one is remembered) -> Configure (the
//             computer has to configure the device: kConfigureMs) -> THE FLIP: hand_to_host() (it re-checks the claims: a job that began meanwhile refuses it), the bridge opens -> Out.
//             THE OWNER FLIPS LAST. Any step that fails before the flip asks for the previous layout back and ends in Idle with a Fail: no state has the owner Host without the Disk layout (F4), and the
//             owner is Tracker for the whole of every failed attempt.
//   eject     Out -> Draining (the engine is told to end the transfer in flight, up to kDrainMs for it to finish: a block is never cut, the card access is synchronous on this very loop) -> the bridge
//             closes -> take_back() (the remount, blocking) -> Returning (the previous layout, with retries) -> Idle. Never before the MSC has stopped, never without an eject: the Tracker takes the
//             card back from Out ONLY through Draining. Three triggers: the computer's START STOP UNIT (take_eject_request), eject() (the unit's key; the confirmation press is the UI's), and a
//             failed open right after the flip (the card did not start: nothing was given to the computer yet).
//   The bus going away (unplug, a suspended computer) is NOT a trigger: Out stays until an eject, with no timeout (Q7, 6.1 (3)); a card pulled from the slot is the bridge's business (follow()).
//
// poll_event() is the one-shot answer for the app: the strip text and the card_changed(Ready / NotReadable) the remount owes (hazard 13).

#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/usb/usb_types.h>

#include <cstdint>

namespace o3sdk::platform::mk66 {

class DiskShare {
 public:
  static constexpr std::uint32_t kSaveWaitMs = 2000;   // 4.2: a settings/recent write that is due is done first, then the hand-over is refused
  static constexpr std::uint32_t kLayoutMs = 2500;     // the detach (120 + 700 ms) and the re-attach, with margin
  static constexpr std::uint32_t kConfigureMs = 8000;  // 4.2 (2): the computer configures the Disk device. Longer than a host's 5 s control timeout (usbmon, int.14: a control URB can hang that long once and the enumeration goes on after it)
  static constexpr std::uint32_t kDrainMs = 3000;      // the transfer in flight ends (1 MiB at the unit's 432 KiB/s write rate is 2.4 s; the eject of the engine ends it at the next block)
  static constexpr std::uint32_t kReturnMs = 4000;     // the previous layout is back (the same bound a rollback has)

  struct Layout {
    o3sdk::usb::Mode mode = o3sdk::usb::Mode::Standard;
    o3sdk::usb::Profile profile = o3sdk::usb::Profile::P10;
    bool usb_return = true;
    bool operator==(const Layout& o) const noexcept { return mode == o.mode && profile == o.profile && usb_return == o.usb_return; }
  };

  // Everything the machine touches. The image's implementation is src/disk_share_port.cpp.
  class Port {
   public:
    virtual storage::CardOwner owner() const noexcept = 0;
    virtual bool card_busy() const noexcept = 0;                   // any claim (storage/card_claims.h)
    virtual bool save_pending() const noexcept = 0;                // a settings/recent write is due or running (the save service's own state)
    virtual Layout layout() const noexcept = 0;                    // the set presented now
    virtual bool layout_changing() const noexcept = 0;             // a re-enumeration is in progress
    // The three ways the USB layout changes (usb0): into DISK from another set, out of DISK, and an ordinary change when DISK is NOT presented (the attach fell back to STANDARD). False: refused, nothing changed.
    virtual bool enter_disk_layout(const Layout& to) noexcept = 0;
    virtual bool leave_disk_layout(const Layout& to) noexcept = 0;
    virtual bool change_layout(const Layout& to) noexcept = 0;
    virtual bool configured() const noexcept = 0;                  // the computer has configured the Disk device
    virtual storage::CardSwap::HandOver hand_to_host() noexcept = 0;
    virtual bool disk_open(bool writable) noexcept = 0;            // the bridge starts the card (blocking)
    virtual void disk_close() noexcept = 0;                        // the bridge answers NOT READY, MEDIUM NOT PRESENT from here
    virtual hal::storage::Status take_back() noexcept = 0;         // the remount (blocking)
    virtual bool engine_busy() const noexcept = 0;                 // a SCSI command is in flight
    virtual bool take_eject_request() noexcept = 0;                // the computer ejected (once)
    virtual void unit_eject() noexcept = 0;                        // the unit's eject: the computer's next command is UNIT ATTENTION, then NOT READY
    virtual void pump(std::uint32_t now_ms) noexcept = 0;          // the SCSI state machine (usb0::disk_pass)
    virtual void hold_background(bool on) noexcept = 0;            // the save service: no RECENT or settings write starts while on
    virtual void log(const char* line) noexcept = 0;

   protected:
    ~Port() = default;
  };

  enum class State : std::uint8_t { Idle, Quiesce, Layout, Configure, Out, Draining, Returning };
  enum class Fail : std::uint8_t { None, NoCard, Busy, SaveBusy, Layout, NoHost, Owner, Open };
  enum class Refusal : std::uint8_t { None, NotIdle, NoCard, Busy, SaveBusy };  // share() / eject() answered at once
  struct Event {
    enum class Kind : std::uint8_t { None, Shared, Ejected, EjectedNotReadable, Failed };
    Kind kind = Kind::None;
    Fail fail = Fail::None;
  };

  explicit DiskShare(Port& port) noexcept : port_(port) {}

  // The user asked for the card to go to the computer. `writable` is this session's policy (the READ ONLY row of the Config page). Refusals touch nothing. NoCard: the owner is not Tracker (a card
  // already out included); Busy: a claim says the card is in use; SaveBusy never at once (the wait is the Quiesce state), listed for the strip text.
  Refusal share(std::uint32_t now_ms, bool writable) noexcept {
    if (state_ != State::Idle) {
      return Refusal::NotIdle;
    }
    if (port_.owner() != storage::CardOwner::Tracker) {
      return Refusal::NoCard;
    }
    if (port_.card_busy()) {
      return Refusal::Busy;
    }
    writable_ = writable;
    fail_ = Fail::None;
    stuck_ = false;
    if (port_.layout().mode != o3sdk::usb::Mode::Disk) {  // (a layout stuck in DISK keeps the layout it was entered from: DISK is never the one to go back to)
      prev_ = port_.layout();
    }
    enter(State::Quiesce, now_ms);
    return Refusal::None;
  }
  // The unit's eject (the second F7 press: the confirmation is the UI's). Only from Out; the engine is told first so the computer sees UNIT ATTENTION and the transfer in flight ends.
  // A layout that is stuck (the previous one could not be restored) is retried from Idle: the eject asks for it again, with no card involved.
  Refusal eject(std::uint32_t now_ms) noexcept {
    if (state_ == State::Idle && stuck_) {
      port_.log("disk share: restoring the previous layout");
      asked_ = false;
      enter(State::Returning, now_ms);
      return Refusal::None;
    }
    if (state_ != State::Out) {
      return Refusal::NotIdle;
    }
    port_.unit_eject();
    begin_drain(now_ms);
    return Refusal::None;
  }

  // The one call of every pass of the main loop (the image's disk_share_pass). Latched: the shutdown has the card. The machine is frozen where it is (nothing flips, nothing is taken back); a computer that
  // has the card keeps being pumped so that a command in flight ends (the shutdown waits for it) and the save service stays held (its Host gate); with the card the Tracker's, the hold is lifted so that the
  // settings flush of the shutdown can be written. Otherwise: the pump while the computer configures or has the card, the step, and then the hold for the save service (set AFTER the step, and not in
  // Quiesce: the write that Quiesce waits for has to start).
  void pass(std::uint32_t now_ms, bool latched) noexcept {
    if (latched) {
      const bool host = port_.owner() == storage::CardOwner::Host;
      port_.hold_background(host);
      if (host) {
        port_.pump(now_ms);
      }
      return;
    }
    if (state_ == State::Configure || out()) {
      port_.pump(now_ms);
    }
    step(now_ms);
    port_.hold_background(holding());
  }

  // Every pass of the main loop (pass() calls it).
  void step(std::uint32_t now_ms) noexcept {
    switch (state_) {
      case State::Idle:
        return;
      case State::Quiesce:
        if (!port_.save_pending() && !port_.card_busy()) {
          start_layout(now_ms);
        } else if (elapsed(now_ms) >= kSaveWaitMs) {
          abandon(port_.card_busy() ? Fail::Busy : Fail::SaveBusy);  // (nothing was asked of the bus yet: no rollback)
        }
        return;
      case State::Layout:
        if (!port_.layout_changing() && port_.layout().mode == o3sdk::usb::Mode::Disk) {
          enter(State::Configure, now_ms);
        } else if (elapsed(now_ms) >= kLayoutMs) {
          rollback(Fail::Layout, now_ms);
        }
        return;
      case State::Configure:
        if (!port_.configured()) {
          if (elapsed(now_ms) >= kConfigureMs) {
            rollback(Fail::NoHost, now_ms);
          }
          return;
        }
        flip(now_ms);
        return;
      case State::Out:
        if (port_.take_eject_request()) {
          begin_drain(now_ms);
        }
        return;
      case State::Draining:
        if (!port_.engine_busy() || elapsed(now_ms) >= kDrainMs) {
          finish_drain(now_ms);
        }
        return;
      case State::Returning:
        step_returning(now_ms);
        return;
    }
  }

  State state() const noexcept { return state_; }
  bool idle() const noexcept { return state_ == State::Idle; }
  bool out() const noexcept { return state_ == State::Out || state_ == State::Draining; }  // the computer has the card (the strip text; every card path refuses)
  // The save service's background writes (RECENT, settings) wait: from the layout change to the end of the way back. Not in Quiesce (the write it waits for must start) and not in Out (the gate Host holds them).
  bool holding() const noexcept { return state_ == State::Layout || state_ == State::Configure || state_ == State::Draining || state_ == State::Returning; }
  Fail fail() const noexcept { return fail_; }
  const Layout& previous_layout() const noexcept { return prev_; }
  // The one-shot answer: what happened since the last call.
  Event poll_event() noexcept {
    const Event e = event_;
    event_ = Event{};
    return e;
  }
  // True if the previous layout could not be restored within kReturnMs (the owner is Tracker, MSC answers NOT READY: harmless, logged). eject() asks for it again from Idle; share() goes on from a layout stuck in DISK.
  bool layout_stuck() const noexcept { return stuck_; }

  // The strip's words for a refusal share() / eject() answered at once: the Config row's own first-press texts (param_actions.cpp), said by the platform when a request got through the row and the machine still refused.
  static constexpr const char* refusal_text(Refusal r) noexcept {
    switch (r) {
      case Refusal::None: return "";
      case Refusal::NoCard: return "NO CARD";
      case Refusal::Busy: return "STOP THE EXPORT FIRST";
      case Refusal::SaveBusy: return "CARD BUSY: WAIT FOR THE SAVE";
      case Refusal::NotIdle: return "THE CARD IS CHANGING HANDS: WAIT";
    }
    return "";
  }

  static constexpr const char* fail_text(Fail f) noexcept {
    switch (f) {
      case Fail::None: return "";
      case Fail::NoCard: return "NO CARD";
      case Fail::Busy: return "STOP THE EXPORT FIRST";  // (a load, an export or a firmware upload holds the card)
      case Fail::SaveBusy: return "CARD BUSY: WAIT FOR THE SAVE";
      case Fail::Layout:
      case Fail::NoHost:
      case Fail::Owner:
      case Fail::Open: return "COULD NOT SHARE THE CARD";
    }
    return "";
  }

 private:
  std::uint32_t elapsed(std::uint32_t now_ms) const noexcept { return now_ms - since_; }
  void enter(State s, std::uint32_t now_ms) noexcept {
    state_ = s;
    since_ = now_ms;
  }
  void abandon(Fail f) noexcept {
    fail_ = f;
    event_ = Event{Event::Kind::Failed, f};
    state_ = State::Idle;
  }
  void start_layout(std::uint32_t now_ms) noexcept {
    Layout disk = prev_;
    disk.mode = o3sdk::usb::Mode::Disk;
    if (port_.layout().mode != o3sdk::usb::Mode::Disk && !port_.enter_disk_layout(disk)) {  // (already DISK: a layout stuck from an earlier attempt, nothing to ask)
      abandon(Fail::Layout);
      return;
    }
    enter(State::Layout, now_ms);
  }
  // The owner flips here and nowhere else on the way in. A refusal (a job began during the waits, the card went) is a rollback with the owner untouched.
  void flip(std::uint32_t now_ms) noexcept {
    switch (port_.hand_to_host()) {
      case storage::CardSwap::HandOver::Ok:
        break;
      case storage::CardSwap::HandOver::Busy:
        rollback(port_.save_pending() ? Fail::SaveBusy : Fail::Busy, now_ms);
        return;
      case storage::CardSwap::HandOver::NoCard:
      case storage::CardSwap::HandOver::AlreadyHost:
        rollback(Fail::Owner, now_ms);
        return;
    }
    if (!port_.disk_open(writable_)) {
      // The card did not start: nothing was given to the computer yet, so the way back is the eject path without a transfer to wait for.
      pending_fail_ = Fail::Open;
      begin_drain(now_ms);
      return;
    }
    port_.log("disk share: out");
    event_ = Event{Event::Kind::Shared, Fail::None};
    enter(State::Out, now_ms);
  }
  void begin_drain(std::uint32_t now_ms) noexcept { enter(State::Draining, now_ms); }
  // MSC stopped (close), THEN the remount, THEN the layout: the order of 4.2.
  void finish_drain(std::uint32_t now_ms) noexcept {
    port_.disk_close();
    const hal::storage::Status s = port_.take_back();
    const bool readable = s == hal::storage::Status::Ok;
    if (pending_fail_ != Fail::None) {
      fail_ = pending_fail_;
      event_ = Event{Event::Kind::Failed, pending_fail_};
    } else {
      event_ = Event{readable ? Event::Kind::Ejected : Event::Kind::EjectedNotReadable, Fail::None};
    }
    pending_fail_ = Fail::None;
    port_.log(readable ? "disk share: ejected, remounted" : "disk share: ejected, card not readable");
    asked_ = false;
    enter(State::Returning, now_ms);
  }
  // Before the flip only: the owner is Tracker, the previous layout is asked back, the Fail is reported when it is done.
  void rollback(Fail f, std::uint32_t now_ms) noexcept {
    fail_ = f;
    event_ = Event{Event::Kind::Failed, f};
    port_.log("disk share: rolled back");
    asked_ = false;
    enter(State::Returning, now_ms);
  }
  // The previous layout, asked again every pass until the layout driver takes it (it refuses while a change is in progress), then waited for. An unreachable layout ends in Idle after kReturnMs.
  void step_returning(std::uint32_t now_ms) noexcept {
    if (port_.layout() == prev_ && !port_.layout_changing()) {
      stuck_ = false;
      state_ = State::Idle;
      return;
    }
    if (!asked_ && !port_.layout_changing()) {
      asked_ = port_.layout().mode == o3sdk::usb::Mode::Disk ? port_.leave_disk_layout(prev_) : port_.change_layout(prev_);
    }
    if (elapsed(now_ms) >= kReturnMs) {
      stuck_ = true;
      port_.log("disk share: previous layout not restored");
      state_ = State::Idle;
    }
  }

  Port& port_;
  State state_ = State::Idle;
  Fail fail_ = Fail::None;
  Fail pending_fail_ = Fail::None;
  Event event_;
  Layout prev_;
  bool writable_ = false;
  bool asked_ = false;
  bool stuck_ = false;
  std::uint32_t since_ = 0;
};

}  // namespace o3sdk::platform::mk66
