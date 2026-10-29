// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The kit's console commands: names and one-line help as plain data that the host tests read. Copied from track3r-os
// (platform/mk66/usb/command_table.h) for the commands this console has; the image's table (src/console.cpp) takes every name and help
// text from here by enumerator and binds only the handler. A help text that named a sub-command this console does not have (cpu perf,
// usb map and profile, enc accel) names only the ones it has. `fw` is the only command that writes the card or reaches the bootloader
// (core.h keeps an app's commands from using the words of that path).

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::console {

// The order of the table, and of `help`: track3r-os's relative order for the commands in scope; `midi` (docs/M2B2-PLAN.md, slice MD), `files` (docs/M2C-PLAN.md, slice FA) and `radio` and `mon` (slice AI: `radio` is track3r-os's OS-RAD1 command, `mon` its line-in monitor level) and `rec` (slice RC) are o3-sdk's own and come last.
enum class Cmd : unsigned {
  Help, Version, Stage, Cpu, Mem, Crash, Usb, Log, Input, Peek, Poke, Rot, Sd, Ls, Reset, Fw, Key, Pad, Enc, State, Shot, Leds, Cache, Power, Guard, Midi, Files, Radio, Mon, Rec,
  Count
};

struct CommandInfo {
  const char* name;  // 2-7 lowercase letters
  const char* help;  // one line, at most 70 characters
};

inline constexpr CommandInfo kCommandInfo[] = {
    {"help", "list the commands"},
    {"version", "build identity and USB identity"},
    {"stage", "boot/run stage, uptime, why the chip reset"},
    {"cpu", "cpu [reset]: audio load: cycles per stage, underruns, jitter"},
    {"mem", "memory map, SRAM budgets, free SRAM"},
    {"crash", "the crash record the previous run left"},
    {"usb", "usb [audio|mode ..]: counters, AUDIO mode"},
    {"log", "log [count]: the last lines boot code and drivers wrote"},
    {"input", "input [stats]: live input events (any key stops)"},
    {"peek", "peek <hexaddr> [1..16]: read 32-bit words (clocked modules only)"},
    {"poke", "poke <hexaddr> <hexvalue>: write one word, I2S0 registers only"},
    {"rot", "rot <0..7>: set REG_ROTATE now (display orientation, not saved)"},
    {"sd", "sd [mount]: the SD card (slot, mount, identity, speed, counters)"},
    {"ls", "ls [path]: list a directory of the card (read only)"},
    {"reset", "mute the outputs, then restart the unit"},
    {"fw", "fw [put|rm|arm|update ..]: firmware files on the SD card (OS-006c)"},
    {"key", "key <button> [tap|down|up|hold <ms>]: inject a button press"},
    {"pad", "pad <0..47> [tap|down|up] [vel]: inject a pad press"},
    {"enc", "enc <n>: n detents, -64..64 (minus = counter-clockwise)"},
    {"state", "app state as one key=value line (app, screen, playing, ...)"},
    {"shot", "shot [565|8888 [w]] [pclk0]: the screen as framed base64 (o3console)"},
    {"leds", "leds: pad LED levels as the driver holds them, one line"},
    {"cache", "cache [on|off|inv|check]: the code cache (LMEM): state, switch, check"},
    {"power", "power [press|release|pin|off [dry]]: the Power switch and the shutdown"},
    {"guard", "guard [n <1..255>]: the main-loop guard's window (OS-MAINGUARD)"},
    {"midi", "midi: MIDI in and out: follow state, tempo, counters, the last bytes"},
    {"files", "files [crc <path>]: app files on the card: last job, cost, CRC-32"},
    {"radio", "radio [retry]: FM radio state, last failure, registers, pins"},
    {"mon", "mon [off|on|0..100]: audio-in monitor level (0 = off, on = 70)"},
    {"rec", "rec: the recorder's state, frames, overrun, tap"},
};
static_assert(sizeof(kCommandInfo) / sizeof(kCommandInfo[0]) == static_cast<std::size_t>(Cmd::Count), "one row per Cmd");

inline constexpr std::size_t kKitCommandCount = static_cast<std::size_t>(Cmd::Count);

constexpr const CommandInfo& info(Cmd c) noexcept { return kCommandInfo[static_cast<unsigned>(c)]; }

// OS-POWER P1 (D2): once the shutdown has begun the shell runs only these, the reads that diagnose it (console.cpp power_gate); every other command, the app's included, is answered `ERR powering off`. One bit per Cmd.
static_assert(static_cast<unsigned>(Cmd::Count) <= 64u, "kPowerReads has a bit per Cmd");
constexpr std::uint64_t cmd_bit(Cmd c) noexcept { return std::uint64_t{1} << static_cast<unsigned>(c); }
inline constexpr std::uint64_t kPowerReads = cmd_bit(Cmd::Help) | cmd_bit(Cmd::Version) | cmd_bit(Cmd::Stage) | cmd_bit(Cmd::Log) | cmd_bit(Cmd::Crash) | cmd_bit(Cmd::Power) | cmd_bit(Cmd::State) |
                                             cmd_bit(Cmd::Cpu) | cmd_bit(Cmd::Mem);
constexpr bool power_read(Cmd c) noexcept { return (kPowerReads & cmd_bit(c)) != 0u; }

}  // namespace o3sdk::platform::mk66::console
