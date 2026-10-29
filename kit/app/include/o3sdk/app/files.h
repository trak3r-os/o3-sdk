// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The app file API (docs/M2C-PLAN.md section 3): an app saves and loads its own files on the SD card through Context::files(), and the kit keeps its system settings (the theme,
// the MIDI settings) on the card for the app. Main loop only.
//
//   Where. The app's folder is `Apps/<id>/` (<id>: the app's CMake id, o3sdk_app(<id> ...), at most 15 bytes), with one level of project folders under it. A `folder` argument
//   is "" (the app's folder) or one project folder ("Song 1"); a `name` is "<stem>.<ext>" with the extension one of o3a (an app's own data), o3s, wav, mid, syx, txt, json, csv
//   (hal/app_guard.h has the rows: it is a SUPERSET of track3r-os's guard, so the writer's own guard also admits Projects/, Settings/, instruments/ and AppExport/; Firmware/ is the update
//   path's, with a guard of its own and no path in common). The service narrows it: every change it makes goes through a ScopedWriter (files_work.h) that refuses, with no card call,
//   any path outside `Apps/<id>/` and `Samples/`. `system.o3s` of the app's folder is the kit's settings file: no call of the app's may change it (RESERVED NAME), whatever its case.
//   "Samples" is no project name: as a `folder` it means the card's Samples/ and only for save_wav.
//
//   Never blocking. Every call only records a request; the work is a job the runtime advances a slice of every pass of the main loop (poll(): a budget of kJobBudgetUs), so the
//   render and the screen never wait for the card. One card job runs at a time: a call made while another runs is refused (false) and changes nothing, status() keeps describing the
//   running job (it is not set to "BUSY": that would hide the job's own result). The pointers a call is given (`data`, `dst`, `out`, `sink`, a source) must stay valid and, for `data`,
//   unchanged until the job has ended (Done or Failed, or cancel()). The kit's own settings save steps aside for an app's request.
//
//   Never a half-written file. A save writes a temporary file, reads it back with its CRC and only then renames it over the old one, which stays as `<name>.bak`
//   (app/atomic_save.h has the sequence and what a power cut leaves): a power cut, a pulled card or a failing write at any step leaves the old file or the new one, whole.
//   A load whose file is gone reads the `.bak`. The next save of that file heals the name.
//
//   Reporting. Every call that returns false, and every job that ends, sets status() (polled) and, unless set_quiet(true), a line on the kit's strip (the progress while it runs,
//   the result for a few seconds). The settings save is always silent. An app that never calls files() pays nothing in the render; the runtime still keeps the settings.
//
//     files.save("", "riff.o3a", buf, n);                 // returns at once; poll files.status() for Done or Failed. `buf` stays as it is until then.
//     files.load("", "riff.o3a", buf, sizeof buf);        // the whole file; files.loaded() bytes when Done
//     files.list("", entries, 32);                        // files and project folders, sorted; files.listed() entries when Done
//
// The memory the jobs need is a Files::Work (files_work.h, about 16 KB) the platform places in the SDRAM and hands over in Platform::files_work; the Files object itself is a few
// hundred bytes in the runtime. Without a card capability (Platform::storage or writer null) available() is false and every call is refused with "NO CARD", and the app runs.

#include <o3sdk/app/settings.h>
#include <o3sdk/core/byte_io.h>
#include <o3sdk/core/fixed_string.h>
#include <o3sdk/core/job.h>
#include <o3sdk/hal/clock.h>
#include <o3sdk/hal/storage.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::hal::storage {
class StorageWriter;  // (hal/storage_write.h: only the files service, the writers and the platform's main name it)
}  // namespace o3sdk::hal::storage

namespace o3sdk::app {

class SaveSource;

class Files {
 public:
  class Work;  // the jobs' memory (files_work.h)

