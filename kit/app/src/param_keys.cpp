// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/param_keys.h>

namespace o3sdk::app {

namespace {

unsigned lowest_bit(unsigned mask) noexcept {
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    if ((mask >> i & 1u) != 0u) {
      return i;
    }
  }
  return 0;
}

}  // namespace

void ParamKeys::set_pages(const ParamPage* pages, unsigned count) noexcept {
  pages_ = pages;
  count_ = pages != nullptr ? (count < kMaxPages ? count : kMaxPages) : 0u;
  page_ = 0;
  latched_ = 0;
  held_ = 0;
  turned_ = false;
}

void ParamKeys::next_page() noexcept {
  if (count_ != 0u) {
    set_page((page_ + 1u) % count_);
  }
}

void ParamKeys::prev_page() noexcept {
  if (count_ != 0u) {
    set_page((page_ + count_ - 1u) % count_);
  }
}

void ParamKeys::set_page(unsigned index) noexcept {
  if (index < count_) {
    page_ = index;
    // Keep the focus on the same key if the new page has it, else F1.
    if (latched_ >= pages_[page_].count) {
      latched_ = 0;
    }
  }
}

unsigned ParamKeys::focus() const noexcept { return held_ != 0u ? last_held_ : latched_; }

bool ParamKeys::input(Context& ctx, const Input& in) noexcept {
  const ParamPage* p = page();
  if (p == nullptr) {
    return false;
  }
  if (in.button() && is_fkey(in.which())) {
    const unsigned k = fkey_index(in.which());
    if (k >= p->count) {
      return true;  // a blank key on this page: taken, does nothing
    }
    if (in.type == Input::Type::Press) {
      if (held_ == 0u) {
        before_hold_ = latched_;
        turned_ = false;
      }
      held_ |= 1u << k;
      last_held_ = k;
      latched_ = k;  // a tap latches at the press: the strip shows the new focus at once
      ctx.redraw();
    } else if (in.type == Input::Type::Release) {
      held_ &= ~(1u << k);
      if (held_ == 0u && turned_) {
        latched_ = before_hold_;  // it was a hold-and-turn: the latched key comes back
      } else if (held_ != 0u) {
        last_held_ = lowest_bit(held_);
      }
      ctx.redraw();
    }
    return true;
  }
  if (in.turn()) {
    turn(ctx, in.delta(), in.shift);
    return true;
  }
  return false;
}

void ParamKeys::turn(Context& ctx, int detents, bool coarse) noexcept {
  const ParamPage* p = page();
  if (p == nullptr || detents == 0) {
    return;
  }
  if (held_ != 0u) {
    turned_ = true;
    for (unsigned k = 0; k < p->count; ++k) {
      if ((held_ >> k & 1u) != 0u) {
        (void)ctx.engine().step_param(p->unit, p->ids[k], detents, coarse);
      }
    }
  } else if (latched_ < p->count) {
    (void)ctx.engine().step_param(p->unit, p->ids[latched_], detents, coarse);
  }
  ctx.redraw();
}

}  // namespace o3sdk::app
