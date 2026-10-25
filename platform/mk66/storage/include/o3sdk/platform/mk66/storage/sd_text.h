// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The text of `diag sd` and of one line of `diag ls` (OS-007e), pure functions over plain structs like usb/diag_text.h: the command handlers
// in the image only gather the numbers. audio::diag::Text is the bounded builder (no printf).

#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/platform/mk66/storage/card_swap.h>
#include <o3sdk/platform/mk66/storage/sd_card.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::storage {

// What `diag sd` prints, gathered by the image.
struct SdReport {
  bool card_present = false;                                  // the card-detect switch
  bool mounted = false;                                       // storage.mounted()
  hal::storage::Status mount_status = hal::storage::Status::NotMounted;  // what the last mount() answered
  bool mount_tried = false;                                   // a mount() has run
  hal::storage::VolumeInfo volume{};
  const sd::CardInfo* card = nullptr;
  const sd::Counters* counters = nullptr;
  sd::InitStage stage = sd::InitStage::None;                  // where the last init stopped (Done when it did not)
  int fatfs_error = 0;                                        // FRESULT of the last failing FatFs call
  // OS-SDSWAP: who has the card, and how many settled removals and insertions the slot has had since boot.
  CardOwner owner = CardOwner::NoCard;
  std::uint32_t removals = 0;
  std::uint32_t insertions = 0;
  // The last project load, if any (the loader's own summary line, "" when none ran).
  const char* load_summary = "";
  // What its sample load did (OS-007f, app::describe_samples: "samples 18 loaded, 2 missing, 3210 KB"), "" when none ran.
  const char* sample_summary = "";
};

// `diag sd`: the slot, the mount, the card (kind, speed, identity, capacity), the counters and the access statement (read only except the firmware writer).
std::size_t format_sd(const SdReport& r, char* out, std::size_t cap) noexcept;

// One line of a listing, "d             Projects/" for a directory and "      123456  KICK.WAV" for a file (the size right-aligned in ten columns), newline included. The name is
// printed as the card holds it (UTF-8). Returns the length.
std::size_t format_ls_entry(const hal::storage::DirEntry& e, char* out, std::size_t cap) noexcept;

}  // namespace o3sdk::platform::mk66::storage