  enum class Card : std::uint8_t {
    None,         // no card in the slot, or it is not mounted (or there is no card capability at all: see available())
    Ready,        // mounted, FAT12/16/32, writable
    ReadOnly,     // write protected: loads and listings work, saves are refused
    Unsupported,  // a file system that is not FAT12/16/32 (exFAT): saves are refused
    OnComputer,   // the card is the computer's (the disk mode): nothing is read or written until the eject
    InUse,        // the firmware update path holds the card
  };
  enum class Mode : std::uint8_t { Replace, NewOnly };  // Replace: the old file is kept as .bak. NewOnly: the file must not exist
  static constexpr std::size_t kEntryNameBytes = 40;    // "<stem 31>.json" and the NUL
  struct Entry {
    char name[kEntryNameBytes];  // a project folder, or "<stem>.<ext>"
    std::uint32_t size;          // bytes (0 for a folder)
    bool folder;
    bool recover;  // only the .bak is left (a save was cut after the old file moved aside): load() reads it, the next save of the name heals it
  };
  struct Status {
    enum Phase : std::uint8_t { Idle, Running, Done, Failed } phase;
    std::uint16_t permille;  // 0..1000 while Running, 1000 when Done
    const char* text;        // upper case, static: the job's verb while Running ("SAVE"), what happened when it ended ("SAVED", "CARD IS FULL"); "" while Idle
  };
  // What the jobs have cost since boot, for the console's `files` (max_step_us is the longest slice of the main loop a job took, the number the 2 ms budget is judged by).
  struct Stats {
    std::uint32_t jobs = 0;          // jobs that ended, whatever way
    std::uint32_t failed = 0;
    std::uint32_t steps = 0;
    std::uint32_t max_step_us = 0;
    std::uint32_t overruns = 0;      // steps that took clearly longer than their slice (core::JobRunner::overran)
    std::uint32_t last_bytes = 0;    // the last save's bytes, and
    std::uint32_t last_us = 0;       // the time its steps took, so bytes / us is the card's write speed
  };

  static constexpr std::uint32_t kJobBudgetUs = 2000;       // what the running job gets of one pass of the main loop (track3r-os's kJobBudgetUs)
  static constexpr std::size_t kLoadBytesPerPass = 16 * 1024;  // a load reads this much a pass (track3r-os's, about 2 ms of card at 8 MB/s)
  static constexpr std::uint32_t kSettingsDebounceMs = 1500;   // the kit's settings are written this long after the last change (track3r-os's)
  static constexpr std::size_t kStripChars = 72;

  // ---- What an app uses --------------------------------------------------------------------------------------------------------------

  // The platform has a card capability and this app's id is a valid folder name. false: every call is refused with "NO CARD".
  bool available() const noexcept { return ready_; }
  Card card() const noexcept;
  // The volume of the mounted card (the CARD section shows it); empty (FsKind::None) when none is mounted or there is no card capability.
  hal::storage::VolumeInfo volume() const noexcept { return ready_ && storage_ != nullptr ? storage_->volume() : hal::storage::VolumeInfo{}; }
  // A card job (or a request waiting to start) is there: no new call is taken, and the FIRMWARE section says SAVING.
  bool busy() const noexcept { return kind_ != Kind::None || pend_ != Kind::None; }

  // Saves `bytes` at `data` as `folder`/`name` (Replace: the old one kept as .bak; NewOnly: refused when the name is taken). `data` stays unchanged until the job is Done or Failed.
  // false: refused now (busy, no card, a name or folder the guard refuses, nothing to save); status() says why.
  bool save(const char* folder, const char* name, const void* data, std::size_t bytes, Mode mode = Mode::Replace) noexcept;
  // The same from a source of your own (atomic_save.h SaveSource, save_sources.h): it must stay valid until the job ends, which calls its end().
  bool save(const char* folder, const char* name, SaveSource& source, Mode mode) noexcept;
  // A 16-bit PCM WAV, NewOnly, as `<stem>_NNN.wav` with the first free NNN (001..999). `folder` "" is the app's folder, a project folder, or "Samples" for the card's own Samples/ (a
  // take the user wants in any firmware). `pcm` stays unchanged until the job ends. Stem: a valid name of at most 27 bytes.
  bool save_wav(const char* folder, const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, const char* stem) noexcept;
  // Loads the whole file into `dst` (`cap` bytes; a larger file fails with "FILE TOO BIG" and `dst` is not touched); the .bak when the file is gone (recovered()). loaded() is its size.
  bool load(const char* folder, const char* name, void* dst, std::size_t cap) noexcept;
  // The same pushed into `sink` in slices (a slice of 2 KiB at a time, as the sink takes it), for a file that does not fit in memory at once.
  bool load(const char* folder, const char* name, core::ByteSink& sink) noexcept;
  // The files and project folders of `folder`, sorted (folders first), into `out[0..cap)`; listed() entries when Done, list_truncated() when there were more. Names the guard would
  // refuse are not listed; a folder marked deleted is not listed; the kit's system.o3s is not listed.
  bool list(const char* folder, Entry* out, std::size_t cap) noexcept;
  // DELETE (decision D4): marks the project `folder` with an empty `.deleted` file; nothing is removed, the listing hides the folder, and a save into it is refused.
  bool remove_folder(const char* folder) noexcept;

