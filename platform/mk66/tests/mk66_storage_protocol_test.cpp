// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SD command set and the decoding of its answers (storage/sd_protocol.h): the allowlist that makes the card read only, the argument and
// response encodings, the CSD (versions 1 and 2) and CID parsers against known cards, and the R2 layout of the controller. Pure; runs on the host
// and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3sdk/platform/mk66/storage/sd_protocol.h>

#include <cstdint>
#include <cstring>

namespace sd = o3sdk::platform::mk66::storage::sd;
using fakes::set_bits;

TEST_CASE("sd protocol: the command allowlist is exactly the 16 commands (CMD24 the one write), and no other write-side command can be named") {
  // Every value of the type, accepted or not: the accepted ones are the documented set and nothing else.
  unsigned accepted = 0;
  for (unsigned v = 0; v < 256; ++v) {
    const sd::Cmd c = static_cast<sd::Cmd>(v);
    if (sd::known(c)) {
      ++accepted;
      CHECK_MESSAGE(sd::wire_index(c) == (v & 0x3Fu), "value " << v);
    }
  }
  CHECK(accepted == sd::kCommandCount);
  CHECK(accepted == 16u);
  const unsigned plain[] = {0, 2, 3, 6, 7, 8, 9, 12, 13, 16, 17, 18, 24, 55};
  for (const unsigned index : plain) {
    CHECK_MESSAGE(sd::known(static_cast<sd::Cmd>(index)), "CMD" << index);
  }
  CHECK(sd::known(sd::Cmd::AcmdSetBusWidth));
  CHECK(sd::known(sd::Cmd::AcmdSdSendOpCond));
  CHECK(sd::wire_index(sd::Cmd::AcmdSetBusWidth) == 6u);
  CHECK(sd::wire_index(sd::Cmd::AcmdSdSendOpCond) == 41u);
  CHECK(sd::is_app(sd::Cmd::AcmdSetBusWidth));
  CHECK(!sd::is_app(sd::Cmd::SwitchFunc));  // CMD6 and ACMD6 share a wire index and are two commands
  // Write-side commands of the SD spec other than CMD24, plain and application: none is a Cmd, whichever way it is spelled. (CMD24 as an
  // application command is no command either: only the plain index is on the list.)
  const unsigned write_side[] = {25, 26, 27, 28, 29, 32, 33, 38, 42, 56};  // WRITE_MULTIPLE, 26 (reserved), PROGRAM_CSD, SET_WRITE_PROT,
                                                                           // CLR_WRITE_PROT, ERASE_WR_BLK_START/END, ERASE, LOCK_UNLOCK, GEN_CMD
  CHECK(!sd::known(static_cast<sd::Cmd>(24u | sd::kAppFlag)));
  CHECK(sd::known(sd::Cmd::WriteBlock));
  CHECK(sd::wire_index(sd::Cmd::WriteBlock) == 24u);
  for (const unsigned index : write_side) {
    CHECK_MESSAGE(!sd::known(static_cast<sd::Cmd>(index)), "CMD" << index);
    CHECK_MESSAGE(!sd::known(static_cast<sd::Cmd>(index | sd::kAppFlag)), "ACMD" << index);
  }
  const unsigned app_write_side[] = {22, 23, 25, 26, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49};  // SD_NUM_WR_BLOCKS, SET_WR_BLK_ERASE_COUNT, ... SECURE_WRITE_MKB
  for (const unsigned index : app_write_side) {
    CHECK_MESSAGE(!sd::known(static_cast<sd::Cmd>(index | sd::kAppFlag)), "ACMD" << index);
  }
  // Exactly one command has a write data phase, CMD24; the three with a read data phase are CMD6, CMD17 and CMD18.
  unsigned writing = 0;
  for (unsigned v = 0; v < 256; ++v) {
    const sd::Cmd c = static_cast<sd::Cmd>(v);
    writing += (sd::known(c) && sd::writes_data(c)) ? 1u : 0u;
  }
  CHECK(writing == 1u);
  CHECK(sd::writes_data(sd::Cmd::WriteBlock));
  CHECK(!sd::reads_data(sd::Cmd::WriteBlock));
  CHECK(sd::response_of(sd::Cmd::WriteBlock) == sd::Resp::R1);
  unsigned reading = 0;
  for (unsigned v = 0; v < 256; ++v) {
    const sd::Cmd c = static_cast<sd::Cmd>(v);
    reading += (sd::known(c) && sd::reads_data(c)) ? 1u : 0u;
  }
  CHECK(reading == 3u);
  CHECK(sd::reads_data(sd::Cmd::SwitchFunc));
  CHECK(sd::reads_data(sd::Cmd::ReadSingleBlock));
  CHECK(sd::reads_data(sd::Cmd::ReadMultipleBlock));
}

