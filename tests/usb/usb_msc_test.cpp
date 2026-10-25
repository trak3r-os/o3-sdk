// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U1a: the Bulk-Only Transport and SCSI engine (usb/msc.h) against a RAM block store and a host simulator that speaks BOT at packet level:
// the CBW and CSW bytes, the 13 data cases with their stalls and residues, the reset recovery, every CDB of the spec, sense data, unit attention,
// flow control (NAK), eject, store failures and a random-CBW fuzz. docs/specs/OS-USBDISK.md 3.2, 3.3, 3.4 and 11.
#include <doctest/doctest.h>

#include <o3sdk/usb/msc.h>

#include "usb_test_util.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3sdk::usb::msc;

namespace {

constexpr std::uint32_t kMps = 64;  // full-speed bulk

// A RAM image with the failure and call counters the tests need.
class RamStore final : public BlockStore {
 public:
  explicit RamStore(std::uint32_t blocks) : img_(static_cast<std::size_t>(blocks) * kBlockSize) {
    for (std::size_t i = 0; i < img_.size(); ++i) {
      img_[i] = static_cast<std::uint8_t>((i * 7u + (i >> 9) * 13u + 1u) & 0xFFu);
    }
  }
  bool present() const noexcept override { return present_; }
  std::uint32_t block_count() const noexcept override { return static_cast<std::uint32_t>(img_.size() / kBlockSize); }
  bool write_protected() const noexcept override { return wp_; }
  // The engine owes the store three things, and a call that breaks one is a failed test, not a Failed result the engine might swallow: it never reads or writes outside the
  // image, never writes to a write-protected store, and never touches the store after an eject (`guard` is the engine under test, when the test wants that checked).
  IoResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override {
    ++read_calls;
    if (guard != nullptr && guard->ejected()) {
      FAIL_CHECK("store read after an eject");
    }
    if (!present_) {
      return IoResult::NoMedium;
    }
    if (static_cast<std::uint64_t>(lba) + count > block_count()) {
      FAIL_CHECK("store read out of range: lba " << lba << " count " << count);
      return IoResult::Failed;
    }
    if (fail_read_lba == static_cast<std::int64_t>(lba)) {
      return IoResult::Failed;
    }
    std::memcpy(dst, &img_[static_cast<std::size_t>(lba) * kBlockSize], static_cast<std::size_t>(count) * kBlockSize);
    blocks_read += count;
    return IoResult::Ok;
  }
  IoResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override {
    ++write_calls;
    if (guard != nullptr && guard->ejected()) {
      FAIL_CHECK("store write after an eject");
    }
    if (!present_) {
      return IoResult::NoMedium;
    }
    if (wp_) {
      FAIL_CHECK("store write while write protected");
      return IoResult::WriteProtected;
    }
    if (static_cast<std::uint64_t>(lba) + count > block_count()) {
      FAIL_CHECK("store write out of range: lba " << lba << " count " << count);
      return IoResult::Failed;
    }
    if (fail_write_lba == static_cast<std::int64_t>(lba)) {
      return IoResult::Failed;
    }
    std::memcpy(&img_[static_cast<std::size_t>(lba) * kBlockSize], src, static_cast<std::size_t>(count) * kBlockSize);
    blocks_written += count;
    return IoResult::Ok;
  }
  void resize(std::uint32_t blocks) { img_.resize(static_cast<std::size_t>(blocks) * kBlockSize); }

  std::vector<std::uint8_t> img_;
  bool present_ = true;
  bool wp_ = false;
  const Engine* guard = nullptr;
  std::int64_t fail_read_lba = -1;
  std::int64_t fail_write_lba = -1;
  std::uint32_t read_calls = 0, write_calls = 0, blocks_read = 0, blocks_written = 0;
};

struct Csw {
  bool ok = false;  // signature and length right
  std::uint32_t tag = 0;
  std::uint32_t residue = 0;
  std::uint8_t status = 0xFF;
};

struct Reply {
  Csw csw;
  std::vector<std::uint8_t> data;  // what the host received in its data stage
  bool in_stalled = false;         // the IN pipe halted during the command (a halt the host had to clear)
  bool out_stalled = false;
  bool timeout = false;
};

constexpr std::uint8_t kIn = 0x80, kOut = 0x00;

std::vector<std::uint8_t> cbw(std::uint32_t tag, std::uint32_t hlen, std::uint8_t flags, std::uint8_t lun, const std::vector<std::uint8_t>& cdb,
                              std::uint8_t cb_len_field = 0xFF) {
  std::vector<std::uint8_t> c(31, 0);
  c[0] = 'U';
  c[1] = 'S';
  c[2] = 'B';
  c[3] = 'C';
  for (int i = 0; i < 4; ++i) {
    c[4 + i] = static_cast<std::uint8_t>(tag >> (8 * i));
    c[8 + i] = static_cast<std::uint8_t>(hlen >> (8 * i));
  }
  c[12] = flags;
  c[13] = lun;
  c[14] = cb_len_field == 0xFF ? static_cast<std::uint8_t>(cdb.size()) : cb_len_field;
  for (std::size_t i = 0; i < cdb.size() && i < 16; ++i) {
    c[15 + i] = cdb[i];
  }
  return c;
}

// The host side of one command, packet by packet, the way a host stack drives BOT: CBW, data stage until the length or a short packet or a stall,
// clear the stall, read the CSW. NAK is answered by running the main-loop service() (a bounded number of times: a livelock is a timeout).
class Host {
 public:
  explicit Host(Engine& e) : e_(e) {}

  Reply run_cbw(const std::vector<std::uint8_t>& c, std::uint32_t hlen, bool host_in, const std::vector<std::uint8_t>& out_data = {}) {
    Reply r;
    if (!send_out(c.data(), static_cast<std::uint32_t>(c.size()))) {
      r.timeout = true;
      return r;
    }
    e_.service();  // decodes the CBW
    if (hlen != 0) {
      if (host_in) {
        data_in(r, hlen);
      } else {
        data_out(r, hlen, out_data);
      }
    }
    read_csw(r);
    return r;
  }

  Reply run(std::uint32_t hlen, bool host_in, const std::vector<std::uint8_t>& cdb, const std::vector<std::uint8_t>& out_data = {}) {
    return run_cbw(cbw(++tag_, hlen, host_in ? kIn : kOut, 0, cdb), hlen, host_in, out_data);
  }

  std::uint32_t tag() const { return tag_; }

  bool send_out(const std::uint8_t* p, std::uint32_t n) {
    for (int guard = 0; guard < 100000; ++guard) {
      if (e_.out_stalled()) {
        return false;
      }
      if (e_.can_accept_out(kMps)) {
        e_.out_packet(p, n);
        return true;
      }
      e_.service();
    }
    return false;
  }

 private:
  void data_in(Reply& r, std::uint32_t hlen) {
    std::uint8_t buf[kMaxPacket];
    for (int guard = 0; guard < 1000000 && r.data.size() < hlen; ++guard) {
      const InPacket p = e_.in_packet(buf, kMps);
      if (p.kind == InPacket::Kind::Stall) {
        r.in_stalled = true;
        return;  // the host sees the halt, clears it below in read_csw
      }
      if (p.kind == InPacket::Kind::Nak) {
        e_.service();
        continue;
      }
      r.data.insert(r.data.end(), buf, buf + p.len);
      if (p.len < kMps) {
        return;  // a short packet ends the data stage
      }
    }
  }

  void data_out(Reply& r, std::uint32_t hlen, const std::vector<std::uint8_t>& src) {
    std::uint32_t sent = 0;
    for (int guard = 0; guard < 1000000 && sent < hlen; ++guard) {
      if (e_.out_stalled()) {
        r.out_stalled = true;
        return;
      }
      const std::uint32_t n = hlen - sent < kMps ? hlen - sent : kMps;
      if (e_.can_accept_out(kMps)) {
        std::uint8_t buf[kMps] = {};
        for (std::uint32_t i = 0; i < n; ++i) {
          buf[i] = sent + i < src.size() ? src[sent + i] : 0xA5;
        }
        e_.out_packet(buf, n);
        sent += n;
      } else {
        e_.service();
      }
    }
    // Let the main loop drain the block buffers; the engine may halt OUT at the end (cases 11, 13).
    for (int i = 0; i < 16; ++i) {
      e_.service();
    }
    if (e_.out_stalled()) {
      r.out_stalled = true;
    }
  }

