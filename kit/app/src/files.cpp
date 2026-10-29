// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/files.h>

#include <o3sdk/app/atomic_save.h>
#include <o3sdk/app/files_work.h>
#include <o3sdk/hal/app_guard.h>
#include <o3sdk/hal/storage_write.h>

#include <cstring>

// The app's id for the files service, when the app's build did not give one: the kit's default is weak and has none, the CMake function o3sdk_app() (cmake/O3sdkTargets.cmake) generates the
// app's own (strong, from the CMake id). A test rig that builds its own o3sdk_app() has none, and passes Platform::app_id if it wants files.
__attribute__((weak)) const char* o3sdk_app_id() noexcept { return nullptr; }

namespace o3sdk::app {

namespace guard = hal::storage::app_guard;
using St = hal::storage::Status;  // (Files::Status is the app's)
using hal::storage::WriteStatus;

namespace {

// What the listing reads of a folder: every folder, the allowlisted files and their backups (a lone backup is a file that needs recovery), nothing hidden. (DirScan ignores its skip_probe
// without a probe, a ported behaviour: the folders the DELETE marker hides are dropped by the step that looks up the sizes, below.)
constexpr ListFilter kListFilter = {".o3a .o3s .wav .mid .syx .txt .json .csv .bak", nullptr, true, nullptr, nullptr, nullptr, false, nullptr};

constexpr std::size_t kStatsPerStep = 8;      // directory lookups for the sizes of a listing, a step
constexpr unsigned kWavProbesPerStep = 8;      // names tried for the next WAV number, a step
constexpr unsigned kWavMax = 999;              // track3r-os's kMaxWavNumber
constexpr std::size_t kReadChunk = 4096;       // one read call of a whole-file load

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_ci(const char* a, const char* b) noexcept {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (lower(*a) != lower(*b)) {
      return false;
    }
  }
  return *a == *b;
}

bool ends_with_ci(const char* s, const char* tail) noexcept {
  const std::size_t n = std::strlen(s);
  const std::size_t t = std::strlen(tail);
  return n >= t && same_ci(s + (n - t), tail);
}

// `src` into `dst` (cap bytes with the NUL) in upper case; cut when it does not fit.
void upper_into(char* dst, std::size_t cap, const char* src) noexcept {
  std::size_t i = 0;
  for (; src[i] != '\0' && i + 1 < cap; ++i) {
    const char c = src[i];
    dst[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  dst[i] = '\0';
}

const char* write_text(WriteStatus s) noexcept {
  switch (s) {
    case WriteStatus::Ok: return "";
    case WriteStatus::NoCard: return "NO CARD";
    case WriteStatus::WriteProtected: return "CARD WRITE PROTECTED";
    case WriteStatus::Unsupported: return "EXFAT CARD NOT SUPPORTED";
    case WriteStatus::BadPath: return "PATH REFUSED";
    case WriteStatus::Exists: return "NAME EXISTS";
    case WriteStatus::NotFound: return "NOT FOUND";
    case WriteStatus::NoSpace: return "CARD IS FULL";
    case WriteStatus::Busy: return "BUSY";
    case WriteStatus::Damaged: return "CARD NEEDS A CHECK";
    case WriteStatus::Io: return "I/O ERROR";
    case WriteStatus::Host: return "CARD ON COMPUTER";
  }
  return "I/O ERROR";
}

const char* read_text(St s) noexcept {
  switch (s) {
    case St::NoCard:
    case St::NotMounted: return "NO CARD";
    case St::NotFound: return "NOT FOUND";
    case St::Unsupported: return "EXFAT CARD NOT SUPPORTED";
    case St::NoHandle: return "BUSY";
    default: return "I/O ERROR";
  }
}

}  // namespace

bool ScopedWriter::in_scope(const char* path) const noexcept {
  if (path == nullptr) {
    return false;
  }
  // `root` then end or '/': case-insensitive, as FAT is.
  const auto under = [&](const char* root, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (path[i] == '\0' || lower(path[i]) != lower(root[i])) {
        return false;
      }
    }
    return path[n] == '\0' || path[n] == '/';
  };
  if (scope_ == Scope::Screenshots) {  // the kit's screenshot: the one folder (hal/screenshot_file.h names its files)
    return under(hal::storage::save::kScreenshotsDir, sizeof hal::storage::save::kScreenshotsDir - 1u);
  }
  if (under(guard::kSamplesDir, sizeof guard::kSamplesDir - 1u)) {
    return true;
  }
  const std::size_t apps = sizeof guard::kAppsDir - 1u;
  if (!under(guard::kAppsDir, apps)) {
    return false;
  }
  if (path[apps] == '\0') {
    return true;  // Apps itself (a folder the first save makes)
  }
  const std::size_t id = std::strlen(id_);
  if (id == 0u) {
    return false;
  }
  for (std::size_t i = 0; i < id; ++i) {
    if (path[apps + 1u + i] == '\0' || lower(path[apps + 1u + i]) != lower(id_[i])) {
      return false;
    }
  }
  const char after = path[apps + 1u + id];
  return after == '\0' || after == '/';
}

// ---- The capability ---------------------------------------------------------------------------------------------------------------

void Files::bind(hal::storage::Storage* storage, hal::storage::StorageWriter* writer, Work* work, const char* app_id, bool (*claim)() noexcept) noexcept {
  storage_ = storage;
  w_ = work;
  claim_ = claim;
  ready_ = storage != nullptr && writer != nullptr && work != nullptr && guard::app_name_ok(app_id);
  id_ = ready_ ? app_id : "";
  writer_ = writer;
  if (ready_) {
    w_->scope.bind(writer, id_);
    writer_ = &w_->scope;  // every change the service makes is checked against Apps/<id>/ and Samples/ before it reaches the writer
  }
}

Files::Card Files::card() const noexcept {
  if (!ready_) {
    return Card::None;
  }
  if (claim_ != nullptr && claim_()) {
    return Card::InUse;
  }
  switch (writer_->gate()) {
    case WriteStatus::Ok: return Card::Ready;
    case WriteStatus::WriteProtected: return Card::ReadOnly;
    case WriteStatus::Unsupported: return Card::Unsupported;
    case WriteStatus::Host: return Card::OnComputer;
    default: return Card::None;
  }
}

const char* Files::card_text(Card c, bool writes) noexcept {
  switch (c) {
    case Card::None: return "NO CARD";
    case Card::OnComputer: return "CARD ON COMPUTER - EJECT FIRST";
    case Card::InUse: return "FIRMWARE UPDATE RUNNING";
    case Card::ReadOnly: return writes ? "CARD WRITE PROTECTED" : nullptr;
    case Card::Unsupported: return writes ? "EXFAT CARD NOT SUPPORTED" : nullptr;
    case Card::Ready: return nullptr;
  }
  return nullptr;
}

// The card as this pass sees it; generation() moves when it is another card, or the same one in another state (out, in, protected, the computer's, the update path's).
void Files::note_card() noexcept {
  const Card c = card();
  if (c == Card::None || c == Card::OnComputer) {
    resume_ = c == Card::OnComputer && (resume_ || sread_ == SettingsRead::Writable);  // (a card that is out is another card when it comes back: it is read)
    sread_ = SettingsRead::NotRead;  // the card is gone: whatever comes next is read before anything is written to it
  }
  const std::uint32_t id = c == Card::None || c == Card::InUse ? 0u : writer_->card_id();
  if (c != last_card_ || id != last_card_id_) {
    last_card_ = c;
    last_card_id_ = id;
    ++generation_;
  }
}

// ---- Requests: the calls of the app only record; launch() starts the job in the next pass -------------------------------------------

bool Files::refuse(const char* text) noexcept {
  phase_ = Status::Failed;
  text_ = text;
  listed_ = 0;
  if (!quiet_) {
    notice_.assign(text);
    notice_fresh_ = true;
  }
  return false;
}

bool Files::guard_request(bool writes) noexcept {
  if (!ready_) {
    return refuse("NO CARD");
  }
  if (power_) {
    return refuse("POWERING OFF");
  }
  if (busy() && !(kind_ == Kind::Settings && pend_ == Kind::None)) {
    return false;  // a job of the app's is there: the caller's request waits for its own turn (status keeps describing that job)
  }
  const char* why = card_text(card(), writes);
  return why == nullptr ? true : refuse(why);
}

bool Files::folder_ok(const char* folder, bool allow_samples) noexcept {
  if (folder == nullptr || folder[0] == '\0') {
    return true;
  }
  if (same_ci(folder, "Samples")) {
    return allow_samples;  // the card's Samples/, for a WAV only: not a project name
  }
  return guard::project_name_ok(folder);
}

bool Files::accept(Kind kind, const char* folder, const char* name, bool writes, bool allow_samples) noexcept {
  if (!guard_request(writes)) {
    return false;
  }
  if (!folder_ok(folder, allow_samples) || (name != nullptr && !guard::file_name_ok(name))) {
    return refuse("BAD NAME");
  }
  const char* f = folder != nullptr ? folder : "";
  if (writes && name != nullptr && f[0] == '\0' && same_ci(name, guard::kSystemFile)) {
    return refuse("RESERVED NAME");  // the kit's settings file: no call of the app's may change it
  }
  const bool samples = f[0] != '\0' && same_ci(f, "Samples");
  const bool ok = samples ? guard::join(w_->dir, sizeof w_->dir, guard::kSamplesDir)
                          : guard::join(w_->dir, sizeof w_->dir, guard::kAppsDir, id_, f[0] != '\0' ? f : nullptr);
  if (!ok) {
    return refuse("BAD NAME");
  }
  std::strncpy(w_->folder, samples ? "" : f, sizeof w_->folder - 1u);
  w_->folder[sizeof w_->folder - 1u] = '\0';
  if (name != nullptr) {
    std::strncpy(w_->name, name, sizeof w_->name - 1u);
    w_->name[sizeof w_->name - 1u] = '\0';
  }
  pend_ = kind;
  phase_ = Status::Running;
  text_ = kind == Kind::List ? "LIST" : (kind == Kind::Load || kind == Kind::LoadStream) ? "LOAD" : kind == Kind::Marker ? "DELETE" : "SAVE";
  fail_text_ = "";
  listed_ = 0;
  truncated_ = false;
  loaded_ = 0;
  recovered_ = false;
  notice_fresh_ = false;
  return true;
}

bool Files::save(const char* folder, const char* name, const void* data, std::size_t bytes, Mode mode) noexcept {
  if (data == nullptr || bytes == 0u || bytes > 0xFFFFFFFFu) {
    return ready_ && guard_request(true) ? refuse("NOTHING TO SAVE") : false;
  }
  if (!accept(Kind::Save, folder, name, true, false)) {
    return false;
  }
  req_ = Request{};
  req_.data = data;
  req_.bytes = bytes;
  req_.mode = mode;
  return true;
}

bool Files::save(const char* folder, const char* name, SaveSource& source, Mode mode) noexcept {
  if (!accept(Kind::SaveStream, folder, name, true, false)) {
    return false;
  }
  req_ = Request{};
  req_.source = &source;
  req_.mode = mode;
  return true;
}

bool Files::save_wav(const char* folder, const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, const char* stem) noexcept {
  // "<stem>_NNN" is a valid name of at most 31 bytes, so the stem has 27 (checked as a whole: a stem that does not make a valid file name is refused).
  char probe[Work::kNameBytes];
  const std::size_t n = stem != nullptr ? std::strlen(stem) : 0u;
  if (stem == nullptr || n == 0u || n > 27u || pcm == nullptr || frames == 0u || channels < 1u || channels > 2u || rate == 0u) {
    return ready_ && guard_request(true) ? refuse(pcm == nullptr || frames == 0u ? "NOTHING TO SAVE" : "BAD NAME") : false;
  }
  std::memcpy(probe, stem, n);
  std::memcpy(probe + n, "_000.wav", 9);
  if (!accept(Kind::Wav, folder, probe, true, true)) {
    return false;
  }
  std::memcpy(w_->stem, stem, n + 1u);
  req_ = Request{};
  req_.data = pcm;
  req_.frames = frames;
  req_.channels = channels;
  req_.rate = rate;
  return true;
}

bool Files::load(const char* folder, const char* name, void* dst, std::size_t cap) noexcept {
  if (dst == nullptr || cap == 0u) {
    return ready_ && guard_request(false) ? refuse("NOTHING TO LOAD INTO") : false;
  }
  if (!accept(Kind::Load, folder, name, false, false)) {
    return false;
  }
  req_ = Request{};
  req_.dst = dst;
  req_.cap = cap;
  return true;
}

bool Files::load(const char* folder, const char* name, core::ByteSink& sink) noexcept {
  if (!accept(Kind::LoadStream, folder, name, false, false)) {
    return false;
  }
  req_ = Request{};
  req_.sink = &sink;
  return true;
}

bool Files::list(const char* folder, Entry* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0u) {
    return ready_ && guard_request(false) ? refuse("NOTHING TO LIST INTO") : false;
  }
  if (!accept(Kind::List, folder, nullptr, false, false)) {
    return false;
  }
  req_ = Request{};
  req_.out = out;
  req_.cap = cap;
  return true;
}