TEST_CASE("sd protocol: the answer each command has, and the arguments") {
  CHECK(sd::response_of(sd::Cmd::GoIdle) == sd::Resp::None);
  CHECK(sd::response_of(sd::Cmd::AllSendCid) == sd::Resp::R2);
  CHECK(sd::response_of(sd::Cmd::SendCsd) == sd::Resp::R2);
  CHECK(sd::response_of(sd::Cmd::SendRelativeAddr) == sd::Resp::R6);
  CHECK(sd::response_of(sd::Cmd::SendIfCond) == sd::Resp::R7);
  CHECK(sd::response_of(sd::Cmd::AcmdSdSendOpCond) == sd::Resp::R3);
  CHECK(sd::response_of(sd::Cmd::SelectCard) == sd::Resp::R1b);
  CHECK(sd::response_of(sd::Cmd::StopTransmission) == sd::Resp::R1b);
  CHECK(sd::response_of(sd::Cmd::ReadSingleBlock) == sd::Resp::R1);
  CHECK(sd::response_of(sd::Cmd::ReadMultipleBlock) == sd::Resp::R1);
  CHECK(sd::response_of(sd::Cmd::AppCmd) == sd::Resp::R1);

  CHECK(sd::kIfCondArg == 0x1AAu);
  CHECK(sd::acmd41_arg(true) == 0x40FF8000u);   // HCS, 2.7 - 3.6 V
  CHECK(sd::acmd41_arg(false) == 0x00FF8000u);
  CHECK(sd::rca_arg(0xAAAA) == 0xAAAA0000u);
  CHECK(sd::kSwitchCheckHighSpeed == 0x00FFFFF1u);  // mode 0, group 1 function 1, the other groups unchanged
  CHECK(sd::kSwitchSetHighSpeed == 0x80FFFFF1u);    // mode 1
  CHECK(sd::kBusWidthArg4 == 2u);
}

TEST_CASE("sd protocol: R1, R6, R7 and OCR") {
  // R1 error bits: each of the spec's error bits counts, state and the ready flag do not.
  const unsigned errors[] = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 16, 15, 3};
  for (const unsigned bit : errors) {
    CHECK_MESSAGE(sd::r1_errors(1u << bit) != 0u, "bit " << bit);
  }
  const unsigned not_errors[] = {0, 1, 2, 4, 5, 8, 9, 10, 11, 12, 13, 14, 17, 18};
  for (const unsigned bit : not_errors) {
    CHECK_MESSAGE(sd::r1_errors(1u << bit) == 0u, "bit " << bit);
  }
  CHECK(sd::r1_errors(0x00000900u) == 0u);  // tran state, ready for data
  CHECK(sd::r1_state(0x00000900u) == sd::kStateTran);
  CHECK(sd::r1_state(0x00000120u) == 0u);   // idle, APP_CMD, ready: what CMD55 answers in the idle state
  // R7: the voltage bits and the pattern echo, whatever the upper bits are.
  CHECK(sd::r7_ok(0x000001AAu));
  CHECK(sd::r7_ok(0xFFFFF1AAu));
  CHECK(!sd::r7_ok(0x00000100u));
  CHECK(!sd::r7_ok(0x000000AAu));
  CHECK(!sd::r7_ok(0x000001ABu));
  // R6: RCA in the high half, three error bits in the low half.
  CHECK(sd::r6_rca(0x45670500u) == 0x4567u);
  CHECK(!sd::r6_error(0x45670500u));
  CHECK(sd::r6_error(0x45678500u));  // COM_CRC_ERROR
  CHECK(sd::r6_error(0x45674500u));  // ILLEGAL_COMMAND
  CHECK(sd::r6_error(0x45672500u));  // ERROR
  // The OCR: busy is bit 31 (set = ready), CCS bit 30.
  CHECK(sd::kOcrBusy == 0x80000000u);
  CHECK(sd::kOcrCcs == 0x40000000u);
  CHECK(sd::capacity_mb(15'523'840u) == 7580u);  // an 8 GB card: 7,580 MiB
}

