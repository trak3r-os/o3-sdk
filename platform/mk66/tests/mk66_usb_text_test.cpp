// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the text of the platform's `diag` commands (usb/diag_text.h) as golden strings. The numbers are made up; the layout is
// what a person at a terminal reads, so a change here is a change of the console and deserves a look.
#include <doctest/doctest.h>

#include <o3sdk/core/action.h>
#include <o3sdk/platform/mk66/platform.h>
#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace o3sdk::platform::mk66;
namespace crash = o3sdk::platform::mk66::crash;

TEST_CASE("mk66 diag text: usb") {
  o3sdk::usb::DeviceCounters d;
  d.bus_resets = 3;
  d.isr_count = 5678;
  d.isr_cycles_last = 312;
  d.isr_cycles_max = 1850;
  d.tokens_ep0 = 42;
  d.tokens_cdc_out = 4;
  d.cdc_rx_bytes = 20;
  d.cdc_tx_packets = 12;
  d.cdc_tx_bytes = 345;
  d.midi_in_packets = 1;
  d.cdc_rx_holds = 3;
  d.errors[2] = 1;
  d.errors[7] = 2;
  o3sdk::usb::ControlCounters c;
  c.setups = 21;
  c.stalls = 2;
  c.suspends = 1;
  c.resumes = 1;
  c.configurations = 1;
  c.last_stall.request_type = 0x40;
  c.last_stall.request = 1;
  c.last_stall.value = 0x1234;
  c.last_stall.index = 0x5678;
  c.last_stall.length = 9;
  o3sdk::usb::Counters a;
  a.sof = 12345;
  console::UsbReport r{&d, &c, &a, o3sdk::usb::DeviceState::Configured, 7, false, true, false, {}, 2, 3, 1000, 0, 1, 1, 48000000};
  char buf[2048];
  const std::size_t n = console::format_usb(r, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strcmp(buf,
                    "usb: Configured, address 7, suspended no, pull-up yes, clock 48 MHz IRC48M\n"
                    "terminal: DTR yes, RTS no, line coding 115200 8N1 (stored, ignored)\n"
                    "bus: resets 3, suspends 1, resumes 1, SOF 12345\n"
                    "isr: entries 5678, last-cycles 312, worst-cycles 1850, loop-limit 0 (worst 10.2 us)\n"
                    "ep0: setups 21, tokens 42, stalls 2, configs 1, out-errors 0\n"
                    "  last stalled request: type 0x40 req 0x01 value 0x1234 index 0x5678 length 9\n"
                    "cdc-to-host: packets 12, bytes 345, written 1000, dropped-chunks 0\n"
                    "cdc-from-host: packets 4, bytes 20, dropped-chunks 0, holds 3, waiting 1\n"
                    "midi: to-host 1, from-host 0, events 0, skipped 0, ragged 0, overrun 0\n"
                    "errors: pid 0, crc5 0, crc16 1, dfn8 0, timeout 0, dma 0, stuff 2\n"
                    "faults: stray 0, resyncs 0, ep0-stalls 0\n") == 0);
  // With no stalled request the "last stalled" line is absent.
  c.stalls = 0;
  console::format_usb(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "last stalled") == nullptr);
  // A small buffer is cut, never overrun, and stays a C string.
  char tiny[40];
  std::memset(tiny, 'Z', sizeof tiny);
  console::format_usb(r, tiny, 32);
  CHECK(std::strlen(tiny) == 31u);
  CHECK(tiny[32] == 'Z');
}

