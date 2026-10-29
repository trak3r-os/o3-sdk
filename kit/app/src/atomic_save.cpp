// SPDX-License-Identifier: GPL-3.0-or-later
// The atomic save (OS-007n): the state machine of app/atomic_save.h. Every change of the card goes through hal::storage::StorageWriter, which applies the path
// guard and the card gate itself; this file only builds its paths from the guard's own names and keeps to the sequence documented in the header.
#include <o3sdk/app/atomic_save.h>
#include <o3sdk/app/og_timing.h>
#include <o3sdk/hal/app_guard.h>

#include <cstring>

namespace o3sdk::app {

namespace {

namespace guard = hal::storage::app_guard;
using hal::storage::Status;
using hal::storage::WriteStatus;

}  // namespace

core::IoResult AtomicSave::Sink::write(const void* src, std::size_t n) noexcept {
  if (status != WriteStatus::Ok || n == 0u || writer == nullptr) {
    return {0, core::IoStatus::Failed};
  }
  if (n > 0xFFFFFFFFu - total) {
    status = WriteStatus::NoSpace;  // a file the card's file system cannot hold
    return {0, core::IoStatus::Failed};
  }
  {
    const OgTiming::Scope timed(timing, OgTiming::Stage::Write, false);  // (T16) the card's share of a step
    status = writer->write(src, n);
  }
  if (status != WriteStatus::Ok) {
    return {0, core::IoStatus::Failed};
  }
  crc.update(static_cast<const std::uint8_t*>(src), n);
  total += static_cast<std::uint32_t>(n);
  return {n, core::IoStatus::Ok};
}

AtomicSave::Error AtomicSave::from_write(WriteStatus s) noexcept {
  switch (s) {
    case WriteStatus::Ok: return Error::None;
    case WriteStatus::NoCard:
    case WriteStatus::Host: return Error::NoCard;  // (the hand-over refuses while a job runs; a job that still met Host ends as for a pulled card)
    case WriteStatus::WriteProtected: return Error::WriteProtected;
    case WriteStatus::Unsupported: return Error::Unsupported;
    case WriteStatus::BadPath: return Error::Internal;
    case WriteStatus::Exists: return Error::Exists;
    case WriteStatus::NoSpace: return Error::NoSpace;
    case WriteStatus::Busy: return Error::Busy;
    case WriteStatus::Damaged: return Error::Damaged;
    case WriteStatus::NotFound:
    case WriteStatus::Io: break;
  }
  return Error::Io;
}

bool AtomicSave::begin(hal::storage::Storage& card, hal::storage::StorageWriter& writer, const char* dir, const char* name, Mode mode, SaveSource& source,
                       const char* label, const char* verb) noexcept {
  if (running() && writer_ != nullptr) {
    repair();  // a save in progress is given up
  }
  stage_ = Stage::Idle;
  error_ = Error::None;
  timing_ = nullptr;
  sink_.timing = nullptr;
  const std::size_t name_len = name != nullptr ? std::strlen(name) : 0u;
  char backup_name[guard::kPathCap];
  if (dir == nullptr || name == nullptr || name_len + sizeof guard::kBackupSuffix > sizeof backup_name ||
      !guard::join(dir_, sizeof dir_, dir) || !guard::join(target_, sizeof target_, dir, name) || !guard::join(temp_, sizeof temp_, dir, guard::kTempName)) {
    return false;
  }
  std::memcpy(backup_name, name, name_len);
  std::memcpy(backup_name + name_len, guard::kBackupSuffix, sizeof guard::kBackupSuffix);
  if (!guard::join(backup_, sizeof backup_, dir, backup_name) || guard::classify(dir_) != guard::Place::Folder || guard::classify(target_) != guard::Place::Target ||
      guard::classify(temp_) != guard::Place::Temp || (mode == Mode::Replace && guard::classify(backup_) != guard::Place::Backup)) {
    return false;
  }
  card_ = &card;
  writer_ = &writer;
  source_ = &source;
  mode_ = mode;
  have_target_ = have_backup_ = have_temp_ = created_ = moved_old_ = healed_ = unchanged_ = false;
  held_ = nullptr;
  verify_ = hal::storage::FileHandle{};
  dir_pos_ = 0;
  verified_ = expect_total_ = expect_crc_ = 0;
  sink_.writer = &writer;
  sink_.crc.reset();
  sink_.total = 0;
  sink_.status = WriteStatus::Ok;
  name_.assign(verb != nullptr ? verb : "SAVE ");
  name_.append(label != nullptr ? label : "");
  stage_ = Stage::Gate;
  return true;
}

// Puts the card back as a failure leaves no excuse for: the file being written is closed, F is moved back from B if this job moved it, and T is removed. Each call is
// best effort (a card that is gone fails them all, and the table in the header says what that leaves).
void AtomicSave::repair() noexcept {
  if (source_ != nullptr) {
    source_->end();  // the save is over, whatever the card says next: what the source holds is let go first
  }
  if (writer_ == nullptr) {
    return;
  }
  if (writer_->writing()) {
    writer_->abandon();
  }
  if (verify_.is_open() && card_ != nullptr) {
    card_->close(verify_);
  }
  if (moved_old_) {
    if (writer_->rename(backup_, target_) == WriteStatus::Ok) {
      moved_old_ = false;
    }
  }
  if (created_) {
    if (writer_->remove(temp_) == WriteStatus::Ok) {
      created_ = false;
    }
  }
}

void AtomicSave::fail(Error e) noexcept {
  repair();
  error_ = e;
  stage_ = Stage::Failed;
}

void AtomicSave::cancel() noexcept {
  if (running()) {
    repair();
    stage_ = Stage::Failed;  // error() stays None: nothing went wrong
  }
}

// Makes the next folder of `dir_` ("Projects", then "Projects/Track 01"); true once there is none left.
bool AtomicSave::next_dir() noexcept {
  const std::size_t total = std::strlen(dir_);
  if (dir_pos_ >= total) {
    return true;
  }
  std::size_t end = dir_pos_;
  while (end < total && dir_[end] != '/') {
    ++end;
  }
  char prefix[guard::kPathCap];
  std::memcpy(prefix, dir_, end);
  prefix[end] = '\0';
  const WriteStatus s = writer_->make_dir(prefix);
  if (s != WriteStatus::Ok) {
    fail(from_write(s));
    return false;
  }
  dir_pos_ = end < total ? end + 1u : total;
  return dir_pos_ >= total;
}

// The stage of the timing report that the rest of a step in `s` belongs to: what is left of it once the f_write calls (Write) and the source's set-up (Convert) are taken out.
OgTiming::Stage AtomicSave::timing_stage(Stage s) noexcept {
  switch (s) {
    case Stage::Write: return OgTiming::Stage::Convert;
    case Stage::Finish: return OgTiming::Stage::Sync;
    case Stage::VerifyOpen:
    case Stage::VerifyRead:
    case Stage::CompareOpen:
    case Stage::CompareRead: return OgTiming::Stage::Verify;
    case Stage::Restore:
    case Stage::RemoveTemp:
    case Stage::DropBackup:
    case Stage::MoveOld:
    case Stage::Commit: return OgTiming::Stage::Rename;
    case Stage::Idle:
    case Stage::Done:
    case Stage::Failed: return OgTiming::Stage::Other;
    case Stage::Gate:
    case Stage::MakeDirs:
    case Stage::Probe:
    case Stage::Create: break;
  }
  return OgTiming::Stage::Dir;
}

core::JobStatus AtomicSave::step(std::uint32_t budget_us) noexcept {
  OgTiming* const timing = timing_ != nullptr && timing_->on() ? timing_ : nullptr;  // (T16) off: one branch
  const Stage entered = stage_;
  const std::uint32_t started = timing != nullptr ? timing->now() : 0u;
  if (timing != nullptr) {
    timing->begin_step();
  }
  step_stage(budget_us);
  if (timing != nullptr) {
    timing->end_step(timing_stage(entered), started);
  }
  return stage_ == Stage::Done ? core::JobStatus::Done : (stage_ == Stage::Failed ? core::JobStatus::Failed : core::JobStatus::Running);
}

void AtomicSave::step_stage(std::uint32_t budget_us) noexcept {
  bool is_dir = false;
  switch (stage_) {
    case Stage::Idle:
    case Stage::Done:
    case Stage::Failed:
      break;
    case Stage::Gate: {
      const WriteStatus g = writer_->gate();
      if (g != WriteStatus::Ok) {
        fail(from_write(g));
      } else {
        stage_ = Stage::MakeDirs;
      }
      break;
    }
    case Stage::MakeDirs:
      if (next_dir() && stage_ == Stage::MakeDirs) {
        stage_ = Stage::Probe;
      }
      break;
    case Stage::Probe: {
      const auto look = [&](const char* path, bool& found) {
        const Status s = card_->exists(path, &is_dir);
        if (s != Status::Ok && s != Status::NotFound) {
          return false;
        }
        found = s == Status::Ok && !is_dir;
        if (s == Status::Ok && is_dir) {
          return false;  // a folder where a file belongs: never touched
        }
        return true;
      };
      if (!look(target_, have_target_) || !look(temp_, have_temp_) || (mode_ == Mode::Replace && !look(backup_, have_backup_))) {
        fail(is_dir ? Error::Exists : Error::Io);
        break;
      }
      if (mode_ == Mode::NewOnly && have_target_) {
        fail(Error::Exists);
      } else if (mode_ == Mode::Replace && !have_target_ && have_backup_) {
        stage_ = Stage::Restore;
      } else if (mode_ == Mode::Replace && have_target_ && held_ != nullptr) {
        stage_ = Stage::CompareOpen;
      } else {
        stage_ = have_temp_ ? Stage::RemoveTemp : Stage::Create;
      }
      break;
    }
    case Stage::Restore: {  // step 2 of the header: a cut between its steps 8 and 9 left no F, and B is the old file
      const WriteStatus s = writer_->rename(backup_, target_);
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      healed_ = true;
      have_target_ = true;
      have_backup_ = false;
      stage_ = have_temp_ ? Stage::RemoveTemp : Stage::Create;
      break;
    }
    case Stage::CompareOpen: {  // step 2b: any doubt is a write, as without the check
      std::uint32_t size = 0;
      if (card_->open(target_, verify_) == Status::Ok) {
        if (card_->size(verify_, size) == Status::Ok && held_->start(size)) {
          verified_ = 0;
          expect_total_ = size;
          stage_ = Stage::CompareRead;
          break;
        }
        card_->close(verify_);
      }
      stage_ = have_temp_ ? Stage::RemoveTemp : Stage::Create;
      break;
    }
    case Stage::CompareRead: {
      std::size_t bytes = budget_us > kMaxStepBytes / kReadBytesPerUs ? kMaxStepBytes : static_cast<std::size_t>(budget_us) * kReadBytesPerUs;
      bytes = bytes < kMinStepBytes ? kMinStepBytes : bytes;
      formats::Progress p = formats::Progress::Running;
      while (bytes != 0u && p == formats::Progress::Running) {  // (every pass takes at least a byte or ends: bounded by the file's size)
        std::size_t got = 0;
        const std::size_t ask = bytes < sizeof buffer_ ? bytes : sizeof buffer_;
        if (card_->read(verify_, buffer_, ask, got) != Status::Ok || got == 0u || got > ask) {
          p = formats::Progress::Failed;  // a read that fails, or the end of the file before the answer: it does not hold it
          break;
        }
        verified_ += static_cast<std::uint32_t>(got);
        bytes -= got;
        p = held_->feed(buffer_, got);
      }
      if (p == formats::Progress::Running) {
        break;
      }
      card_->close(verify_);
      if (p == formats::Progress::Done) {
        source_->end();  // nothing is written: what the source holds is let go as at the end of the write
        unchanged_ = true;
        stage_ = Stage::Done;
      } else {
        verified_ = expect_total_ = 0;
        stage_ = have_temp_ ? Stage::RemoveTemp : Stage::Create;
      }
      break;
    }
    case Stage::RemoveTemp: {
      const WriteStatus s = writer_->remove(temp_);  // Damaged when T shares its data with another name: nothing is removed
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
      } else {
        stage_ = Stage::Create;
      }
      break;
    }
    case Stage::Create: {
      const WriteStatus s = writer_->create(temp_);
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      created_ = true;
      {
        const OgTiming::Scope timed(timing_, OgTiming::Stage::Convert, false);  // (T16) the source's set-up is not the directory's
        source_->begin(sink_);
      }
      stage_ = Stage::Write;
      break;
    }
    case Stage::Write: {
      std::size_t bytes = budget_us > kMaxStepBytes / kBytesPerUs ? kMaxStepBytes : static_cast<std::size_t>(budget_us) * kBytesPerUs;
      bytes = bytes < kMinStepBytes ? kMinStepBytes : bytes;
      const formats::Progress p = source_->step(bytes);
      if (sink_.status != WriteStatus::Ok) {
        fail(from_write(sink_.status));
      } else if (p == formats::Progress::Failed) {
        fail(Error::Source);
      } else if (p == formats::Progress::Done) {
        source_->end();  // its last byte is written: the read-back and the renames need nothing of it
        stage_ = Stage::Finish;
      }
      break;
    }
    case Stage::Finish: {
      const WriteStatus s = writer_->finish();
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      expect_total_ = sink_.total;
      expect_crc_ = sink_.crc.value();
      stage_ = Stage::VerifyOpen;
      break;
    }
    case Stage::VerifyOpen: {
      const Status s = card_->open(temp_, verify_);
      if (s != Status::Ok) {
        fail(s == Status::NoHandle ? Error::Busy : Error::Verify);
        break;
      }
      std::uint32_t size = 0;
      if (card_->size(verify_, size) != Status::Ok || size != expect_total_) {
        fail(Error::Verify);
        break;
      }
      verify_crc_.reset();
      verified_ = 0;
      stage_ = Stage::VerifyRead;
      break;
    }
    case Stage::VerifyRead: {
      std::size_t got = 0;
      if (card_->read(verify_, buffer_, sizeof buffer_, got) != Status::Ok) {
        fail(Error::Verify);
        break;
      }
      if (got != 0u) {
        verify_crc_.update(buffer_, got);
        verified_ += static_cast<std::uint32_t>(got);
        if (verified_ > expect_total_) {
          fail(Error::Verify);
        }
        break;
      }
      card_->close(verify_);
      if (verified_ != expect_total_ || verify_crc_.value() != expect_crc_) {
        fail(Error::Verify);
      } else if (mode_ == Mode::NewOnly) {
        stage_ = Stage::Commit;
      } else {
        stage_ = have_backup_ ? Stage::DropBackup : (have_target_ ? Stage::MoveOld : Stage::Commit);
      }
      break;
    }
    case Stage::DropBackup: {  // step 7: our own previous backup, and only now that the new file is checked
      const WriteStatus s = writer_->remove(backup_);
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      have_backup_ = false;
      stage_ = have_target_ ? Stage::MoveOld : Stage::Commit;
      break;
    }
    case Stage::MoveOld: {  // step 8
      const WriteStatus s = writer_->rename(target_, backup_);
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      moved_old_ = true;
      stage_ = Stage::Commit;
      break;
    }
    case Stage::Commit: {  // step 6 (NewOnly) or 9
      const WriteStatus s = writer_->rename(temp_, target_);  // refused when the name appeared meanwhile: nothing is replaced
      if (s != WriteStatus::Ok) {
        fail(from_write(s));
        break;
      }
      created_ = false;
      moved_old_ = false;
      stage_ = Stage::Done;
      break;
    }
  }
}