bool Files::remove_folder(const char* folder) noexcept {
  if (folder == nullptr || folder[0] == '\0') {
    return ready_ && guard_request(true) ? refuse("BAD NAME") : false;
  }
  if (!accept(Kind::Marker, folder, nullptr, true, false)) {
    return false;
  }
  req_ = Request{};
  return true;
}

Files::Status Files::status() const noexcept {
  Status s;
  s.phase = phase_;
  s.text = text_;
  s.permille = phase_ == Status::Done ? 1000u : 0u;
  if (phase_ == Status::Running && kind_ != Kind::None && kind_ != Kind::Settings && slot_ >= 0) {
    s.permille = static_cast<std::uint16_t>(job_progress());
  }
  return s;
}

void Files::drop_pending(const char* text) noexcept {
  if (pend_ == Kind::None) {
    return;
  }
  if (pend_ == Kind::SaveStream && req_.source != nullptr) {
    req_.source->end();  // it was never begun: what it holds is let go
  }
  pend_ = Kind::None;
  phase_ = Status::Failed;
  text_ = text;
}

void Files::cancel() noexcept {
  drop_pending("CANCELLED");
  if (kind_ != Kind::None && kind_ != Kind::Settings && slot_ >= 0) {
    jobs_.cancel(static_cast<unsigned>(slot_));
    reap();
  }
}

