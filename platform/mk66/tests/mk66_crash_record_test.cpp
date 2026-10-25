// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crash record (crash_record.h): encode, validate, decode, the CRC, the frame check. Pure functions, so they run on the
// host (ASan, UBSan) and on QEMU's Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/crash_record.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace crash = o3sdk::platform::mk66::crash;

namespace {

crash::FaultSnapshot sample() {
  crash::FaultSnapshot s{};
  s.exception = 6;           // UsageFault
  s.exc_return = 0xFFFFFFF9u;
  s.cfsr = 0x00010000u;      // UNDEFINSTR
  s.hfsr = 0x40000000u;      // FORCED
  s.mmfar = 0x20001234u;
  s.bfar = 0x70000010u;
  s.pc = 0x0000A1B2u;
  s.lr = 0x0000A1B9u;
  s.xpsr = 0x61000000u;
  s.sp = 0x2002FF00u;
  s.context = crash::make_context(0x0042, crash::Stage::Running);
  s.uptime_ms = 123456u;
  return s;
}

crash::CrashRecord nothing() {
  crash::CrashRecord r;
  std::memset(&r, 0, sizeof r);
  return r;
}

// Every field of two snapshots is equal.
bool same(const crash::FaultSnapshot& a, const crash::FaultSnapshot& b) {
  return std::memcmp(&a, &b, sizeof a) == 0;
}

}  // namespace

TEST_CASE("crash: crc32 is the IEEE CRC-32") {
  CHECK(crash::crc32("123456789", 9) == 0xCBF43926u);  // the standard check value
  CHECK(crash::crc32("", 0) == 0u);
  const std::uint8_t zeros[4] = {0, 0, 0, 0};
  CHECK(crash::crc32(zeros, 4) == 0x2144DF1Cu);
  const std::uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(crash::crc32(ff, 4) == 0xFFFFFFFFu);
}

TEST_CASE("crash: the layout is the documented one") {
  CHECK(sizeof(crash::CrashRecord) == 64);
  CHECK(alignof(crash::CrashRecord) == 8);
  CHECK(offsetof(crash::CrashRecord, magic) == 0);
  CHECK(offsetof(crash::CrashRecord, version) == 4);
  CHECK(offsetof(crash::CrashRecord, size) == 6);
  CHECK(offsetof(crash::CrashRecord, count) == 8);
  CHECK(offsetof(crash::CrashRecord, exception) == 12);
  CHECK(offsetof(crash::CrashRecord, exc_return) == 16);
  CHECK(offsetof(crash::CrashRecord, cfsr) == 20);
  CHECK(offsetof(crash::CrashRecord, hfsr) == 24);
  CHECK(offsetof(crash::CrashRecord, mmfar) == 28);
  CHECK(offsetof(crash::CrashRecord, bfar) == 32);
  CHECK(offsetof(crash::CrashRecord, pc) == 36);
  CHECK(offsetof(crash::CrashRecord, lr) == 40);
  CHECK(offsetof(crash::CrashRecord, xpsr) == 44);
  CHECK(offsetof(crash::CrashRecord, sp) == 48);
  CHECK(offsetof(crash::CrashRecord, context) == 52);
  CHECK(offsetof(crash::CrashRecord, uptime_ms) == 56);
  CHECK(offsetof(crash::CrashRecord, crc) == 60);
}

TEST_CASE("crash: magic reads 'O3CR' in memory") {
  crash::CrashRecord r = crash::encode(sample(), nothing());
  std::uint8_t bytes[4];
  std::memcpy(bytes, &r, 4);
  CHECK(bytes[0] == 'O');
  CHECK(bytes[1] == '3');
  CHECK(bytes[2] == 'C');
  CHECK(bytes[3] == 'R');
}

TEST_CASE("crash: encode, validate, decode round trip") {
  const crash::FaultSnapshot s = sample();
  const crash::CrashRecord r = crash::encode(s, nothing());
  CHECK(crash::valid(r));
  CHECK(r.count == 1u);
  CHECK(r.version == crash::kVersion);
  CHECK(r.size == 64);
  CHECK(same(crash::decode(r), s));
  CHECK(crash::module_of(r.context) == 0x0042);
  CHECK(crash::stage_of(r.context) == crash::Stage::Running);
}

