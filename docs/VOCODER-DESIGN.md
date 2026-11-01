# VOCODER: a vocoder engine with voice loops and auto-tune (design)

Status: design, 2026-11-01; nothing built. Work starts the week of 2026-11-02. The maintainer asked for "un vocoder como si fuera 2040, que
nos de los efectos de daft punk o de chromeo", as a new engine "con entrada de micro, o via line-in, o usb input", with an auto-tune ("el
tune investiga, tener un auto tune me interesa tambien"), and then "samplear la voz, meterla en loop y poder cambiar la portadora". This
document records that conversation: what the engine is, what the kit already gives it, what it costs, what the kit would need, and the
order of work. The research cards behind section 4 are in `docs/ports/`. Every figure marked [est] is an estimate from cycle counts; none
is measured on a unit.

## 1. What it is

An instrument with two roles: the **modulator** (what speaks: a voice, a drum loop, the radio) and the **carrier** (what sounds: a synth,
a chord, noise). The modulator's spectral shape is imposed on the carrier. The target sounds: the talk box (Chromeo), and the mix of
vocoder, talk box and hard pitch correction that Daft Punk used. Three modes:

| Mode | What | How | Cost [est] |
|---|---|---|---|
| TALK | a talk box without the tube | linear prediction (LPC) of the voice, its all-pole filter applied to the carrier (mda Talkbox) | about 11 % of a block |
| BANDS | the classic channel vocoder | an analysis and a synthesis filter bank with envelope followers | about 20 % with 16 bands, 40 % with 32 |
| TUNE | pitch correction, hard ("One More Time") to natural | pitch detection and a pitch shifter, snapped to a scale or to the held note | section 4 |

On top of the modes: **loops**. Any source can be recorded into a tempo-aligned loop, and a loop can then take either role. This is
also the answer to the board's one limit (section 2).

## 2. Sources, and the one rule

What the kit gives today (`kit/app/include/o3sdk/app/audio_in.h`, `kit/dsp/include/o3sdk/dsp/unit.h`):

- `AudioIn::select(source, gain)` points the SGTL5000's ADC at the line jack (`LineMix`, `LineLeft`, `LineRight`), the microphone
  (`MicLow`, `MicHigh`), the FM receiver (`Radio`) or the computer's USB return (`Usb`). The chosen source reaches an **Instrument** unit as
  `in_l` / `in_r` with the notes, so a vocoder unit needs no kit change to hear a live voice.
- A source switch settles for about 200 ms (`kSettleMs`), with the engine's input gate holding the planes silent meanwhile.
- The line sources are mono in both planes (the mix, or one channel). The microphone, the radio and the USB return reach the planes as they
  are, so the USB return is stereo: two different live signals at once (voice left, carrier right, from the computer).
- With `Usb` as the source the return is also mixed after the render at RETURN LEVEL (0 by default, `usb_audio.h`): the app keeps it at 0
  so the dry voice is not heard.
- The simulator feeds the input from a WAV (`platform/host/fake_capture.h`, `sim/README.md`: `input tone|wav|off`, `--audio-in`): the
  engine can be built and heard on the host before any unit session.

**The one rule.** The codec has one ADC; the microphone, the line jack and the radio (which shares the line input behind an analogue switch)
are alternatives. So **every modulator × carrier pair works except two live analogue inputs at once**. Loops, the internal synth, noise and
speech use no ADC and combine with anything.

| Speaks ↓ / sounds → | internal synth | live line or USB | radio | a loop | noise |
|---|---|---|---|---|---|
| live microphone | yes: the classic vocoder, chords from the pads | no, one ADC (USB: a kit change, section 7) | no | yes: sing over a recorded chord | yes: a whispering robot |
| live voice by line or USB | yes | USB stereo: yes (voice L, carrier R); line: a kit change | no | yes | yes |
| radio | yes: the presenter sings your chords | no | n/a | yes | yes |
| a voice loop | yes: the robot sings the sequencer's pattern | yes: the phrase through an external synth | yes | yes: both recorded, the input free | yes |
| any other loop (drums, guitar) | yes: drums "talk" through the chords | yes | yes | yes: the guitar talks through the voice | yes |
| speech (text, no microphone) | yes | yes | yes | yes | yes |

Four ways to play it: live × internal (sing, play chords on the pads); loop × live (record the phrase, then run it through whatever comes
in); live × loop (record the carrier, sing over it); loop × loop (everything recorded, the input free for another live layer such as TUNE).

## 3. Loops

