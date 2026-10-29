// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The atomic save (OS-007n): one file of the card replaced, or created, so that a power cut, a pulled card or a failing write at ANY step leaves the user either
// the old file or the new one, whole, and never a half-written final file. A core::Job: the main loop gives it a slice of every pass (a chunk of a write, a
// chunk of a read-back, one directory operation per step), so the screen keeps running. It writes through hal::storage::StorageWriter (which refuses everything
// the path guard of hal/save_guard.h does not name, and everything while no writable FAT card is mounted: hal/storage_write.h) and reads back
// through hal::storage::Storage.
//
// The sequence (T = <dir>/_o3save.tmp, F = <dir>/<name>, B = F + ".bak"; FAT has no atomic replace, f_rename refuses an existing target):
//   1  gate, make the folders          nothing of the user's is touched
//   2  heal: F missing and B present (a cut between steps 8 and 9 of an earlier save): rename B -> F; a stale T is removed. Only a save of that same file runs it: see
//      "Getting the old file back" below for what lets the user, and the boot, get as far as such a save
//   2b Replace only, and only when the caller gave a HeldCheck (OS-BUG-OGSAVE): F is read, a chunk a step, and asked whether it already holds what the source would write (a sample
//      WAV the folder has in another form: an official one with an odd data chunk and no pad byte). If it does the job ends Done with unchanged() true and NOTHING is written, renamed or
//      removed: no T, no B. Any doubt (it differs, the read fails, no handle, the file ends early) goes on to 3 as if there were no check.
//   3  create T (never over an existing file), write it chunk by chunk (the source produces the bytes, the sink counts and sums them)
//   4  sync + close T
//   5  read T back through the read-only side: its size and CRC-32 must be those of what was written (a card that took the write but cannot give it back fails here)
//   6  NewOnly: rename T -> F (refused if F appeared: nothing is replaced). Replace: 7, 8, 9
//   7  remove B (our own previous backup) if there is one
//   8  rename F -> B
//   9  rename T -> F
// What a power cut leaves (every row: the user's data is whole in at least one of the names; "heal" is step 2 of a later save of that file, which only runs if something
// reaches it: see "Getting the old file back"):
//   during 1, 2     F as it was (heal may have restored it from B)
//   during 2b       F as it was (the check only reads)
//   during 3, 4, 5  F as it was (or none), T partial or complete but unchecked: the next save removes T; nothing reads T
//   after 7         F as it was, T complete and checked, no B
//   during 8        F and B both whole (a cut inside the rename can leave both names on the same data; the writer then refuses to remove either: "card needs a check")
//   after 8         no F; B = the old file, whole; T = the new file, whole. A later save of the file heals (B back as F: the save that was cut is lost, the old version is
//                   not); until then the readers use B (below)
//   during 9        as after 8, or F and T both the new file (the same cross-link rule)
//   after 9         F = the new file, B = the old one, no T
//   NewOnly: during 6, F is absent or is T renamed; there is no B and nothing of the user's to lose.
// Getting the old file back (OS-007o). After the cut "after 8" there is no F: nothing opens the file by its name, and a Replace save is the only thing here that renames B -> F. So
// the readers know the backup's name, and each is read only:
//   project   the File list shows the folder as RECOVER (no project.o3pj, a project.o3pj.bak: app/dir_scan.h); OPEN reads the backup as it would the file (app/native_load.h,
//             `backup`); the project is then in memory and the folder is the one it came from, so the next SAVE is a Replace into it: step 2 renames B -> F, the cut save's stale T
//             goes, and the save goes on as any other (the recovered version ends as the new file's backup). Nothing is renamed by the OPEN: it runs on a card that may not be written.
//   settings  load_device_settings (app/save_service.h) reads device.o3st.bak when there is no device.o3st; the next save of the settings heals the name.
// The new version the cut save had put in T is not promoted: it was checked by the read-back of step 5, but only the running job knew that, and a file named T may be anything.
// A failure that is not a power cut (no space, a read-back that differs, the source failing, a rename refused) runs the same repairs at once: T is abandoned and
// removed, and if F had already moved to B it is moved back. Nothing but T and B is ever removed, and B only after T has been checked. Maintainer-approved policy (2026-10-12): the saver
// removes its own old backup, and the device settings save themselves after a change has stayed still (save_service.h).

