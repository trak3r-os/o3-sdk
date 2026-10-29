# CRASH TEST: the tester's checklist (session only, never a release)

What it proves, on the unit, for the first time: the **crash record survives the reset it causes** (`crash`, `stage`, `log` after the boot), and the **status LED
shows the crash code** (6 flashes, twice, then the heartbeat; `docs/FRAMEWORK.md` section 10, the row "Crash record, status LED" is "Host only" until this runs).
The app has one console command, `fault`; nothing on the unit's own controls provokes anything. Spec: `docs/M2C-PLAN.md` sections 9 and 10, decision D15.

The image is a valid one (`fw-gate-check` may pass it) and says what it is: the boot screen and the header strip read `CRASH TEST SESSION ONLY`, the version is
`0.0.0 SESSION ONLY`, `state` starts `app=CRASH_TEST_SESSION_ONLY`. It is installed for a session and taken off at the end. Never hand it on as a release.

`O3=../o3-sdk/platform/mk66/tools` (from a sibling checkout; from o3-sdk itself `platform/mk66/tools`). The image is `build-<dir>/apps/crashtest/crashtest.ptf` and its
`crashtest.elf` next to it (keep the `.elf` of the installed build: it turns the record's addresses into names).

## 0. Before

- The unit is shared: note what runs now (`version`, `fw`: the images on the card, `first-ptf`) and restore exactly that at the end (section 5). Only the maintainer's go
  installs anything (`CLAUDE.md`, "Hard rules"); this app needs no card, no pads, no audio.
- A known-good image is on the card as the reset button's target (`NEW-APP.md` section 11). Do not leave `crashtest` as `first-ptf`: if it is, `fw rm` it first
  or have a person fix the card.
- The image passed `fw-gate-check` (`crashtest_image`, `_flashconfig`, `_ramfunc`, `_usb` pass; no `__aeabi_d*`).
- `fire` does not check for a card job: arm a fault only when `state` shows `fw=closed` and `files=idle` and no console `put` or update is running, never during a `put`, an update or a save,
  so a fault never lands in the middle of a card write.

## 1. Install and baseline

```sh
python3 -I tools/o3install.py build-mk66/apps/crashtest/crashtest.ptf --yes --name crashtest-session.ptf
python3 -I $O3/o3console.py version state crash stage "log 20" help
```

| Command | Must show |
|---|---|
| `version` | the build label of this image (o3install checked it) |
| `state` | `app=CRASH_TEST_SESSION_ONLY screen=FAULTS ... armed=none prev=none nofault=none seq=0` (`seq` may be above 0 if the RAM kept a marker of an earlier session: not a fault) |
| `crash` | `no crash record` |
| `stage` | `reset: ...`, `crash record at boot: no` |
| `help` | a row `fault` among the app's commands (and no "skipped" line in `log`) |
| the LED | the heartbeat (100 ms every second), no code |
| `fault` | the list of the twelve kinds, "session only" in its first line |

Take one `--shot` (`$O3/o3console.py --shot /tmp/crashtest.png`): the screen says `CRASH TEST: SESSION ONLY` and lists the twelve kinds.

## 2. Each fault

One at a time, in this order (the first four are the most certain; `buswr` last of the faults, it may legitimately do nothing). For each kind `K`:

```sh
python3 -I $O3/o3console.py "fault K"             # says what it does and what `crash` must show; does nothing
python3 -I $O3/o3console.py "fault K confirm"     # arms it: the reply goes out, 1.5 s later the unit faults and resets
# the console drops. A hands person watches the LED from the reset on (section 3). The console is back in 25-40 s (the crash code plays first, about 8 s).
python3 -I $O3/o3console.py --list                # the unit is back
python3 -I $O3/o3console.py crash stage "log 20" state
arm-none-eabi-addr2line -f -C -e crashtest.elf <pc> <lr>     # the names of the record's pc and lr (the toolchain's, the .elf of the installed build)
```

Typing `fault K` alone, `fault K yes`, `fault K CONFIRM` or a wrong name arms nothing. `fault cancel` disarms during the 1.5 s. A second `fault` while one is armed
is refused.

Every kind, after the reset, must show **all** of these:

- `crash` answers `crash #1: <name> at <s> s of that run` (count 1: the record is erased at every boot, so `#2` means a record that was not erased: a finding), with `stage Running, module 0`.
- `stage`: `reset: 0x.... (software)` (SYSRESETREQ; `watchdog`, `lockup`, `power-on` or `pin` here is a finding), and `crash record at boot: yes`.
- `log 20`: `boot: previous run crashed, exception 0x000000NN` (the exception number below, in hex).
- `state`: `prev=K armed=none nofault=none`, `seq` one higher than before.
- The LED: after the reset the steady-on of the SDRAM test, then **6 flashes, a pause, 6 flashes, a pause** (200 ms on, 200 ms off, the pause brings the period to
  4.0 s: about 8 s in all), then the heartbeat. Any other code is a finding (`platform/mk66/README.md`).

What differs per kind (the numbers are the architecture's, written before any unit ran this: where the unit disagrees, the unit is right and this table is corrected):

| K | exception | `crash` fields | pc is in (`addr2line`) |
|---|---|---|---|
| `bus` | 5 BusFault | cfsr `0x00008200` (PRECISERR, BFARVALID), bfar `0x40008008`, hfsr 0 | `bus` |
| `hard` | 3 HardFault | hfsr `0x80000000` (DEBUGEVT), cfsr 0 | `hard` (the BKPT) |
| `undef` | 6 UsageFault | cfsr `0x00010000` (UNDEFINSTR) | `undef` |
| `div0` | 6 UsageFault | cfsr `0x02000000` (DIVBYZERO: proves CCR.DIV_0_TRP) | `divide_by_zero` |
| `mem` | 4 MemManage | cfsr `0x00000001` (IACCVIOL), pc `0x40000000`, mmfar meaningless | pc is not in the image; `lr` is in the app (`CrashApp::update`, `fire` is inlined into it) |
| `badfn` | 6 UsageFault | cfsr `0x00020000` (INVSTATE), pc even | `nop_target` |
| `unalign` | 6 UsageFault | cfsr `0x01000000` (UNALIGNED: the multi-word access traps although CCR.UNALIGN_TRP is off) | `unaligned` |
| `nmi` | 2 NMI | cfsr 0, hfsr 0 | `nmi` or `settle` |
| `irq` | `0x55` = 85: `IRQ 69 (no handler)` | cfsr 0 | `unhandled_irq` or `settle` |
| `panic` | `0xFF`: `panic` | `exc_return 0x00000000`, `frame unreadable`, xpsr 0, pc = lr | `__cxa_pure_virtual` |
| `render` | 5 BusFault | as `bus`; **exc_return `0xFFFFFFF1` or `0xFFFFFFE1`** (handler mode, not `F9`/`E9`), xpsr low nine bits `0x050` (the render interrupt, IRQ 64) | `bus`, `lr` in `RenderFault::block` |
| `buswr` | 5 BusFault | cfsr `0x00000400` (IMPRECISERR), bfar not valid, pc near the store | `bus_write` or `settle` |

For every kind but `render` and `panic` the record must show thread mode: exc_return `0xFFFFFFF9` (or `0xFFFFFFE9` when the main loop held live FPU state), xpsr low nine bits 0, `frame`
an SRAM_U address under the 24 KiB main stack (`0x2002A000..0x20030000`). `render` against `bus` is the one comparison that proves the record can be taken from an interrupt.

`buswr` may end with `state nofault=buswr` and no reset: the write to the reserved eDMA word was ignored, not refused. That is a fact about the chip, not a failure of the record: note
it and carry on. The same `nofault=K` for any other kind is a finding (the instruction did not fault: say which).

## 3. By eye (a person at the unit)

The console cannot see the LED. Per fault, from `fault K confirm`: the heartbeat stops at the fault (about 1.5 s after the reply); the unit resets (the screen goes dark); the LED is on
steady while the SDRAM tests run, then 6 flashes twice (`blink.h`: 200 ms on, 200 ms off, a 4.0 s period), then the heartbeat. Film it if you can and note the flash count; once is enough per
kind, but do `bus` twice and `panic` once more to be sure the count is stable.

## 4. What the results decide

- Record and marker both there (`crash` has the record, `state` says `prev=K`): the noinit SRAM survives SYSRESETREQ and the record is right. `docs/FRAMEWORK.md` section 10's row can say
  "verified on the unit" for the record and the code.
- `state` says `prev=K` but `crash` says `no crash record`: the RAM survived and the record was not written, or not valid: the fault handler path is the suspect (record it exactly: `crash`, `stage`, `log`).
- Both `none` and `stage` says `power-on`: the RAM did not survive (or the unit lost power: a hang that needed a power cycle says nothing). If it is the reset, the record belongs in the VBAT register
  file (`crash_record.h`, the header comment): that is a platform change for a later slice, not a thing to fix here.
- A reset that is not `software`, or no reset at all (a hang, steady LED, the console not coming back in 60 s): power-cycle, note what the LED did, and stop the session for that kind.

Per kind, write down: `crash` (all lines), `stage`'s reset line, `log` line, `state`, the LED count, and the verdict (pass, differs, no fault).

## 5. What is not offered, and the way back

Not provoked, and why (no platform change is allowed here, decision D15):

- **A stack overflow.** The image has no stack guard and no MPU: an overflow of the 24 KiB main stack runs down through the 32 KiB or more of free SRAM and then into `.bss`, silently; deep enough it
  would trample the crash record itself. Nothing faults, so there is nothing to catch. (The fault stack protects the *handler* from a bad stack; a bad stack pointer cannot be provoked without the
  core locking up, which writes no record.)
- **The watchdog.** The reset sequence switches it off and nothing re-enables it (`main.cpp`, "Watchdog re-enable point"); a watchdog reset is therefore not something the platform can have. If the
  maintainer wants the record to survive a watchdog or a lockup reset (README, "open hardware check"), that needs the watchdog on in the platform, or a lockup, and its own session.
- **MemManage by the MPU.** The MPU is off; the one MemManage the core can raise is the default memory map's execute-never rule, which is what `mem` uses.
- **A plain unaligned `LDR`.** `CCR.UNALIGN_TRP` is not set, so a single-word unaligned access works; `unalign` uses `LDRD`, which always traps.
- **An app assert.** The kit has none; `panic` reaches the platform's `panic()` through the C++ runtime's pure-virtual handler (`__cxa_pure_virtual`).

The way back: put the unit in the state you found it in. The image is on the card, so `python3 -I $O3/o3console.py --update <the previous image>.ptf --crc <its crc> --yes-update --expect-version <its label>`
(about 45 s), then `fw rm crashtest-session.ptf` for the file you put there (never a file you did not put), `version`, `state`, `crash` as at the start. If the unit does not boot: the hidden reset
button flashes the card's first `.ptf` (`NEW-APP.md` section 11).