  void read_csw(Reply& r) {
    std::uint8_t buf[kMaxPacket];
    for (int guard = 0; guard < 1000000; ++guard) {
      // A halted pipe: the host clears it (CLEAR_FEATURE) and reads the CSW. A persistent stall (after an invalid CBW) does not clear.
      if (e_.out_stalled()) {
        r.out_stalled = true;
        e_.clear_halt_out();
      }
      if (e_.in_stalled()) {
        r.in_stalled = true;
        e_.clear_halt_in();
        if (e_.in_stalled()) {
          return;  // persistent
        }
      }
      const InPacket p = e_.in_packet(buf, kMps);
      if (p.kind == InPacket::Kind::Stall) {
        r.in_stalled = true;
        continue;
      }
      if (p.kind == InPacket::Kind::Nak) {
        e_.service();
        continue;
      }
      r.csw.ok = p.len == kCswSize && buf[0] == 'U' && buf[1] == 'S' && buf[2] == 'B' && buf[3] == 'S';
      r.csw.tag = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (static_cast<std::uint32_t>(buf[7]) << 24);
      r.csw.residue = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (static_cast<std::uint32_t>(buf[11]) << 24);
      r.csw.status = buf[12];
      return;
    }
    r.timeout = true;
  }

  Engine& e_;
  std::uint32_t tag_ = 0x1000;
};

// CDB builders.
std::vector<std::uint8_t> tur() { return {0x00, 0, 0, 0, 0, 0}; }
std::vector<std::uint8_t> req_sense(std::uint8_t alloc = 18) { return {0x03, 0, 0, 0, alloc, 0}; }
std::vector<std::uint8_t> inquiry(std::uint16_t alloc = 36, std::uint8_t b1 = 0, std::uint8_t page = 0) {
  return {0x12, b1, page, static_cast<std::uint8_t>(alloc >> 8), static_cast<std::uint8_t>(alloc), 0};
}
std::vector<std::uint8_t> rw10(std::uint8_t op, std::uint32_t lba, std::uint16_t n) {
  return {op,
          0,
          static_cast<std::uint8_t>(lba >> 24),
          static_cast<std::uint8_t>(lba >> 16),
          static_cast<std::uint8_t>(lba >> 8),
          static_cast<std::uint8_t>(lba),
          0,
          static_cast<std::uint8_t>(n >> 8),
          static_cast<std::uint8_t>(n),
          0};
}
std::vector<std::uint8_t> read10(std::uint32_t lba, std::uint16_t n) { return rw10(0x28, lba, n); }
std::vector<std::uint8_t> write10(std::uint32_t lba, std::uint16_t n) { return rw10(0x2A, lba, n); }
std::vector<std::uint8_t> read_cap() { return {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }
std::vector<std::uint8_t> mode_sense6(std::uint8_t page = 0x3F, std::uint8_t alloc = 192) { return {0x1A, 0, page, 0, alloc, 0}; }
std::vector<std::uint8_t> start_stop(std::uint8_t b4) { return {0x1B, 0, 0, 0, b4, 0}; }
std::vector<std::uint8_t> prevent(std::uint8_t v) { return {0x1E, 0, 0, 0, v, 0}; }
std::vector<std::uint8_t> sync_cache() { return {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }
std::vector<std::uint8_t> verify10(std::uint32_t lba, std::uint16_t n, std::uint8_t b1 = 0) {
  std::vector<std::uint8_t> c = rw10(0x2F, lba, n);
  c[1] = b1;
  return c;
}
std::vector<std::uint8_t> read_fmt_caps(std::uint16_t alloc = 12) { return {0x23, 0, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(alloc >> 8), static_cast<std::uint8_t>(alloc), 0}; }

std::uint32_t rd_be32(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Runs a command that must pass and returns its reply.
struct Rig {
  RamStore store;
  Engine eng;
  Host host;
  explicit Rig(std::uint32_t blocks = 64) : store(blocks), eng(store), host(eng) { store.guard = &eng; }
  // Takes the unit attention that the first command after a change gets, so the test starts from a settled unit.
  void settle() { (void)host.run(0, false, tur()); }
};

bool passed(const Reply& r) { return r.csw.ok && r.csw.status == 0 && !r.timeout; }
bool failed(const Reply& r) { return r.csw.ok && r.csw.status == 1 && !r.timeout; }
bool phase_err(const Reply& r) { return r.csw.ok && r.csw.status == 2 && !r.timeout; }

Sense sense_of(Rig& rig) {
  const Reply s = rig.host.run(18, true, req_sense());
  Sense out;
  if (s.data.size() == 18 && s.data[0] == 0x70) {
    out = {s.data[2], s.data[12], s.data[13]};
  } else {
    out = {0xFF, 0xFF, 0xFF};
  }
  return out;
}

}  // namespace

TEST_CASE("msc: the CSW carries the signature, the tag, the residue and the status") {
  Rig rig;
  const Reply r = rig.host.run(0, false, tur());
  CHECK(r.csw.ok);
  CHECK(r.csw.tag == rig.host.tag());
  CHECK(r.csw.residue == 0);
  // The first command after construction sees a settled medium: no unit attention.
  CHECK(r.csw.status == 0);
  CHECK(rig.eng.idle());
  CHECK(Engine::max_lun() == 0);
}

TEST_CASE("msc: INQUIRY") {
  Rig rig;
  const Reply r = rig.host.run(36, true, inquiry());
  USB_REQUIRE(passed(r));
  USB_REQUIRE(r.data.size() == 36);
  CHECK(r.data[0] == 0x00);  // direct access
  CHECK((r.data[1] & 0x80) == 0x80);  // removable
  CHECK(r.data[4] == 31);
  CHECK(std::memcmp(&r.data[8], "Track3r ", 8) == 0);
  CHECK(std::memcmp(&r.data[16], "SD CARD         ", 16) == 0);
  CHECK(r.csw.residue == 0);
  // Allocation length smaller than the data: truncated, host length equal to it.
  const Reply s = rig.host.run(5, true, inquiry(5));
  CHECK(passed(s));
  CHECK(s.data.size() == 5);
  // VPD pages and the command support bit are refused.
  const Reply v = rig.host.run(0, false, inquiry(36, 1, 0x80));
  CHECK(failed(v));
  CHECK(sense_of(rig) == sense::kBadField);
  const Reply cd = rig.host.run(0, false, inquiry(36, 2, 0));
  CHECK(failed(cd));
  CHECK(sense_of(rig) == sense::kBadField);
  // INQUIRY does not disturb pending sense, and answers while a unit attention is pending.
  rig.eng.medium_changed();
  const Reply inq = rig.host.run(36, true, inquiry());
  CHECK(passed(inq));
  CHECK(sense_of(rig) == sense::kMediumChanged);  // REQUEST SENSE reports the attention INQUIRY left alone
}

TEST_CASE("msc: READ CAPACITY(10), READ FORMAT CAPACITIES, MODE SENSE(6)") {
  Rig rig(100);
  const Reply c = rig.host.run(8, true, read_cap());
  USB_REQUIRE(passed(c));
  USB_REQUIRE(c.data.size() == 8);
  CHECK(rd_be32(&c.data[0]) == 99);
  CHECK(rd_be32(&c.data[4]) == 512);

  const Reply f = rig.host.run(12, true, read_fmt_caps());
  USB_REQUIRE(passed(f));
  USB_REQUIRE(f.data.size() == 12);
  CHECK(f.data[3] == 8);
  CHECK(rd_be32(&f.data[4]) == 100);
  CHECK(f.data[8] == 2);
  CHECK(((f.data[9] << 16) | (f.data[10] << 8) | f.data[11]) == 512);

  const Reply m = rig.host.run(192, true, mode_sense6());
  USB_REQUIRE(passed(m));
  USB_REQUIRE(m.data.size() == 4);  // header only, no pages, short read of the host's 192
  CHECK(m.data[0] == 3);
  CHECK(m.data[2] == 0x00);
  CHECK(m.csw.residue == 192 - 4);
  rig.store.wp_ = true;
  const Reply w = rig.host.run(4, true, mode_sense6(0x3F, 4));
  USB_REQUIRE(passed(w));
  CHECK((w.data[2] & 0x80) == 0x80);  // write protected
  // A specific page, the caching page for one, is not offered.
  const Reply pg = rig.host.run(0, false, mode_sense6(0x08));
  CHECK(failed(pg));
  CHECK(sense_of(rig) == sense::kBadField);
  // READ CAPACITY with a nonzero LBA and no PMI is an invalid field.
  std::vector<std::uint8_t> bad = read_cap();
  bad[5] = 1;
  CHECK(failed(rig.host.run(0, false, bad)));
}

TEST_CASE("msc: READ(10) returns the image, WRITE(10) changes it") {
  Rig rig(64);
  const Reply r = rig.host.run(3 * 512, true, read10(5, 3));
  USB_REQUIRE(passed(r));
  USB_REQUIRE(r.data.size() == 3 * 512);
  CHECK(std::memcmp(r.data.data(), &rig.store.img_[5 * 512], 3 * 512) == 0);
  CHECK(r.csw.residue == 0);
  CHECK(rig.store.blocks_read == 3);

  std::vector<std::uint8_t> payload(2 * 512);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i * 31u + 7u);
  }
  const Reply w = rig.host.run(2 * 512, false, write10(10, 2), payload);
  USB_REQUIRE(passed(w));
  CHECK(w.csw.residue == 0);
  CHECK(std::memcmp(&rig.store.img_[10 * 512], payload.data(), payload.size()) == 0);
  CHECK(rig.store.blocks_written == 2);
  // Read it back through the engine.
  const Reply b = rig.host.run(2 * 512, true, read10(10, 2));
  CHECK(b.data == payload);
  // SYNCHRONIZE CACHE and VERIFY are accepted.
  CHECK(passed(rig.host.run(0, false, sync_cache())));
  CHECK(passed(rig.host.run(0, false, verify10(0, 64))));
  CHECK(failed(rig.host.run(0, false, verify10(0, 64, 0x02))));  // BYTCHK
  CHECK(failed(rig.host.run(0, false, verify10(60, 5))));        // past the end
}

TEST_CASE("msc: the last block, one past it, zero length, 64-bit range") {
  Rig rig(16);
  CHECK(passed(rig.host.run(512, true, read10(15, 1))));
  const Reply past = rig.host.run(512, true, read10(16, 1));
  CHECK(failed(past));
  CHECK(past.data.empty());
  CHECK(past.in_stalled);
  CHECK(past.csw.residue == 512);
  CHECK(sense_of(rig) == sense::kLbaRange);
  CHECK(failed(rig.host.run(1024, true, read10(15, 2))));
  CHECK(failed(rig.host.run(512, true, read10(0xFFFFFFFFu, 2))));  // lba + n overflows 32 bits
  CHECK(sense_of(rig) == sense::kLbaRange);
  // Zero blocks is not an error and moves nothing.
  const Reply z = rig.host.run(0, false, read10(3, 0));
  CHECK(passed(z));
  CHECK(passed(rig.host.run(0, false, write10(3, 0))));
  CHECK(passed(rig.host.run(0, false, read10(16, 0))));  // at the end, with nothing to move
  // WRITE past the end writes nothing.
  const std::uint32_t before = rig.store.blocks_written;
  const Reply wp = rig.host.run(1024, false, write10(15, 2));
  CHECK(failed(wp));
  CHECK(wp.out_stalled);
  CHECK(wp.csw.residue == 1024);
  CHECK(rig.store.blocks_written == before);
}

TEST_CASE("msc: unsupported commands answer ILLEGAL REQUEST, INVALID COMMAND OPERATION CODE") {
  Rig rig;
  const std::uint8_t ops[] = {0x88 /*READ(16)*/, 0x8A /*WRITE(16)*/, 0x5A /*MODE SENSE(10)*/, 0x9E /*SERVICE ACTION IN*/, 0x42 /*UNMAP-ish*/,
                              0xA0 /*REPORT LUNS*/, 0xFF, 0x08 /*READ(6)*/, 0x0A /*WRITE(6)*/, 0x55 /*MODE SELECT(10)*/};
  for (const std::uint8_t op : ops) {
    INFO("opcode " << static_cast<int>(op));
    std::vector<std::uint8_t> cdb(16, 0);
    cdb[0] = op;
    const Reply r = rig.host.run(0, false, cdb);
    CHECK(failed(r));
    CHECK(sense_of(rig) == sense::kBadOpcode);
  }
  // With a data stage the host expects: case 4 (stall IN, failed, full residue).
  std::vector<std::uint8_t> rd16(16, 0);
  rd16[0] = 0x88;
  rd16[13] = 1;
  const Reply r = rig.host.run(512, true, rd16);
  CHECK(failed(r));
  CHECK(r.in_stalled);
  CHECK(r.csw.residue == 512);
  CHECK(r.data.empty());
  // A CDB shorter than its opcode needs.
  CHECK(failed(rig.host.run(0, false, std::vector<std::uint8_t>{0x28, 0, 0, 0, 0, 0})));
  CHECK(sense_of(rig) == sense::kBadField);
}

TEST_CASE("msc: REQUEST SENSE reports and clears the sense of the failed command") {
  Rig rig;
  CHECK(sense_of(rig) == sense::kNone);
  CHECK(failed(rig.host.run(0, false, read10(1000, 1))));
  const Reply s = rig.host.run(18, true, req_sense());
  USB_REQUIRE(s.data.size() == 18);
  CHECK(s.data[0] == 0x70);
  CHECK(s.data[2] == 0x05);
  CHECK(s.data[7] == 10);
  CHECK(s.data[12] == 0x21);
  CHECK(s.data[13] == 0x00);
  CHECK(sense_of(rig) == sense::kNone);  // cleared by the REQUEST SENSE before
  // The next command clears the sense a failed one left (no autosense).
  CHECK(failed(rig.host.run(0, false, read10(1000, 1))));
  CHECK(passed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kNone);
  // Allocation length 8: truncated.
  CHECK(failed(rig.host.run(0, false, read10(1000, 1))));
  const Reply t = rig.host.run(8, true, req_sense(8));
  CHECK(t.data.size() == 8);
  // DESC=1 (descriptor format) is refused.
  std::vector<std::uint8_t> d = req_sense();
  d[1] = 1;
  CHECK(failed(rig.host.run(0, false, d)));
}

TEST_CASE("msc: write protect, no medium, medium change") {
  Rig rig;
  rig.store.wp_ = true;
  const Reply w = rig.host.run(512, false, write10(1, 1), std::vector<std::uint8_t>(512, 0x11));
  CHECK(failed(w));
  CHECK(w.out_stalled);
  CHECK(w.csw.residue == 512);
  CHECK(sense_of(rig) == sense::kDataProtect);
  CHECK(rig.store.write_calls == 0);
  CHECK(passed(rig.host.run(512, true, read10(1, 1))));  // read-only still reads

  rig.store.wp_ = false;
  rig.store.present_ = false;  // the card is pulled
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kMediumChanged);  // first the attention ...
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kNoMedium);  // ... then NOT READY, MEDIUM NOT PRESENT
  const std::uint32_t reads = rig.store.read_calls;
  const Reply rd = rig.host.run(512, true, read10(0, 1));
  CHECK(failed(rd));
  CHECK(rd.in_stalled);
  CHECK(rd.data.empty());
  CHECK(sense_of(rig) == sense::kNoMedium);
  CHECK(rig.store.read_calls == reads);  // the store is not asked for a medium that is not there
  CHECK(failed(rig.host.run(8, true, read_cap())));
  // INQUIRY and REQUEST SENSE answer without a medium; PREVENT ALLOW and READ FORMAT CAPACITIES too.
  CHECK(passed(rig.host.run(36, true, inquiry())));
  CHECK(passed(rig.host.run(0, false, prevent(0))));
  const Reply f = rig.host.run(12, true, read_fmt_caps());
  CHECK(passed(f));
  CHECK(f.data.size() == 12);
  CHECK(f.data[8] == 3);  // no media
  CHECK(rd_be32(&f.data[4]) == 0);

  // The card comes back: a unit attention first, then it works, with the capacity read again.
  rig.store.present_ = true;
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kMediumChanged);
  CHECK(passed(rig.host.run(0, false, tur())));
  // A different capacity is a change too.
  rig.store.resize(32);
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kMediumChanged);
  const Reply c = rig.host.run(8, true, read_cap());
  USB_REQUIRE(passed(c));
  CHECK(rd_be32(&c.data[0]) == 31);
  // medium_changed() (a remount behind the host's back) raises the attention once.
  rig.eng.medium_changed();
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(passed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc: an empty store (0 blocks) is no medium") {
  RamStore empty(0);
  Engine eng(empty);
  Host host(eng);
  CHECK(failed(host.run(0, false, tur())));
  CHECK(failed(host.run(8, true, read_cap())));
}

