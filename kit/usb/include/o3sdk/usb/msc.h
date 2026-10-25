// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-USBDISK U1a: the USB Mass Storage Bulk-Only Transport (BOT) and SCSI transparent command engine, as a pure state machine over an abstract
// block store. It knows nothing about the USB0 controller, FatFs or the SD card: the driver (U1c, not written yet) hands it bulk OUT packets,
// takes bulk IN packets from it and applies its stall flags; the card sits behind BlockStore. docs/specs/OS-USBDISK.md sections 3.2 to 3.4.
//
// What it does: the CBW and CSW, all 13 host/device data cases of the BOT specification (6.7) with the residue and the stall each one needs,
// persistent stalls and the reset recovery, and the SCSI set of the spec (3.3): TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY(10),
// READ(10), WRITE(10), MODE SENSE(6), PREVENT ALLOW MEDIUM REMOVAL, START STOP UNIT, SYNCHRONIZE CACHE(10), READ FORMAT CAPACITIES, VERIFY(10);
// anything else answers CHECK CONDITION, ILLEGAL REQUEST, INVALID COMMAND OPERATION CODE. There is no write cache: a WRITE(10) block is on the
// store when write() returned, so SYNCHRONIZE CACHE has nothing to flush.
//
// Threading: ONE context. Every member function is called from the same one (the main loop pass in U1c); nothing here is atomic or locked.
// How the USB interrupt hands packets to that context is the driver's protocol (U1c), not this class's; this class is not interrupt-safe as it
// stands. No heap, no allocation, no blocking of its own: the card I/O is one block per service() call, on the caller's context.

#include <cstddef>
#include <cstdint>

namespace o3sdk::usb::msc {

inline constexpr std::uint32_t kBlockSize = 512;
inline constexpr std::uint32_t kCbwSize = 31;
inline constexpr std::uint32_t kCswSize = 13;
inline constexpr std::uint32_t kCbwSignature = 0x43425355u;  // "USBC" little-endian
inline constexpr std::uint32_t kCswSignature = 0x53425355u;  // "USBS"
inline constexpr std::uint32_t kMaxPacket = 512;             // the largest bulk packet (high speed); full speed is 64

// bCSWStatus.
enum class CswStatus : std::uint8_t { Passed = 0, Failed = 1, PhaseError = 2 };

// SCSI sense triple.
struct Sense {
  std::uint8_t key = 0;
  std::uint8_t asc = 0;
  std::uint8_t ascq = 0;
  constexpr bool operator==(const Sense& o) const noexcept { return key == o.key && asc == o.asc && ascq == o.ascq; }
};
namespace sense {
inline constexpr std::uint8_t kKeyNone = 0x00, kKeyNotReady = 0x02, kKeyMediumError = 0x03, kKeyIllegalRequest = 0x05, kKeyUnitAttention = 0x06,
                              kKeyDataProtect = 0x07;
inline constexpr Sense kNone{0x00, 0x00, 0x00};
inline constexpr Sense kNoMedium{kKeyNotReady, 0x3A, 0x00};            // NOT READY, MEDIUM NOT PRESENT
inline constexpr Sense kReadError{kKeyMediumError, 0x11, 0x00};        // MEDIUM ERROR, UNRECOVERED READ ERROR
inline constexpr Sense kWriteError{kKeyMediumError, 0x0C, 0x00};       // MEDIUM ERROR, WRITE ERROR
inline constexpr Sense kBadOpcode{kKeyIllegalRequest, 0x20, 0x00};     // ILLEGAL REQUEST, INVALID COMMAND OPERATION CODE
inline constexpr Sense kLbaRange{kKeyIllegalRequest, 0x21, 0x00};      // ILLEGAL REQUEST, LOGICAL BLOCK ADDRESS OUT OF RANGE
inline constexpr Sense kBadField{kKeyIllegalRequest, 0x24, 0x00};      // ILLEGAL REQUEST, INVALID FIELD IN CDB
inline constexpr Sense kBadLun{kKeyIllegalRequest, 0x25, 0x00};        // ILLEGAL REQUEST, LOGICAL UNIT NOT SUPPORTED
inline constexpr Sense kRemovalPrevented{kKeyIllegalRequest, 0x53, 0x02};  // ILLEGAL REQUEST, MEDIA REMOVAL PREVENTED
inline constexpr Sense kDataProtect{kKeyDataProtect, 0x27, 0x00};      // DATA PROTECT, WRITE PROTECTED
inline constexpr Sense kMediumChanged{kKeyUnitAttention, 0x28, 0x00};  // UNIT ATTENTION, NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED
}  // namespace sense

enum class IoResult : std::uint8_t { Ok, NoMedium, WriteProtected, Failed };

// The medium behind the LUN: 512-byte blocks. The getters are cheap (no bus traffic); read and write block for the card (the caller's context
// is the main loop). Adapted to platform/mk66 storage::BlockDevice in U1c; the tests use a RAM image.
class BlockStore {
 public:
  virtual bool present() const noexcept = 0;  // a medium is in and answers
  virtual std::uint32_t block_count() const noexcept = 0;
  virtual bool write_protected() const noexcept = 0;
  virtual IoResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept = 0;
  virtual IoResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept = 0;

