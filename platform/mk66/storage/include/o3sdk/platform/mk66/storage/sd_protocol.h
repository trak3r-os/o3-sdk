// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SD card's command set as this driver uses it, and the decoding of what comes back (OS-007e). Pure: no register, no clock, nothing
// that blocks, so the host tests and QEMU run it, and the init sequence on top of it (sd_card.h) runs against a fake card.
//
// THE ALLOWLIST STARTS HERE. `Cmd` is the allowlist: the 16 commands below are the only ones the driver can name, and the bus refuses a
// value that is not one of them (`known()`). The one command with a write data phase is WRITE_BLOCK (CMD24, OS-006c: the firmware writer of
// the `diag` console, platform/mk66/update/, is its only caller, and only through the FirmwareStore); there is no WRITE_MULTIPLE_BLOCK
// (CMD25), PROGRAM_CSD, SET/CLR_WRITE_PROT, ERASE_WR_BLK_START/END, ERASE, LOCK_UNLOCK or GEN_CMD (CMD56) anywhere in this tree: not as a
// constant, not in a table, not behind a flag. `sd_protocol_test.cpp` walks all 256 values of the type and fails if one that is not on the
// list is accepted.
//
// SOURCES: the SD Physical Layer Simplified Specification (command and response formats, the CSD and CID registers, the switch-function
// status), written from the register tables, no code of any SD stack copied. The init issues CMD0, 8, 55 + 41, 2, 3, 9, 7, 55 + 6
// (4-bit), 6 (high speed).

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::storage::sd {

inline constexpr std::uint32_t kBlockSize = 512;  // every read is in 512-byte blocks (SDSC cards are set to it with CMD16)

// ---- The command set ------------------------------------------------------------------------------------------------------------------
// The wire index is the low 6 bits; bit 6 marks an application command (it goes after CMD55, which announces it).
inline constexpr std::uint8_t kAppFlag = 0x40;

enum class Cmd : std::uint8_t {
  GoIdle = 0,              // CMD0: every card to the idle state (no response)
  AllSendCid = 2,          // CMD2: the card puts its CID on the bus (R2)
  SendRelativeAddr = 3,    // CMD3: the card names its RCA (R6)
  SwitchFunc = 6,          // CMD6: check or switch a function (high speed); the 64-byte status comes back as data (R1)
  SelectCard = 7,          // CMD7: select the card with the RCA in the argument (R1b)
  SendIfCond = 8,          // CMD8: the host's voltage range and a check pattern (R7)
  SendCsd = 9,             // CMD9: the CSD of the card with that RCA (R2)
  StopTransmission = 12,   // CMD12: end a multiple-block read (R1b); the controller sends it itself after the last block, this is for error recovery
  SendStatus = 13,         // CMD13: the card's status register (R1)
  SetBlockLen = 16,        // CMD16: the block length, set to 512 on a byte-addressed (SDSC) card (R1)
  ReadSingleBlock = 17,    // CMD17 (R1, one block of data)
  ReadMultipleBlock = 18,  // CMD18 (R1, blocks until stopped)
  WriteBlock = 24,         // CMD24 (R1, one block of data to the card, then the card is busy while it programs it) - OS-006c
  AppCmd = 55,             // CMD55: the next command is an application command (R1)
  AcmdSetBusWidth = kAppFlag | 6,     // ACMD6: 1 or 4 data lines (R1)
  AcmdSdSendOpCond = kAppFlag | 41,   // ACMD41: the OCR and the HCS bit; the card answers busy until it is ready (R3)
};
inline constexpr unsigned kCommandCount = 16;

enum class Resp : std::uint8_t { None, R1, R1b, R2, R3, R6, R7 };

// The enumerators and nothing else: an out-of-range value of the type (a cast, a corrupted byte) is not a command.
constexpr bool known(Cmd c) noexcept {
  switch (c) {
    case Cmd::GoIdle:
    case Cmd::AllSendCid:
    case Cmd::SendRelativeAddr:
    case Cmd::SwitchFunc:
    case Cmd::SelectCard:
    case Cmd::SendIfCond:
    case Cmd::SendCsd:
    case Cmd::StopTransmission:
    case Cmd::SendStatus:
    case Cmd::SetBlockLen:
    case Cmd::ReadSingleBlock:
    case Cmd::ReadMultipleBlock:
    case Cmd::WriteBlock:
    case Cmd::AppCmd:
    case Cmd::AcmdSetBusWidth:
    case Cmd::AcmdSdSendOpCond:
      return true;
  }
  return false;
}

constexpr std::uint8_t wire_index(Cmd c) noexcept { return static_cast<std::uint8_t>(static_cast<std::uint8_t>(c) & 0x3Fu); }
constexpr bool is_app(Cmd c) noexcept { return (static_cast<std::uint8_t>(c) & kAppFlag) != 0u; }

// The response each command has (the caller cannot pair a command with the wrong one).
constexpr Resp response_of(Cmd c) noexcept {
  switch (c) {
    case Cmd::GoIdle: return Resp::None;
    case Cmd::AllSendCid: return Resp::R2;
    case Cmd::SendCsd: return Resp::R2;
    case Cmd::SendRelativeAddr: return Resp::R6;
    case Cmd::SendIfCond: return Resp::R7;
    case Cmd::AcmdSdSendOpCond: return Resp::R3;
    case Cmd::SelectCard: return Resp::R1b;
    case Cmd::StopTransmission: return Resp::R1b;
    case Cmd::SwitchFunc:
    case Cmd::SendStatus:
    case Cmd::SetBlockLen:
    case Cmd::ReadSingleBlock:
    case Cmd::ReadMultipleBlock:
    case Cmd::WriteBlock:
    case Cmd::AppCmd:
    case Cmd::AcmdSetBusWidth:
      return Resp::R1;
  }
  return Resp::None;
}

// Commands whose data phase is a read from the card to the host, and the one whose data phase is a write to it.
constexpr bool reads_data(Cmd c) noexcept { return c == Cmd::SwitchFunc || c == Cmd::ReadSingleBlock || c == Cmd::ReadMultipleBlock; }
constexpr bool writes_data(Cmd c) noexcept { return c == Cmd::WriteBlock; }

// ---- Arguments ------------------------------------------------------------------------------------------------------------------------
inline constexpr std::uint32_t kIfCondArg = 0x000001AAu;      // CMD8: VHS 0001b (2.7-3.6 V), check pattern 0xAA
inline constexpr std::uint32_t kOcrVoltageWindow = 0x00FF8000u;  // 2.7-3.6 V: bits 23..15
inline constexpr std::uint32_t kOcrBusy = 1u << 31;           // set when the card has finished its power-up (ACMD41's answer)
inline constexpr std::uint32_t kOcrCcs = 1u << 30;            // card capacity status: block addressed (SDHC, SDXC)
inline constexpr std::uint32_t kOcrHcs = 1u << 30;            // in the ACMD41 argument: the host supports high capacity
inline constexpr std::uint32_t kBusWidthArg1 = 0;             // ACMD6: 1 bit
inline constexpr std::uint32_t kBusWidthArg4 = 2;             // ACMD6: 4 bits
// CMD6: mode 0 (check) or 1 (switch) in bit 31, function groups 6..1 in nibbles 23..0, 0xF = leave that group alone; group 1 is the access
// mode and function 1 is high speed (up to 50 MHz).
inline constexpr std::uint32_t kSwitchCheckHighSpeed = 0x00FFFFF1u;
inline constexpr std::uint32_t kSwitchSetHighSpeed = 0x80FFFFF1u;
inline constexpr std::uint32_t kSwitchStatusBytes = 64;

constexpr std::uint32_t rca_arg(std::uint16_t rca) noexcept { return static_cast<std::uint32_t>(rca) << 16; }
constexpr std::uint32_t acmd41_arg(bool host_supports_high_capacity) noexcept {
  return kOcrVoltageWindow | (host_supports_high_capacity ? kOcrHcs : 0u);
}

// ---- Responses ------------------------------------------------------------------------------------------------------------------------
// R1: the card status. These bits are errors (spec 4.10.1: OUT_OF_RANGE, ADDRESS_ERROR, BLOCK_LEN_ERROR, ERASE_SEQ_ERROR, ERASE_PARAM,
// WP_VIOLATION, CARD_IS_LOCKED, LOCK_UNLOCK_FAILED, COM_CRC_ERROR, ILLEGAL_COMMAND, CARD_ECC_FAILED, CC_ERROR, ERROR, CSD_OVERWRITE,
// WP_ERASE_SKIP, AKE_SEQ_ERROR); the rest are state (CURRENT_STATE 12:9, READY_FOR_DATA 8, APP_CMD 5) or sticky bits that are not
// failures of this command (ERASE_RESET 13, CARD_ECC_DISABLED 14).
inline constexpr std::uint32_t kR1ErrorMask = 0xFFF98008u;
inline constexpr std::uint32_t kR1AppCmd = 1u << 5;
inline constexpr std::uint32_t kR1ReadyForData = 1u << 8;
constexpr std::uint32_t r1_errors(std::uint32_t r1) noexcept { return r1 & kR1ErrorMask; }
constexpr unsigned r1_state(std::uint32_t r1) noexcept { return (r1 >> 9) & 0xFu; }  // 4 = tran, 3 = stby, 1 = idle...
inline constexpr unsigned kStateTran = 4;

// R7 (the answer to CMD8): the voltage bits and the check pattern come back as sent; a card that does not echo them is not usable.
constexpr bool r7_ok(std::uint32_t r7) noexcept { return (r7 & 0xFFFu) == kIfCondArg; }

// R6 (the answer to CMD3): the new RCA in 31:16, a short card status in 15:0 (COM_CRC_ERROR 15, ILLEGAL_COMMAND 14, ERROR 13).
constexpr std::uint16_t r6_rca(std::uint32_t r6) noexcept { return static_cast<std::uint16_t>(r6 >> 16); }
constexpr bool r6_error(std::uint32_t r6) noexcept { return (r6 & 0xE000u) != 0u; }

// R2 (CID, CSD): 136 bits on the wire, of which the controller hands over bits 127:8 (it keeps the CRC7 and the end bit) in four words:
// word[0] is bits 39:8, word[1] 71:40, word[2] 103:72, word[3] 127:104 in its low 24 bits (K66 RM, SDHC CMDRSP0..3). `unpack_r2` rebuilds
// the register as the spec writes it, 16 bytes, most significant first, the last byte (the CRC and end bit) zero.
void unpack_r2(const std::uint32_t word[4], std::uint8_t out[16]) noexcept;
// Bits hi..lo (hi - lo < 32) of a 128-bit register in that byte order, bit 127 being the top bit of out[0].
std::uint32_t reg_bits(const std::uint8_t reg[16], unsigned hi, unsigned lo) noexcept;

// ---- CID and CSD ----------------------------------------------------------------------------------------------------------------------
struct Cid {
  std::uint8_t manufacturer;   // MID, 127:120
  char oem[3];                 // OID, 119:104, two ASCII characters
  char product[6];             // PNM, 103:64, five ASCII characters
  std::uint8_t revision_major;  // PRV, 63:56, two BCD digits
  std::uint8_t revision_minor;
  std::uint32_t serial;        // PSN, 55:24
  std::uint16_t year;          // MDT, 19:8: 2000 + the year field
  std::uint8_t month;
};
Cid parse_cid(const std::uint8_t reg[16]) noexcept;

struct Csd {
  std::uint8_t version;          // CSD_STRUCTURE + 1: 1 (SDSC), 2 (SDHC, SDXC); 3 (SDUC, 2 TiB and up) is refused
  bool valid;
  std::uint32_t blocks;          // capacity in 512-byte blocks; 0 when not valid or too big for 32 bits
  std::uint8_t read_bl_len_log2;  // v1: READ_BL_LEN (the card's block, 9..11); 9 for v2
  std::uint16_t ccc;             // CCC, 95:84: the command classes the card supports (bit 10: switch, CMD6)
  bool write_protected;          // PERM_WRITE_PROTECT (13) or TMP_WRITE_PROTECT (12) is set: the card refuses writes (OS-006c)
};
inline constexpr std::uint16_t kCccSwitch = 1u << 10;
// CSD v1: capacity = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes, expressed in 512-byte blocks. CSD v2:
// (C_SIZE + 1) * 1024 blocks (C_SIZE 22 bits, 69:48). A structure value of 2 or more, or a block length under 512, is not valid.
Csd parse_csd(const std::uint8_t reg[16]) noexcept;

// The capacity in MiB (rounded down) of a block count.
constexpr std::uint32_t capacity_mb(std::uint32_t blocks) noexcept { return blocks >> 11; }

// ---- CMD6 switch-function status (64 bytes, most significant byte first, spec 4.3.10) -----------------------------------------------
// Bit 401 of the 512-bit status says function 1 of group 1 (high speed) is supported; bits 379:376 name the function group 1 ended up in
// after a switch (0xF: the switch failed). Both are read at the byte positions the spec's bit numbers give (byte 0 holds bits 511:504).
constexpr bool switch_status_high_speed_supported(const std::uint8_t status[kSwitchStatusBytes]) noexcept {
  return (status[13] & 0x02u) != 0u;  // bits 407:400, bit 401
}
constexpr unsigned switch_status_group1_selected(const std::uint8_t status[kSwitchStatusBytes]) noexcept {
  return status[16] & 0x0Fu;  // bits 383:376, the low nibble: 379:376
}

}  // namespace o3sdk::platform::mk66::storage::sd