TEST_CASE("msc: PREVENT ALLOW MEDIUM REMOVAL and START STOP UNIT (eject)") {
  Rig rig;
  CHECK_FALSE(rig.eng.removal_prevented());
  CHECK(passed(rig.host.run(0, false, prevent(1))));
  CHECK(rig.eng.removal_prevented());
  // The host cannot eject while it prevents removal.
  const Reply e = rig.host.run(0, false, start_stop(0x02));
  CHECK(failed(e));
  CHECK(sense_of(rig) == sense::kRemovalPrevented);
  CHECK_FALSE(rig.eng.take_eject_request());
  CHECK_FALSE(rig.eng.ejected());
  CHECK(failed(rig.host.run(0, false, prevent(2))));  // only 0 and 1
  CHECK(passed(rig.host.run(0, false, prevent(0))));
  CHECK_FALSE(rig.eng.removal_prevented());
  // Now it can: the owner is told once, and the medium is gone for the host.
  CHECK(passed(rig.host.run(0, false, start_stop(0x02))));
  CHECK(rig.eng.ejected());
  CHECK(rig.eng.take_eject_request());
  CHECK_FALSE(rig.eng.take_eject_request());
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kNoMedium);
  CHECK(failed(rig.host.run(512, true, read10(0, 1))));
  CHECK(passed(rig.host.run(0, false, start_stop(0x02))));  // eject again: nothing to do, no new request
  CHECK_FALSE(rig.eng.take_eject_request());
  CHECK(failed(rig.host.run(0, false, start_stop(0x03))));  // the host cannot load it back
  CHECK(failed(rig.host.run(0, false, start_stop(0x10))));  // power conditions are not offered
  // The owner brings the medium back (a remount): attention, then ready.
  rig.eng.medium_changed();
  CHECK_FALSE(rig.eng.ejected());
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(passed(rig.host.run(0, false, tur())));
  CHECK(passed(rig.host.run(0, false, start_stop(0x01))));  // start without LoEj with a medium
}

