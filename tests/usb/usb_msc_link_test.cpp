// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U1c part 2: the ISR-to-main link of the Mass Storage function (usb/msc_link.h, docs/specs/OS-USBDISK.md 3.4) with the real UsbDevice interrupt logic over the fake controller of
// usb_fake_controller.h: GET MAX LUN, Bulk-Only Reset, the OUT/IN rings (queue full -> NAK), the stall after the last IN packet, a reset mid-write, a clear-halt mid-read, stale data dropped
// by the epoch, and a pseudo-random interleave of host packets and pump steps standing in for the interrupt racing the main loop.
#include <doctest/doctest.h>

#include <o3sdk/usb/msc.h>
#include <o3sdk/usb/msc_link.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3sdk::usb;
using usb_rig::Rig;

namespace {

constexpr unsigned kOutEp = 4, kInEp = 5;
constexpr std::uint8_t kInAddr = 0x85;

class RamStore final : public msc::BlockStore {
 public:
  explicit RamStore(std::uint32_t blocks) : img(static_cast<std::size_t>(blocks) * msc::kBlockSize) {
    for (std::size_t i = 0; i < img.size(); ++i) {
      img[i] = static_cast<std::uint8_t>((i * 5u + (i >> 9) * 11u + 3u) & 0xFFu);
    }
  }
  bool present() const noexcept override { return true; }
  std::uint32_t block_count() const noexcept override { return static_cast<std::uint32_t>(img.size() / msc::kBlockSize); }
  bool write_protected() const noexcept override { return false; }
  msc::IoResult read(std::uint32_t lba, std::uint32_t count, void* dst) noexcept override {
    std::memcpy(dst, &img[static_cast<std::size_t>(lba) * msc::kBlockSize], static_cast<std::size_t>(count) * msc::kBlockSize);
    return msc::IoResult::Ok;
  }
  msc::IoResult write(std::uint32_t lba, std::uint32_t count, const void* src) noexcept override {
    std::memcpy(&img[static_cast<std::size_t>(lba) * msc::kBlockSize], src, static_cast<std::size_t>(count) * msc::kBlockSize);
    return msc::IoResult::Ok;
  }
  std::vector<std::uint8_t> img;
};

Rig* g_rig = nullptr;
void kick() { g_rig->dev.isr(); }  // the pended USB interrupt

struct Fixture {
  Fixture() : store(64), engine(store), pump(link, engine, &kick) {
    Config cfg;
    cfg.mode = Mode::Disk;
    r = &usb_rig::fresh(cfg);
    g_rig = r;
    r->dev.bind_msc(&link);
    USB_REQUIRE(r->enumerate());
  }
  // The interrupt and the main loop, a few rounds.
  void settle(int rounds = 3) {
    for (int i = 0; i < rounds; ++i) {
      r->dev.isr();
      pump.step();
    }
  }
  bool send(const std::uint8_t* d, std::uint32_t n) {
    for (int i = 0; i < 40; ++i) {
      if (r->host.bulk_out(kOutEp, d, n) == usb_rig::Reply::Ack) {
        return true;
      }
      settle(1);
    }
    return false;
  }
  // One IN packet, running the main loop while the device NAKs. Returns Ack, Stall or Nak (gave up).
  usb_rig::Reply recv(std::uint8_t* d, std::uint32_t& n) {
    for (int i = 0; i < 40; ++i) {
      const usb_rig::Reply a = r->host.bulk_in(kInEp, d, 64, n);
      if (a != usb_rig::Reply::Nak) {
        return a;
      }
      settle(1);
    }
    return usb_rig::Reply::Nak;
  }
  void cbw(std::uint32_t tag, std::uint32_t hlen, bool in, const std::vector<std::uint8_t>& cdb) {
    std::uint8_t c[31] = {'U', 'S', 'B', 'C'};
    std::memcpy(c + 4, &tag, 4);
    std::memcpy(c + 8, &hlen, 4);
    c[12] = in ? 0x80 : 0x00;
    c[14] = static_cast<std::uint8_t>(cdb.size());
    std::memcpy(c + 15, cdb.data(), cdb.size());
    USB_REQUIRE(send(c, 31));
  }
  static std::vector<std::uint8_t> rw10(std::uint8_t op, std::uint32_t lba, std::uint16_t n) {
    return {op, 0, static_cast<std::uint8_t>(lba >> 24), static_cast<std::uint8_t>(lba >> 16), static_cast<std::uint8_t>(lba >> 8), static_cast<std::uint8_t>(lba), 0,
            static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n), 0};
  }
  // CSW: 13 bytes, status in the last. Returns the status or 0xFF.
  int csw(std::uint32_t tag) {
    std::uint8_t d[64];
    std::uint32_t n = 0;
    if (recv(d, n) != usb_rig::Reply::Ack || n != 13u) {
      return 0xFF;
    }
    std::uint32_t t = 0;
    std::memcpy(&t, d + 4, 4);
    return t == tag ? d[12] : 0xFE;
  }
  std::vector<std::uint8_t> read_data(std::uint32_t bytes) {
    std::vector<std::uint8_t> out;
    while (out.size() < bytes) {
      std::uint8_t d[64];
      std::uint32_t n = 0;
      if (recv(d, n) != usb_rig::Reply::Ack) {
        break;
      }
      out.insert(out.end(), d, d + n);
    }
    return out;
  }
  bool endpoint_halted(std::uint8_t ep) {
    std::uint8_t raw[8], d[2] = {};
    usb_fake::make_setup(raw, 0x82, 0x00, 0, ep, 2);
    const auto res = r->host.control_in(raw, d, 2);
    return res.ok && (d[0] & 1u) != 0u;
  }
  void clear_halt(std::uint8_t ep) {
    USB_REQUIRE(r->set_feature_halt(false, ep).ok);
    settle();
  }
  Rig* r = nullptr;
  RamStore store;
  msc::Engine engine;
  MscLink link;
  MscPump pump;
};

}  // namespace