// ---- Starting a job ---------------------------------------------------------------------------------------------------------------

void Files::set_label(const char* verb, const char* name) noexcept {
  label_.assign(verb);
  label_.append(" ");
  char up[Work::kNameBytes];
  upper_into(up, sizeof up, name);
  label_.append(up);
}

bool Files::start_job() noexcept {
  slot_ = jobs_.start(pump_);
  return slot_ >= 0;
}

// The save job over `source`, into w_->dir / `name`. false: the guard refuses the path (a bug: the names were checked).
bool Files::begin_save_job(const char* name, Mode mode, SaveSource& source) noexcept {
  writer_->begin_save();  // the save's card is the one it ends on
  pinned_ = true;
  const bool ok = w_->save.begin(*storage_, *writer_, w_->dir, name, mode == Mode::Replace ? AtomicSave::Mode::Replace : AtomicSave::Mode::NewOnly, source, label_.c_str() + 5);
  if (!ok) {
    writer_->end_save();
    pinned_ = false;
    source.end();
  }
  return ok;
}

// Runs in the pass after the request, with nothing else running: the card is looked at again (it may have changed since the call), then the job is begun.
void Files::launch() noexcept {
  const Kind k = pend_;
  pend_ = Kind::None;
  kind_ = k;
  fail_text_ = "";
  file_ = hal::storage::FileHandle{};
  file_size_ = read_ = 0;
  stage_have_ = stage_pos_ = 0;
  list_pos_ = 0;
  started_generation_ = generation_;
  const bool writes = k == Kind::Save || k == Kind::SaveStream || k == Kind::Wav || k == Kind::Marker;
  const bool folder = w_->folder[0] != '\0';
  stage_ = k == Kind::Load || k == Kind::LoadStream ? Stage::Open : Stage::Run;
  switch (k) {
    case Kind::Save:
    case Kind::SaveStream:
    case Kind::Wav:
    case Kind::Marker:
    case Kind::Load:
    case Kind::LoadStream:
      set_label(k == Kind::Marker ? "DELETE" : (k == Kind::Load || k == Kind::LoadStream) ? "LOAD" : "SAVE", k == Kind::Marker ? w_->folder : (k == Kind::Wav ? w_->stem : w_->name));
      break;
    default:
      set_label("LIST", w_->folder[0] != '\0' ? w_->folder : id_);
      break;
  }
  if (const char* why = card_text(card(), writes)) {
    fail_text_ = why;
    if (k == Kind::SaveStream && req_.source != nullptr) {
      req_.source->end();
    }
    finish(false, false);
    return;
  }
  bool ok = true;
  switch (k) {
    case Kind::Save:
      w_->buffer_source.set(static_cast<const std::uint8_t*>(req_.data), req_.bytes);
      ok = begin_save_job(w_->name, req_.mode, w_->buffer_source);
      stage_ = folder ? Stage::CheckFolder : Stage::Run;
      break;
    case Kind::SaveStream:
      ok = begin_save_job(w_->name, req_.mode, *req_.source);
      stage_ = folder ? Stage::CheckFolder : Stage::Run;
      break;
    case Kind::Wav:
      ok = w_->wav_source.set(static_cast<const std::int16_t*>(req_.data), req_.frames, req_.channels, req_.rate);
      if (ok) {
        writer_->begin_save();  // the numbering and the save are one save
        pinned_ = true;
      }
      stage_ = folder ? Stage::CheckFolder : Stage::Number;
      wav_next_ = 1;
      break;
    case Kind::Marker:
      writer_->begin_save();
      pinned_ = true;
      break;
    case Kind::List:
      w_->scan.invalidate();
      ok = w_->scan.begin(*storage_, w_->dir, kListFilter) != DirScan::Begin::BadPath;
      break;
    default:
      break;
  }
  if (!ok || !start_job()) {
    fail_text_ = ok ? "BUSY" : "BAD NAME";
    finish(false, false);
  }
}