TEST_CASE("msc: the unit's own eject overrides PREVENT and the store is not touched again") {
  Rig rig;
  CHECK(passed(rig.host.run(0, false, prevent(1))));
  rig.eng.unit_eject();
  CHECK(rig.eng.ejected());
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kMediumChanged);
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kNoMedium);
  const std::uint32_t r0 = rig.store.read_calls, w0 = rig.store.write_calls;
  CHECK(failed(rig.host.run(512, true, read10(0, 1))));
  CHECK(failed(rig.host.run(512, false, write10(0, 1), std::vector<std::uint8_t>(512, 1))));
  CHECK(rig.store.read_calls == r0);
  CHECK(rig.store.write_calls == w0);
}

TEST_CASE("msc: unit_eject in the middle of a WRITE: no store access afterwards, the command fails") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(7, 4 * 512, kOut, 0, write10(0, 4));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  rig.eng.service();  // decode
  std::uint8_t buf[kMps];
  std::memset(buf, 0x5A, sizeof buf);
  // 600 bytes in: block 0 is complete, block 1 half there.
  std::uint32_t sent = 0;
  while (sent < 600) {
    USB_REQUIRE(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(buf, 64);
    sent += 64;
  }
  rig.eng.service();  // block 0 is written
  CHECK(rig.store.write_calls == 1);
  rig.eng.unit_eject();
  const std::uint32_t w = rig.store.write_calls;
  for (int i = 0; i < 20; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.write_calls == w);
  CHECK(rig.eng.out_stalled());
  rig.eng.clear_halt_out();
  std::uint8_t o[kMps];
  InPacket p = rig.eng.in_packet(o, kMps);
  if (p.kind == InPacket::Kind::Stall) {
    rig.eng.clear_halt_in();
    p = rig.eng.in_packet(o, kMps);
  }
  USB_REQUIRE(p.kind == InPacket::Kind::Packet);
  CHECK(p.len == 13);
  CHECK(o[12] == 1);  // Failed
  CHECK(rig.eng.idle());
}

// The writable step (OS-USBDISK): a medium change ends the data stage in flight. The command was planned on the old medium (its LBAs, its capacity check); it must not go on, block after
// block, onto the card that was put in while the host's data was still arriving.
TEST_CASE("msc: a medium change in the middle of a WRITE ends the command, and the new medium gets none of its blocks") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(9, 4 * 512, kOut, 0, write10(10, 4));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  rig.eng.service();  // decode
  std::uint8_t buf[kMps];
  std::memset(buf, 0x77, sizeof buf);
  std::uint32_t sent = 0;
  while (sent < 600) {  // block 0 complete, block 1 half there
    USB_REQUIRE(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(buf, 64);
    sent += 64;
  }
  rig.eng.service();  // block 0 reaches the old medium
  CHECK(rig.store.write_calls == 1);
  CHECK(rig.store.blocks_written == 1);

  rig.eng.medium_changed();  // the card was pulled and another one started (the platform calls this from the same pass that let the first go)
  CHECK_FALSE(rig.eng.idle());  // the CSW is still owed
  CHECK(rig.eng.out_stalled());  // the host still had data to send: the pipe is halted until it clears it
  for (int i = 0; i < 20; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.write_calls == 1);  // the new medium got nothing of the command
  rig.eng.clear_halt_out();
  for (int i = 0; i < 20; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.write_calls == 1);  // (the packets the host sends after the clear are dropped by the pump's epoch in the image; here the engine is in SendCsw and takes none as data)
  std::uint8_t o[kMps];
  InPacket p = rig.eng.in_packet(o, kMps);
  if (p.kind == InPacket::Kind::Stall) {
    rig.eng.clear_halt_in();
    p = rig.eng.in_packet(o, kMps);
  }
  USB_REQUIRE(p.kind == InPacket::Kind::Packet);
  CHECK(p.len == 13);
  CHECK(o[12] == 1);                       // Failed
  CHECK((o[8] | (o[9] << 8) | (o[10] << 16)) == 3 * 512);  // the CSW residue (little-endian): what the whole blocks the old medium took (one) leave of the 4
  CHECK(rig.eng.sense() == sense::kNoMedium);
  CHECK(rig.eng.idle());
  // The next command is the host's first on the new medium: the unit attention, and then it works.
  CHECK(failed(rig.host.run(0, false, tur())));
  CHECK(sense_of(rig) == sense::kMediumChanged);
  CHECK(passed(rig.host.run(512, false, write10(2, 1), std::vector<std::uint8_t>(512, 0x33))));
  CHECK(rig.store.blocks_written == 2);
}

