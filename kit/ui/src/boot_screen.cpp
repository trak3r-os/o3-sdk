// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/ui/boot_screen.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/eve/dl.h>
#include <o3sdk/ui/plot/ram_g_map.h>
#include <o3sdk/ui/theme.h>

#include <cstring>

namespace o3sdk::ui {

namespace {

namespace dl = eve::dl;

// The list the boot frame is built in: alignment declared, as every display-list buffer (eve/display_list_builder.h).
struct alignas(8) BootList {
  std::uint32_t words[kBootScreenMaxWords];
};
static_assert(alignof(BootList) >= 8 && sizeof(BootList) % 8 == 0, "an aligned display-list buffer without padding");

// "  o3sdk " and the build id, cut at kBootBuildChars, into `out`.
constexpr char kLead[] = "  o3sdk ";
constexpr std::size_t kRestChars = sizeof(kLead) - 1 + kBootBuildChars;

void compose_rest(const char* build_id, char (&out)[kRestChars + 1]) noexcept {
  std::size_t n = 0;
  for (const char* p = kLead; *p != '\0'; ++p) {
    out[n++] = *p;
  }
  for (const char* p = build_id != nullptr ? build_id : ""; *p != '\0' && n < kRestChars; ++p) {
    out[n++] = *p;
  }
  out[n] = '\0';
}

}  // namespace

void draw_boot_screen(Renderer& r, const char* name, const char* build_id) noexcept {
  // The logo: one raw section, so that the bitmap handle, the transform and the colour leave nothing behind for what follows
  // (RESTORE_CONTEXT brings the transform and the colour back; BITMAP_TRANSFORM is part of the graphics context, guide 4.1).
  r.begin_raw();
  r.raw(dl::bitmap_handle(plot::kHandleBootLogo));
  r.raw(dl::bitmap_source(plot::kRamGBootLogo));
  r.raw(dl::bitmap_layout(kBootLogoFormat, kBootLogoStride, kBootLogoH));
  r.raw(dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, static_cast<std::uint32_t>(kBootLogoDrawW),
                        static_cast<std::uint32_t>(kBootLogoDrawH)));
  r.raw(dl::bitmap_transform_a(kBootLogoTransform));
  r.raw(dl::bitmap_transform_e(kBootLogoTransform));
  r.raw(r.color_word(Color::Text));
  r.raw(dl::cell(0));
  r.raw(dl::begin(eve::Primitive::Bitmaps));
  r.raw(dl::vertex2f(kBootLogoX * 16, kBootLogoY * 16));
  r.raw(dl::end());
  r.end_raw();

  // The line, centred as a whole: the name bright, the rest dim.
  name = name != nullptr ? name : "";
  char rest[kRestChars + 1];
  compose_rest(build_id, rest);
  const int name_w = r.text_width(name, Face::Micro);
  const int x = (kScreenW - name_w - r.text_width(rest, Face::Micro)) / 2;
  r.text(x, kBootTextY, name, Color::Text, Face::Micro);
  r.text(x + name_w, kBootTextY, rest, Color::TextDim, Face::Micro);
}

bool show_boot_screen(hal::Display& display, const char* name, const char* build_id) noexcept {
  BootList list;
  eve::DisplayListBuilder builder(display, list.words, kBootScreenMaxWords);
  Renderer r(builder, boot_theme());
  if (!r.begin(Color::Pane)) {
    return false;
  }
  draw_boot_screen(r, name, build_id);
  return r.end();
}

}  // namespace o3sdk::ui
