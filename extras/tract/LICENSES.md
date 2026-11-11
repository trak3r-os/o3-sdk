# Licences and credits of extras/tract

The code and the tables of this directory are GPL-3.0-or-later (see the SPDX line of each file), written for this repository.

No code from any other project is included. The sources of the ideas are these:

- **The lattice.** The Kelly-Lochbaum scattering junction (J. L. Kelly and C. C. Lochbaum, "Speech synthesis", Proc. 4th Int. Congress
  on Acoustics, 1962) and the one-multiplier form in J. O. Smith's *Physical Audio Signal Processing* (online book, waveguide chapters).
  Textbook material, written out here from the equations.
- **The vowel targets.** The male formant averages of G. E. Peterson and H. L. Barney, "Control methods used in a study of the vowels",
  J. Acoust. Soc. Am. 24 (1952). The fitted area model and vowel table (`shape.h`, `tools/fit_vowels.cpp`) are our own fit to those numbers.
- **The half-band FIRs.** Designed by `tools/design_halfband.py` (windowed sinc, 23 taps); the coefficients are numbers from that script.
- **Inspiration and reference only: Chimera** (Jameson Hodge, MIT licence) and **Pink Trombone** (Neil Thapen, MIT licence). The idea of
  driving a vocal-tract tube from an arbitrary audio signal, and of a continuous vowel/tongue/lips/closure control, comes from their
  public demonstrations. Nothing was copied: no source line, constant, table or structure of either was used.