TEST_CASE("msc: a medium change in the middle of a READ ends the command; one that follows a finished command changes nothing") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(1, 4 * 512, kIn, 0, read10(0, 4));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  rig.eng.service();
  rig.eng.service();
  const std::uint32_t reads = rig.store.read_calls;
  rig.eng.medium_changed();
  for (int i = 0; i < 6; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.read_calls == reads);  // no further block of the old command is read from the new medium
  CHECK(rig.eng.in_stalled());
  rig.eng.clear_halt_in();
  std::uint8_t b[kMps];
  // medium_changed() aborted the data stage and clear_ring() dropped the block already read: nothing is left to drain, the first packet after the halt is the CSW (Failed).
  const InPacket p = rig.eng.in_packet(b, kMps);
  USB_REQUIRE(p.kind == InPacket::Kind::Packet);
  CHECK(p.len == 13u);
  CHECK(b[12] == 1);
  CHECK(rig.eng.sense() == sense::kNoMedium);

  // A finished command is not touched by a later medium change: this READ passes, its CSW says so, and the change only raises the attention for the next command.
  Rig other;
  other.settle();
  CHECK(passed(other.host.run(512, true, read10(0, 1))));
  other.eng.medium_changed();
  CHECK(other.eng.idle());
  CHECK(failed(other.host.run(0, false, tur())));
  CHECK(sense_of(other) == sense::kMediumChanged);
}

// ---- The thirteen cases of the Bulk-Only Transport, 6.7 -----------------------------------------------------------------------

TEST_CASE("msc BOT case 1: Hn = Dn") {
  Rig rig;
  const Reply ok = rig.host.run(0, false, tur());
  CHECK(passed(ok));
  CHECK(ok.csw.residue == 0);
  CHECK_FALSE(ok.in_stalled);
  CHECK_FALSE(ok.out_stalled);
  rig.store.present_ = false;
  rig.settle();  // the attention
  const Reply bad = rig.host.run(0, false, tur());
  CHECK(failed(bad));  // the command failed, not the transport
  CHECK(bad.csw.residue == 0);
  CHECK_FALSE(bad.in_stalled);
}

TEST_CASE("msc BOT case 2: Hn < Di is a phase error and moves nothing") {
  Rig rig;
  const Reply r = rig.host.run(0, false, read10(0, 1));
  CHECK(phase_err(r));
  CHECK(r.csw.residue == 0);
  CHECK(r.data.empty());
  CHECK(rig.store.read_calls == 0);
  CHECK(rig.eng.idle());
  // The engine is still in step: the next command works.
  CHECK(passed(rig.host.run(512, true, read10(0, 1))));
}

TEST_CASE("msc BOT case 3: Hn < Do is a phase error and writes nothing") {
  Rig rig;
  const Reply r = rig.host.run(0, false, write10(0, 1));
  CHECK(phase_err(r));
  CHECK(r.csw.residue == 0);
  CHECK(rig.store.write_calls == 0);
  CHECK(passed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc BOT case 4: Hi > Dn stalls IN, then the CSW with the whole length as residue") {
  Rig rig;
  const Reply r = rig.host.run(36, true, tur());
  CHECK(passed(r));
  CHECK(r.in_stalled);
  CHECK(r.data.empty());
  CHECK(r.csw.residue == 36);
  CHECK_FALSE(rig.eng.in_stalled());  // the host cleared it
  CHECK(passed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc BOT case 5: Hi > Di is a short transfer: the data, a stall, residue = the difference") {
  Rig rig;
  const Reply r = rig.host.run(255, true, inquiry(36));
  CHECK(passed(r));
  CHECK(r.data.size() == 36);
  CHECK(r.in_stalled);
  CHECK(r.csw.residue == 255 - 36);
  // A multi-packet short read: READ(10) of 1 block when the host asked for 2 is case 5, with a full-packet last packet.
  const Reply b = rig.host.run(1024, true, read10(2, 1));
  CHECK(passed(b));
  CHECK(b.data.size() == 512);
  CHECK(b.in_stalled);
  CHECK(b.csw.residue == 512);
  CHECK(std::memcmp(b.data.data(), &rig.store.img_[2 * 512], 512) == 0);
}

TEST_CASE("msc BOT case 6: Hi = Di") {
  Rig rig;
  const Reply r = rig.host.run(1024, true, read10(3, 2));
  CHECK(passed(r));
  CHECK(r.data.size() == 1024);
  CHECK_FALSE(r.in_stalled);
  CHECK(r.csw.residue == 0);
}

TEST_CASE("msc BOT case 7: Hi < Di sends what the host asked for, stalls, phase error") {
  Rig rig;
  const Reply r = rig.host.run(512 + 10, true, read10(3, 2));
  CHECK(phase_err(r));
  CHECK(r.data.size() == 522);
  CHECK(std::memcmp(r.data.data(), &rig.store.img_[3 * 512], 522) == 0);
  CHECK(r.in_stalled);
  CHECK(r.csw.residue == 0);
  // After the host's reset recovery the engine is clean (the recovery here is the stall clear done by the host).
  CHECK(passed(rig.host.run(512, true, read10(3, 1))));
}

TEST_CASE("msc BOT case 8: Hi < Do stalls IN with a phase error and the whole length as residue") {
  Rig rig;
  const Reply r = rig.host.run(512, true, write10(0, 1));
  CHECK(phase_err(r));
  CHECK(r.in_stalled);
  CHECK(r.csw.residue == 512);
  CHECK(rig.store.write_calls == 0);
}

TEST_CASE("msc BOT case 9: Ho > Dn stalls OUT, the CSW carries the whole length as residue") {
  Rig rig;
  const Reply r = rig.host.run(512, false, tur(), std::vector<std::uint8_t>(512, 3));
  CHECK(passed(r));
  CHECK(r.out_stalled);
  CHECK(r.csw.residue == 512);
  CHECK_FALSE(rig.eng.out_stalled());
  CHECK(passed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc BOT case 10: Ho <> Di stalls OUT with a phase error and the whole length as residue") {
  Rig rig;
  const Reply r = rig.host.run(512, false, read10(0, 1), std::vector<std::uint8_t>(512, 3));
  CHECK(phase_err(r));
  CHECK(r.out_stalled);
  CHECK(r.csw.residue == 512);
  CHECK(rig.store.read_calls == 0);
}

TEST_CASE("msc BOT case 11: Ho > Do takes what the device wants, stalls OUT, residue = the excess") {
  Rig rig;
  std::vector<std::uint8_t> payload(1024);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i ^ 0x5Cu);
  }
  const Reply r = rig.host.run(1024, false, write10(20, 1), payload);
  CHECK(passed(r));
  CHECK(r.out_stalled);
  CHECK(r.csw.residue == 512);
  CHECK(std::memcmp(&rig.store.img_[20 * 512], payload.data(), 512) == 0);
  CHECK(rig.store.blocks_written == 1);
  CHECK(passed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc BOT case 12: Ho = Do") {
  Rig rig;
  std::vector<std::uint8_t> payload(2048);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i * 3u);
  }
  const Reply r = rig.host.run(2048, false, write10(8, 4), payload);
  CHECK(passed(r));
  CHECK_FALSE(r.out_stalled);
  CHECK(r.csw.residue == 0);
  CHECK(std::memcmp(&rig.store.img_[8 * 512], payload.data(), 2048) == 0);
}

TEST_CASE("msc BOT case 13: Ho < Do keeps the whole blocks, drops the partial one, stalls OUT, phase error") {
  Rig rig;
  std::vector<std::uint8_t> payload(512 + 100, 0xC3);
  const std::vector<std::uint8_t> before(rig.store.img_.begin() + 31 * 512, rig.store.img_.begin() + 32 * 512);
  const Reply r = rig.host.run(512 + 100, false, write10(30, 2), payload);
  CHECK(phase_err(r));
  CHECK(r.out_stalled);
  CHECK(r.csw.residue == 0);
  CHECK(rig.store.blocks_written == 1);
  CHECK(std::memcmp(&rig.store.img_[30 * 512], payload.data(), 512) == 0);
  CHECK(std::equal(before.begin(), before.end(), rig.store.img_.begin() + 31 * 512));  // the partial block never reached the store
}

// ---- Invalid and not meaningful CBWs, reset recovery ---------------------------------------------------------------------------