unsigned AtomicSave::progress() const noexcept {
  switch (stage_) {
    case Stage::Idle:
    case Stage::Gate:
    case Stage::MakeDirs:
    case Stage::Probe:
    case Stage::Restore:
    case Stage::CompareOpen: return 0;
    case Stage::RemoveTemp:
    case Stage::Create: return held_ != nullptr ? 50u : 0u;  // after the held check's read (0 .. 5 %) the write is next: the bar does not fall back to 0 between them
    case Stage::CompareRead: return core::permille_of(verified_, expect_total_) / 20u;  // 0 .. 5 %: the write starts at 5 %
    case Stage::Write: {
      const int own = source_ != nullptr ? source_->progress_permille() : -1;
      if (own >= 0) {
        return 50u + static_cast<unsigned>(own > 1000 ? 1000 : own) * 4u / 10u;  // 5 % .. 45 %, by the source's own account
      }
      const std::uint32_t hint = source_ != nullptr ? source_->size_hint() : 0u;
      return 50u + core::permille_of(sink_.total, hint != 0u ? hint : sink_.total + 1u) * 4u / 10u;  // 5 % .. 45 %
    }
    case Stage::Finish:
    case Stage::VerifyOpen: return 450;
    case Stage::VerifyRead: return 450u + core::permille_of(verified_, expect_total_) / 2u;  // 45 % .. 95 %
    case Stage::DropBackup:
    case Stage::MoveOld:
    case Stage::Commit: return 950;
    case Stage::Done:
    case Stage::Failed: break;
  }
  return stage_ == Stage::Done ? core::kJobProgressMax : 0u;
}

const char* AtomicSave::detail() const noexcept {
  switch (error_) {
    case Error::None: return "";
    case Error::NoCard: return "NO CARD";
    case Error::WriteProtected: return "CARD WRITE PROTECTED";
    case Error::Unsupported: return "EXFAT CARD NOT SUPPORTED";
    case Error::Exists: return "NAME EXISTS";
    case Error::NoSpace: return "CARD IS FULL";
    case Error::Busy: return "BUSY";
    case Error::Damaged: return "CARD NEEDS A CHECK";
    case Error::Source: return source_ != nullptr && source_->failure()[0] != '\0' ? source_->failure() : "SOURCE FAILED";
    case Error::Verify: return "VERIFY FAILED";
    case Error::Io: return "I/O ERROR";
    case Error::Internal: return "INTERNAL ERROR";
  }
  return "";
}

}  // namespace o3sdk::app
