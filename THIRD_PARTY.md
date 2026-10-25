# Third-party components

Vendored code keeps its own licence. The Spleen and Terminus glyph data are linked into the target libraries (as `ui`'s face
tables); everything else listed here is host-only.

| Component | Version | Files | Licence | Used by |
|---|---|---|---|---|
| doctest | 2.4.11 | `third_party/doctest/doctest.h`, `third_party/doctest/LICENSE.txt` | MIT, (c) 2016-2023 Viktor Kirilov | unit tests, host builds only |
| stb_image_write | v1.16 | `third_party/stb/stb_image_write.h`, `third_party/stb/LICENSE` | MIT or public domain (Unlicense), your choice; (c) 2017 Sean Barrett | PNG snapshots of the simulator (`platform/host`), host builds only |
| Spleen | 2.2.0 | `third_party/fonts/spleen/spleen-8x16.bdf`, `spleen-6x12.bdf`, `LICENSE`, `AUTHORS` | BSD-2-Clause, (c) 2018-2026 Frederic Cambus | the Label (8x16) and Micro (6x12) faces: `tools/font_convert.py` turns the BDFs into the `ui` glyph tables |
| Terminus | 4.49.1 | `third_party/fonts/terminus/ter-u20b.bdf`, `ter-u32b.bdf`, `OFL.TXT`, `AUTHORS` | SIL Open Font License 1.1, (c) 2020 Dimitar Toshkov Zhekov, Reserved Font Name "Terminus Font" | the Data (10x20) and Display (16x32) faces |

All four were copied byte for byte from track3r-os (`third_party/`), whose `THIRD_PARTY.md` records where each was fetched
from and the SHA-256 of every file. A firmware image that contains the glyph tables must reproduce the Spleen and Terminus
licence texts in its documentation.

Referenced, not vendored: the Bridgetek *FT81X Series Programmers Guide* v1.2 (BRT_000031), the source of the display-list
encodings in `kit/eve/include/o3sdk/eve/dl.h`.

Referenced, not vendored: no file of these is in this repository, and each is cited only in a comment or a test of `platform/mk66`
(the platform is ported from track3r-os, which states its own sources):

- Riedel EmbeddedVideoEngine (FT800-FT813), MIT, (c) 2016-2024 Rudolph Riedel: the FT812 bring-up order (`EVE_init()`) and the
  panel timing preset cited by `platform/mk66/display/` (`chip.h`, `chip.cpp`, `panel.h`, `ft812_regs.h`, and the display tests).
  The pinned release (tag 5.0.9) is in track3r-os; nothing is copied here.
- NXP K66 Sub-Family Reference Manual (Rev. 2) and K66 data sheet (Rev. 4): register addresses, field positions, clock and
  memory limits, cited throughout `platform/mk66`.
- NXP MK66F18 device header and the Teensy core's `kinetis.h`, pin map and divider tables: fact tables for addresses, pin mux and
  field positions (numbers, not code). Their licences were not read for this file: to be recorded before any text is copied.
- SGTL5000 data sheet (Freescale/NXP; the revision PJRC hosts): register map and power-up order (`audio/.../sgtl5000.h`).
- TI TCA8418 data sheet (SCPS206) and Lumissil IS31FL3731 data sheet: the key scanner and the pad LED driver
  (`input/.../tca8418.h`, `input/.../pad_leds.h`).
- PJRC Audio Library for Teensy 3.X, MIT, Copyright (c) 2014 Paul Stoffregen: a cross-check of register numbers and of the
  standard I2S setup (`audio/.../audio_config.h`, `sgtl5000.h`) and the software interrupt line of the render (`irq_map.h`). No
  code is copied; the version is not pinned.