TEST_CASE("msc: an invalid CBW stalls both pipes until the reset; clearing the halts first does nothing") {
  const auto bad_cbws = []() {
    std::vector<std::vector<std::uint8_t>> v;
    std::vector<std::uint8_t> sig = cbw(1, 0, kOut, 0, tur());
    sig[0] = 'X';
    v.push_back(sig);
    std::vector<std::uint8_t> short30 = cbw(1, 0, kOut, 0, tur());
    short30.pop_back();
    v.push_back(short30);
    std::vector<std::uint8_t> long32 = cbw(1, 0, kOut, 0, tur());
    long32.push_back(0);
    v.push_back(long32);
    v.push_back(std::vector<std::uint8_t>(31, 0));
    v.push_back(std::vector<std::uint8_t>(1, 'U'));
    return v;
  }();
  for (const auto& bad : bad_cbws) {
    INFO("len " << bad.size());
    Rig rig;
    rig.settle();
    CHECK(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(bad.data(), static_cast<std::uint32_t>(bad.size()));
    CHECK(rig.eng.in_stalled());
    CHECK(rig.eng.out_stalled());
    CHECK_FALSE(rig.eng.can_accept_out(kMps));
    std::uint8_t b[kMps];
    CHECK(rig.eng.in_packet(b, kMps).kind == InPacket::Kind::Stall);
    // The host clears too early: the stall persists.
    rig.eng.clear_halt_in();
    rig.eng.clear_halt_out();
    CHECK(rig.eng.in_stalled());
    CHECK(rig.eng.out_stalled());
    // Reset recovery: the reset, then both clears.
    rig.eng.bot_reset();
    CHECK(rig.eng.in_stalled());  // the reset does not clear a halt
    rig.eng.clear_halt_in();
    rig.eng.clear_halt_out();
    CHECK_FALSE(rig.eng.in_stalled());
    CHECK_FALSE(rig.eng.out_stalled());
    CHECK(passed(rig.host.run(0, false, tur())));
    CHECK(passed(rig.host.run(512, true, read10(0, 1))));
  }
}

TEST_CASE("msc: a CBW in the middle of a data stage or while the CSW is owed puts the engine out of step") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(9, 512, kIn, 0, read10(0, 1));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  rig.eng.service();
  // The host sends another CBW instead of reading the data.
  CHECK_FALSE(rig.eng.can_accept_out(kMps));
  rig.eng.out_packet(c.data(), 31);
  CHECK(rig.eng.in_stalled());
  CHECK(rig.eng.out_stalled());
  rig.eng.bot_reset();
  rig.eng.clear_halt_in();
  rig.eng.clear_halt_out();
  CHECK(rig.eng.idle());
  CHECK(passed(rig.host.run(512, true, read10(1, 1))));
}

TEST_CASE("msc: a CBW that is valid but not meaningful fails the command") {
  Rig rig;
  rig.settle();
  struct Case {
    std::uint8_t flags, lun, cb_len;
    Sense expect;
    const char* what;
  };
  const Case cases[] = {
      {kOut, 1, 6, sense::kBadLun, "LUN 1"},
      {kOut, 0x10, 6, sense::kBadField, "reserved bits of the LUN byte"},
      {0x01, 0, 6, sense::kBadField, "reserved flag bit"},
      {kOut, 0, 0, sense::kBadField, "CDB length 0"},
      {kOut, 0, 17, sense::kBadField, "CDB length 17"},
      {kOut, 0, 0x26, sense::kBadField, "reserved bits of the length byte"},
  };
  for (const Case& cs : cases) {
    INFO(cs.what);
    const Reply r = rig.host.run_cbw(cbw(5, 0, cs.flags, cs.lun, tur(), cs.cb_len), 0, false);
    CHECK(failed(r));
    CHECK(sense_of(rig) == cs.expect);
  }
  // With a data stage expected, the pipe is stalled as in case 4.
  const Reply r = rig.host.run_cbw(cbw(5, 36, kIn, 1, inquiry(), 6), 36, true);
  CHECK(failed(r));
  CHECK(r.in_stalled);
  CHECK(r.csw.residue == 36);
}

TEST_CASE("msc: the CSW tag echoes the CBW tag, whatever it is") {
  Rig rig;
  for (const std::uint32_t tag : {0u, 1u, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu}) {
    const Reply r = rig.host.run_cbw(cbw(tag, 0, kOut, 0, tur()), 0, false);
    CHECK(r.csw.ok);
    CHECK(r.csw.tag == tag);
  }
}

TEST_CASE("msc: a Bulk-Only reset drops the command in flight; blocks already read or written stay as they were") {
  Rig rig(64);
  rig.settle();
  // A read cut after 100 bytes.
  const std::vector<std::uint8_t> rc = cbw(1, 8 * 512, kIn, 0, read10(0, 8));
  USB_REQUIRE(rig.host.send_out(rc.data(), 31));
  rig.eng.service();
  std::uint8_t b[kMps];
  rig.eng.service();
  InPacket p = rig.eng.in_packet(b, kMps);
  CHECK(p.kind == InPacket::Kind::Packet);
  rig.eng.bot_reset();
  CHECK(rig.eng.idle());
  CHECK(passed(rig.host.run(512, true, read10(0, 1))));
  // A write cut mid-block: whole blocks written before the cut stay; the engine takes the next CBW.
  const std::vector<std::uint8_t> wc = cbw(2, 4 * 512, kOut, 0, write10(40, 4));
  USB_REQUIRE(rig.host.send_out(wc.data(), 31));
  rig.eng.service();
  std::uint8_t d[kMps];
  std::memset(d, 0x77, sizeof d);
  for (int i = 0; i < 9; ++i) {  // 576 bytes
    USB_REQUIRE(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(d, kMps);
  }
  rig.eng.service();
  const std::uint32_t written = rig.store.blocks_written;
  rig.eng.bot_reset();
  for (int i = 0; i < 10; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.blocks_written == written);  // nothing more is written after the reset
  CHECK(rig.eng.idle());
  CHECK(passed(rig.host.run(0, false, tur())));
  // The reset keeps the SCSI state: PREVENT stays.
  CHECK(passed(rig.host.run(0, false, prevent(1))));
  rig.eng.bot_reset();
  CHECK(rig.eng.removal_prevented());
}

// ---- Flow control and the block buffers ---------------------------------------------------------------------------------------

TEST_CASE("msc: IN is NAKed until the store has delivered, and the engine reads two blocks ahead, one per service()") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(1, 4 * 512, kIn, 0, read10(0, 4));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  std::uint8_t b[kMps];
  CHECK(rig.eng.in_packet(b, kMps).kind == InPacket::Kind::Nak);  // not decoded yet
  rig.eng.service();                                              // decode: no I/O yet
  CHECK(rig.store.read_calls == 0);
  CHECK(rig.eng.in_packet(b, kMps).kind == InPacket::Kind::Nak);  // the card has not been asked
  rig.eng.service();
  CHECK(rig.store.read_calls == 1);  // one block per pass
  rig.eng.service();
  CHECK(rig.store.read_calls == 2);
  rig.eng.service();
  CHECK(rig.store.read_calls == 2);  // both buffers full: no third read until one drains
  // Drain block 0 (8 packets); then the engine reads block 2 on the next pass.
  for (int i = 0; i < 8; ++i) {
    const InPacket p = rig.eng.in_packet(b, kMps);
    USB_REQUIRE(p.kind == InPacket::Kind::Packet);
    CHECK(p.len == kMps);
    CHECK(std::memcmp(b, &rig.store.img_[static_cast<std::size_t>(i) * kMps], kMps) == 0);
  }
  rig.eng.service();
  CHECK(rig.store.read_calls == 3);
  // Finish the transfer.
  std::size_t got = 512;
  for (int guard = 0; guard < 1000 && got < 4 * 512; ++guard) {
    const InPacket p = rig.eng.in_packet(b, kMps);
    if (p.kind == InPacket::Kind::Nak) {
      rig.eng.service();
      continue;
    }
    USB_REQUIRE(p.kind == InPacket::Kind::Packet);
    CHECK(std::memcmp(b, &rig.store.img_[got], p.len) == 0);
    got += p.len;
  }
  CHECK(got == 4 * 512);
  CHECK(rig.store.read_calls == 4);
  const InPacket csw = rig.eng.in_packet(b, kMps);
  CHECK(csw.kind == InPacket::Kind::Packet);
  CHECK(csw.len == 13);
  CHECK(b[12] == 0);
}