TEST_CASE("MSC link: GET MAX LUN answers one byte 0, and only with the right shape") {
  Fixture f;
  std::uint8_t raw[8], d[4] = {0x55};
  usb_fake::make_setup(raw, 0xA1, 0xFE, 0, layout::kIfMsc, 1);
  const auto ok = f.r->host.control_in(raw, d, 4);
  CHECK(ok.ok);
  CHECK(ok.length == 1u);
  CHECK(d[0] == 0u);
  usb_fake::make_setup(raw, 0xA1, 0xFE, 0, layout::kIfMsc, 2);  // wrong wLength
  CHECK(f.r->host.control_in(raw, d, 4).stalled);
  usb_fake::make_setup(raw, 0xA1, 0xFE, 0, layout::kIfCdcComm, 1);  // another interface's request number
  CHECK(f.r->host.control_in(raw, d, 4).stalled);
}

TEST_CASE("MSC link: Bulk-Only Reset is a status-only class request that bumps the bot counter") {
  Fixture f;
  const std::uint32_t before = f.link.events();
  std::uint8_t raw[8];
  usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 0);
  CHECK(f.r->host.control_none(raw).ok);
  CHECK(MscLink::count(f.link.events(), MscLink::kBot) == static_cast<std::uint8_t>(MscLink::count(before, MscLink::kBot) + 1u));
  usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 1);  // with a data stage: invalid
  CHECK(f.r->host.control_none(raw).stalled);
}

TEST_CASE("MSC link: a READ(10) crosses the rings and the CSW follows") {
  Fixture f;
  f.cbw(7, 1024, true, Fixture::rw10(0x28, 3, 2));
  const std::vector<std::uint8_t> data = f.read_data(1024);
  USB_REQUIRE(data.size() == 1024u);
  CHECK(std::memcmp(data.data(), &f.store.img[3 * 512], 1024) == 0);
  CHECK(f.csw(7) == 0);
  CHECK(f.r->host.toggle_errors() == 0u);
}