#include <o3sdk/app/og_timing.h>
#include <o3sdk/core/byte_io.h>
#include <o3sdk/core/fixed_string.h>
#include <o3sdk/core/job.h>
#include <o3sdk/formats/crc32.h>
#include <o3sdk/formats/progress.h>
#include <o3sdk/hal/save_guard.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// What a save writes: the bytes of one file, produced a slice at a time into the sink the job hands over. begin() starts (or restarts) it.
class SaveSource {
 public:
  virtual void begin(core::ByteSink& sink) noexcept = 0;
  virtual formats::Progress step(std::size_t byte_budget) noexcept = 0;
  // After Failed: why, in a few upper-case words ("CHANGED WHILE WRITING"). "" when it has nothing to say.
  virtual const char* failure() const noexcept { return ""; }
  // The size of the file when it is known up front (0: not yet, or not known); it only scales the progress bar.
  virtual std::uint32_t size_hint() const noexcept { return 0; }
  // How far the source is, 0..1000, when it knows better than the bytes it has written (an export that renders before it writes: app/export_source.h); -1: the bytes tell (size_hint).
  virtual int progress_permille() const noexcept { return -1; }
  // The job will not step this source again: it has produced its last byte, or the save failed or was cancelled (also before begin() was called). Idempotent; what the source holds
  // for the save (the engine, for an export) is let go here.
  virtual void end() noexcept {}

 protected:
  SaveSource() = default;
  ~SaveSource() = default;
  SaveSource(const SaveSource&) = delete;
  SaveSource& operator=(const SaveSource&) = delete;
};

// OS-BUG-OGSAVE: what a Replace save may ask before it writes anything: does the file it would replace already hold what the source would write? The job reads the file in order, a chunk
// a step, and hands the bytes over; the answer is the object's (WavSaveSource: the same whole frames in the same format, whatever else the file's header holds).
class HeldCheck {
 public:
  // The file's size, before a byte is read; false: it cannot hold it (the job writes as it would without the check).
  virtual bool start(std::uint32_t file_size) noexcept = 0;
  // The next bytes of the file. Running: more are wanted; Done: it holds it (the rest of the file is not read); Failed: it does not.
  virtual formats::Progress feed(const std::uint8_t* data, std::size_t n) noexcept = 0;

 protected:
  HeldCheck() = default;
  ~HeldCheck() = default;
  HeldCheck(const HeldCheck&) = delete;
  HeldCheck& operator=(const HeldCheck&) = delete;
};

class AtomicSave final : public core::Job {
 public:
  enum class Mode : std::uint8_t { NewOnly, Replace };  // NewOnly: F must not exist (a WAV, Save As). Replace: F is replaced, the old one kept as B (project, settings)
  enum class Error : std::uint8_t {
    None,
    NoCard,
    WriteProtected,
    Unsupported,  // exFAT
    Exists,       // NewOnly: the name is taken
    NoSpace,
    Busy,         // no file handle to read back with, or the writer is in use
    Damaged,      // cross-linked names: the card needs a check
    Source,       // the source failed (the project changed while it was written, ...)
    Verify,       // the read-back differs from what was written
    Io,
    Internal,     // the guard refused a path this job built: a bug, never input
  };

  static constexpr std::uint32_t kBytesPerUs = 2;  // the write side of the cost model of ImportJob: about 2 MB/s of card, PROVISIONAL until measured (JobStats::max_step_us)
  static constexpr std::size_t kMinStepBytes = 512;
  static constexpr std::size_t kMaxStepBytes = 8192;
  static constexpr std::size_t kVerifyChunk = 2048;
  static constexpr std::uint32_t kReadBytesPerUs = 4;  // the read side (HeldCheck): ImportJob's 4 MB/s of card, PROVISIONAL until measured (JobStats::max_step_us)

  // Starts a save of `dir`/`name` from `source`. `dir` is a folder of the guard's list ("Projects/Track 01", "Samples", "Settings"), `name` the final file in it; Replace needs
  // a name that has a Backup form (project.o3pj, device.o3st). `label` is the short text of the notification ("TRACK 01"). False, nothing started, when a path is not one the
  // guard knows or does not fit. Everything given must stay valid until the job has ended. The job is not a Job of the runner until the caller starts it there.
  // `verb` is the first word of the job's name on the notification strip ("SAVE TRACK 01", "EXPORT SONG0001").
  bool begin(hal::storage::Storage& card, hal::storage::StorageWriter& writer, const char* dir, const char* name, Mode mode, SaveSource& source, const char* label,
             const char* verb = "SAVE ") noexcept;

