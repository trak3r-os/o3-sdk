// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/sd_text.h>

#include <cstring>

namespace o3sdk::platform::mk66::storage {
namespace {

using audio::diag::Text;

// Two digits, for the CID's month and the revision.
void two(Text& t, unsigned v) {
  t.chr(static_cast<char>('0' + (v / 10u) % 10u)).chr(static_cast<char>('0' + v % 10u));
}

// 16 bytes as 32 lowercase hex digits, the form Linux prints a card's cid and csd in.
void hex_bytes(Text& t, const std::uint8_t* bytes, std::size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  for (std::size_t i = 0; i < n; ++i) {
    t.chr(kDigits[bytes[i] >> 4]).chr(kDigits[bytes[i] & 0xFu]);
  }
}

const char* kind_name(const sd::CardInfo& c) noexcept {
  if (!c.block_addressed) {
    return "SDSC";
  }
  return c.blocks > (1u << 26) ? "SDXC" : "SDHC";  // SDHC up to 32 GB (2^26 blocks of 512 bytes), SDXC above
}

}  // namespace

std::size_t format_sd(const SdReport& r, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("slot:    ").str(r.card_present ? "a card is in" : "no card (detect switch open)").nl();
  t.str("mount:   ");
  if (r.mounted) {
    t.str("mounted, ").str(hal::storage::fs_kind_name(r.volume.kind)).str(", ").dec(r.volume.capacity_mb).str(" MB, cluster ")
        .dec(r.volume.cluster_bytes).str(" B");
  } else if (r.mount_tried) {
    t.str("not mounted: ").str(hal::storage::status_name(r.mount_status));
    if (r.fatfs_error != 0) {
      t.str(" (FatFs ").dec(static_cast<std::uint32_t>(r.fatfs_error)).chr(')');
    }
  } else {
    t.str("not tried");
  }
  t.nl();
  t.str("owner:   ").str(card_owner_name(r.owner)).str(", ").dec(r.removals).str(" removed, ").dec(r.insertions).str(" inserted since boot").nl();
  if (r.card != nullptr && r.card->ready) {
    const sd::CardInfo& c = *r.card;
    t.str("card:    ").str(kind_name(c)).str(c.version2 ? ", spec 2.00+" : ", spec 1.x").str(", CSD v").dec(c.csd_version).str(", ")
        .dec(c.capacity_mb).str(" MB (").dec(c.blocks).str(" blocks)").nl();
    t.str("bus:     ").dec(c.bus_width).str(" data lines, ").dec(c.clock_hz / 1000u).str(" kHz").str(c.high_speed ? " (high speed)" : " (default speed)").nl();
    t.str("id:      manufacturer ").hex32(c.cid.manufacturer).str(" oem '").str(c.cid.oem).str("' product '").str(c.cid.product).str("' rev ")
        .dec(c.cid.revision_major).chr('.').dec(c.cid.revision_minor).str(" serial ").hex32(c.cid.serial).str(" made ").dec(c.cid.year).chr('-');
    two(t, c.cid.month);
    t.nl();
    t.str("rca:     ").hex32(c.rca).str(", ocr ").hex32(c.ocr).nl();
    t.str("cid:     ");
    hex_bytes(t, c.cid_raw, sizeof c.cid_raw);
    t.nl().str("csd:     ");
    hex_bytes(t, c.csd_raw, sizeof c.csd_raw);
    t.nl();
  } else {
    t.str("card:    not initialised");
    if (r.counters != nullptr && r.counters->inits != 0u) {
      t.str(": init stopped at ").str(sd::stage_name(r.counters->last_error_stage)).str(", ").str(sd::result_name(r.counters->last_error));
    }
    t.nl();
  }
  if (r.counters != nullptr) {
    const sd::Counters& k = *r.counters;
    t.str("counts:  ").dec(k.inits).str(" inits, ").dec(k.commands).str(" commands, ").dec(k.reads).str(" reads, ").dec(k.blocks).str(" blocks, ")
        .dec(k.retries).str(" retries, ").dec(k.failures).str(" failed reads, ").dec(k.blocks_written).str(" blocks written, ")
        .dec(k.write_failures).str(" failed writes").nl();
    t.str("errors:  no-response ").dec(k.no_response).str(", command ").dec(k.command_errors).str(", card ").dec(k.card_errors)
        .str(", data-timeout ").dec(k.data_timeouts).str(", data ").dec(k.data_errors).str(", last: ").str(sd::result_name(k.last_error)).nl();
  }
  if (r.load_summary != nullptr && r.load_summary[0] != '\0') {
    t.str("load:    ").str(r.load_summary).nl();
  }
  if (r.sample_summary != nullptr && r.sample_summary[0] != '\0') {
    t.str("pcm:     ").str(r.sample_summary).nl();
  }
  t.str("access:  read only except the firmware writer (`fw`, Firmware/ of the card): CMD24 is the one write command (sd_protocol.h).").nl();
  return t.size();
}

std::size_t format_ls_entry(const hal::storage::DirEntry& e, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  if (e.is_dir) {
    t.str("d             ").str(e.name).chr('/');  // "d" and 13 spaces: the name starts where a file's does
  } else {
    // size right-aligned in 12 columns: "  " + 10 digits
    char digits[11];
    std::size_t n = 0;
    std::uint32_t v = e.size;
    do {
      digits[n++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u && n < sizeof digits);
    t.str("  ");
    for (std::size_t pad = n; pad < 10u; ++pad) {
      t.chr(' ');
    }
    while (n != 0u) {
      t.chr(digits[--n]);
    }
    t.str("  ").str(e.name);
  }
  t.nl();
  return t.size();
}

}  // namespace o3sdk::platform::mk66::storage
