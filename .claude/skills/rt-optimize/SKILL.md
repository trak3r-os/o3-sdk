---
name: rt-optimize
description: Performance doctrine for o3sdk on the Tracker hardware (Cortex-M4F DSP code, unit process() structure, memory placement, DMA, FT812 offload) - how to measure, what to try in which order, and the rules each technique follows. Use when writing or reviewing a unit, the engine, a sequencer hook, UI rendering or a driver, or when the CPU budget is tight.
---

# Real-time optimisation (MK66 Cortex-M4F + SGTL5000 + FT812)

The device is not one 180 MHz CPU: it is the MK66 plus DMA engines, the codec and the FT812 graphics coprocessor, around 16 MiB of SDRAM. Spend MK66 cycles only on what nothing else can do. One 64-frame block is 261,224 cycles at 44.1 kHz and is shared by everything that renders (the engine, every unit, the clocked hooks); keep audio at 60 % of it or less.

## 0. Order of work (never skip a step)
1. **Measure first.** The engine's cycle counter is `hal::cycles` (the DWT counter, `engine.set_cycle_counter`); `stats()` carries the render's cycles and the app header shows the CPU %. Worst case, not the average. The host proves correctness and relative op counts only: a cycle claim needs the unit (skill `hw-session`). Until a bench exists, compare the header's CPU % with the feature off and on, for a fixed scenario (a held note, a pattern), and write down the build id, the scenario and min/mean/max over a stated window.
2. **Algorithm before micro-optimisation.** A better filter topology, interpolation, voice-stealing policy or control rate beats any intrinsic.
3. **Then data and memory, then CPU tricks, then offload.** Keep each step's before/after numbers in the report. Label unmeasured numbers *estimate*.

## 1. Structure of `Unit::process()`
- **Two-level loop.** An outer loop runs once every N samples (4 to 16) and does the control work: coefficients, envelope segment changes, idle tests, countdowns. The inner loop runs those N samples with no branches beyond the loop. `for_each_run()` already splits the block at events.
- **Register pressure first.** The M4F has 32 single-precision registers. Copy the state used per sample into locals before the loop and write it back after; never read the state struct inside the sample loop. If more than about 24 floats are live, split into passes through a scratch buffer.
- **No division in the sample loop** (VDIV/VSQRT take about 14 cycles): precompute reciprocals at control rate. `expf`, `logf`, `sinf`, `tanhf` are calls into cold code: control rate only, or your own short inline polynomial per sample.
- **Envelopes by segment**: compute the per-sample increment at control rate; the inner loop adds.
- **Silence costs nothing**: detect a settled voice at control rate and skip its work.
- **Oversampling is expensive**: prove it is needed; decimate with a polyphase half-band, not cascaded biquads.
- **Accept any frame count from 1 to the block**; scratch buffers sized for the block; coefficients smoothed per block.
- Review aid, on the unit's object file: `arm-none-eabi-objdump -dC --disassemble=<symbol> <obj> | grep -cE 'vldr|vstr|vdiv|blx'` (guidance from track3r-os's TB-303 study: FP loads/stores under about 15 % of instructions, no `vdiv` or `blx` inside the sample loop).

## 2. CPU (Cortex-M4F)
- **Single-precision FPU only** (`fpv4-sp-d16`): `float`, `f` literals; `double` is software-emulated and banned (`-fsingle-precision-constant` in the kit and `-Wdouble-promotion` catch the common slips; check the cross build for `__aeabi_d*`). Prefer multiplying by a reciprocal.
- **SIMD intrinsics** (`__SMLAD`, `__SSAT`, ...) pay for 16-bit mixing and gain; float for resonant filters, feedback and envelopes. Convert once per block at the boundaries.
- **CMSIS-DSP** (Apache-2.0) before hand-writing biquads, FIR or FFT, if the maintainer agrees to vendoring it (`THIRD_PARTY.md`).
- **LUTs** for sine, pitch, dB, curves: small hot ones in SRAM, large ones in the SDRAM, generated at boot when cheap.
- Hot loops: `__restrict`, counted loops, no virtual calls or function pointers inside the sample loop, branch-free selection.

## 3. Memory
- Static allocation only; voices, events and blocks reserved at init.
- **Placement** (`docs/M2-PLAN.md`, "Memory"): code in flash runs behind the LMEM code cache; SRAM_L (`0x1FFF0000`, 64 KiB) holds `.ramfunc_l`, today only the audio ISRs and the pipeline; SRAM_U (192 KiB) the stacks, DMA rings, `.data` and `.bss`; the SDRAM (`0x70000000`) every `O3SDK_BIG` object. Moving the engine's hot loop to SRAM_L is a later, measured change, and `.ramfunc_l` code must call nothing in flash (`platform/mk66/tools/check_ramfunc.py`).
- **Alignment is declared, never placed**: every buffer touched by word or multi-word loads gets `alignas` plus a `static_assert` (track3r-os hit an UNALIGNED fault from a 2-byte-aligned pool under `-fdata-sections`).
- Zero-copy: pass block pointers; single-producer single-consumer rings between the render context and the main loop with a documented writer (`core` rings, the engine's command ring).
- Structure of arrays for per-voice state iterated in the sample loop.

## 4. Offload
- I2S audio, the FT812 SPI and (later) the SD card go by DMA; the CPU starts transfers and handles completion, never copying byte by byte. SD I/O never happens in the render context.
- **FT812**: the MK66 builds the display list (at most 2048 words a frame), the coprocessor rasterises. Static panels on change, meters at 20-30 Hz, all below audio priority. Large assets are linked data in flash today.
- **SGTL5000 DAP** (tone control, PEQ/GEQ, AVC) sits only on the analogue output path: using it is a decision (WAV export and recording would not hear it), not a default.

## Reviewers reject
Optimisations without before and after numbers; `double`, allocation or SD I/O in a real-time path; a division or a libm call per sample; unsafe ISR sharing introduced for speed; code moved to SRAM_L without a measurement; a cycle claim without a unit measurement.
