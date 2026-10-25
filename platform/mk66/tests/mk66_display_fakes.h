// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test doubles for the FT812 display driver: a model of the chip behind the Link (enough of the protocol and of the
// registers for the bring-up, the frame swap and the command FIFO), and a BulkEngine that completes after a few polls and
// applies its buffer to the model as one chip-select cycle. The host suite uses them (no files, no threads).
//
// The model is a model: it follows the Programmers Guide's register behaviour as the driver relies on it (REG_ID 0x7C after
// ACTIVE, REG_CPURESET back to 0, REG_FRAMES counting scanned frames while REG_PCLK is non-zero, REG_DLSWAP reading 0 only after
// the next frame start, REG_CMD_READ 0xFFF on a coprocessor fault) and each knob below breaks one of those.

#include <o3sdk/platform/mk66/display/ft812_regs.h>
#include <o3sdk/platform/mk66/display/link.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace o3sdk::platform::mk66::display::fake {

namespace ft = ft812;

// One chip-select cycle, decoded.
struct Txn {
  enum class Kind : std::uint8_t { Host, Write, Read } kind;
  std::uint32_t addr;    // memory address (Write, Read) or the command byte (Host)
  std::uint32_t value;   // first up to 4 data bytes, little-endian (Write), the host parameter (Host), the value returned (Read)
  std::size_t bytes;     // data bytes in a Write / Read
};

class FakeChip final : public Link {
 public:
  // ---- knobs --------------------------------------------------------------------------------------------------------
  bool present = true;             // false: no chip, MISO reads 0xFF
  bool pd_wired = true;            // false: the PD# pulse does nothing
  bool cpureset_stuck = false;     // REG_CPURESET never reads 0
  bool scan_dead = false;          // the pixel clock runs but REG_FRAMES does not advance
  bool recover_fails = false;      // REG_CMD_READ stays non-zero even after the recovery
  bool swap_stuck = false;         // REG_DLSWAP never reads 0 again (the scan-out is dead), whatever is done
  bool swap_stuck_until_reset = false;  // the same until the chip is reset (PD# or the reset command), then it works
  std::uint32_t boot_ms = 25;      // time after ACTIVE until the chip answers
  std::uint32_t frame_us = 18'480; // this panel: 1056 x 525 clocks at 30 MHz

  explicit FakeChip(bool warm = false) : ram_g_(ft::kRamGBytes, 0), ram_dl_(ft::kRamDlBytes, 0) {
    if (warm) {
      awake_ = true;
      id_ready_us_ = 0;
      pclk_started_us_ = 1;  // frames were running long before this test began
      frames_base_ = 12345;
      clock_base_ = 987'654'321u;
      regs_[ft::reg::kPclk] = 2;
    }
  }

  // ---- Link ---------------------------------------------------------------------------------------------------------
  void select() noexcept override {
    if (bus_owned_) {
      ++bus_violations_;  // the driver touched the chip-select line under a DMA transfer: it would cut the transfer short
    }
    cs_ = true;
    cur_.clear();
    ++selects_;
    if (first_access_us_ == 0) {
      first_access_us_ = us_;
    }
  }
  void deselect() noexcept override {
    if (bus_owned_) {
      ++bus_violations_;
    }
    if (cs_) {
      cs_ = false;
      apply(cur_);
    }
  }
  std::uint8_t xfer(std::uint8_t out) noexcept override {
    if (bus_owned_) {
      ++bus_violations_;
    }
    ++xfers_;
    us_ += 4;  // about a byte at 2 MHz: the model's SPI is not free either
    if (!cs_) {
      return 0xFF;
    }
    cur_.push_back(out);
    const std::size_t i = cur_.size() - 1;
    if (!present) {
      return 0xFF;
    }
    // A read returns its data on the bytes after the 3 address bytes and the dummy byte.
    if (i >= 4 && (cur_[0] & 0x80u) == 0 && !is_host(cur_[0], cur_.size())) {
      return read_byte(address_of(cur_) + static_cast<std::uint32_t>(i - 4));
    }
    return 0;
  }
  void power_down(bool asserted) noexcept override {
    pd_asserted_ = asserted;
    transcript_.push_back(asserted ? "pd low" : "pd high");
    if (asserted && pd_wired) {
      reset_chip();
      awake_ = false;
    }
  }
  void set_speed(SpiSpeed speed) noexcept override {
    speed_ = speed;
    transcript_.push_back(speed == SpiSpeed::Init ? "speed init" : "speed data");
  }
  void delay_ms(std::uint32_t ms) noexcept override {
    us_ += static_cast<std::uint64_t>(ms) * 1000u;
    if (ms >= 5) {  // the polling loops' 1 ms waits stay out of the transcript
      transcript_.push_back("delay " + std::to_string(ms));
    }
  }
  std::uint32_t now_ms() const noexcept override {
    us_ += 10;  // time passes while the driver spins
    return static_cast<std::uint32_t>(us_ / 1000u);
  }
  bool take_error() noexcept override { return false; }