TEST_CASE("crash: the count carries on from a valid record and restarts from anything else") {
  crash::CrashRecord first = crash::encode(sample(), nothing());
  CHECK(first.count == 1u);
  crash::CrashRecord second = crash::encode(sample(), first);
  CHECK(second.count == 2u);
  crash::CrashRecord third = crash::encode(sample(), second);
  CHECK(third.count == 3u);

  // A corrupt previous record is not trusted: the count restarts.
  crash::CrashRecord damaged = third;
  damaged.pc ^= 1u;
  CHECK_FALSE(crash::valid(damaged));
  CHECK(crash::encode(sample(), damaged).count == 1u);

  // The count saturates instead of wrapping to 0.
  crash::CrashRecord big = crash::encode(sample(), nothing());
  big.count = 0xFFFFFFFFu;
  big.crc = crash::crc32(&big, offsetof(crash::CrashRecord, crc));
  CHECK(crash::valid(big));
  CHECK(crash::encode(sample(), big).count == 0xFFFFFFFFu);
}

TEST_CASE("crash: any single flipped bit makes the record invalid") {
  const crash::CrashRecord good = crash::encode(sample(), nothing());
  CHECK(crash::valid(good));
  if (!crash::valid(good)) {
    return;  // no REQUIRE in a no-exceptions build
  }
  std::uint8_t bytes[sizeof good];
  std::memcpy(bytes, &good, sizeof good);
  unsigned accepted = 0;
  for (std::size_t byte = 0; byte < sizeof bytes; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
      crash::CrashRecord bad;
      std::memcpy(&bad, bytes, sizeof bad);
      if (crash::valid(bad)) {
        ++accepted;
      }
      bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
    }
  }
  CHECK(accepted == 0u);
}

TEST_CASE("crash: what RAM holds at power-up is not a record") {
  crash::CrashRecord r = nothing();
  CHECK_FALSE(crash::valid(r));
  std::memset(&r, 0xFF, sizeof r);
  CHECK_FALSE(crash::valid(r));
  std::memset(&r, 0xA5, sizeof r);
  CHECK_FALSE(crash::valid(r));
  // A deterministic scatter of garbage: none of it validates.
  std::uint32_t x = 0x12345678u;
  unsigned accepted = 0;
  for (int trial = 0; trial < 2000; ++trial) {
    std::uint32_t words[16];
    for (auto& w : words) {
      x = x * 1664525u + 1013904223u;
      w = x;
    }
    std::memcpy(&r, words, sizeof r);
    if (crash::valid(r)) {
      ++accepted;
    }
  }
  CHECK(accepted == 0u);
}

TEST_CASE("crash: magic, version and size are each checked on their own") {
  const crash::CrashRecord good = crash::encode(sample(), nothing());
  const auto reseal = [](crash::CrashRecord r) {
    r.crc = crash::crc32(&r, offsetof(crash::CrashRecord, crc));  // a record with a correct CRC but a wrong header
    return r;
  };
  crash::CrashRecord r = good;
  r.magic ^= 1u;
  CHECK_FALSE(crash::valid(reseal(r)));
  r = good;
  r.version = static_cast<std::uint16_t>(crash::kVersion + 1);
  CHECK_FALSE(crash::valid(reseal(r)));
  r = good;
  r.size = 60;
  CHECK_FALSE(crash::valid(reseal(r)));
  CHECK(crash::valid(reseal(good)));
}

TEST_CASE("crash: context packs a module and a stage") {
  for (const crash::Stage stage : {crash::Stage::Reset, crash::Stage::Clocks, crash::Stage::Memory, crash::Stage::Interrupts,
                                   crash::Stage::Sdram, crash::Stage::Running, crash::Stage::Unknown}) {
    for (const std::uint16_t module : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0xD1}, std::uint16_t{0xFFFF}}) {
      const std::uint32_t c = crash::make_context(module, stage);
      CHECK(crash::module_of(c) == module);
      CHECK(crash::stage_of(c) == stage);
    }
  }
  CHECK(crash::stage_of(crash::kUnknownContext) == crash::Stage::Unknown);
}