TEST_CASE("MSC link: a WRITE(10) fills the OUT ring and lands on the store") {
  Fixture f;
  std::vector<std::uint8_t> payload(1024);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(0xA0 + i * 3);
  }
  f.cbw(9, 1024, false, Fixture::rw10(0x2A, 10, 2));
  for (std::size_t off = 0; off < payload.size(); off += 64) {
    USB_REQUIRE(f.send(&payload[off], 64));
  }
  CHECK(f.csw(9) == 0);
  CHECK(std::memcmp(&f.store.img[10 * 512], payload.data(), payload.size()) == 0);
  CHECK(f.r->host.toggle_errors() == 0u);
}

TEST_CASE("MSC link: a full OUT ring NAKs the host, a pump step frees it") {
  Fixture f;
  std::uint8_t pkt[64] = {};
  unsigned acked = 0;
  while (f.r->host.bulk_out(kOutEp, pkt, 64) == usb_rig::Reply::Ack && acked < 32u) {
    ++acked;
  }
  CHECK(acked == MscLink::kSlots);  // exactly the ring: the descriptors were never armed beyond it
  CHECK(f.r->host.bulk_out(kOutEp, pkt, 64) == usb_rig::Reply::Nak);
  f.pump.step();  // the engine takes the (invalid, non-CBW) packets and halts: the ring is free again
  f.r->dev.isr();
  CHECK(f.link.out_tail() == f.link.out_head());
}

TEST_CASE("MSC link: the IN stall reaches the wire after the last data packet, and clear-halt releases the CSW") {
  Fixture f;
  f.cbw(5, 1024, true, Fixture::rw10(0x28, 0, 1));  // the host wants 1024, the device has 512: case 5, data then a stall
  const std::vector<std::uint8_t> data = f.read_data(512);
  USB_REQUIRE(data.size() == 512u);
  CHECK(std::memcmp(data.data(), &f.store.img[0], 512) == 0);
  std::uint8_t d[64];
  std::uint32_t n = 0;
  CHECK(f.recv(d, n) == usb_rig::Reply::Stall);
  CHECK(f.endpoint_halted(kInAddr));  // GET_STATUS agrees (mark_halt)
  f.clear_halt(kInAddr);
  CHECK(!f.endpoint_halted(kInAddr));
  CHECK(f.csw(5) == 0);  // case 5: the CSW passes with the residue (the stall only told the host the data stage is over)
}

TEST_CASE("MSC link: an invalid CBW halts both endpoints until the reset recovery") {
  Fixture f;
  std::uint8_t junk[31] = {1, 2, 3};
  USB_REQUIRE(f.send(junk, 31));
  f.settle();
  std::uint8_t d[64];
  std::uint32_t n = 0;
  CHECK(f.recv(d, n) == usb_rig::Reply::Stall);
  CHECK(f.endpoint_halted(kInAddr));
  CHECK(f.endpoint_halted(0x04));
  std::uint8_t raw[8];
  usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 0);
  USB_REQUIRE(f.r->host.control_none(raw).ok);
  f.settle();
  f.clear_halt(kInAddr);
  f.clear_halt(0x04);
  CHECK(!f.endpoint_halted(kInAddr));
  CHECK(!f.endpoint_halted(0x04));
  f.cbw(2, 0, true, {0x00, 0, 0, 0, 0, 0});  // TEST UNIT READY
  CHECK(f.csw(2) == 0);
}

TEST_CASE("MSC link: a persistent stall is re-asserted after a clear, and packets ACKed before the stall are dropped") {
  Fixture f;
  std::uint8_t junk[64] = {1, 2, 3};
  USB_REQUIRE(f.r->host.bulk_out(kOutEp, junk, 31) == usb_rig::Reply::Ack);  // an invalid CBW
  for (int i = 0; i < 3; ++i) {
    USB_REQUIRE(f.r->host.bulk_out(kOutEp, junk, 64) == usb_rig::Reply::Ack);  // ACKed before the pump looked
  }
  f.settle();
  CHECK(f.endpoint_halted(0x04));
  CHECK(f.pump.dropped_out() == 3u);  // the engine was halted when they were reached
  f.clear_halt(0x04);                 // the host clears early: the engine is dead until a reset, so the stall comes back
  f.settle();
  CHECK(f.endpoint_halted(0x04));
  CHECK(f.r->host.bulk_out(kOutEp, junk, 31) == usb_rig::Reply::Stall);
  f.clear_halt(kInAddr);
  f.settle();
  std::uint8_t d[64];
  std::uint32_t n = 0;
  CHECK(f.recv(d, n) == usb_rig::Reply::Stall);  // and the IN side too
}