 protected:
  BlockStore() = default;
  ~BlockStore() = default;  // never deleted through the interface
  BlockStore(const BlockStore&) = delete;
  BlockStore& operator=(const BlockStore&) = delete;
};

// What the driver gets when it asks for a bulk IN packet.
struct InPacket {
  enum class Kind : std::uint8_t {
    Nak,     // nothing to send yet (the card has not delivered the block): leave the IN descriptor unarmed
    Packet,  // `len` bytes were written to the caller's buffer (len 0 is not produced)
    Stall,   // the IN endpoint is halted: answer STALL
  };
  Kind kind = Kind::Nak;
  std::uint32_t len = 0;
};

class Engine {
 public:
  explicit Engine(BlockStore& store) noexcept;

  // ---- The transport side (the USB driver) ----

  // The OUT descriptor may be armed for a packet of up to `max_len` bytes: the engine can take it. False means NAK (flow control by the
  // controller; MSC never drops).
  bool can_accept_out(std::uint32_t max_len) const noexcept;
  // One bulk OUT packet from the host. A CBW is one packet of exactly 31 bytes with the right signature; anything else in the command phase
  // is an invalid CBW and halts both endpoints until the reset recovery.
  void out_packet(const std::uint8_t* data, std::uint32_t len) noexcept;
  // The next bulk IN packet, at most `max` bytes (13 or more). The data stage is delivered in full packets, never a short one before the end.
  InPacket in_packet(std::uint8_t* out, std::uint32_t max) noexcept;

  // The halt flags the driver applies to the endpoints (EPSTALL). Separate endpoint numbers: each direction halts and clears on its own. The K66 driver applies the
  // EPSTALL of an IN endpoint only after the last IN packet already queued has completed, so a stall reaches the wire after the data it follows: the engine does not
  // count on it being visible at once (spec 3.4).
  bool in_stalled() const noexcept { return halt_in_; }
  bool out_stalled() const noexcept { return halt_out_; }
  // CLEAR_FEATURE(ENDPOINT_HALT) on the bulk IN / OUT endpoint. After an invalid CBW the stall is persistent: it stays until the Bulk-Only Mass
  // Storage Reset, whatever the host clears before it.
  void clear_halt_in() noexcept;
  void clear_halt_out() noexcept;
  // Bulk-Only Mass Storage Reset (class request 0xFF): the command in flight is dropped, the next packet is a CBW. The halts stay until the host
  // clears them (reset recovery: reset, clear IN, clear OUT). SCSI state (sense, unit attention, prevent) is kept. A write cut by the reset keeps
  // the blocks already written (not all-or-nothing).
  void bot_reset() noexcept;
  // A bus reset or SET_CONFIGURATION: bot_reset() and both halts cleared, and PREVENT, the unit attention and the sense data forgotten. The eject latch (unit_eject(), the host's eject) is KEPT until medium_changed(): a new session must not re-present a card the unit owns again.
  void reset_all() noexcept;
  // GET MAX LUN (class request 0xFE): one LUN.
  static constexpr std::uint8_t max_lun() noexcept { return 0; }

  // ---- The main loop ----

  // Decodes a received CBW and does at most one block of card I/O. Call it every pass while the interface is configured.
  void service() noexcept;
  // No command in flight and no packet owed to the host.
  bool idle() const noexcept { return phase_ == Phase::ExpectCbw; }

  // ---- The owner's side (what the unit knows about the medium) ----