  Status status() const noexcept;
  std::size_t listed() const noexcept { return listed_; }
  bool list_truncated() const noexcept { return truncated_; }
  std::uint32_t loaded() const noexcept { return loaded_; }
  bool recovered() const noexcept { return recovered_; }  // the last load read the .bak
  // Gives up the running job (a save repairs at once: its temporary file is removed and the old file stays whole) and a request waiting to start; status() is Failed "CANCELLED".
  void cancel() noexcept;
  // Moves when the card goes out, comes in or goes to a computer: a list the app holds is stale. A load or a list in progress fails with "CARD CHANGED".
  std::uint32_t generation() const noexcept { return generation_; }
  // No strip line for this app's jobs (the settings save never has one).
  void set_quiet(bool quiet) noexcept { quiet_ = quiet; }
  const char* app_id() const noexcept { return id_; }

  // ---- What the runtime and the platform use ------------------------------------------------------------------------------------------

  // Binds the card capability: `work` and the card (`storage` for reading, `writer` for the saves; both null for none) and the app's id. `claim`, when given, says that something else holds
  // the card (the firmware update path): then card() is InUse. Once, before the first poll. A null argument or an id the guard refuses leaves available() false.
  void bind(hal::storage::Storage* storage, hal::storage::StorageWriter* writer, Work* work, const char* app_id, bool (*claim)() noexcept) noexcept;

  // The job pass, once a pass of the main loop: starts what was asked, runs the job within `budget_us` and settles its result; the kit's settings (`values`: what they are now) are
  // written kSettingsDebounceMs after the last change, or at once after power_begin().
  void poll(const hal::Clock& clock, std::uint32_t budget_us, const KitValues& values) noexcept;

  enum class SettingsLoad : std::uint8_t {
    Unavailable,  // no card capability
    NoCard,       // the card is not mounted
    NoFile,       // none on the card (a first run): the defaults stay
    Loaded,       // `values` now hold the file's
    FromBackup,   // system.o3s was gone and system.o3s.bak was read (a save was cut): the next save of the settings heals the name
    Refused,      // the file is damaged or of a newer major version: the defaults stay (settings_error() says which)
    NotRead,      // load_settings() has not been called (settings_load() before the boot's read)
  };
  // At boot, after the mount and before the loop: one read of at most settings::kMaxBytes. A key the file lacks keeps the value in `values` (the app's default), one it has wins.
  // What `values` is after this is what counts as saved: nothing is written until it changes. The file is read from the card again whenever a card is put in (poll() does it: see
  // take_loaded()), and nothing is ever written for a card whose file has not been read, or found missing: a card put in later never has its settings overwritten by defaults.
  //   * system.o3s missing: the backup is read (a save cut between its renames). Damaged (CRC, truncated, malformed): the backup is tried before the defaults.
  //   * A newer major version, a file past kMaxBytes, or a file the card cannot read: the values stay in memory for the session and NOTHING is written to that card until it is taken out
  //     (settings_protected(): the user's file, and its .bak, are never overwritten by an older firmware; the strip says so once).
  SettingsLoad load_settings(KitValues& values) noexcept;
  // poll() read the settings of a card put in after the boot: the values to apply (Runtime does, as it does after load_settings). true once per read that found a file.
  bool take_loaded(KitValues& out) noexcept {
    if (!loaded_pending_) {
      return false;
    }
    loaded_pending_ = false;
    out = loaded_values_;
    return true;
  }
  bool settings_protected() const noexcept { return sread_ == SettingsRead::Protected; }
  SettingsLoad settings_load() const noexcept { return settings_load_; }  // what the last load_settings() found
  settings::Result settings_error() const noexcept { return settings_result_; }
  const settings::Report& settings_report() const noexcept { return settings_report_; }