void Files::launch_settings() noexcept {
  sent_ = sig_;
  const std::size_t n = settings::write(sent_, w_->buf.settings, settings::kMaxBytes);
  if (n == 0u || !guard::join(w_->dir, sizeof w_->dir, guard::kAppsDir, id_)) {
    saved_ = sent_;  // (it does not fit: not asked again until the values change)
    return;
  }
  kind_ = Kind::Settings;
  stage_ = Stage::Run;
  fail_text_ = "";
  set_label("SAVE", guard::kSystemFile);
  w_->buffer_source.set(w_->buf.settings, n);
  if (!begin_save_job(guard::kSystemFile, Mode::Replace, w_->buffer_source) || !start_job()) {
    finish(false, false);
  }
}

// ---- The job ----------------------------------------------------------------------------------------------------------------------

core::JobStatus Files::Pump::step(std::uint32_t budget_us) noexcept { return f_.run_step(budget_us); }
unsigned Files::Pump::progress() const noexcept { return f_.job_progress(); }
void Files::Pump::cancel() noexcept { f_.abort_op(); }
const char* Files::Pump::name() const noexcept { return f_.label_.c_str(); }
const char* Files::Pump::detail() const noexcept { return f_.fail_text_; }

unsigned Files::job_progress() const noexcept {
  switch (kind_) {
    case Kind::Save:
    case Kind::SaveStream:
    case Kind::Settings:
      return w_->save.progress();
    case Kind::Wav:
      return stage_ == Stage::Run ? w_->save.progress() : 0u;
    case Kind::Load:
    case Kind::LoadStream:
      return core::permille_of(read_, file_size_);
    default:
      return 0;
  }
}

core::JobStatus Files::fail(const char* text) noexcept {
  fail_text_ = text;
  return core::JobStatus::Failed;
}

core::JobStatus Files::run_step(std::uint32_t budget_us) noexcept {
  switch (kind_) {
    case Kind::Save:
    case Kind::SaveStream:
    case Kind::Settings:
      return step_save(budget_us);
    case Kind::Wav:
      return stage_ == Stage::Number ? step_number() : step_save(budget_us);
    case Kind::Load:
    case Kind::LoadStream:
      return step_load(budget_us);
    case Kind::List:
      return step_list(budget_us);
    case Kind::Marker:
      return step_marker();
    case Kind::None:
      break;
  }
  return fail("INTERNAL ERROR");
}

// The save's steps; a project folder is looked at once before the first (a folder the user deleted is not written into).
core::JobStatus Files::step_save(std::uint32_t budget_us) noexcept {
  if (stage_ == Stage::CheckFolder) {
    if (folder_marked_deleted(*storage_, w_->dir)) {
      return fail("FOLDER DELETED");
    }
    stage_ = kind_ == Kind::Wav ? Stage::Number : Stage::Run;
    return core::JobStatus::Running;
  }
  const core::JobStatus s = w_->save.step(budget_us);
  if (s == core::JobStatus::Failed) {
    fail_text_ = w_->save.detail()[0] != '\0' ? w_->save.detail() : "SAVE FAILED";
  }
  return s;
}

// The first free <stem>_NNN.wav, a few names a step; then the save of it begins.
core::JobStatus Files::step_number() noexcept {
  for (unsigned probes = 0; probes < kWavProbesPerStep; ++probes) {
    if (wav_next_ > kWavMax) {
      return fail("NO FREE NAME");
    }
    char name[Work::kNameBytes];
    const std::size_t n = std::strlen(w_->stem);
    std::memcpy(name, w_->stem, n);
    name[n] = '_';
    name[n + 1] = static_cast<char>('0' + (wav_next_ / 100u) % 10u);
    name[n + 2] = static_cast<char>('0' + (wav_next_ / 10u) % 10u);
    name[n + 3] = static_cast<char>('0' + wav_next_ % 10u);
    std::memcpy(name + n + 4, ".wav", 5);
    if (!guard::join(w_->path, sizeof w_->path, w_->dir, name)) {
      return fail("BAD NAME");
    }
    const St s = storage_->exists(w_->path, nullptr);
    if (s == St::NotFound) {  // it is free
      std::memcpy(w_->name, name, n + 9u);
      set_label("SAVE", w_->name);
      if (!begin_save_job(w_->name, Mode::NewOnly, w_->wav_source)) {
        return fail("BAD NAME");
      }
      stage_ = Stage::Run;
      return core::JobStatus::Running;
    }
    if (s != St::Ok) {
      return fail(read_text(s));  // the card is gone, or failed
    }
    ++wav_next_;
  }
  return core::JobStatus::Running;
}

void Files::close_file() noexcept {
  if (file_.is_open() && storage_ != nullptr) {
    storage_->close(file_);
  }
  file_ = hal::storage::FileHandle{};
}