  // The medium was swapped, put back or its contents are not the ones the host cached (a remount): the next command answers UNIT ATTENTION
  // (NOT READY TO READY CHANGE), the capacity is read again, and a medium that was ejected is back. A READ or WRITE in its data stage was planned on the
  // medium that is gone, so it ends here (abort_data: NOT READY, MEDIUM NOT PRESENT, the data pipe stalled, CSW Failed): the block buffers are dropped with it
  // (a block already read is not delivered) and no further block touches the new medium.
  void medium_changed() noexcept;
  // The unit's own eject (Config page, F7): the host's next command is UNIT ATTENTION, then NOT READY, MEDIUM NOT PRESENT, whatever PREVENT says.
  void unit_eject() noexcept;
  // The host ejected (START STOP UNIT with LoEj, allowed by PREVENT): true once, then false until the next eject.
  bool take_eject_request() noexcept;
  bool ejected() const noexcept { return ejected_; }
  // PREVENT ALLOW MEDIUM REMOVAL state (true: the host asked to keep the medium in).
  bool removal_prevented() const noexcept { return prevent_; }
  // The sense data a REQUEST SENSE would return now (introspection for the tests and `diag`).
  const Sense& sense() const noexcept { return sense_; }

 private:
  enum class Phase : std::uint8_t {
    ExpectCbw,    // idle: the next OUT packet is a CBW
    CbwReceived,  // service() has not decoded it yet
    DataIn,       // device to host
    DataOut,      // host to device
    SendCsw,      // the CSW is owed on the IN endpoint (after any stall has been cleared)
    Dead,         // invalid CBW: both endpoints halted until the reset recovery
  };
  enum class Op : std::uint8_t { None, Small, Read, Write };
  enum class Dir : std::uint8_t { None, In, Out };

  void decode() noexcept;
  void plan_scsi() noexcept;
  bool gate_medium(bool needs_medium) noexcept;  // false: sense_ is set and the command fails
  void fail(const Sense& s) noexcept;
  void make_small(std::uint32_t natural, std::uint32_t alloc) noexcept;
  void cmd_rw(bool write) noexcept;
  void start_data() noexcept;
  void end_with(CswStatus status, std::uint32_t residue) noexcept;
  void finish_in() noexcept;
  void finish_out() noexcept;
  void abort_data(const Sense& s) noexcept;
  void refresh_medium() noexcept;
  void clear_ring() noexcept;
  void go_dead() noexcept;

  BlockStore& store_;

  // Medium belief (what the host was last told).
  bool seen_present_ = false;
  std::uint32_t seen_blocks_ = 0;
  bool ua_ = false;
  bool ejected_ = false;
  bool eject_request_ = false;
  bool prevent_ = false;
  Sense sense_{};

  // Protocol state.
  Phase phase_ = Phase::ExpectCbw;
  bool halt_in_ = false;
  bool halt_out_ = false;
  bool dead_ = false;  // persistent stall until bot_reset()

  // The CBW in flight.
  std::uint8_t cbw_[kCbwSize] = {};
  std::uint32_t tag_ = 0;
  std::uint32_t hlen_ = 0;  // dCBWDataTransferLength
  bool h_in_ = false;       // bmCBWFlags bit 7

  // The plan of the command.
  Op op_ = Op::None;
  Dir dev_dir_ = Dir::None;
  std::uint32_t dev_len_ = 0;  // bytes the device intends to move
  CswStatus scsi_status_ = CswStatus::Passed;
  std::uint32_t lba_ = 0;      // first block of READ / WRITE
  std::uint32_t xfer_ = 0;     // bytes that actually cross the bus in the data stage: min(hlen_, dev_len_)
  std::uint32_t moved_ = 0;    // bytes moved so far
  std::uint32_t io_blocks_ = 0;  // blocks read from / written to the store so far
  std::uint8_t small_[40] = {};

  // Two block buffers: the USB side fills one while the store takes the other (write), the store fills one while the USB side drains the other
  // (read). rd_ is the consumer's slot, wr_ the producer's, fill_ how many slots hold a block, pos_ the byte position in the slot the packet side
  // works on.
  alignas(4) std::uint8_t buf_[2][kBlockSize] = {};
  std::uint8_t rd_ = 0;
  std::uint8_t wr_ = 0;
  std::uint8_t fill_ = 0;
  std::uint32_t pos_ = 0;

  // The CSW owed.
  CswStatus csw_status_ = CswStatus::Passed;
  std::uint32_t csw_residue_ = 0;
};

static_assert(kBlockSize % 4u == 0u, "the block buffers are moved in words");

}  // namespace o3sdk::usb::msc