TEST_CASE("mk66 diag text: usb ep0 trace lines") {
  using Tag = o3sdk::usb::Ep0Trace::Tag;
  char buf[96];
  o3sdk::usb::Ep0Trace::Entry e;
  e.tag = static_cast<std::uint8_t>(Tag::Setup);
  e.w0 = 0x02000680u;  // GET_DESCRIPTOR(configuration 0)
  e.extra = 0;
  e.bytes = 9;
  e.dt64 = 450;  // 450 * 64 cycles at 180 MHz = 160 us
  e.flags = 1;
  console::format_usb_ep0_entry(3, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#03 +160us setup type 80 req 06 value 0200 index 0000 len 0009 odd\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::ArmIn);
  e.w0 = 0x2000ABC0u;
  e.bytes = 9;
  e.flags = o3sdk::usb::Ep0Trace::make_flags(false, true, true, 0);
  console::format_usb_ep0_entry(12, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#12 +0us arm-in   tx buf 0x2000ABC0 n 9 DATA1 even\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::InDone);
  e.w0 = 0x2000ABC0u;
  e.bytes = 9;
  e.flags = o3sdk::usb::Ep0Trace::make_flags(true, true, true, 9);
  console::format_usb_ep0_entry(13, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#13 +0us in-done  tx buf 0x2000ABC0 n 9 DATA1 odd pid 9\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::Address);
  e.w0 = 19;
  console::format_usb_ep0_entry(4, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#04 +0us address 19\n") == 0);
  e.tag = static_cast<std::uint8_t>(Tag::StallSeen);
  console::format_usb_ep0_entry(5, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#05 +0us stall sent\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::SetupArmed);
  e.w0 = 3;
  e.extra = 1;
  console::format_usb_ep0_entry(6, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#06 +0us setup-armed even owned odd owned freeze released TXSUSPENDTOKENBUSY clear\n") == 0);
  e.w0 = 1;
  e.extra = 3;  // released, and the controller is still frozen on the readback
  console::format_usb_ep0_entry(7, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#07 +0us setup-armed even owned odd FREE freeze released TXSUSPENDTOKENBUSY set\n") == 0);
  e.extra = 2;  // a TOKDNE was pending: CTL not written, the freeze stays
  console::format_usb_ep0_entry(8, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#08 +0us setup-armed even owned odd FREE freeze kept TXSUSPENDTOKENBUSY set\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::StallCleared);
  console::format_usb_ep0_entry(9, e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "#09 +0us stall-cleared\n") == 0);
  e = {};
  e.tag = static_cast<std::uint8_t>(Tag::SetupArmed);
  e.w0 = 3;
  e.extra = 1;
  console::format_usb_ep0_retries(0, buf, sizeof buf);
  CHECK(std::strcmp(buf, "stall retries 0\n") == 0);
  console::format_usb_ep0_retries(17, buf, sizeof buf);
  CHECK(std::strcmp(buf, "stall retries 17\n") == 0);
  // A small buffer is cut, never overrun.
  char tiny[12];
  std::memset(tiny, 'Z', sizeof tiny);
  console::format_usb_ep0_entry(5, e, tiny, 8);
  CHECK(std::strlen(tiny) == 7u);
  CHECK(tiny[8] == 'Z');
}

TEST_CASE("mk66 diag text: stage, reset sources, crash") {
  char buf[512];
  console::format_stage(crash::make_context(0, crash::Stage::Running), 12345, 0x0400, true, 0, buf, sizeof buf);
  CHECK(std::strcmp(buf,
                    "stage: Running, module 0\nuptime: 12.3 s\nreset: 0x0400 (software), crash record at boot: yes\nstart-up faults: none\n") == 0);
  console::format_stage(crash::kUnknownContext, 999, 0x0080, false, console::kFaultAudio | console::kFaultApp, buf, sizeof buf);
  CHECK(std::strcmp(buf,
                    "stage: unknown (the stage words are not trustworthy)\nuptime: 0.9 s\nreset: 0x0080 (power-on), crash record at boot: no\n"
                    "start-up faults: audio (outputs muted, no sound), app (display held dark)\n") == 0);
  console::format_stage(crash::make_context(0, crash::Stage::Running), 5000, 0x0080, false, console::kFaultDisplay, buf, sizeof buf);
  CHECK(std::strstr(buf, "start-up faults: display (headless)\n") != nullptr);
  console::format_stage(crash::make_context(0, crash::Stage::Running), 5000, 0x0080, false, console::kFaultSd, buf, sizeof buf);
  CHECK(std::strstr(buf, "start-up faults: sd card (not mounted)\n") != nullptr);  // OS-007e
  char r[64];
  console::format_reset_sources(0x0480, r, sizeof r);
  CHECK(std::strcmp(r, "power-on+software") == 0);
  console::format_reset_sources(0, r, sizeof r);
  CHECK(std::strcmp(r, "none") == 0);
  console::format_reset_sources(kResetWatchdog | kResetLockup, r, sizeof r);
  CHECK(std::strcmp(r, "watchdog+lockup") == 0);

  console::format_crash(nullptr, buf, sizeof buf);
  CHECK(std::strcmp(buf, "no crash record\n") == 0);
  crash::CrashRecord rec{};
  rec.count = 2;
  rec.exception = 6;
  rec.pc = 0x1234;
  rec.lr = 0x5678;
  rec.xpsr = 0x01000000;
  rec.cfsr = 0x10000;
  rec.hfsr = 0x40000000;
  rec.exc_return = 0xFFFFFFF9;
  rec.sp = 0x2002FF00;
  rec.context = crash::make_context(0xD1, crash::Stage::Running);
  rec.uptime_ms = 10042;
  console::format_crash(&rec, buf, sizeof buf);
  CHECK(std::strcmp(buf,
                    "crash #2: UsageFault at 10.0 s of that run\n"
                    "  pc 0x00001234  lr 0x00005678  xpsr 0x01000000\n"
                    "  cfsr 0x00010000  hfsr 0x40000000\n"
                    "  mmfar 0x00000000  bfar 0x00000000\n"
                    "  exc_return 0xFFFFFFF9  frame 0x2002FF00\n"
                    "  stage Running, module 209\n") == 0);
  // An unusable frame and an unknown context say so.
  rec.sp = crash::kInvalidFrame;
  rec.context = crash::kUnknownContext;
  rec.exception = 16 + 53;
  console::format_crash(&rec, buf, sizeof buf);
  CHECK(std::strstr(buf, "IRQ 53 (no handler)") != nullptr);
  CHECK(std::strstr(buf, "frame unreadable") != nullptr);
  CHECK(std::strstr(buf, "stage unknown") != nullptr);
  char name[24];
  CHECK(std::strcmp(console::exception_name(3, name), "HardFault") == 0);
  CHECK(std::strcmp(console::exception_name(crash::kPanicException, name), "panic") == 0);
  CHECK(std::strcmp(console::exception_name(11, name), "exception 11") == 0);
}

TEST_CASE("mk66 diag text: log, input, version") {
  char buf[256];
  o3sdk::usb::diag::LogEntry e;
  e.ms = 25;
  std::strcpy(e.text, "usb: reset");
  console::format_log_entry(e, buf, sizeof buf);
  CHECK(std::strcmp(buf, "25 ms  usb: reset\n") == 0);
  o3sdk::core::InputEvent ev{12000, o3sdk::core::InputKind::Pad, 12, 127};
  console::format_input_event(ev, buf, sizeof buf);
  CHECK(std::strcmp(buf, "pad 12 down 127  @12 ms\n") == 0);
  ev = o3sdk::core::InputEvent{13000, o3sdk::core::InputKind::Encoder, 0, -2};
  console::format_input_event(ev, buf, sizeof buf);
  CHECK(std::strcmp(buf, "encoder 0 -2  @13 ms\n") == 0);
  ev = o3sdk::core::InputEvent{14000, o3sdk::core::InputKind::Button, 3, 0};
  console::format_input_event(ev, buf, sizeof buf);
  CHECK(std::strcmp(buf, "button 3 up  @14 ms\n") == 0);
  ev = o3sdk::core::InputEvent{0, o3sdk::core::InputKind::Pad, 47, 0};
  console::format_input_event(ev, buf, sizeof buf);
  CHECK(std::strcmp(buf, "pad 47 up  @0 ms\n") == 0);
  o3sdk::hal::InputStats s;
  s.online = 3;
  s.dropped = 1;
  console::format_input_stats(s, buf, sizeof buf);
  CHECK(std::strcmp(buf, "input: pads yes, buttons yes online; dropped 1, bus errors 0, filtered 0, resyncs 0\n") == 0);
  console::format_version("abc123-dirty", 0x16D0, 0x0E9F, 0x0001, "9CC819A7AD93F175", buf, sizeof buf);
  CHECK(std::strcmp(buf, "Track3r o3sdk, build abc123-dirty\nusb 16D0:0E9F rev 0001 serial 9CC819A7AD93F175\n") == 0);
}

TEST_CASE("mk66 diag text: audio start, the clock fault detail") {
  char buf[256];
  AudioClockFault f;
  f.where = AudioClockFault::MdrWait;
  f.mcr = 0xC0000000u;
  f.mdr = 0x000F00FEu;
  console::format_audio_start(4, f, buf, sizeof buf);
  CHECK(std::strcmp(buf, "audio start: ClockFault (4)\naudio clock fault at mdr, MCR 0xC0000000, MDR 0x000F00FE\n") == 0);
  f.where = AudioClockFault::McrWait;
  console::format_audio_start(4, f, buf, sizeof buf);
  CHECK(std::strstr(buf, "fault at mcr,") != nullptr);
  f.where = AudioClockFault::NoBoundaries;
  console::format_audio_start(4, f, buf, sizeof buf);
  CHECK(std::strstr(buf, "fault at boundaries,") != nullptr);
  // No detail unless it is a ClockFault with a recorded place; success says the outputs are muted.
  console::format_audio_start(0, f, buf, sizeof buf);
  CHECK(std::strcmp(buf, "audio start: Ok (0), outputs open when clean\n") == 0);
  console::format_audio_start(2, f, buf, sizeof buf);
  CHECK(std::strcmp(buf, "audio start: CodecAbsent (2)\n") == 0);
  f.where = AudioClockFault::None;
  console::format_audio_start(4, f, buf, sizeof buf);
  CHECK(std::strcmp(buf, "audio start: ClockFault (4)\n") == 0);
  CHECK(std::strcmp(console::audio_result_name(200), "?") == 0);
  // The log lines the image writes from the same names stay inside the log's 55 characters.
  CHECK(std::strlen("boot: audio clock fault at boundaries") <= 55u);
  CHECK(std::strlen("audio start MCR 0xC0000000 MDR 0x000F00FE") <= 55u);
}

TEST_CASE("mk66 diag text: usb audio (OS-006d)") {
  o3sdk::usb::DeviceCounters d;
  d.iso_in_starts = 2;
  d.iso_in_packets = 5000;
  d.iso_in_missed = 1;
  d.iso_out_starts = 1;
  d.iso_out_empty = 3;
  d.iso_fb_packets = 700;
  o3sdk::usb::Counters c;
  c.in_size_lo = 4400;
  c.in_size_hi = 600;
  c.in_prime_packets = 1;
  c.in_underruns = 0;
  c.in_dropped_blocks = 2;
  c.in_fill_min = 300;
  c.in_fill_max = 420;
  c.out_packets = 4900;
  c.out_dropped_frames = 5;
  c.out_underruns = 1;
  c.out_fill_min = 180;
  c.out_fill_max = 330;
  c.fb_last = 0x0B0788;
  c.fb_min = 0x0B0780;
  c.fb_max = 0x0B0790;
  console::UsbAudioReport r{o3sdk::usb::Mode::Audio, o3sdk::usb::Profile::P10, 0, o3sdk::usb::Mode::Audio, o3sdk::usb::Profile::P10, true, true, true, 10, &c, &d,
                            true, 180000u << 8, 0, o3sdk::usb::nominal_frames_q16(o3sdk::usb::Speed::Full), o3sdk::usb::nominal_frames_q16(o3sdk::usb::Speed::Full) + 64u,
                            -5, 372, 205};
  char buf[2048];
  const std::size_t n = console::format_usb_audio(r, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  CHECK(std::strstr(buf, "usb audio: AUDIO, profile 10 (10 ch), host configured\n") == buf);
  CHECK(std::strstr(buf, "layout: 10 OUT / 2 IN\nstreams: stems streaming 10 ch, return streaming\n") != nullptr);  // (a line of its own after the first)
  // The frame rates as "44.1000" (44.1176 with -DO3SDK_RATE_44100=OFF): the nominal rate and 64/65536 above it, to four places rounded, derived from the clock.
  const auto four_places = [](std::uint32_t q16) {
    std::uint32_t whole = q16 >> 16;
    std::uint32_t frac = ((q16 & 0xFFFFu) * 10000u + 32768u) >> 16;
    if (frac >= 10000u) {
      frac -= 10000u;
      ++whole;
    }
    char t[24];
    std::snprintf(t, sizeof t, "%u.%04u", static_cast<unsigned>(whole), static_cast<unsigned>(frac));
    return std::string(t);
  };
  const std::string nominal = four_places(o3sdk::usb::nominal_frames_q16(o3sdk::usb::Speed::Full));
  const std::string fed = four_places(o3sdk::usb::nominal_frames_q16(o3sdk::usb::Speed::Full) + 64u);
  CHECK(std::strstr(buf, ("rate: locked, " + nominal + " frames/SOF (+0 ppm vs nominal), SOF 180000.0 cycles, resyncs 0\n").c_str()) != nullptr);
  CHECK(std::strstr(buf, ("feedback: " + fed + " frames/SOF, trim -5/65536, value 0x0B0788 min 0x0B0780 max 0x0B0790, sent 700\n").c_str()) != nullptr);
  CHECK(std::strstr(buf, "in: starts 2, packets 5000, size44 4400, size45 600, other 0, prime 1, underruns 0, dropped-blocks 2, missed 1, fill 300..420 now 372\n") != nullptr);
  CHECK(std::strstr(buf, "out: starts 1, packets 4900, empty 3, bad 0, dropped-frames 5, underruns 1, fill 180..330 now 205\n") != nullptr);
  // A change in progress is said so; a stream that is idle is idle; no packet yet shows a dash for the ranges.
  r.change = 2;
  r.next_mode = o3sdk::usb::Mode::Standard;
  r.in_active = false;
  r.out_active = false;
  c = o3sdk::usb::Counters{};
  console::format_usb_audio(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "CHANGING to STANDARD 10 (detached)\n") != nullptr);
  // OS-USBRET: the layout line says what is presented and what is coming.
  r.usb_return = true;
  r.next_profile = o3sdk::usb::Profile::P8;
  r.next_return = false;
  console::format_usb_audio(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "layout: 10 OUT / 2 IN, next 8 OUT\n") != nullptr);
  r.change = 0;
  console::format_usb_audio(r, buf, sizeof buf);
  CHECK(std::strstr(buf, "layout: 10 OUT / 2 IN\n") != nullptr);
  r.change = 2;
  CHECK(std::strstr(buf, "streams: stems idle, return idle\n") != nullptr);
  CHECK(std::strstr(buf, "fill - now") != nullptr);
  // A rate 300 ppm fast and 300 ppm slow.
  // (867 / 2,890,138 = 300.0 ppm at 44,100 Hz; 299.9 at 44,117.647 Hz, printed 299: the integer ppm of the report, worked out from the nominal here.)
  const std::uint32_t nominal_q16 = o3sdk::usb::nominal_frames_q16(o3sdk::usb::Speed::Full);
  const std::string ppm = std::to_string(867u * 1000u / (nominal_q16 / 1000u));
  r.rate_q16 = nominal_q16 + 867u;
  console::format_usb_audio(r, buf, sizeof buf);
  CHECK(std::strstr(buf, ("(+" + ppm + " ppm vs nominal)").c_str()) != nullptr);
  r.rate_q16 = nominal_q16 - 867u;
  console::format_usb_audio(r, buf, sizeof buf);
  CHECK(std::strstr(buf, ("(-" + ppm + " ppm vs nominal)").c_str()) != nullptr);
  char tiny[40];
  std::memset(tiny, 'Z', sizeof tiny);
  console::format_usb_audio(r, tiny, 32);
  CHECK(std::strlen(tiny) == 31u);
  CHECK(tiny[32] == 'Z');
}

TEST_CASE("mk66 diag text: the mode names (OS-006d)") {
  CHECK(std::strcmp(console::usb_mode_name(o3sdk::usb::Mode::Standard), "STANDARD") == 0);
  CHECK(std::strcmp(console::usb_mode_name(o3sdk::usb::Mode::Audio), "AUDIO") == 0);
  CHECK(std::strcmp(console::usb_profile_name(o3sdk::usb::Profile::P4S), "4S") == 0);
  // OS-USBRET: the user's names of the layouts.
  char layout[20];
  CHECK(std::strcmp(console::usb_layout_name(o3sdk::usb::Profile::P10, false, layout), "10 OUT") == 0);
  CHECK(std::strcmp(console::usb_layout_name(o3sdk::usb::Profile::P8, true, layout), "8 OUT / 2 IN") == 0);
  CHECK(std::strcmp(console::usb_layout_name(o3sdk::usb::Profile::P2, true, layout), "2 OUT / 2 IN") == 0);
  CHECK(std::strcmp(console::usb_layout_name(o3sdk::usb::Profile::P11, false, layout), "11 OUT") == 0);
  CHECK(std::strcmp(console::usb_layout_name(o3sdk::usb::Profile::P10, true, layout), "10 OUT / 2 IN") == 0);
}