// Opens the file (the .bak when it is gone) and reads its size.
bool Files::open_for_load() noexcept {
  if (!guard::join(w_->path, sizeof w_->path, w_->dir, w_->name)) {
    fail_text_ = "BAD NAME";
    return false;
  }
  St s = storage_->open(w_->path, file_);
  if (s == St::NotFound) {
    const std::size_t n = std::strlen(w_->name);
    std::memcpy(w_->backup, w_->name, n);
    std::memcpy(w_->backup + n, guard::kBackupSuffix, sizeof guard::kBackupSuffix);
    if (guard::join(w_->path, sizeof w_->path, w_->dir, w_->backup)) {
      s = storage_->open(w_->path, file_);
      recovered_ = s == St::Ok;
    }
  }
  if (s != St::Ok) {
    file_ = hal::storage::FileHandle{};
    fail_text_ = read_text(s);
    return false;
  }
  if (storage_->size(file_, file_size_) != St::Ok) {
    close_file();
    fail_text_ = "I/O ERROR";
    return false;
  }
  if (kind_ == Kind::Load && file_size_ > req_.cap) {
    close_file();
    fail_text_ = "FILE TOO BIG";
    return false;
  }
  return true;
}

core::JobStatus Files::step_load(std::uint32_t /*budget_us*/) noexcept {
  if (started_generation_ != generation_) {
    return fail("CARD CHANGED");
  }
  if (stage_ == Stage::Open) {
    if (!open_for_load()) {
      return core::JobStatus::Failed;
    }
    stage_ = Stage::Run;
  }
  std::size_t pass = 0;
  if (kind_ == Kind::Load) {
    auto* dst = static_cast<std::uint8_t*>(req_.dst);
    while (read_ < file_size_ && pass < kLoadBytesPerPass) {
      std::size_t want = file_size_ - read_;
      want = want < kReadChunk ? want : kReadChunk;
      std::size_t got = 0;
      const St s = storage_->read(file_, dst + read_, want, got);
      if (s != St::Ok || got == 0u) {
        close_file();
        return fail(s != St::Ok ? read_text(s) : "SHORT FILE");
      }
      read_ += static_cast<std::uint32_t>(got);
      pass += got;
    }
  } else {
    while (pass < kLoadBytesPerPass) {
      if (stage_pos_ >= stage_have_) {  // the staging buffer is empty: refill it
        if (read_ >= file_size_) {
          break;
        }
        std::size_t want = file_size_ - read_;
        want = want < Work::kStageBytes ? want : Work::kStageBytes;
        std::size_t got = 0;
        const St s = storage_->read(file_, w_->buf.stage, want, got);
        if (s != St::Ok || got == 0u) {
          close_file();
          return fail(s != St::Ok ? read_text(s) : "SHORT FILE");
        }
        stage_have_ = got;
        stage_pos_ = 0;
        read_ += static_cast<std::uint32_t>(got);
        pass += got;
      }
      const std::size_t left = stage_have_ - stage_pos_;
      const core::IoResult r = req_.sink->write(w_->buf.stage + stage_pos_, left);
      if (r.status == core::IoStatus::WouldBlock) {
        return core::JobStatus::Running;  // the sink is full: the rest waits for the next pass
      }
      if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > left) {
        close_file();
        return fail("SINK FAILED");
      }
      stage_pos_ += r.count;
    }
  }
  if (read_ >= file_size_ && stage_pos_ >= stage_have_) {
    close_file();
    loaded_ = read_;
    return core::JobStatus::Done;
  }
  return core::JobStatus::Running;
}

core::JobStatus Files::step_marker() noexcept {
  bool is_dir = false;
  const St e = storage_->exists(w_->dir, &is_dir);
  if (e != St::Ok || !is_dir) {
    return fail(e == St::NotFound || e == St::Ok ? "NO SUCH FOLDER" : read_text(e));
  }
  if (!guard::join(w_->path, sizeof w_->path, w_->dir, guard::kDeletedMarker)) {
    return fail("BAD NAME");
  }
  const WriteStatus st = writer_->create_marker(w_->path);
  if (st == WriteStatus::Ok) {
    return core::JobStatus::Done;
  }
  if (st == WriteStatus::Exists) {  // marked already: the same answer. A FOLDER called .deleted is the user's, not a marker
    return folder_marked_deleted(*storage_, w_->dir) ? core::JobStatus::Done : fail("NAME EXISTS");
  }
  return fail(write_text(st));
}