TEST_CASE("msc: OUT is NAKed when both buffers are full and taken again after a block is written") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(1, 4 * 512, kOut, 0, write10(0, 4));
  CHECK(rig.eng.can_accept_out(kMps));  // a CBW
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  CHECK_FALSE(rig.eng.can_accept_out(kMps));  // a CBW is waiting to be decoded
  rig.eng.service();
  std::uint8_t d[kMps];
  std::memset(d, 0x42, sizeof d);
  int packets = 0;
  while (rig.eng.can_accept_out(kMps)) {
    rig.eng.out_packet(d, kMps);
    ++packets;
    USB_REQUIRE(packets <= 16);
  }
  CHECK(packets == 16);  // two blocks of 8 packets, with no service() in between
  CHECK(rig.store.write_calls == 0);
  rig.eng.service();  // block 0 to the store
  CHECK(rig.store.write_calls == 1);
  CHECK(rig.eng.can_accept_out(kMps));
  for (int i = 0; i < 8; ++i) {
    USB_REQUIRE(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(d, kMps);
  }
  CHECK_FALSE(rig.eng.can_accept_out(kMps));  // three blocks are in and two buffers hold blocks 1 and 2
  rig.eng.service();                          // block 1 to the store
  CHECK(rig.store.write_calls == 2);
  for (int i = 0; i < 8; ++i) {
    USB_REQUIRE(rig.eng.can_accept_out(kMps));
    rig.eng.out_packet(d, kMps);
  }
  CHECK_FALSE(rig.eng.can_accept_out(kMps));  // all four blocks are in: nothing more is expected
  for (int i = 0; i < 6; ++i) {
    rig.eng.service();
  }
  CHECK(rig.store.write_calls == 4);
  std::uint8_t o[kMps];
  const InPacket p = rig.eng.in_packet(o, kMps);
  CHECK(p.kind == InPacket::Kind::Packet);
  CHECK(o[12] == 0);
}

TEST_CASE("msc: high speed packets (512 bytes) move whole blocks") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(1, 1024, kIn, 0, read10(4, 2));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  std::uint8_t b[kMaxPacket];
  std::size_t got = 0;
  for (int guard = 0; guard < 100 && got < 1024; ++guard) {
    rig.eng.service();
    const InPacket p = rig.eng.in_packet(b, 512);
    if (p.kind == InPacket::Kind::Packet) {
      CHECK(p.len == 512);
      CHECK(std::memcmp(b, &rig.store.img_[(4u + got / 512) * 512], 512) == 0);
      got += p.len;
    }
  }
  CHECK(got == 1024);
  // An odd packet size (100) still gives full packets mid-stream and crosses the block boundary.
  const std::vector<std::uint8_t> c2 = cbw(2, 1024, kIn, 0, read10(4, 2));
  std::uint8_t cs[kMaxPacket];
  CHECK(rig.eng.in_packet(cs, 512).kind == InPacket::Kind::Packet);  // the first CSW
  USB_REQUIRE(rig.host.send_out(c2.data(), 31));
  std::vector<std::uint8_t> all;
  for (int guard = 0; guard < 1000 && all.size() < 1024; ++guard) {
    rig.eng.service();
    const InPacket p = rig.eng.in_packet(b, 100);
    if (p.kind == InPacket::Kind::Packet) {
      if (all.size() + p.len < 1024) {
        CHECK(p.len == 100);
      }
      all.insert(all.end(), b, b + p.len);
    }
  }
  CHECK(all.size() == 1024);
  CHECK(std::memcmp(all.data(), &rig.store.img_[4 * 512], 1024) == 0);
}

// ---- The store fails --------------------------------------------------------------------------------------------------------

TEST_CASE("msc: a read error mid-transfer stalls IN and fails the command with the residue of what was delivered") {
  Rig rig;
  rig.store.fail_read_lba = 7;
  const Reply r = rig.host.run(4 * 512, true, read10(5, 4));
  CHECK(failed(r));
  CHECK(r.in_stalled);
  CHECK((r.data.size() >= 512 && r.data.size() <= 2 * 512));  // block 5 (and maybe 6) reached the host, block 7 never
  CHECK(std::memcmp(r.data.data(), &rig.store.img_[5 * 512], r.data.size()) == 0);
  CHECK(r.csw.residue == 4 * 512 - r.data.size());
  CHECK(sense_of(rig) == sense::kReadError);
  rig.store.fail_read_lba = -1;
  CHECK(passed(rig.host.run(512, true, read10(7, 1))));
}

TEST_CASE("msc: a write error mid-transfer stalls OUT and fails the command; the residue counts whole blocks the store took") {
  Rig rig;
  rig.store.fail_write_lba = 12;
  // (8 blocks: the ring holds two, so when block 12 fails the host still has data to send and OUT is stalled; a transfer the host had sent whole is the next test)
  const Reply r = rig.host.run(8 * 512, false, write10(10, 8), std::vector<std::uint8_t>(8 * 512, 0x99));
  CHECK(failed(r));
  CHECK(r.out_stalled);
  CHECK(r.csw.residue == 6 * 512);
  CHECK(rig.store.blocks_written == 2);
  CHECK(sense_of(rig) == sense::kWriteError);
  rig.store.fail_write_lba = -1;
  CHECK(passed(rig.host.run(512, false, write10(12, 1), std::vector<std::uint8_t>(512, 1))));
}

TEST_CASE("msc: a write error after the host sent every byte does not stall OUT (the host waits for the CSW)") {
  Rig rig;
  rig.store.fail_write_lba = 12;
  const Reply r = rig.host.run(512, false, write10(12, 1), std::vector<std::uint8_t>(512, 0x99));
  CHECK(failed(r));
  CHECK_FALSE(r.out_stalled);
  CHECK(r.csw.residue == 512);
  CHECK(sense_of(rig) == sense::kWriteError);
  rig.store.fail_write_lba = -1;
  CHECK(passed(rig.host.run(512, false, write10(12, 1), std::vector<std::uint8_t>(512, 1))));
}

TEST_CASE("msc: reset_all (bus reset, SET_CONFIGURATION) forgets PREVENT, the unit attention and the sense data, drops the command in flight, and keeps the eject latch") {
  Rig rig;
  CHECK(passed(rig.host.run(0, false, prevent(1))));
  rig.eng.unit_eject();
  CHECK(failed(rig.host.run(0, false, tur())));  // the attention
  CHECK(failed(rig.host.run(0, false, tur())));  // NOT READY, MEDIUM NOT PRESENT
  CHECK(rig.eng.ejected());
  CHECK(rig.eng.removal_prevented());
  CHECK(rig.eng.sense() == sense::kNoMedium);
  // a command half way, the IN pipe halted by a bad CBW
  const std::vector<std::uint8_t> bad(31, 0x55);
  USB_REQUIRE(rig.host.send_out(bad.data(), 31));
  CHECK(rig.eng.in_stalled());
  CHECK(rig.eng.out_stalled());

  rig.eng.reset_all();
  CHECK(rig.eng.ejected());  // the card is the unit's: a new session must not see it
  CHECK_FALSE(rig.eng.removal_prevented());
  CHECK(rig.eng.sense() == Sense{});
  CHECK_FALSE(rig.eng.in_stalled());
  CHECK_FALSE(rig.eng.out_stalled());
  CHECK(rig.eng.idle());
  CHECK_FALSE(rig.eng.take_eject_request());
  CHECK(failed(rig.host.run(0, false, tur())));  // still NOT READY, MEDIUM NOT PRESENT: no new session brings the card back
  CHECK(rig.eng.sense() == sense::kNoMedium);
  CHECK(failed(rig.host.run(512, true, read10(0, 1))));
  rig.eng.medium_changed();  // a fresh hand-over to the host lifts it
  CHECK(failed(rig.host.run(0, false, tur())));  // the one UNIT ATTENTION
  CHECK(passed(rig.host.run(0, false, tur())));
  CHECK(passed(rig.host.run(512, true, read10(0, 1))));
}

TEST_CASE("msc: a host eject survives a bus reset and SET_CONFIGURATION (the unit's eject request is still owed)") {
  Rig rig;
  CHECK(passed(rig.host.run(0, false, start_stop(0x02))));
  rig.eng.reset_all();
  CHECK(rig.eng.ejected());
  CHECK(rig.eng.take_eject_request());
  CHECK(failed(rig.host.run(0, false, tur())));
}