  // ---- the model ----------------------------------------------------------------------------------------------------
  // Applies one chip-select cycle's bytes (the BulkEngine fake calls this with a whole buffer).
  void apply(const std::vector<std::uint8_t>& bytes) {
    if (!present || bytes.empty()) {
      return;
    }
    if (is_host(bytes[0], bytes.size())) {
      host_command(bytes[0], bytes[1]);
      return;
    }
    if (bytes.size() < 3) {
      return;
    }
    const std::uint32_t addr = address_of(bytes);
    if ((bytes[0] & 0x80u) != 0) {
      const std::size_t n = bytes.size() - 3;
      std::uint32_t first = 0;
      for (std::size_t i = 0; i < n && i < 4; ++i) {
        first |= static_cast<std::uint32_t>(bytes[3 + i]) << (8u * i);
      }
      transcript_event(Txn{Txn::Kind::Write, addr, first, n});
      for (std::size_t i = 0; i < n; ++i) {
        write_byte(addr + static_cast<std::uint32_t>(i), bytes[3 + i]);
      }
    } else {
      const std::size_t n = bytes.size() > 4 ? bytes.size() - 4 : 0;
      txns_.push_back(Txn{Txn::Kind::Read, addr, n ? read_byte(addr) : 0u, n});
    }
  }