  // OS-BUG-OGSAVE: Replace only, after begin(): before it writes, the job asks `held` whether F already holds what the source would write, and ends Done with nothing touched when it does. It must stay valid until the job ends.
  void set_held(HeldCheck* held) noexcept { held_ = held; }
  bool unchanged() const noexcept { return unchanged_; }  // Done by the check: F was left as it was

  // OS-FILES-C T16: a job whose steps are timed by stage (og_timing.h); null (begin() clears it): not. Main loop, set after begin().
  void set_timing(OgTiming* timing) noexcept {
    timing_ = timing;
    sink_.timing = timing;
  }

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  unsigned progress() const noexcept override;
  // The steps between two calls are short and the repairs run at once, so a cancel at any point leaves the card as a failure would.
  void cancel() noexcept override;
  const char* name() const noexcept override { return name_.c_str(); }
  const char* detail() const noexcept override;

  bool running() const noexcept { return stage_ != Stage::Idle && stage_ != Stage::Done && stage_ != Stage::Failed; }
  // The source has produced its last byte and the job is reading back, committing or moving the old file: from here a cancel is a rollback of a nearly finished save, so a caller that can choose lets it end.
  bool past_write() const noexcept { return stage_ == Stage::Finish || stage_ == Stage::VerifyOpen || stage_ == Stage::VerifyRead || stage_ == Stage::DropBackup || stage_ == Stage::MoveOld || stage_ == Stage::Commit; }
  bool done() const noexcept { return stage_ == Stage::Done; }
  bool failed() const noexcept { return stage_ == Stage::Failed; }
  Error error() const noexcept { return error_; }
  bool healed() const noexcept { return healed_; }            // step 2 put a backup back
  std::uint32_t bytes() const noexcept { return sink_.total; }  // bytes written to T so far
  const char* target() const noexcept { return target_; }     // the final path
  const char* dir() const noexcept { return dir_; }

 private:
  enum class Stage : std::uint8_t {
    Idle, Gate, MakeDirs, Probe, Restore, RemoveTemp, Create, Write, Finish, VerifyOpen, VerifyRead, DropBackup, MoveOld, Commit, CompareOpen, CompareRead, Done, Failed
  };

  class Sink final : public core::ByteSink {
   public:
    core::IoResult write(const void* src, std::size_t n) noexcept override;
    hal::storage::StorageWriter* writer = nullptr;
    OgTiming* timing = nullptr;  // times each f_write (T16)
    formats::Crc32 crc;
    std::uint32_t total = 0;
    hal::storage::WriteStatus status = hal::storage::WriteStatus::Ok;  // the first write that failed
  };

  void step_stage(std::uint32_t budget_us) noexcept;
  static OgTiming::Stage timing_stage(Stage s) noexcept;
  void fail(Error e) noexcept;
  void repair() noexcept;
  static Error from_write(hal::storage::WriteStatus s) noexcept;
  bool next_dir() noexcept;

  hal::storage::Storage* card_ = nullptr;
  hal::storage::StorageWriter* writer_ = nullptr;
  SaveSource* source_ = nullptr;
  Mode mode_ = Mode::NewOnly;
  Stage stage_ = Stage::Idle;
  Error error_ = Error::None;
  bool have_target_ = false;  // F existed at the probe
  bool have_backup_ = false;
  bool have_temp_ = false;
  bool created_ = false;      // T was created by this job (so repair removes it)
  bool moved_old_ = false;    // F was renamed to B by this job and not yet replaced
  bool healed_ = false;
  bool unchanged_ = false;    // OS-BUG-OGSAVE: Done because F already held what the source would write
  HeldCheck* held_ = nullptr;
  hal::storage::FileHandle verify_{};
  std::size_t dir_pos_ = 0;   // bytes of dir_ made into folders so far
  std::uint32_t verified_ = 0;
  std::uint32_t expect_total_ = 0;
  std::uint32_t expect_crc_ = 0;
  formats::Crc32 verify_crc_;
  Sink sink_;
  char dir_[hal::storage::save::kPathCap] = {};
  char target_[hal::storage::save::kPathCap] = {};
  char temp_[hal::storage::save::kPathCap] = {};
  char backup_[hal::storage::save::kPathCap] = {};
  core::FixedString<40> name_;
  std::uint8_t buffer_[kVerifyChunk];
  OgTiming* timing_ = nullptr;
};

}  // namespace o3sdk::app
