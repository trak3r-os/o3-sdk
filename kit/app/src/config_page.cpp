// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/config_page.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/settings.h>

namespace o3sdk::app {

static_assert(kMaxConfigPages * kMaxConfigItems <= kAppValueSlots, "every value of every page has a slot in the settings file");
static_assert(kMaxConfigItems <= 8, "ConfigView::Page::mask is a byte");

const ConfigItem* ConfigView::Page::nth(unsigned n) const noexcept {
  for (unsigned i = 0; i < count; ++i) {
    if (((mask >> i) & 1u) != 0u) {
      if (n == 0u) {
        return &items[i];
      }
      --n;
    }
  }
  return nullptr;
}

unsigned ConfigView::Page::valid() const noexcept {
  unsigned n = 0;
  for (unsigned i = 0; i < count; ++i) {
    n += (mask >> i) & 1u;
  }
  return n;
}

void ConfigView::build(const App& app) noexcept {
  *this = ConfigView{};
  const unsigned wanted = app.config_pages();
  for (unsigned i = 0; i < wanted && i < kMaxConfigPages && pages < kMaxConfigPages; ++i) {
    ConfigPage p{nullptr, nullptr, 0};
    if (!app.config_page(i, p) || p.items == nullptr || p.count == 0u) {
      continue;
    }
    Page& out = page[pages];
    out.title = p.title != nullptr ? p.title : "APP";
    out.items = p.items;
    out.count = static_cast<std::uint8_t>(p.count < kMaxConfigItems ? p.count : kMaxConfigItems);
    out.mask = 0;
    for (unsigned k = 0; k < out.count; ++k) {
      const ConfigItem& it = p.items[k];
      const bool value_ok = it.value == nullptr || core::param_desc_valid(*it.value);
      if (it.key == 0u || it.label == nullptr || it.label[0] == '\0' || !value_ok) {
        continue;
      }
      bool seen = false;  // a key that an earlier item has (on this page or an earlier one) is left out
      for (unsigned q = 0; q <= pages && !seen; ++q) {
        const Page& pg = page[q];
        for (unsigned j = 0; j < (q == pages ? k : pg.count) && !seen; ++j) {
          seen = ((pg.mask >> j) & 1u) != 0u && pg.items[j].key == it.key;
        }
      }
      if (seen) {
        continue;
      }
      out.mask = static_cast<std::uint8_t>(out.mask | (1u << k));
      values = static_cast<std::uint8_t>(values + (it.value != nullptr ? 1u : 0u));
    }
    if (out.mask != 0u) {
      ++pages;
    }
  }
}

const ConfigItem* ConfigView::find_value(std::uint16_t key) const noexcept {
  for (unsigned p = 0; p < pages; ++p) {
    for (unsigned i = 0; i < page[p].count; ++i) {
      if (((page[p].mask >> i) & 1u) != 0u && page[p].items[i].key == key && page[p].items[i].value != nullptr) {
        return &page[p].items[i];
      }
    }
  }
  return nullptr;
}

}  // namespace o3sdk::app