// The folder's entries as the app may use them, a few directory lookups a step: the names the guard takes, a lone backup as a file to recover, no folder DELETE marked (one lookup), the
// size of each file (one lookup), sorted by DirScan's index and, once a recovered name is among them, in the place its own name sorts.
core::JobStatus Files::step_collect(std::uint32_t budget_us) noexcept {
  const DirIndex& ix = w_->scan.index();
  Entry* out = req_.out;
  const bool root = w_->folder[0] == '\0';
  std::size_t looked = 0;
  // Bounded by count and by time: a directory lookup scans the folder (FatFs f_stat), so in a big folder each one is long. The time is the job pass's clock (the simulator's does not
  // move inside a pass, and the count holds there); at least one lookup is made a step so the listing always moves. JobStats::max_step_us on the unit says what a step cost.
  const core::Tick started = clock_ != nullptr ? clock_->now() : 0u;
  while (list_pos_ < ix.count() && looked < kStatsPerStep) {
    if (looked != 0u && clock_ != nullptr && core::tick_elapsed(clock_->now(), started) >= budget_us) {
      break;
    }
    const unsigned i = static_cast<unsigned>(list_pos_++);
    const char* name = ix.name(i);
    char base[kEntryNameBytes];
    bool folder = false;
    bool recover = false;
    if (ix.is_dir(i)) {
      if (!root || !guard::project_name_ok(name) || same_ci(name, "Samples")) {
        continue;  // (a project folder holds files only; a name the guard would refuse cannot be used by the app)
      }
      folder = true;
    } else if (ends_with_ci(name, guard::kBackupSuffix)) {
      const std::size_t len = std::strlen(name) - (sizeof guard::kBackupSuffix - 1u);
      if (len >= sizeof base) {
        continue;
      }
      std::memcpy(base, name, len);
      base[len] = '\0';
      if (!guard::file_name_ok(base) || ix.find_name(base) != DirIndex::kNone) {
        continue;  // not ours, or the backup of a file that is there
      }
      name = base;
      recover = true;
    } else if (!guard::file_name_ok(name)) {
      continue;
    }
    if (!folder && root && same_ci(name, guard::kSystemFile)) {
      continue;  // the kit's own settings
    }
    if (std::strlen(name) >= kEntryNameBytes) {
      continue;
    }
    if (listed_ >= req_.cap) {
      truncated_ = true;  // there is more than the app gave room for
      list_pos_ = ix.count();
      break;
    }
    ++looked;
    std::uint32_t size = 0;
    if (folder) {
      bool marker_is_dir = false;
      if (!guard::join(w_->path, sizeof w_->path, w_->dir, name, guard::kDeletedMarker)) {
        return fail("BAD NAME");
      }
      const St s = storage_->exists(w_->path, &marker_is_dir);
      if (s == St::Ok && !marker_is_dir) {
        continue;  // DELETE marked it: the folder is not listed
      }
      if (s != St::NotFound && s != St::NotADirectory) {
        return fail(read_text(s));
      }
    } else {
      char full[Work::kNameBytes];
      std::strcpy(full, name);
      if (recover) {
        std::strcat(full, guard::kBackupSuffix);  // (its size is the backup's)
      }
      hal::storage::FileStamp stamp;
      if (!guard::join(w_->path, sizeof w_->path, w_->dir, full)) {
        return fail("BAD NAME");
      }
      const St s = storage_->stat(w_->path, stamp);
      if (s == St::Ok) {
        size = stamp.size;
      } else if (s != St::NotFound) {
        return fail(read_text(s));
      }
    }
    std::strcpy(out[listed_].name, name);
    out[listed_].size = size;
    out[listed_].folder = folder;
    out[listed_].recover = recover;
    any_recover_ = any_recover_ || recover;
    ++listed_;
  }
  if (list_pos_ < ix.count()) {
    return core::JobStatus::Running;
  }
  if (any_recover_) {  // a recovered name was listed where its backup sorted: put it where its own name does
    for (std::size_t i = 1; i < listed_; ++i) {
      const Entry e = out[i];
      std::size_t j = i;
      while (j > 0 && DirIndex::compare(e.folder, e.name, out[j - 1].folder, out[j - 1].name) < 0) {
        out[j] = out[j - 1];
        --j;
      }
      out[j] = e;
    }
  }
  return core::JobStatus::Done;
}

core::JobStatus Files::step_list(std::uint32_t budget_us) noexcept {
  if (started_generation_ != generation_) {
    w_->scan.cancel();
    return fail("CARD CHANGED");
  }
  if (stage_ == Stage::Run) {
    if (w_->scan.step(budget_us) == core::JobStatus::Running) {
      return core::JobStatus::Running;
    }
    switch (w_->scan.state()) {
      case DirScan::State::Done:
      case DirScan::State::NoFolder:  // a first run: Apps/<id> is not there yet, and the list is empty
        stage_ = Stage::Sizes;
        list_pos_ = 0;
        listed_ = 0;
        any_recover_ = false;
        return core::JobStatus::Running;
      case DirScan::State::NoCard: return fail("NO CARD");
      default: return fail("I/O ERROR");
    }
  }
  return step_collect(budget_us);
}

// Gives up the running job: a save repairs at once, a load closes its file, a scan its directory. Whatever the cause (the app's cancel, the power-off, a settings save giving way).
void Files::abort_op() noexcept {
  switch (kind_) {
    case Kind::Save:
    case Kind::SaveStream:
    case Kind::Wav:
    case Kind::Settings:
      w_->save.cancel();
      break;
    case Kind::Load:
    case Kind::LoadStream:
      close_file();
      break;
    case Kind::List:
      w_->scan.cancel();
      break;
    default:
      break;
  }
  fail_text_ = "CANCELLED";
}

// ---- The result ---------------------------------------------------------------------------------------------------------------------

void Files::reap() noexcept {
  if (slot_ < 0) {
    return;
  }
  const core::SlotState s = jobs_.state(static_cast<unsigned>(slot_));
  if (s == core::SlotState::Running) {
    return;
  }
  finish(s == core::SlotState::Done, s == core::SlotState::Cancelled);
}

// The job (or a request refused at its start) is over: the writer is let go, the numbers kept, the result set for status() and the strip.
void Files::finish(bool done, bool cancelled) noexcept {
  const Kind k = kind_;
  close_file();
  if (pinned_) {
    writer_->end_save();
    pinned_ = false;
  }
  if (slot_ >= 0) {
    const core::JobStats& js = jobs_.stats(static_cast<unsigned>(slot_));
    ++stats_.jobs;
    stats_.failed += done ? 0u : 1u;
    stats_.steps += js.steps;
    stats_.max_step_us = js.max_step_us > stats_.max_step_us ? js.max_step_us : stats_.max_step_us;
    stats_.overruns += js.overruns;
    if (done && k != Kind::Settings && (k == Kind::Save || k == Kind::SaveStream || k == Kind::Wav)) {
      stats_.last_bytes = w_->save.bytes();
      stats_.last_us = js.total_us;
    }
    jobs_.release(static_cast<unsigned>(slot_));
    slot_ = -1;
  } else {
    ++stats_.jobs;
    ++stats_.failed;
  }
  kind_ = Kind::None;
  if (k == Kind::Settings) {
    if (!cancelled) {
      saved_ = sent_;  // written, or failed: not asked again until the values change. A cancelled one (it gave way, or the power-off) is asked again
    }
    return;  // (silent: the app's status and the strip are the app's)
  }
  phase_ = done ? Status::Done : Status::Failed;
  const char* what = fail_text_[0] != '\0' ? fail_text_ : "FAILED";
  if (cancelled) {
    what = "CANCELLED";
  } else if (!done && card() == Card::None) {
    what = "NO CARD";  // the card went out under the job: whatever the step said (a failed read-back, an I/O error), that is the reason
  }
  if (done) {
    switch (k) {
      case Kind::Load:
      case Kind::LoadStream: text_ = recovered_ ? "LOADED BACKUP" : "LOADED"; break;
      case Kind::List: text_ = "LISTED"; break;
      case Kind::Marker: text_ = "DELETED"; break;
      default: text_ = "SAVED"; break;
    }
  } else {
    text_ = what;
  }
  if (!quiet_ && !cancelled && (!done || k != Kind::List)) {
    const char* name = label_.c_str() + 5;  // "SAVE " / "LOAD " / "LIST "; DELETE is one longer
    if (k == Kind::Marker) {
      name = label_.c_str() + 7;
    }
    if (done) {
      notice_.assign(text_);
      notice_.append(" ");
      notice_.append(name);
    } else {
      notice_.assign(k == Kind::Marker ? "DELETE" : (k == Kind::Load || k == Kind::LoadStream) ? "LOAD" : k == Kind::List ? "LIST" : "SAVE");
      notice_.append(" FAILED: ");
      notice_.append(text_);
    }
    notice_fresh_ = true;
  }
}