  // The card was taken out, put in or handed over (the platform's card watch): generation() moves.
  void card_changed() noexcept {
    ++generation_;
    resume_ = resume_ || sread_ == SettingsRead::Writable;  // (the same card back from the computer with settings still due: poll() keeps them, see resume_)
    sread_ = SettingsRead::NotRead;  // a card put in: its settings are read before anything is written to it
  }

  // The strip line the files service wants this pass: false when there is none. `result`: the job has ended (shown for a few seconds); otherwise the line is kept up while it runs.
  struct Strip {
    char text[kStripChars];
    int progress;  // 0..1000, or -1
    bool result;
  };
  bool strip(Strip& out) noexcept;

  // The power-off (power/power_service.h): power_begin() once at its first stage: the settings are written at once, and no new request is taken; power_busy() while a job runs or the
  // settings flush is due (a card that cannot be written does not hold the shutdown); past_write() the running save is reading back or renaming (a cancel then is a rollback of a nearly
  // finished save); power_cancel() abandons the job and the flush.
  void power_begin() noexcept;
  bool power_busy() noexcept;
  bool past_write() const noexcept;
  void power_cancel() noexcept;

  const Stats& stats() const noexcept { return stats_; }
  bool settings_dirty() const noexcept { return seen_ && sig_ != saved_; }
  // The kit's settings are waiting for a write that will happen (changed since written, on a card that takes writes) or are being written: what CARD AS DISK waits out before it hands the card over.
  bool settings_pending() const noexcept {
    return kind_ == Kind::Settings || (seen_ && sig_ != saved_ && sread_ == SettingsRead::Writable && !gave_up_ && card() == Card::Ready);
  }
  // A job of the app (a save, a load, a listing, a request waiting to start), not the kit's own settings write.
  bool app_busy() const noexcept { return (kind_ != Kind::None && kind_ != Kind::Settings) || pend_ != Kind::None; }
  // The settings that are due are written in the next pass, without the rest of the 1.5 s of quiet.
  void flush_settings() noexcept { flush_ = true; }

 private:
  enum class Kind : std::uint8_t { None, Save, SaveStream, Wav, Load, LoadStream, List, Marker, Settings };
  enum class Stage : std::uint8_t { CheckFolder, Number, Open, Run, Sizes };

  // The runner's job: a thin adapter, so that the app-facing cancel() is not the runner's.
  class Pump final : public core::Job {
   public:
    explicit Pump(Files& f) noexcept : f_(f) {}
    core::JobStatus step(std::uint32_t budget_us) noexcept override;
    unsigned progress() const noexcept override;
    void cancel() noexcept override;
    const char* name() const noexcept override;
    const char* detail() const noexcept override;

   private:
    Files& f_;
  };

  struct Request {
    const void* data = nullptr;       // Save, Wav (pcm)
    std::size_t bytes = 0;
    SaveSource* source = nullptr;     // SaveStream
    void* dst = nullptr;              // Load
    std::size_t cap = 0;              // Load, List
    core::ByteSink* sink = nullptr;   // LoadStream
    Entry* out = nullptr;             // List
    std::uint32_t frames = 0;         // Wav
    std::uint32_t rate = 0;
    std::uint8_t channels = 0;
    Mode mode = Mode::Replace;
  };

  bool guard_request(bool writes) noexcept;
  static bool folder_ok(const char* folder, bool allow_samples) noexcept;
  bool accept(Kind kind, const char* folder, const char* name, bool writes, bool allow_samples) noexcept;
  bool refuse(const char* text) noexcept;
  static const char* card_text(Card c, bool writes) noexcept;
  void note_card() noexcept;

  void launch() noexcept;
  void launch_settings() noexcept;
  bool begin_save_job(const char* name, Mode mode, SaveSource& source) noexcept;
  bool start_job() noexcept;
  void set_label(const char* verb, const char* name) noexcept;
  void reap() noexcept;
  void finish(bool done, bool cancelled) noexcept;
  void abort_op() noexcept;
  void drop_pending(const char* text) noexcept;
  void track_settings(const KitValues& values, std::uint32_t now_ms) noexcept;