TEST_CASE("MSC link: a Bulk-Only Reset mid-write drops the packets still in the ring") {
  Fixture f;
  f.cbw(3, 1024, false, Fixture::rw10(0x2A, 20, 2));
  f.settle();
  std::uint8_t pkt[64];
  std::memset(pkt, 0x5A, sizeof pkt);
  for (int i = 0; i < 3; ++i) {
    USB_REQUIRE(f.r->host.bulk_out(kOutEp, pkt, 64) == usb_rig::Reply::Ack);  // published, not yet pumped
  }
  std::uint8_t raw[8];
  usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 0);
  USB_REQUIRE(f.r->host.control_none(raw).ok);
  f.settle();
  CHECK(f.pump.dropped_out() == 3u);
  CHECK(f.engine.idle());
  f.r->host.reset_toggles();
  f.clear_halt(kInAddr);
  f.clear_halt(0x04);
  f.cbw(4, 0, true, {0x00, 0, 0, 0, 0, 0});
  CHECK(f.csw(4) == 0);
}

TEST_CASE("MSC link: a clear-halt mid-read drops nothing") {
  Fixture f;
  f.cbw(6, 1024, true, Fixture::rw10(0x28, 8, 2));
  std::vector<std::uint8_t> data;
  for (int i = 0; i < 4; ++i) {
    std::uint8_t d[64];
    std::uint32_t n = 0;
    USB_REQUIRE(f.recv(d, n) == usb_rig::Reply::Ack);
    data.insert(data.end(), d, d + n);
  }
  f.clear_halt(kInAddr);  // a host may do this mid-transfer (it also puts its toggle back to DATA0)
  const std::vector<std::uint8_t> rest = f.read_data(static_cast<std::uint32_t>(1024 - data.size()));
  data.insert(data.end(), rest.begin(), rest.end());
  USB_REQUIRE(data.size() == 1024u);
  CHECK(std::memcmp(data.data(), &f.store.img[8 * 512], 1024) == 0);
  CHECK(f.csw(6) == 0);
  CHECK(f.r->host.toggle_errors() == 0u);
}

TEST_CASE("MSC link: a configuration event makes the queued IN packets stale") {
  Fixture f;
  f.cbw(8, 1024, true, Fixture::rw10(0x28, 1, 2));
  f.settle(4);  // the pump has published a full ring of packets, two descriptors are armed
  USB_REQUIRE(f.r->set_configuration(1).ok);
  f.r->dev.isr();
  std::uint8_t d[64];
  std::uint32_t n = 0;
  CHECK(f.r->host.bulk_in(kInEp, d, 64, n) == usb_rig::Reply::Nak);  // nothing stale is armed
  CHECK(f.r->dev.counters().msc_in_stale > 0u);
  f.settle(4);
  CHECK(f.engine.idle());  // the pump saw the event and reset the engine
  f.r->host.reset_toggles();
  f.cbw(10, 0, true, {0x00, 0, 0, 0, 0, 0});
  CHECK(f.csw(10) == 0);
}