bool Files::strip(Strip& out) noexcept {
  if (!ready_) {
    return false;
  }
  if (settings_note_) {  // the kit's own word, not the app's job: shown whatever set_quiet says
    settings_note_ = false;
    std::strncpy(out.text, "SETTINGS NOT SAVED: THE CARD'S FILE IS NEWER OR UNREADABLE", sizeof out.text - 1u);
    out.text[sizeof out.text - 1u] = '\0';
    out.progress = -1;
    out.result = true;
    return true;
  }
  if (quiet_) {
    notice_fresh_ = false;
    return false;
  }
  if (kind_ != Kind::None && kind_ != Kind::Settings && slot_ >= 0) {
    std::strncpy(out.text, label_.c_str(), sizeof out.text - 1u);
    out.text[sizeof out.text - 1u] = '\0';
    out.progress = static_cast<int>(job_progress());
    out.result = false;
    return true;
  }
  if (notice_fresh_) {
    std::strncpy(out.text, notice_.c_str(), sizeof out.text - 1u);
    out.text[sizeof out.text - 1u] = '\0';
    out.progress = -1;
    out.result = true;
    notice_fresh_ = false;
    return true;
  }
  return false;
}

// ---- The pass ---------------------------------------------------------------------------------------------------------------------

void Files::poll(const hal::Clock& clock, std::uint32_t budget_us, const KitValues& values) noexcept {
  if (!ready_) {
    return;
  }
  clock_ = &clock;
  note_card();
  const auto now_ms = static_cast<std::uint32_t>(clock.uptime_ms());
  track_settings(values, now_ms);
  if (!(seen_ && sig_ != saved_) && kind_ != Kind::Settings) {
    flush_ = false;
  }
  reap();
  if (kind_ == Kind::Settings && pend_ != Kind::None && slot_ >= 0 && !w_->save.past_write()) {
    jobs_.cancel(static_cast<unsigned>(slot_));  // the settings give way to the app: they are asked again once it is done
    reap();
  }
  if (kind_ == Kind::None) {
    if (pend_ != Kind::None) {
      launch();
    } else if (resume_ && sread_ == SettingsRead::NotRead && card() == Card::Ready && read_id_ == writer_->card_id() && seen_ && sig_ != saved_) {
      sread_ = SettingsRead::Writable;  // the same card, back from the computer, with settings changed meanwhile: they are the newer, and go to it after the debounce
      resume_ = false;
    } else if (sread_ == SettingsRead::NotRead || read_id_ != writer_->card_id()) {
      poll_settings_read(sig_);  // (before anything is written to this card, or after it was changed for another)
    } else if (sread_ == SettingsRead::Writable && seen_ && sig_ != saved_ && !gave_up_ && (power_ || flush_ || now_ms - moved_ms_ >= kSettingsDebounceMs) && card() == Card::Ready) {
      launch_settings();  // (a card that cannot be written now keeps them: they go when it can)
    }
  }
  if (kind_ != Kind::None) {
    (void)jobs_.run(clock, budget_us);
    reap();
  }
}

// What the values are now against what was last written: a change restarts the 1.5 s of quiet.
void Files::track_settings(const KitValues& values, std::uint32_t now_ms) noexcept {
  if (!seen_) {  // the first pass (when nothing was loaded): what the values are now is what counts as saved
    seen_ = true;
    sig_ = saved_ = values;
    moved_ms_ = now_ms;
    return;
  }
  if (values != sig_) {
    sig_ = values;
    moved_ms_ = now_ms;
  }
}

Files::Small Files::read_small(const char* path, std::uint8_t* buf, std::size_t cap, std::size_t& got) noexcept {
  hal::storage::FileHandle f;
  const St s = storage_->open(path, f);
  if (s != St::Ok) {
    return s == St::NotFound ? Small::Missing : (s == St::NoHandle ? Small::Busy : Small::Io);
  }
  std::uint32_t size = 0;
  if (storage_->size(f, size) != St::Ok) {
    storage_->close(f);
    return Small::Io;
  }
  if (size > cap) {
    storage_->close(f);
    return Small::TooBig;
  }
  std::size_t pos = 0;
  while (pos < size) {
    std::size_t n = 0;
    if (storage_->read(f, buf + pos, size - pos, n) != St::Ok || n == 0u) {
      storage_->close(f);
      return Small::Io;
    }
    pos += n;
  }
  storage_->close(f);
  got = pos;
  return Small::Ok;
}