  core::JobStatus run_step(std::uint32_t budget_us) noexcept;
  core::JobStatus fail(const char* text) noexcept;
  core::JobStatus step_save(std::uint32_t budget_us) noexcept;
  core::JobStatus step_number() noexcept;
  core::JobStatus step_load(std::uint32_t budget_us) noexcept;
  core::JobStatus step_list(std::uint32_t budget_us) noexcept;
  core::JobStatus step_marker() noexcept;
  bool open_for_load() noexcept;
  void close_file() noexcept;
  core::JobStatus step_collect(std::uint32_t budget_us) noexcept;
  enum class Small : std::uint8_t { Ok, Missing, TooBig, Io, Busy };
  Small read_small(const char* path, std::uint8_t* buf, std::size_t cap, std::size_t& got) noexcept;
  SettingsLoad read_settings_file(KitValues& values, bool& retry) noexcept;
  void poll_settings_read(const KitValues& values) noexcept;
  unsigned job_progress() const noexcept;

  // The card capability.
  hal::storage::Storage* storage_ = nullptr;
  hal::storage::StorageWriter* writer_ = nullptr;
  Work* w_ = nullptr;
  const char* id_ = "";
  bool (*claim_)() noexcept = nullptr;
  bool ready_ = false;

  // The one job.
  core::JobRunner jobs_;
  Pump pump_{*this};
  Kind kind_ = Kind::None;     // the job that is running
  Kind pend_ = Kind::None;     // a request recorded and not started (it waits at most for the settings job, which gives way)
  Stage stage_ = Stage::Run;
  int slot_ = -1;
  bool pinned_ = false;        // the writer was told the save began (begin_save): end_save() is owed
  Request req_;
  core::FixedString<40> label_;  // "SAVE RIFF.O3A": the job's name

  // The last result, for status() and the console.
  Status::Phase phase_ = Status::Idle;
  const char* text_ = "";
  const char* fail_text_ = "";
  std::size_t listed_ = 0;
  bool truncated_ = false;
  std::uint32_t loaded_ = 0;
  bool recovered_ = false;
  bool quiet_ = false;
  core::FixedString<kStripChars> notice_;
  bool notice_fresh_ = false;
  Stats stats_;

  // The card's identity as the last pass saw it: generation_ moves when it changes.
  std::uint32_t generation_ = 1;
  Card last_card_ = Card::None;
  std::uint32_t last_card_id_ = 0;
  std::uint32_t started_generation_ = 0;  // generation_ when the load or list began

  // The load.
  const hal::Clock* clock_ = nullptr;  // the job pass's clock, for the steps that bound themselves by time as well as by count
  hal::storage::FileHandle file_{};
  std::uint32_t file_size_ = 0;
  std::uint32_t read_ = 0;
  std::size_t stage_have_ = 0;
  std::size_t stage_pos_ = 0;

  // The listing and the WAV's number.
  std::size_t list_pos_ = 0;  // the next entry of the scan's index to look at
  bool any_recover_ = false;
  unsigned wav_next_ = 1;

  // The kit's settings.
  KitValues sig_;    // the values as of the last pass
  KitValues saved_;  // the values last written (or read, or given up on)
  KitValues sent_;   // the values the running settings job writes
  enum class SettingsRead : std::uint8_t { NotRead, Writable, Protected };  // this card's file: not read yet, read (or none), or one that must not be overwritten
  SettingsRead sread_ = SettingsRead::NotRead;
  std::uint32_t read_id_ = 0;  // card_id() of the card the file was read from
  // The card was Writable and went (to the computer, or out): when the SAME card is back (read_id_) and the values are still due (they changed since they were written: edits made while the
  // computer had it), its file is not read over them: they are kept and written after the debounce (docs/M2C-PLAN.md section 7). Cleared by a read and when the card is gone.
  bool resume_ = false;
  KitValues loaded_values_;
  bool loaded_pending_ = false;
  bool settings_note_ = false;  // the strip owes the user the word that their file is newer and will not be written
  bool seen_ = false;
  std::uint32_t moved_ms_ = 0;
  bool power_ = false;
  bool flush_ = false;  // flush_settings(): cleared when nothing is due
  bool gave_up_ = false;
  SettingsLoad settings_load_ = SettingsLoad::NotRead;
  settings::Result settings_result_ = settings::Result::Ok;
  settings::Report settings_report_;
};

}  // namespace o3sdk::app