TEST_CASE("MSC link: a pseudo-random interleave of host packets and pump steps moves 16 blocks each way intact") {
  Fixture f;
  std::uint32_t seed = 12345;
  auto rnd = [&]() { return seed = seed * 1664525u + 1013904223u; };
  const std::uint32_t blocks = 16;
  std::vector<std::uint8_t> payload(blocks * 512);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i * 7u + 1u);
  }
  // WRITE: the host sends whenever the dice say so; the pump and the interrupt run at other moments.
  f.cbw(11, blocks * 512, false, Fixture::rw10(0x2A, 16, static_cast<std::uint16_t>(blocks)));
  std::size_t sent = 0;
  for (int guard = 0; sent < payload.size() && guard < 100000; ++guard) {
    if ((rnd() >> 16) % 3u != 0u && f.r->host.bulk_out(kOutEp, &payload[sent], 64) == usb_rig::Reply::Ack) {
      sent += 64;
    }
    if ((rnd() >> 16) % 2u != 0u) {
      f.r->dev.isr();
    }
    if ((rnd() >> 16) % 2u != 0u) {
      f.pump.step();
    }
  }
  USB_REQUIRE(sent == payload.size());
  CHECK(f.csw(11) == 0);
  CHECK(std::memcmp(&f.store.img[16 * 512], payload.data(), payload.size()) == 0);
  // READ it back the same way.
  f.cbw(12, blocks * 512, true, Fixture::rw10(0x28, 16, static_cast<std::uint16_t>(blocks)));
  std::vector<std::uint8_t> got;
  for (int guard = 0; got.size() < payload.size() && guard < 100000; ++guard) {
    std::uint8_t d[64];
    std::uint32_t n = 0;
    if ((rnd() >> 16) % 3u != 0u && f.r->host.bulk_in(kInEp, d, 64, n) == usb_rig::Reply::Ack) {
      got.insert(got.end(), d, d + n);
    }
    if ((rnd() >> 16) % 2u != 0u) {
      f.r->dev.isr();
    }
    if ((rnd() >> 16) % 2u != 0u) {
      f.pump.step();
    }
  }
  USB_REQUIRE(got.size() == payload.size());
  CHECK(got == payload);
  CHECK(f.csw(12) == 0);
  CHECK(f.r->host.toggle_errors() == 0u);
}

// ---- review fold of part 2a (findings 3 and 5) ------------------------------------------------------------------------------------------------------------------------------
namespace {
// A WRITE(10) of one block whose CBW announces two blocks of host data (the host has more to send than the device wants: case 11): the engine takes the block, then halts OUT (not dead:
// the CSW follows after the clear) .
void write_one_announce_two(Fixture& f, std::uint32_t tag) {
  f.cbw(tag, 1024, false, Fixture::rw10(0x2A, 4, 1));
  std::uint8_t block[64];
  std::memset(block, 0x5C, sizeof block);
  for (int i = 0; i < 8; ++i) {
    USB_REQUIRE(f.send(block, 64));
  }
  f.settle(6);
}
}  // namespace

TEST_CASE("MSC link: a packet published after the OUT stall's mark is older than the stall and never reaches the engine after the clear-halt (case 11)") {
  Fixture f;
  write_one_announce_two(f, 21);
  USB_REQUIRE(f.engine.out_stalled());
  USB_REQUIRE(f.endpoint_halted(0x04));
  // The controller ACKed one more packet just before EPSTALL, and its token completion is handled after the ISR applied the stall (and took its mark): the packet is published above the mark.
  const std::uint32_t dropped = f.pump.dropped_out();
  std::memset(f.link.out_data(f.link.out_head()), 0xEE, 64);
  f.link.out_publish(64, f.link.live_epoch());
  // The host clears the halt; the ISR takes its mark at the clear, and the pump handles the clear event BEFORE it looks at the packet.
  USB_REQUIRE(f.r->set_feature_halt(false, 0x04).ok);
  f.r->dev.isr();
  f.pump.step();
  CHECK(f.pump.dropped_out() == dropped + 1u);  // dropped, not fed: fails without the mark at the clear
  f.settle();
  CHECK_FALSE(f.engine.ejected());
  // The engine is not dead: the CSW of the command comes back (a phase error or failed, but a CSW) and the next command runs.
  const int status = f.csw(21);
  CHECK(status != 0xFF);
  f.cbw(22, 0, false, {0x00, 0, 0, 0, 0, 0});  // TEST UNIT READY
  CHECK(f.csw(22) == 0);
}