  // ---- inspection ---------------------------------------------------------------------------------------------------
  const std::vector<std::string>& transcript() const { return transcript_; }
  const std::vector<Txn>& txns() const { return txns_; }
  bool awake() const { return awake_; }
  bool pd_asserted() const { return pd_asserted_; }
  SpiSpeed speed() const { return speed_; }
  std::uint8_t ram_g(std::uint32_t addr) const { return ram_g_[addr]; }
  const std::vector<std::uint8_t>& ram_g_all() const { return ram_g_; }
  std::uint32_t ram_dl_word(std::size_t i) const {
    std::uint32_t v;
    std::memcpy(&v, &ram_dl_[4 * i], 4);
    return v;
  }
  // The list that was swapped in last (what the chip is scanning), empty before any swap.
  const std::vector<std::uint8_t>& displayed() const { return displayed_; }
  std::uint32_t swaps() const { return swaps_; }
  std::uint32_t reg(std::uint32_t addr) const {
    const auto it = regs_.find(addr);
    return it == regs_.end() ? 0u : it->second;
  }
  std::uint64_t now_us() const { return us_; }
  void advance_ms(std::uint32_t ms) { us_ += static_cast<std::uint64_t>(ms) * 1000u; }
  // Sets the uptime (before anything has run): the boot settle wait looks at it.
  void set_time_ms(std::uint32_t ms) { us_ = static_cast<std::uint64_t>(ms) * 1000u; }
  void inject_fault() { fault_ = true; }
  std::uint32_t xfers() const { return xfers_; }
  // The bus belongs to a DMA transfer in flight (FakeBulk sets it): any polled access meanwhile is a violation, counted.
  void set_bus_owned(bool owned) { bus_owned_ = owned; }
  std::uint32_t bus_violations() const { return bus_violations_; }
  std::uint32_t selects() const { return selects_; }
  // The uptime, in ms, at the first chip-select of the run (0 before any).
  std::uint32_t first_access_ms() const { return static_cast<std::uint32_t>(first_access_us_ / 1000u); }
  std::uint32_t cpureset_writes() const { return cpureset_writes_; }
  int last_write_index(std::uint32_t addr) const {
    for (std::size_t i = writes_.size(); i-- > 0;) {
      if (writes_[i].first == addr) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  // Every register write in order (addr, value as written).
  const std::vector<std::pair<std::uint32_t, std::uint32_t>>& writes() const { return writes_; }
  // The order in which the first write of each address happened, for ordering checks.
  int first_write_index(std::uint32_t addr) const {
    for (std::size_t i = 0; i < writes_.size(); ++i) {
      if (writes_[i].first == addr) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

 private:
  static bool is_host(std::uint8_t first, std::size_t size) { return size == 3 && (first & 0x80u) == 0 && (first == 0x00 || (first & 0x40u) != 0); }
  static std::uint32_t address_of(const std::vector<std::uint8_t>& b) {
    return (static_cast<std::uint32_t>(b[0] & 0x3Fu) << 16) | (static_cast<std::uint32_t>(b[1]) << 8) | b[2];
  }

  void reset_chip() {
    swap_stuck_until_reset = false;
    swap_requested_us_ = 0;
    regs_.clear();
    frames_base_ = 0;
    clock_base_ = 0;
    pclk_started_us_ = 0;
    swap_done_us_ = 0;
    fault_ = false;
    cmd_read_ = cmd_write_ = 0;
    cpureset_until_us_ = 0;
    reset_us_ = us_;
  }

  void host_command(std::uint8_t cmd, std::uint8_t param) {
    transcript_.push_back(hex2("host ", cmd) + hex2(" ", param));
    txns_.push_back(Txn{Txn::Kind::Host, cmd, param, 0});
    if (cmd == ft::host::kActive) {
      if (!awake_) {  // ACTIVE on a chip that is already running changes nothing
        awake_ = true;
        id_ready_us_ = us_ + static_cast<std::uint64_t>(boot_ms) * 1000u;
        cpureset_until_us_ = id_ready_us_ + 5'000u;
      }
    } else if (cmd == ft::host::kRstPulse) {
      const bool was_awake = awake_;
      reset_chip();
      awake_ = was_awake;
      id_ready_us_ = us_;
      cpureset_until_us_ = us_ + 5'000u;
    }
  }

  std::uint32_t frames_now() const {
    const std::uint32_t pclk = reg(ft::reg::kPclk);
    if (pclk == 0 || scan_dead || pclk_started_us_ == 0) {
      return frames_base_;
    }
    return frames_base_ + static_cast<std::uint32_t>((us_ - pclk_started_us_) / frame_us);
  }

  std::uint8_t read_byte(std::uint32_t addr) {
    if (!awake_ || us_ < id_ready_us_) {
      return 0x00;
    }
    if (addr < ft::kRamGBytes) {
      return ram_g_[addr];
    }
    if (addr >= ft::kRamDl && addr < ft::kRamDl + ft::kRamDlBytes) {
      return ram_dl_[addr - ft::kRamDl];
    }
    const std::uint32_t word = addr & ~3u;
    const unsigned shift = 8u * (addr & 3u);
    std::uint32_t v = 0;
    switch (word) {
      case ft::reg::kId: v = ft::kChipId; break;
      case ft::reg::kFrames: v = frames_now(); break;
      case ft::reg::kClock: v = clock_base_ + static_cast<std::uint32_t>((us_ - reset_us_) * 60u); break;
      case ft::reg::kCpuReset: v = cpureset_stuck || us_ < cpureset_until_us_ ? 7u : (reg(word) & 7u); break;
      case ft::reg::kDlSwap: v = swap_pending() ? ft::kDlSwapFrame : 0u; break;
      case ft::reg::kCmdRead: v = fault_ ? ft::kCmdFault : cmd_read_ ; break;
      case ft::reg::kCmdWrite: v = cmd_write_; break;
      case ft::reg::kCmdbSpace: v = fault_ ? 0xFFFu : (ft::kCmdFifoBytes - 4u); break;
      default: v = reg(word); break;
    }
    return static_cast<std::uint8_t>(v >> shift);
  }

  bool swap_pending() const {
    if (swap_requested_us_ == 0) {
      return false;
    }
    if (swap_stuck || swap_stuck_until_reset) {
      return true;
    }
    if (reg(ft::reg::kPclk) == 0 || scan_dead || pclk_started_us_ == 0) {
      return true;  // no scan-out, no frame start, no swap
    }
    return us_ < swap_done_us_;
  }

  void write_byte(std::uint32_t addr, std::uint8_t b) {
    if (addr < ft::kRamGBytes) {
      ram_g_[addr] = b;
      return;
    }
    if (addr >= ft::kRamDl && addr < ft::kRamDl + ft::kRamDlBytes) {
      ram_dl_[addr - ft::kRamDl] = b;
      return;
    }
    if (addr >= ft::reg::kCmdbWrite && addr < ft::reg::kCmdbWrite + 4u) {
      return;  // the window: the model's coprocessor executes everything at once
    }
    const std::uint32_t word = addr & ~3u;
    const unsigned shift = 8u * (addr & 3u);
    std::uint32_t v = reg(word);
    v = (v & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(b) << shift);
    regs_[word] = v;
    side_effects(word, v);
  }

  void side_effects(std::uint32_t word, std::uint32_t v) {
    switch (word) {
      case ft::reg::kPclk:
        if ((v & 0xFFu) != 0 && pclk_started_us_ == 0) {
          pclk_started_us_ = us_ == 0 ? 1 : us_;
        } else if ((v & 0xFFu) == 0) {
          pclk_started_us_ = 0;
        }
        break;
      case ft::reg::kDlSwap:
        if ((v & 0xFFu) == ft::kDlSwapFrame) {
          swap_requested_us_ = us_ == 0 ? 1 : us_;
          // The next frame start after now.
          const std::uint64_t since = pclk_started_us_ == 0 ? 0 : (us_ - pclk_started_us_) % frame_us;
          swap_done_us_ = us_ + (frame_us - since);
          displayed_.assign(ram_dl_.begin(), ram_dl_.end());
          ++swaps_;
          regs_[word] = 0;  // the register itself reads through swap_pending()
        }
        break;
      case ft::reg::kCpuReset:
        ++cpureset_writes_;
        if ((v & 1u) != 0) {
          fault_ = false;  // the coprocessor is held in reset: the fault is gone once it restarts
        }
        break;
      case ft::reg::kCmdRead: cmd_read_ = recover_fails ? 1u : (v & 0xFFFu); break;
      case ft::reg::kCmdWrite: cmd_write_ = v & 0xFFFu; break;
      default: break;
    }
  }

  void transcript_event(const Txn& t) {
    txns_.push_back(t);
    if (t.addr >= ft::kRamDl && t.addr < ft::kRamReg) {
      writes_.emplace_back(t.addr, t.value);
      transcript_.push_back("w" + std::to_string(t.bytes) + " " + hex(t.addr, 6) + " " + hex(t.value, t.bytes >= 4 ? 8 : static_cast<int>(2 * t.bytes)));
      return;
    }
    if (t.addr >= ft::kRamReg && t.addr < ft::kRamCmd) {
      writes_.emplace_back(t.addr, t.value);
      if (t.bytes <= 4) {
        transcript_.push_back("w" + std::to_string(t.bytes) + " " + hex(t.addr, 6) + " " + hex(t.value, static_cast<int>(2 * t.bytes)));
      }
    }
  }

  static std::string hex(std::uint32_t v, int digits) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (int i = digits - 1; i >= 0; --i) {
      s.push_back(d[(v >> (4 * i)) & 15u]);
    }
    return s;
  }
  static std::string hex2(const char* prefix, std::uint32_t v) { return std::string(prefix) + hex(v, 2); }

  mutable std::uint64_t us_ = 1'000'000;  // the image has been up for a second when the display driver starts
  bool cs_ = false;
  bool pd_asserted_ = false;
  bool awake_ = false;
  bool fault_ = false;
  SpiSpeed speed_ = SpiSpeed::Init;
  std::vector<std::uint8_t> cur_;
  std::vector<std::uint8_t> ram_g_;
  std::vector<std::uint8_t> ram_dl_;
  std::vector<std::uint8_t> displayed_;
  std::map<std::uint32_t, std::uint32_t> regs_;
  std::vector<std::string> transcript_;
  std::vector<Txn> txns_;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> writes_;
  std::uint64_t id_ready_us_ = 0;
  std::uint64_t cpureset_until_us_ = 0;
  std::uint64_t pclk_started_us_ = 0;
  std::uint64_t swap_requested_us_ = 0;
  std::uint64_t swap_done_us_ = 0;
  std::uint64_t reset_us_ = 0;
  std::uint32_t frames_base_ = 0;
  std::uint32_t clock_base_ = 0;
  std::uint32_t cmd_read_ = 0;
  std::uint32_t cmd_write_ = 0;
  std::uint32_t swaps_ = 0;
  std::uint32_t xfers_ = 0;
  std::uint32_t selects_ = 0;
  std::uint64_t first_access_us_ = 0;
  std::uint32_t cpureset_writes_ = 0;
  bool bus_owned_ = false;
  std::uint32_t bus_violations_ = 0;
};

// A DMA engine that finishes a few polls after start() and then applies its buffer to the chip as one chip-select cycle.
class FakeBulk final : public BulkEngine {
 public:
  explicit FakeBulk(FakeChip& chip, std::size_t max = 8192 + 4) : chip_(chip), max_(max) {}

  // knobs
  bool refuse = false;        // start() says no
  bool fail = false;          // poll() reports Failed (the data is not applied)
  bool hang = false;          // poll() reports Busy for ever
  bool corrupt = false;       // the data arrives with a bit flipped (a PUSHR command half gone wrong)
  unsigned busy_polls = 2;    // polls that report Busy before the transfer completes

  std::size_t max_len() const noexcept override { return max_; }
  bool start(const std::uint8_t* src, std::size_t len, std::size_t capacity) noexcept override {
    last_capacity_ = capacity;
    last_len_ = len;
    if (refuse || state_ != BulkStatus::Idle || src == nullptr || len == 0 || len > max_ || len > capacity) {
      return false;
    }
    pending_.assign(src, src + len);
    src_ = src;
    left_ = busy_polls;
    state_ = BulkStatus::Busy;
    chip_.set_bus_owned(true);
    ++starts_;
    return true;
  }
  BulkStatus poll() noexcept override {
    if (state_ != BulkStatus::Busy) {
      return BulkStatus::Idle;
    }
    if (hang) {
      return BulkStatus::Busy;
    }
    if (left_ > 0) {
      --left_;
      return BulkStatus::Busy;
    }
    state_ = BulkStatus::Idle;
    chip_.set_bus_owned(false);
    if (fail) {
      return BulkStatus::Failed;
    }
    if (corrupt && pending_.size() > 4) {
      pending_[4] = static_cast<std::uint8_t>(pending_[4] ^ 0x40u);
    }
    chip_.apply(pending_);
    ++completions_;
    return BulkStatus::Done;
  }
  void abort() noexcept override {
    state_ = BulkStatus::Idle;
    chip_.set_bus_owned(false);
    ++aborts_;
  }

  const std::uint8_t* source() const { return src_; }
  // What the display told the engine about the last transfer it offered: its length and the room behind its source.
  std::size_t last_len() const { return last_len_; }
  std::size_t last_capacity() const { return last_capacity_; }
  unsigned starts() const { return starts_; }
  unsigned completions() const { return completions_; }
  unsigned aborts() const { return aborts_; }
  bool busy() const { return state_ == BulkStatus::Busy; }

 private:
  FakeChip& chip_;
  std::size_t max_;
  BulkStatus state_ = BulkStatus::Idle;
  std::vector<std::uint8_t> pending_;
  const std::uint8_t* src_ = nullptr;
  std::size_t last_len_ = 0;
  std::size_t last_capacity_ = 0;
  unsigned left_ = 0;
  unsigned starts_ = 0;
  unsigned completions_ = 0;
  unsigned aborts_ = 0;
};

}  // namespace o3sdk::platform::mk66::display::fake