// Reads this card's settings file into `values` (docs/formats/o3sdk-settings.md section 4) and says whether a save to it is safe now:
//   system.o3s read          -> Loaded
//   missing                  -> system.o3s.bak (a save cut between its renames), else NoFile
//   damaged (CRC, cut, malformed) -> the backup before the defaults; if it is no good either, Refused and the next save writes a good file over the damage
//   newer major, too big, or the card cannot give it -> Refused, and the card is PROTECTED: nothing is written to it for the session, so the user's file and its backup survive an older firmware
// `retry`: no file handle was free, nothing was decided (poll() asks again).
Files::SettingsLoad Files::read_settings_file(KitValues& values, bool& retry) noexcept {
  retry = false;
  char primary[hal::storage::save::kPathCap];
  char backup[hal::storage::save::kPathCap];
  if (!guard::join(primary, sizeof primary, guard::kAppsDir, id_, guard::kSystemFile) || !guard::join(backup, sizeof backup, guard::kAppsDir, id_, "system.o3s.bak")) {
    return settings_load_ = SettingsLoad::Unavailable;
  }
  std::uint8_t* const buf = w_->buf.settings;
  std::size_t n = 0;
  bool protect = false;
  bool need_backup = false;
  bool primary_missing = false;
  SettingsLoad result = SettingsLoad::NoFile;
  settings_report_ = settings::Report{};
  settings_result_ = settings::Result::Ok;
  const Small r = read_small(primary, buf, settings::kMaxBytes, n);
  if (r == Small::Busy) {
    retry = true;
    return SettingsLoad::NotRead;
  }
  if (r == Small::Ok) {
    KitValues t = values;
    settings::Report rep;
    const settings::Result why = settings::read(buf, n, t, rep);
    settings_result_ = why;
    if (why == settings::Result::Ok) {
      values = t;
      settings_report_ = rep;
      result = SettingsLoad::Loaded;
    } else {
      result = SettingsLoad::Refused;
      protect = why == settings::Result::NewerMajor;
      need_backup = !protect;
    }
  } else if (r == Small::Missing) {
    need_backup = true;
    primary_missing = true;
  } else {  // too big or unreadable: it may well be a good file of a newer version
    settings_result_ = r == Small::TooBig ? settings::Result::Malformed : settings::Result::Truncated;
    result = SettingsLoad::Refused;
    protect = true;
    need_backup = true;
  }
  if (need_backup) {
    const Small b = read_small(backup, buf, settings::kMaxBytes, n);
    if (b == Small::Busy) {
      retry = true;
      return SettingsLoad::NotRead;
    }
    if (b == Small::Ok) {
      KitValues t = values;
      settings::Report rep;
      const settings::Result bw = settings::read(buf, n, t, rep);
      if (bw == settings::Result::Ok) {
        values = t;
        settings_report_ = rep;
        settings_result_ = settings::Result::Ok;
        result = SettingsLoad::FromBackup;
      } else {
        result = SettingsLoad::Refused;
        protect = protect || bw == settings::Result::NewerMajor;
        settings_result_ = primary_missing || bw == settings::Result::NewerMajor ? bw : settings_result_;
      }
    } else if (b == Small::Missing) {
      result = primary_missing ? SettingsLoad::NoFile : result;
    } else {
      result = SettingsLoad::Refused;
      protect = true;
      settings_result_ = settings::Result::Truncated;
    }
  }
  sread_ = protect ? SettingsRead::Protected : SettingsRead::Writable;
  resume_ = false;
  read_id_ = writer_->card_id();
  settings_note_ = protect;
  if (result == SettingsLoad::Loaded || result == SettingsLoad::FromBackup || !seen_) {
    seen_ = true;  // what was read is what counts as saved; with no file, what the values are now counts only the first time (a change before the card came stays due)
    sig_ = saved_ = values;
  }
  return settings_load_ = result;
}

Files::SettingsLoad Files::load_settings(KitValues& values) noexcept {
  if (!ready_) {
    return settings_load_ = SettingsLoad::Unavailable;
  }
  if (!storage_->mounted()) {
    return settings_load_ = SettingsLoad::NoCard;
  }
  bool retry = false;
  const SettingsLoad r = read_settings_file(values, retry);
  return retry ? settings_load_ = SettingsLoad::NoCard : r;  // (no handle at boot: poll() reads it once one is free)
}

// A card put in after the boot (or one whose file was not read yet): its settings are read here, in the first idle pass, and handed to the runtime through take_loaded().
void Files::poll_settings_read(const KitValues& values) noexcept {
  const Card c = card();
  if (c == Card::None || c == Card::OnComputer || c == Card::InUse) {
    return;
  }
  KitValues v = values;
  bool retry = false;
  const SettingsLoad r = read_settings_file(v, retry);
  if (!retry && (r == SettingsLoad::Loaded || r == SettingsLoad::FromBackup)) {
    loaded_values_ = v;
    loaded_pending_ = true;
  }
}

// ---- The power-off ----------------------------------------------------------------------------------------------------------------

void Files::power_begin() noexcept {
  power_ = true;
  drop_pending("POWERING OFF");  // no new job: the running one is waited for (power_busy), the settings go at once
}

bool Files::power_busy() noexcept {
  if (!ready_) {
    return false;
  }
  reap();
  return kind_ != Kind::None || pend_ != Kind::None || (power_ && !gave_up_ && seen_ && sig_ != saved_ && sread_ != SettingsRead::Protected && card() == Card::Ready);
}

bool Files::past_write() const noexcept {
  if (!ready_ || slot_ < 0) {
    return false;
  }
  switch (kind_) {
    case Kind::Save:
    case Kind::SaveStream:
    case Kind::Wav:
    case Kind::Settings:
      return w_->save.past_write();
    default:
      return false;
  }
}

void Files::power_cancel() noexcept {
  gave_up_ = true;
  drop_pending("POWERING OFF");
  if (ready_ && kind_ != Kind::None && slot_ >= 0) {
    jobs_.cancel(static_cast<unsigned>(slot_));  // the job repairs: its temporary file is removed now and the old file stays whole
    reap();
  }
}

}  // namespace o3sdk::app