TEST_CASE("sd protocol: the controller's R2 layout is rebuilt as the spec's register") {
  // CMDRSP3 holds bits 127:104 in its low 24 bits, CMDRSP2 103:72, CMDRSP1 71:40, CMDRSP0 39:8: the CRC7 and end bit are not in them.
  const std::uint32_t words[4] = {0xCCDDEEFFu, 0x8899AABBu, 0x44556677u, 0xFF112233u};  // [0] .. [3]; the top byte of word 3 is not part of the answer
  std::uint8_t reg[16];
  sd::unpack_r2(words, reg);
  const std::uint8_t expected[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
  CHECK(std::memcmp(reg, expected, 16) == 0);
  // Bit numbering: bit 127 is the top bit of byte 0, bit 0 the bottom of byte 15.
  CHECK(sd::reg_bits(reg, 127, 120) == 0x11u);
  CHECK(sd::reg_bits(reg, 119, 112) == 0x22u);
  CHECK(sd::reg_bits(reg, 15, 8) == 0xFFu);
  CHECK(sd::reg_bits(reg, 7, 0) == 0u);
  CHECK(sd::reg_bits(reg, 127, 96) == 0x11223344u);
  CHECK(sd::reg_bits(reg, 69, 48) == (((0x88u & 0x3Fu) << 16) | 0x99AAu));  // bits 69:48: the low 6 bits of byte 7 (0x88), then bytes 8 and 9 (0x99, 0xAA)
}

TEST_CASE("sd protocol: the CSD of a known 8 GB card (version 2) and of a 2 GB card (version 1)") {
  // An 8 GB SDHC card: C_SIZE 0x3B37, so (0x3B37 + 1) * 1024 = 15,523,840 blocks, the sector count 8 GB cards report; CCC 0x5B5 (class 10 is the
  // switch class), READ_BL_LEN 9. Bytes as the register is printed: 40 0E 00 32 5B 59 00 00 3B 37 7F 80 0A 40 40 01.
  const std::uint8_t sdhc8[16] = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00, 0x3B, 0x37, 0x7F, 0x80, 0x0A, 0x40, 0x40, 0x01};
  const sd::Csd a = sd::parse_csd(sdhc8);
  CHECK(a.valid);
  CHECK(a.version == 2);
  CHECK(a.blocks == 15'523'840u);
  CHECK(a.ccc == 0x5B5u);
  CHECK((a.ccc & sd::kCccSwitch) != 0u);
  CHECK(a.read_bl_len_log2 == 9);

  // A 2 GB SDSC card: READ_BL_LEN 10 (1,024-byte blocks), C_SIZE 4095, C_SIZE_MULT 7: 4096 * 2^9 * 1024 = 2^31 bytes = 4,194,304 blocks of 512.
  std::uint8_t sdsc[16] = {};
  set_bits(sdsc, 127, 126, 0);
  set_bits(sdsc, 95, 84, 0x1B5);  // class 10 absent: an old card without CMD6
  set_bits(sdsc, 83, 80, 10);
  set_bits(sdsc, 73, 62, 4095);
  set_bits(sdsc, 49, 47, 7);
  const sd::Csd b = sd::parse_csd(sdsc);
  CHECK(b.valid);
  CHECK(b.version == 1);
  CHECK(b.blocks == 4'194'304u);
  CHECK((b.ccc & sd::kCccSwitch) == 0u);
  CHECK(b.read_bl_len_log2 == 10);

  // A 64 MB card with 512-byte blocks: C_SIZE 255, C_SIZE_MULT 7, READ_BL_LEN 9: 256 * 512 = 131,072 blocks.
  std::uint8_t small[16] = {};
  set_bits(small, 83, 80, 9);
  set_bits(small, 73, 62, 255);
  set_bits(small, 49, 47, 7);
  CHECK(sd::parse_csd(small).blocks == 131'072u);

  // Not readable: a block under 512 bytes, the version 3 of the 2 TiB cards, a reserved structure, an empty register.
  std::uint8_t tiny[16] = {};
  set_bits(tiny, 83, 80, 8);
  set_bits(tiny, 73, 62, 100);
  CHECK(!sd::parse_csd(tiny).valid);
  std::uint8_t sduc[16] = {};
  set_bits(sduc, 127, 126, 2);
  const sd::Csd c = sd::parse_csd(sduc);
  CHECK(!c.valid);
  CHECK(c.version == 3);
  std::uint8_t zero[16] = {};
  CHECK(!sd::parse_csd(zero).valid);
  // The largest version 2 value that fits 32 bits, and the one that does not.
  std::uint8_t big[16] = {};
  set_bits(big, 127, 126, 1);
  set_bits(big, 69, 48, 0x3FFFFE);
  CHECK(sd::parse_csd(big).valid);
  CHECK(sd::parse_csd(big).blocks == 0xFFFFFC00u);
  set_bits(big, 69, 48, 0x3FFFFF);
  CHECK(!sd::parse_csd(big).valid);
}

TEST_CASE("sd protocol: the CID") {
  std::uint8_t reg[16] = {};
  set_bits(reg, 127, 120, 0x03);       // SanDisk
  set_bits(reg, 119, 112, 'S');
  set_bits(reg, 111, 104, 'D');
  const char* name = "SU08G";
  for (unsigned i = 0; i < 5; ++i) {
    set_bits(reg, 103u - 8u * i, 96u - 8u * i, static_cast<std::uint8_t>(name[i]));
  }
  set_bits(reg, 63, 56, 0x80);         // revision 8.0
  set_bits(reg, 55, 24, 0xDEADBEEFu);
  set_bits(reg, 19, 12, 23);           // 2023
  set_bits(reg, 11, 8, 9);             // September
  const sd::Cid cid = sd::parse_cid(reg);
  CHECK(cid.manufacturer == 0x03);
  CHECK(std::strcmp(cid.oem, "SD") == 0);
  CHECK(std::strcmp(cid.product, "SU08G") == 0);
  CHECK(cid.revision_major == 8);
  CHECK(cid.revision_minor == 0);
  CHECK(cid.serial == 0xDEADBEEFu);
  CHECK(cid.year == 2023);
  CHECK(cid.month == 9);
  // Bytes that are not printable ASCII do not reach the terminal.
  std::uint8_t odd[16] = {};
  set_bits(odd, 119, 112, 0x07);
  set_bits(odd, 103, 96, 0xFF);
  const sd::Cid o = sd::parse_cid(odd);
  CHECK(o.oem[0] == '.');
  CHECK(o.product[0] == '.');
}

TEST_CASE("sd protocol: the CMD6 switch status is read at the spec's bit positions") {
  std::uint8_t status[sd::kSwitchStatusBytes] = {};
  CHECK(!sd::switch_status_high_speed_supported(status));
  status[13] = 0x02;  // bit 401
  CHECK(sd::switch_status_high_speed_supported(status));
  status[13] = 0x01;  // bit 400: function 0 (default speed) supported, not function 1
  CHECK(!sd::switch_status_high_speed_supported(status));
  status[16] = 0xA1;
  CHECK(sd::switch_status_group1_selected(status) == 1u);  // bits 379:376 are the low nibble of byte 16
  status[16] = 0x0F;
  CHECK(sd::switch_status_group1_selected(status) == 0xFu);  // the switch failed
}