TEST_CASE("msc: READ FORMAT CAPACITIES says no media after an eject, as every other command does") {
  Rig rig;
  rig.settle();
  const Reply on = rig.host.run(12, true, read_fmt_caps());
  USB_REQUIRE(passed(on));
  USB_REQUIRE(on.data.size() == 12);
  CHECK(on.data[8] == 2);  // formatted media
  CHECK(on.data[7] == 64);  // 64 blocks
  rig.eng.unit_eject();
  (void)rig.host.run(0, false, tur());  // the attention
  const Reply off = rig.host.run(12, true, read_fmt_caps());
  USB_REQUIRE(passed(off));
  USB_REQUIRE(off.data.size() == 12);
  CHECK(off.data[8] == 3);  // no media
  CHECK(off.data[4] == 0);
  CHECK(off.data[5] == 0);
  CHECK(off.data[6] == 0);
  CHECK(off.data[7] == 0);
}

TEST_CASE("msc: the card pulled in the middle of a transfer is NOT READY, MEDIUM NOT PRESENT") {
  Rig rig;
  rig.settle();
  const std::vector<std::uint8_t> c = cbw(1, 4 * 512, kIn, 0, read10(0, 4));
  USB_REQUIRE(rig.host.send_out(c.data(), 31));
  rig.eng.service();
  rig.eng.service();
  rig.store.present_ = false;
  for (int i = 0; i < 4; ++i) {
    rig.eng.service();
  }
  std::uint8_t b[kMps];
  // The failed store read aborts the data stage and drops the block already read with it (abort_data, clear_ring): the stall comes at once, with nothing to drain first.
  InPacket p = rig.eng.in_packet(b, kMps);
  CHECK(p.kind == InPacket::Kind::Stall);
  rig.eng.clear_halt_in();
  p = rig.eng.in_packet(b, kMps);
  USB_REQUIRE(p.kind == InPacket::Kind::Packet);
  CHECK(b[12] == 1);
  CHECK(rig.eng.sense() == sense::kNoMedium);
}

// ---- Random input ---------------------------------------------------------------------------------------------------------------

TEST_CASE("msc fuzz: random CBWs and packet interleavings never break the engine; a reset recovery always restores it") {
  usb_test::Rng rng(0xC0FFEE11u);
  RamStore store(32);
  Engine eng(store);
  store.guard = &eng;
  Host host(eng);
  std::uint8_t buf[kMaxPacket];
  for (int round = 0; round < 4000; ++round) {
    std::vector<std::uint8_t> c(64);
    for (auto& x : c) {
      x = static_cast<std::uint8_t>(rng.next());
    }
    if (rng.below(8) != 0) {  // mostly with the right signature
      c[0] = 'U';
      c[1] = 'S';
      c[2] = 'B';
      c[3] = 'C';
    }
    if (rng.below(4) != 0) {
      c[12] &= 0x80;
      c[13] &= 0x01;
      c[14] = static_cast<std::uint8_t>(6 + 4 * rng.below(2));
      static const std::uint8_t ops[] = {0x00, 0x03, 0x12, 0x1A, 0x1B, 0x1E, 0x23, 0x25, 0x28, 0x2A, 0x2F, 0x35, 0x88, 0x5A};
      c[15] = ops[rng.below(sizeof ops)];
      if (rng.below(8) != 0) {  // mostly a small LBA (cdb[2..5] = c[17..20], the LSB last) and a small count (cdb[7..8] = c[22..23]); sometimes the random bytes stay: the engine must range-check
        c[17] = c[18] = c[19] = 0;
        c[20] = static_cast<std::uint8_t>(rng.below(34));
        c[22] = 0;
        c[23] = static_cast<std::uint8_t>(rng.below(5));
      }
      c[8] = static_cast<std::uint8_t>(rng.below(4) == 0 ? rng.next() : (rng.below(3) * 0x00));
      c[9] = static_cast<std::uint8_t>(rng.below(5));
      c[10] = 0;
      c[11] = 0;
    }
    if (rng.below(16) == 0) {
      store.present_ = !store.present_;
    }
    if (rng.below(32) == 0) {
      eng.medium_changed();
    }
    // Deliver it and then do random things: service, packets in and out, clears, resets.
    if (eng.can_accept_out(kMps)) {
      eng.out_packet(c.data(), static_cast<std::uint32_t>(rng.below(20) == 0 ? rng.below(40) : 31));
    }
    for (int step = 0; step < 40; ++step) {
      switch (rng.below(7)) {
        case 0:
          eng.service();
          break;
        case 1: {
          const InPacket p = eng.in_packet(buf, kMps);
          CHECK(p.len <= kMps);
          break;
        }
        case 2:
          if (eng.can_accept_out(kMps)) {
            std::uint8_t d[kMps];
            for (auto& x : d) {
              x = static_cast<std::uint8_t>(rng.next());
            }
            eng.out_packet(d, 1 + rng.below(kMps));
          }
          break;
        case 3:
          eng.clear_halt_in();
          break;
        case 4:
          eng.clear_halt_out();
          break;
        case 5:
          if (rng.below(6) == 0) {
            eng.bot_reset();
          }
          break;
        default:
          eng.service();
          break;
      }
    }
    // Reset recovery, then the unit must answer a plain command.
    eng.bot_reset();
    eng.clear_halt_in();
    eng.clear_halt_out();
    store.present_ = true;
    eng.medium_changed();  // also ends an eject the random START STOP UNIT may have made
    CHECK_FALSE(eng.in_stalled());
    CHECK_FALSE(eng.out_stalled());
    (void)host.run(0, false, tur());  // may carry an attention
    const Reply ok = host.run(0, false, tur());
    USB_REQUIRE(passed(ok));
  }
}

TEST_CASE("msc property: random READ/WRITE with random host lengths agree with a model image") {
  usb_test::Rng rng(0x5EED1234u);
  Rig rig(48);
  rig.settle();
  std::vector<std::uint8_t> model = rig.store.img_;
  for (int round = 0; round < 600; ++round) {
    const std::uint32_t lba = rng.below(48);
    const std::uint16_t n = static_cast<std::uint16_t>(rng.below(5));
    const bool write = rng.below(2) == 0;
    const std::uint32_t dev = n * 512u;
    // The host's length: usually the device's, sometimes shorter or longer, sometimes the wrong direction.
    std::uint32_t hlen = dev;
    switch (rng.below(6)) {
      case 0:
        hlen = dev + 1 + rng.below(700);
        break;
      case 1:
        hlen = dev == 0 ? 0 : rng.below(dev + 1);
        break;
      default:
        break;
    }
    const bool in_range = static_cast<std::uint64_t>(lba) + n <= 48;
    std::vector<std::uint8_t> payload(hlen);
    for (auto& x : payload) {
      x = static_cast<std::uint8_t>(rng.next());
    }
    const bool host_in = !write;  // the right direction; wrong ones are the BOT cases above
    const Reply r = rig.host.run(hlen, host_in, write ? write10(lba, n) : read10(lba, n), payload);
    USB_REQUIRE(r.csw.ok);
    USB_REQUIRE(!r.timeout);
    if (!in_range) {
      CHECK(r.csw.status == 1);
      continue;
    }
    if (write) {
      const std::uint32_t whole = (hlen < dev ? hlen : dev) / 512u;  // whole blocks the store takes
      if (whole != 0) {
        std::memcpy(&model[static_cast<std::size_t>(lba) * 512], payload.data(), static_cast<std::size_t>(whole) * 512);
      }
      CHECK(r.csw.status == (hlen < dev ? 2 : 0));
    } else {
      const std::size_t got = hlen < dev ? hlen : dev;
      CHECK(r.data.size() == got);
      CHECK((got == 0 || std::memcmp(r.data.data(), &model[static_cast<std::size_t>(lba) * 512], got) == 0));
      CHECK(r.csw.status == (hlen < dev ? 2 : 0));
    }
    CHECK(rig.store.img_ == model);
  }
}