TEST_CASE("crash: an exception frame is plausible only wholly inside SRAM, word aligned") {
  CHECK(crash::frame_plausible(0x2002FF00u));
  CHECK(crash::frame_plausible(0x2002FFE0u));    // 32 bytes end exactly at 0x20030000
  CHECK(crash::frame_plausible(0x1FFF0000u));
  CHECK(crash::frame_plausible(0x20000000u));
  CHECK_FALSE(crash::frame_plausible(0x2002FFE4u));  // eight words would run past the end
  CHECK_FALSE(crash::frame_plausible(0x2002FF01u));  // unaligned
  CHECK_FALSE(crash::frame_plausible(0x2002FF02u));
  CHECK_FALSE(crash::frame_plausible(0x1FFEFFE0u));  // below SRAM
  CHECK_FALSE(crash::frame_plausible(0x00000000u));  // null, flash
  CHECK_FALSE(crash::frame_plausible(0x70000000u));  // SDRAM
  CHECK_FALSE(crash::frame_plausible(0xFFFFFFFFu));
  CHECK_FALSE(crash::frame_plausible(crash::kInvalidFrame));
}

TEST_CASE("crash: a record built without a frame says so") {
  crash::FaultSnapshot s = sample();
  s.sp = crash::kInvalidFrame;
  s.pc = 0x12345678u;
  s.exception = crash::kPanicException;
  const crash::CrashRecord r = crash::encode(s, nothing());
  CHECK(crash::valid(r));
  CHECK(r.sp == crash::kInvalidFrame);
  CHECK(r.exception == 0xFFu);
  CHECK(r.pc == 0x12345678u);
}

TEST_CASE("crash: the exception frame layout is r0 r1 r2 r3 r12 lr pc xpsr") {
  // ADR 0011: the frame indices are pinned. Distinct values in every slot: any shifted index shows.
  CHECK(crash::kFrameWords == 8u);
  CHECK(crash::kFrameLr == 5u);
  CHECK(crash::kFramePc == 6u);
  CHECK(crash::kFrameXpsr == 7u);
  const std::uint32_t frame[8] = {0xA0A0A0A0u, 0xA1A1A1A1u, 0xA2A2A2A2u, 0xA3A3A3A3u,
                                  0xAC0C0C0Cu /* r12 */, 0x0000BEEFu /* lr */, 0x0000C0DEu /* pc */, 0x61000000u /* xpsr */};
  const crash::FaultRegisters registers = {6, 0xFFFFFFF9u, 0x10000u, 0x40000000u, 0x20000010u, 0x70000020u,
                                           crash::make_context(7, crash::Stage::Sdram), 4242u};
  const crash::FaultSnapshot s = crash::snapshot(registers, frame, 0x2002FF00u, 0xDEADBEEFu);
  CHECK(s.pc == 0x0000C0DEu);
  CHECK(s.lr == 0x0000BEEFu);
  CHECK(s.xpsr == 0x61000000u);
  CHECK(s.sp == 0x2002FF00u);
  CHECK(s.exception == 6u);
  CHECK(s.exc_return == 0xFFFFFFF9u);
  CHECK(s.cfsr == 0x10000u);
  CHECK(s.hfsr == 0x40000000u);
  CHECK(s.mmfar == 0x20000010u);
  CHECK(s.bfar == 0x70000020u);
  CHECK(crash::module_of(s.context) == 7);
  CHECK(crash::stage_of(s.context) == crash::Stage::Sdram);
  CHECK(s.uptime_ms == 4242u);
}

TEST_CASE("crash: an implausible frame is never read") {
  const crash::FaultRegisters registers = {3, 0xFFFFFFF1u, 0x400u, 0x40000000u, 0, 0, crash::kUnknownContext, 7u};
  // A pointer that would fault the host (and ASan) if it were dereferenced: the frame address says it is not SRAM.
  const auto* poison = reinterpret_cast<const std::uint32_t*>(std::uintptr_t{0x10});
  for (const std::uint32_t address : {0x00000010u, 0x2002FFE4u, 0x2002FF01u, 0xFFFFFFFFu, 0x70000000u}) {
    const crash::FaultSnapshot s = crash::snapshot(registers, poison, address, 0x1234u);
    CHECK(s.sp == crash::kInvalidFrame);
    CHECK(s.pc == 0x1234u);
    CHECK(s.lr == 0x1234u);
    CHECK(s.xpsr == 0u);
    CHECK(s.cfsr == 0x400u);   // the registers still come through
    CHECK(s.exception == 3u);
  }
  const crash::FaultSnapshot none = crash::snapshot(registers, nullptr, 0x2002FF00u, 0x99u);
  CHECK(none.sp == crash::kInvalidFrame);
  CHECK(none.pc == 0x99u);
}
