# Clean-room from the papers: YIN, McLeod Pitch Method, TD-PSOLA

Researched 2026-11-01. Papers are not code: no licence applies to the algorithms; any code written from them is the maintainer's, under the kit's GPL-3.0-or-later. Patents: only the Antares one was checked (`antares-patent.md`); I did not search for patents on YIN, MPM or PSOLA (the PSOLA literature dates from 1989-90, so any such patent would be long expired, which is reasoning, not a verified fact).

## 1. YIN (de Cheveigne and Kawahara, JASA 111(4), 1917-1930, April 2002)

- Citation: doi 10.1121/1.1458024; full text copy https://cagnazzo.wp.imt.fr/files/2013/05/2002_JASA_YIN.pdf (the publisher version is the reliable one).
- Facts from the abstract: an autocorrelation-based estimator with modifications that prevent errors; error rates "about three times lower than the best competing methods" on speech recorded with laryngograph signals; no upper limit on the frequency range; relatively simple, few parameters.
- Steps (the paper's six; the standard summary): (1) autocorrelation, (2) difference function d(tau) = sum over a window of (x[j] - x[j+tau])^2, (3) cumulative-mean-normalised difference d'(tau) = d(tau) / (running mean of d(1..tau)), 1 at tau 0, (4) absolute threshold (0.10-0.15 typical) taking the first dip below it, (5) parabolic interpolation, (6) best local estimate. The value d' at the chosen lag is an **aperiodicity measure**: 1 - d' is the voicing confidence.
- Why it fits the use case: the aperiodicity gives the voiced/unvoiced decision from the same pass; the first-dip rule prevents most octave errors (the failure that makes a shifter jump); no FFT; a cost that is a plain nested loop (about window x lags multiply-accumulates), so it can be sliced across blocks (the Teensy implementation does exactly this, see `teensy-audio-yin.md`).
- Weakness: step (3) needs the sum from lag 1, so the small lags cannot be skipped; low-pitch voices need a longer window.

## 2. McLeod Pitch Method (McLeod and Wyvill; PhD "Fast, Accurate Pitch Detection", Otago)

- Sources: thesis https://www.cs.otago.ac.nz/graphics/Geoff/tartini/papers/Philip_McLeod_PhD.pdf; the TarsosDSP documentation calls it a "fast, accurate and robust method for finding the continuous pitch in monophonic musical sounds", a normalised squared difference function with peak picking, real-time at 44.1 kHz, no low-pass filter needed (https://0110.be/releases/TarsosDSP/TarsosDSP-latest/TarsosDSP-latest-Documentation/be/tarsos/dsp/pitch/McLeodPitchMethod.html). I did not confirm the "ICMC 2005 'A smarter way to find pitch'" citation in a source; check it before citing.
- Shape: the SNAC/NSDF n(tau) = 2 r(tau) / m(tau) lies in [-1, 1]; peaks between positive-going zero crossings, the first peak above k x (highest peak), k about 0.8-1.0; parabolic interpolation; the peak height is the "clarity", the voicing measure. r(tau) is a direct sum (same cost as YIN's d) and m(tau) updates in O(1) per lag.
- Compared with YIN: natural clarity scale and good behaviour on harmonic-rich voices; but the peak-picking needs the small lags too (the first negative lobe), and a decision threshold k has to be tuned. TalentedHack and sevagh use it; the quality is comparable. Not a deciding factor.

## 3. TD-PSOLA (Moulines and Charpentier, Speech Communication 9(5/6), 1990, pp. 453-467)

- Sources: the 1989 Eurospeech precursor https://www.isca-archive.org/eurospeech_1989/charpentier89_eurospeech.html (introduces FD-PSOLA and TD-PSOLA); a course summary https://courses.grainger.illinois.edu/ece420/5_PSOLA.pdf.
- Shape: windowed segments (about two periods long) centred on pitch marks (glottal closure instants), re-spaced in time (or repeated or dropped) and overlap-added. Pitch changes by spacing; the segments keep their own spectral envelope, so **formants stay put** to a first approximation.
- Needs reliable pitch marks, which is the hard part with a sung voice; the detector gives a period but not marks. Marks can be found by locating, within each predicted period, the peak or the zero crossing nearest the previous mark.
- Relation to zita-at1: zita's whole-period jump is the mark-free, degenerate case (the read pointer resamples at the ratio, so the spectrum moves a little by that ratio, and no segment is windowed apart from the crossfade). True TD-PSOLA is the route for the **harmonizer stretch goal** (shifts of several semitones with formants kept), at the price of mark tracking.
- Cost (estimate): per output sample the Hann-windowed overlap-add of two grains is about 2 loads and 2 multiply-accumulates: about 15-20 cycles x 64 = 1,000-1,300 cycles per block (0.4-0.5 %) per voice, plus mark tracking about 500 cycles per block. Below the detector's cost by an order of magnitude.

## 4. Port or clean-room?

**Clean-room is the better route for all three.** Reasons, each checkable:
1. Every available code base is unsuited as it stands (`cycfi-q.md`: C++20, heap and exceptions; `sevagh-pitch-detection.md`: STL, FFT dependencies, whole-buffer API; `autotalent.md`: 46 ms latency and a block-sized FFT; `zita-at1-fat1.md`: FFT detector, 2x resampler; `teensy-audio-yin.md`: AudioStream bound, window sized for guitar).
2. The cost control that matters here (decimation, work sliced across blocks, a detector and shifter designed together) is a structural property, not something to patch into a port.
3. The three algorithms are short (YIN about 100 lines, a period-jump shifter about 200, scale snap and smoothing about 80), and the paper plus the existing GPL and MIT designs described in the cards are a sufficient spec.
4. A host oracle makes a clean-room safe: compare against sevagh's YIN (MIT) or aubio's `pitchyin.c` (GPL-3.0-or-later, Copyright Paul Brossier 2003-2009, https://raw.githubusercontent.com/aubio/aubio/master/src/pitch/pitchyin.c) on synthetic voices (sawtooth through formant filters, vibrato, noise bursts, an octave jump) in `tests/`.

## Sources

- https://cagnazzo.wp.imt.fr/files/2013/05/2002_JASA_YIN.pdf
- https://www.cs.otago.ac.nz/graphics/Geoff/tartini/papers/Philip_McLeod_PhD.pdf
- https://0110.be/releases/TarsosDSP/TarsosDSP-latest/TarsosDSP-latest-Documentation/be/tarsos/dsp/pitch/McLeodPitchMethod.html
- https://www.isca-archive.org/eurospeech_1989/charpentier89_eurospeech.html
- https://courses.grainger.illinois.edu/ece420/5_PSOLA.pdf
- https://raw.githubusercontent.com/aubio/aubio/master/src/pitch/pitchyin.c
