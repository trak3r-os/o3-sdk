// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Who is using the card right now (OS-USBDISK F3): one list, one answer. Every user that holds the card across several passes of the main loop (a save job, a project load, a
// firmware upload, a card mount waiting its turn, a listing or an audition being read) registers a claim once at start-up; busy() is true when any of them says so. It is what the
// hand-over to a host asks before it lets the card go, and what `sd mount` asks before it takes the volume. A fixed table, no allocation; registered once at start-up and read from
// the main loop only.

#include <cstddef>

namespace o3sdk::platform::mk66::storage {

class CardClaims {
 public:
  using Claim = bool (*)() noexcept;  // true: this user needs the card to stay where it is
  static constexpr std::size_t kMax = 8;

  // Idempotent: a claim already in the table is not added twice (start_app() runs again on a recovery) and answers true. false when the table is full or `claim` is null (a
  // start-up bug the caller logs).
  bool add(Claim claim) noexcept {
    if (claim == nullptr) {
      return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      if (claims_[i] == claim) {
        return true;
      }
    }
    if (count_ >= kMax) {
      return false;
    }
    claims_[count_++] = claim;
    return true;
  }
  bool busy() const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (claims_[i]()) {
        return true;
      }
    }
    return false;
  }
  std::size_t count() const noexcept { return count_; }

 private:
  Claim claims_[kMax] = {};
  std::size_t count_ = 0;
};

}  // namespace o3sdk::platform::mk66::storage