- **Recording.** Two ways, to settle in the spec: (a) the kit's recorder (`Context::recorder()`, `docs/FRAMEWORK.md` "Recording": an
  `Input` tap, or `Master` for a resample, into an app-owned `O3SDK_BIG` buffer, ending at the sample of the next bar with
  `stop_at(Align::Bar)`), the take then handed to the unit; or (b) the unit records `in_l` into its own SDRAM buffer, which makes overdub and
  sample-exact loop points simpler. The recorder hears the 200 ms of a source switch; the unit's `in_l` is gated.
- **Memory.** A mono int16 loop at 44.1 kHz is 88.2 KB a second: 16 s (8 bars at 120 BPM) is 1.4 MB, and 8 MB holds about 90 s of loops.
  o3-303's image already has about 7.5 MB of SDRAM in use, so a loop bank of this size wants its own app (section 9).
- **Playback** costs little: 64 int16 reads a block per loop from the SDRAM.
- **Speed without pitch.** In TALK and BANDS the modulator gives only the spectral shape; the carrier gives the pitch. A voice loop played at
  half speed, double speed or reversed keeps the melody: slow-motion words in tune, for free. (TUNE works on the voice itself, so a speed
  change there needs the shifter's time stretch.)
- **Roles.** Any loop is a modulator or a carrier, swapped with one button.
- **Switching the carrier.** Between loops, the internal synth and noise the switch is instant, so it can be a per-step parameter lock. A live
  source cannot switch per step (the 200 ms settle).

## 4. TUNE

Researched 2026-11-01 by the `dsp-scout` agent; the summary is `docs/ports/autotune-approach.md` (pipeline, arithmetic, a draft parameter
table), one card per candidate beside it. Nothing was cloned, built or run.

- **No candidate ports as it is** for live singing. Autotalent (GPL-2.0-or-later) has 46 ms of latency and an FFT spike; zita-at1 / x42
  fat1 (GPL-2.0-or-later) spikes a block with its FFT detector; cycfi/q (MIT) is C++20 with heap; sevagh's library (MIT) uses STL and FFT
  libraries. What they give is **designs**.
- **The recommended pipeline, clean-room:** a YIN detector (the 2002 paper; the Teensy Audio library's YIN, MIT-style, shows it fits a
  Cortex-M4) on the input decimated 4:1, its work sliced across 4 blocks so no block spikes, refined at 44.1 kHz; a voiced/unvoiced gate
  (aperiodicity, level, low-band ratio, a 60 ms hold) so consonants and breath pass through unshifted; a snap to the held note (pads,
  sequencer, MIDI) or to KEY and SCALE with a bias against vibrato flips; a one-pole retune where **SPEED 0 is the hard, stepped sound**
  and 100-400 ms is natural; a pitch-synchronous shifter (a read pointer that jumps whole periods with a 64-sample crossfade), written
  fresh from zita-at1's design and credited to Fons Adriaensen.
- **Cost [est]:** about 11 % of a block typical, 17 % worst (no spike: the worst block is the average); about 14 KiB of SRAM, no SDRAM.
- **Latency [est]:** 3-9 ms of audio delay from 440 Hz down to 70 Hz on average, 16 ms worst at 70 Hz; the pitch decision is 8-18 ms older
  than the audio, audible only as a short wrong onset with SPEED 0.
- **Harmonizer** (shifts of several semitones with the formants kept, LPC order 12): about +4 % a voice [est]; the weakest part of the design.
- **The patent:** Antares' US 5,973,252 has expired (anticipated 2018-10-14, Google Patents).
- **TALK** (section 1) comes from the same research: mda Talkbox, dual MIT / GPL-2.0-or-later, the DSP ports (LPC order about 4-22 at
  44.1 kHz, a 720-sample Hann window, an analysis every 360 samples), about 11 % of a block [est]. TUNE and TALK are alternatives on one
  voice, so the worst case is the larger, not the sum.
- **Unknown until tried:** robustness on sung voice (YIN's evidence is speech; every threshold is a starting point to tune on recordings),
  and every cycle count, stage C (the YIN difference) first.

## 5. The 2040 layer

- **Scratch the formants:** freeze a loop at a point and move through it with the encoder.
- **Rearrange what you said:** slice a voice loop at its syllables (onset detection), one syllable per pad, sequenced with parameter locks.
- **Intelligibility:** detect unvoiced consonants (s, t, f) and pass them through as filtered noise or as the original: what separates a
  vocoder you understand from one you do not.
- **Freeze a vowel:** hold the spectral envelope and play it from the pads, a choir that never breathes.
- **Voice to chords:** sing one note, the pads choose the chord; a robot choir with unison spread.
- **Morph between two carriers** with the encoder or an LFO.
- **Resample:** record the vocoder's output into a new loop (the recorder's `Master` tap, or the unit's own) and vocode it again.
- **A routing grid on the pads:** rows are voice loops, columns carriers; a pad sends voice i through carrier j, live.
- **Per band:** freeze single bands, shift the bands (the voice's character up or down), or sequence each band.
- **No microphone:** Plaits' speech engine (MIT, part of the M5 port) as the modulator.
- **The radio** as modulator or carrier: a talk station gives clear robot words; a music station gives the whole mix (drums and voice) as
  the modulator, a rhythmic vocoder. Radio and line share the codec's line input, so a live radio voice takes the internal synth, a loop or
  noise as its carrier; to run the radio through an external synth, record it into a loop first.
- **Tune the loop** before using it, with the original consonants kept so the words stay clear.

## 6. Cost

| Part | Arithmetic [est] | Share of the 261,224-cycle block [est] |
|---|---|---|
| BANDS, 16 bands | 16 × (2 analysis + 2 synthesis biquads) = 64 biquads × about 12 cycles = 768, plus 16 followers × about 6 = 96: about 870 cycles a sample | about 21 % |
| BANDS, 32 bands | twice that | about 42 % |
| BANDS by FFT | three 512-point real FFTs a 128-sample hop | 10-15 %, plus about 10 ms of latency |
| Internal carrier | 8 band-limited saw or pulse voices × 20-30 cycles | 4-6 % |
| A loop's playback | 64 SDRAM reads a block | under 1 % |
| TALK | `docs/ports/mda-talkbox.md` | about 11 % |
| TUNE | `docs/ports/autotune-approach.md` section 4 | about 11 % typical, 17 % worst |

Each modulator × carrier pair sounding at once is one vocoder: two or three BANDS pairs fit, or three or four TALK pairs. A 64-frame block is 1.45 ms,
so the time-domain modes stay within a few milliseconds of latency, enough to sing live. The cheaper BANDS (second-order bands, followers at a
lower rate) roughly halves its cost. All of it is measured on the unit before a voice or pair count is promised.

## 7. What the kit would need

- **Nothing for the base:** an Instrument unit gets the input with the notes, the recorder records bar-aligned takes, `AudioIn` selects the
  source, the simulator feeds a WAV.
- **Optional changes in o3-sdk**, each its own slice under this repository's rules:
  - a raw stereo line source (left and right as they are), so a line-in voice and a line-in carrier work live at once;
  - the USB return reaching the units next to the codec's input, so a live microphone and a carrier from the computer work at once;
  - an FFT in `dsp` (today's is in `kit/ui/plot`, which a unit may not link), only if BANDS by FFT is chosen;
  - a sidechain route in the engine, so another unit (o3-303's voice, Plaits) can be the carrier.

## 8. Unit gates before anything is relied on

- **Audio in is built and host-tested only** (`docs/FRAMEWORK.md` section 10, `docs/M2C-PLAN.md` section 13): a real line-in signal was never
  seen on a unit and the microphone was never measured. The FM receiver is on the board and works in track3r-os (the maintainer, 2026-11-01),
  but o3-sdk's port of it has not run on a unit. The M2c unit round for audio in is the first gate of this engine.
- **The USB return is host only** (`docs/FRAMEWORK.md` section 10): a unit check of the return as the input comes before the USB columns of
  section 2 are relied on.
- **The CPU of each mode** on the unit (`o3console.py cpu`), before pair, band or voice counts are fixed.

## 9. Where it lives

An engine is an app, and a new app lives in its own repository (`tools/new_app.py`). Inside o3-303 only one pair would fit next to the 303
(its render peaks near 50 % of a block, and its SDRAM is half used). Two homes fit: its **own app**, or **inside the FREAK app**
(`docs/FREAK-DESIGN.md`; the MicroFreak itself has a vocoder mode), with Plaits as a carrier through the sidechain route. To decide at the
start.

## 10. Order of work

1. **Gates and home:** the M2c audio-in unit round (microphone, line, the radio on o3-sdk's port, the USB return); the decision of section 9.
2. **TALK and BANDS** with the internal carrier and a live source; built on the host against a voice WAV in the simulator, with a scenario.
3. **Loops:** record, play, speed, reverse, roles, the per-step carrier switch.
4. **TUNE** (section 4).
5. **The 2040 layer** (section 5), slice by slice.
6. The kit changes of section 7, in o3-sdk, when a slice needs one.

## 11. Open questions for the maintainer

- The home (section 9) and the engine's name.
- How many loops, how long, and recorded by the kit's recorder or by the unit (section 3).
- The internal carrier: how many voices, which waveforms.
- Which kit changes of section 7 are worth doing.
- From the research (`docs/ports/autotune-approach.md` section 9): clean-room TUNE over a port, and whether cycfi/q's detector is worth a
  host benchmark against YIN; the MIT or the GPL option for mda Talkbox; whether the harmonizer is worth its 4-6 %; what TUNE does with no
  note held in NOTES-IN (silence, pass-through or the scale).