TEST_CASE("MSC link: random stalls, resets and clears keep the rings consistent and the next command works") {
  Fixture f;
  std::uint32_t seed = 987654321u;
  auto rnd = [&]() { return seed = seed * 1664525u + 1013904223u; };
  for (int round = 0; round < 60; ++round) {
    switch ((rnd() >> 16) % 6u) {
      case 0: {  // a read cut by a Bulk-Only Reset + the recovery
        f.cbw(100 + round, 4 * 512, true, Fixture::rw10(0x28, 8, 4));
        std::uint8_t d[64];
        std::uint32_t n = 0;
        (void)f.r->host.bulk_in(kInEp, d, 64, n);
        f.settle(1);
        f.r->dev.isr();
        break;
      }
      case 1:
        write_one_announce_two(f, 200 + round);
        break;
      case 2: {  // an invalid CBW (case: both endpoints halted until the reset)
        std::uint8_t junk[64] = {9, 9, 9};
        (void)f.r->host.bulk_out(kOutEp, junk, 31);
        f.settle(2);
        break;
      }
      default:
        break;
    }
    // The reset recovery, in the order the standard gives: reset, clear IN, clear OUT.
    std::uint8_t raw[8];
    usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 0);
    (void)f.r->host.control_none(raw);
    f.settle(2);
    f.clear_halt(kInAddr);
    f.clear_halt(0x04);
    f.settle(4);
    // Invariants: the rings never hold more than their slots, the indices never pass each other, and a command that follows the recovery completes.
    CHECK(f.link.out_head() - f.link.out_tail() <= MscLink::kSlots);
    CHECK(f.link.in_head() - f.link.in_tail() <= MscLink::kSlots);
    f.cbw(1000 + round, 0, false, {0x00, 0, 0, 0, 0, 0});
    USB_REQUIRE(f.csw(1000 + round) == 0);
  }
  CHECK(f.r->host.toggle_errors() == 0u);
}

TEST_CASE("MSC link: a stall request posted before a CLEAR_FEATURE of OUT and not yet applied is not applied after it (review fold: msc_out_stall_seen_ at the clear)") {
  Fixture f;
  f.settle();
  // The pump posted a request (the engine was halted then) and the interrupt has not looked at it; the host's CLEAR_FEATURE(OUT) is handled first, and the engine is idle again when the pump runs.
  f.link.post_out_stall(f.link.live_epoch());
  USB_REQUIRE(f.r->set_feature_halt(false, 0x04).ok);
  f.settle();
  CHECK_FALSE(f.endpoint_halted(0x04));  // fails without the seen-update at the clear: the stale request halts an idle engine
  f.cbw(30, 0, true, {0x00, 0, 0, 0, 0, 0});  // TEST UNIT READY
  CHECK(f.csw(30) == 0);
}

TEST_CASE("MSC link: a Bulk-Only Reset forgets the posted stalls, so the halts the engine keeps are posted again (review fold)") {
  Fixture f;
  std::uint8_t junk[31] = {9, 9, 9};
  USB_REQUIRE(f.send(junk, 31));  // an invalid CBW: the engine halts, the pump posts the OUT stall
  f.settle();
  CHECK(f.endpoint_halted(0x04));
  USB_REQUIRE(f.engine.out_stalled());
  const std::uint32_t seq = f.link.out_stall_word() & 0xFFu;
  std::uint8_t raw[8];
  usb_fake::make_setup(raw, 0x21, 0xFF, 0, layout::kIfMsc, 0);
  USB_REQUIRE(f.r->host.control_none(raw).ok);  // the reset, and no clear-halt after it: only the reset event can forget the posted flag
  f.settle();
  // The engine keeps its halts until the clears (the standard's reset recovery), and the request posted before the reset is stale (epoch): the pump must post again.
  USB_REQUIRE(f.engine.out_stalled());
  CHECK((f.link.out_stall_word() & 0xFFu) == ((seq + 1u) & 0xFFu));  // with the flag left set it would stay at seq
  CHECK((f.link.out_stall_word() >> 8) == f.link.live_epoch());
}
