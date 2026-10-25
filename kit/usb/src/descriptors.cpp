// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB descriptor builder. Byte layouts follow USB 2.0 chapter 9, the Class Definitions for Audio Devices 2.0
// (UAC2), for MIDI Devices 1.0 and the CDC 1.2 PSTN (ACM) subclass; every field is written out by hand, no
// third-party code is used.
#include <o3sdk/usb/descriptors.h>

#include <initializer_list>

namespace o3sdk::usb {
namespace {

// Descriptor types and class codes (the ones that appear in these descriptors).
constexpr std::uint8_t kTypeDevice = 1;
constexpr std::uint8_t kTypeConfig = 2;
constexpr std::uint8_t kTypeString = 3;
constexpr std::uint8_t kTypeInterface = 4;
constexpr std::uint8_t kTypeEndpoint = 5;
constexpr std::uint8_t kTypeIad = 0x0B;
constexpr std::uint8_t kTypeCsInterface = 0x24;
constexpr std::uint8_t kTypeCsEndpoint = 0x25;

constexpr std::uint8_t kClassAudio = 0x01;
constexpr std::uint8_t kSubAudioControl = 0x01;
constexpr std::uint8_t kSubAudioStreaming = 0x02;
constexpr std::uint8_t kSubMidiStreaming = 0x03;
constexpr std::uint8_t kClassCdc = 0x02;
constexpr std::uint8_t kClassCdcData = 0x0A;
constexpr std::uint8_t kClassMassStorage = 0x08;
constexpr std::uint8_t kSubScsiTransparent = 0x06;
constexpr std::uint8_t kProtoBulkOnly = 0x50;
constexpr std::uint8_t kProtoUac2 = 0x20;

constexpr std::uint8_t lo8(std::uint32_t v) { return static_cast<std::uint8_t>(v & 0xFFu); }
constexpr std::uint8_t hi8(std::uint32_t v) { return static_cast<std::uint8_t>((v >> 8) & 0xFFu); }

// Appends bytes to a caller buffer. A write that does not fit is counted but dropped, so size() is
// always the full length the descriptor needs and ok() tells whether it fitted.
class Writer {
 public:
  Writer(std::uint8_t* out, std::size_t cap) noexcept : out_(out), cap_(cap) {}
  void put(std::initializer_list<std::uint8_t> bytes) noexcept {
    for (const std::uint8_t b : bytes) {
      byte(b);
    }
  }
  void byte(std::uint8_t b) noexcept {
    if (n_ < cap_) {
      out_[n_] = b;
    }
    ++n_;
  }
  void patch16(std::size_t at, std::uint32_t v) noexcept {
    if (at + 1u < cap_) {
      out_[at] = lo8(v);
      out_[at + 1u] = hi8(v);
    }
  }
  std::size_t size() const noexcept { return n_; }
  bool ok() const noexcept { return n_ <= cap_; }

 private:
  std::uint8_t* out_;
  std::size_t cap_;
  std::size_t n_ = 0;
};

// Bulk endpoints: 64 bytes at full speed, 512 at high speed (USB 2.0 5.8.3).
constexpr std::uint32_t bulk_max_packet(Speed s) { return s == Speed::Full ? 64u : 512u; }

void write_cdc(Writer& w, Speed speed) {
  // Interface association: the two CDC interfaces are one function.
  w.put({8, kTypeIad, layout::kIfCdcComm, 2, kClassCdc, 0x02, 0x01, kStrCdc});
  // Communications interface (ACM), one interrupt IN endpoint for notifications.
  w.put({9, kTypeInterface, layout::kIfCdcComm, 0, 1, kClassCdc, 0x02, 0x01, kStrCdc});
  w.put({5, kTypeCsInterface, 0x00, 0x10, 0x01});                    // Header, bcdCDC 1.10
  w.put({5, kTypeCsInterface, 0x01, 0x00, layout::kIfCdcData});      // Call Management: none, over the data interface
  w.put({4, kTypeCsInterface, 0x02, 0x02});                          // ACM: line coding + line state requests
  w.put({5, kTypeCsInterface, 0x06, layout::kIfCdcComm, layout::kIfCdcData});  // Union
  // bInterval: 16 ms at full speed (frames), 2^(8-1) microframes = 16 ms at high speed.
  w.put({7, kTypeEndpoint, layout::kEpCdcNotify, 0x03, 16, 0, speed == Speed::Full ? std::uint8_t{16} : std::uint8_t{8}});
  // Data interface, bulk OUT + IN.
  const std::uint32_t mps = bulk_max_packet(speed);
  w.put({9, kTypeInterface, layout::kIfCdcData, 0, 2, kClassCdcData, 0, 0, 0});
  w.put({7, kTypeEndpoint, layout::kEpCdcOut, 0x02, lo8(mps), hi8(mps), 0});
  w.put({7, kTypeEndpoint, layout::kEpCdcIn, 0x02, lo8(mps), hi8(mps), 0});
}

void write_midi(Writer& w, Mode mode, bool usb_return, Speed speed) {
  const std::uint8_t if_control = layout::if_midi_control(mode, usb_return);
  const std::uint8_t if_stream = layout::if_midi_stream(mode, usb_return);
  // MIDI jack IDs: 1 embedded IN (fed by the bulk OUT endpoint), 2 external IN (the DIN input),
  // 3 embedded OUT (feeds the bulk IN endpoint, source jack 2), 4 external OUT (the DIN output, source jack 1).
  constexpr std::uint32_t kJackHeaderTotal = 7u + 6u + 6u + 9u + 9u;  // class-specific MS header + 4 jacks
  w.put({8, kTypeIad, if_control, 2, kClassAudio, kSubMidiStreaming, 0x00, kStrMidi});
  w.put({9, kTypeInterface, if_control, 0, 0, kClassAudio, kSubAudioControl, 0x00, kStrMidi});
  w.put({9, kTypeCsInterface, 0x01, 0x00, 0x01, 9, 0, 1, if_stream});  // AC header, bcdADC 1.00
  w.put({9, kTypeInterface, if_stream, 0, 2, kClassAudio, kSubMidiStreaming, 0x00, kStrMidi});
  w.put({7, kTypeCsInterface, 0x01, 0x00, 0x01, lo8(kJackHeaderTotal), hi8(kJackHeaderTotal)});  // MS header, 1.00
  w.put({6, kTypeCsInterface, 0x02, 0x01, 1, kStrMidi});  // MIDI IN jack, embedded
  w.put({6, kTypeCsInterface, 0x02, 0x02, 2, 0});         // MIDI IN jack, external
  w.put({9, kTypeCsInterface, 0x03, 0x01, 3, 1, 2, 1, kStrMidi});  // MIDI OUT jack, embedded, source (2, pin 1)
  w.put({9, kTypeCsInterface, 0x03, 0x02, 4, 1, 1, 1, 0});         // MIDI OUT jack, external, source (1, pin 1)
  const std::uint32_t mps = bulk_max_packet(speed);
  w.put({9, kTypeEndpoint, layout::kEpMidiOut, 0x02, lo8(mps), hi8(mps), 0, 0, 0});
  w.put({5, kTypeCsEndpoint, 0x01, 1, 1});  // MS general endpoint, associated with embedded IN jack 1
  w.put({9, kTypeEndpoint, layout::kEpMidiIn, 0x02, lo8(mps), hi8(mps), 0, 0, 0});
  w.put({5, kTypeCsEndpoint, 0x01, 1, 3});  // associated with embedded OUT jack 3
}

// DISK mode: one Mass Storage interface (SCSI transparent command set, Bulk-Only Transport), a bulk OUT and a bulk IN endpoint. It is a function of
// its own with no IAD (class drivers bind a lone mass storage interface of a composite by interface class).
void write_msc(Writer& w, Speed speed) {
  const std::uint32_t mps = bulk_max_packet(speed);
  w.put({9, kTypeInterface, layout::kIfMsc, 0, 2, kClassMassStorage, kSubScsiTransparent, kProtoBulkOnly, kStrDisk});
  w.put({7, kTypeEndpoint, layout::kEpMscOut, 0x02, lo8(mps), hi8(mps), 0});
  w.put({7, kTypeEndpoint, layout::kEpMscIn, 0x02, lo8(mps), hi8(mps), 0});
}

// One UAC2 AudioStreaming interface: alt 0 (no endpoints, no bandwidth) and alt 1 (the stream).
// `in` selects the stems stream (device to host) or the return stream (host to device, with feedback).
void write_audio_stream(Writer& w, const Config& cfg, bool in) {
  const std::uint32_t channels = in ? profile_channels(cfg.profile) : kReturnChannels;
  const std::uint8_t interface = in ? layout::kIfAudioIn : layout::kIfAudioOut;
  const std::uint8_t link = in ? layout::kEntStreamToHost : layout::kEntStreamFromHost;
  const std::uint8_t first_name = in ? kStrChannelNames
                                     : static_cast<std::uint8_t>(kStrChannelNames + profile_channels(cfg.profile));
  const std::uint32_t mps = in ? in_stream_max_packet(cfg.profile, cfg.speed) : out_stream_max_packet(cfg.speed);

  w.put({9, kTypeInterface, interface, 0, 0, kClassAudio, kSubAudioStreaming, kProtoUac2, 0});
  w.put({9, kTypeInterface, interface, 1, in ? std::uint8_t{1} : std::uint8_t{2}, kClassAudio, kSubAudioStreaming,
         kProtoUac2, 0});
  // AS general: terminal link, no controls, format type I, PCM, channels, no spatial locations.
  w.put({16, kTypeCsInterface, 0x01, link, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, lo8(channels), 0x00, 0x00, 0x00,
         0x00, first_name});
  w.put({6, kTypeCsInterface, 0x02, 0x01, static_cast<std::uint8_t>(kBytesPerSample), 16});  // Type I: 2-byte slot, 16 bits
  // Data endpoint: isochronous, asynchronous. Interval 1 = every (micro)frame.
  w.put({7, kTypeEndpoint, in ? layout::kEpAudioIn : layout::kEpAudioOut, 0x05, lo8(mps), hi8(mps), 1});
  w.put({8, kTypeCsEndpoint, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});  // AS general endpoint: no controls, no lock delay
  if (!in) {
    // Explicit feedback: isochronous, no synchronisation, usage = feedback. 3 bytes (Q10.14) at full speed,
    // 4 bytes (Q16.16) at high speed; interval 1 frame at FS, 2^(4-1) microframes = 1 ms at HS.
    const bool fs = cfg.speed == Speed::Full;
    w.put({7, kTypeEndpoint, layout::kEpAudioFeedback, 0x11, fs ? std::uint8_t{3} : std::uint8_t{4}, 0,
           fs ? std::uint8_t{1} : std::uint8_t{4}});
  }
}

void write_audio(Writer& w, const Config& cfg) {
  const std::uint32_t n = profile_channels(cfg.profile);
  const std::uint8_t in_names = kStrChannelNames;
  const std::uint8_t out_names = static_cast<std::uint8_t>(kStrChannelNames + n);
  const bool ret = cfg.usb_return;
  // AC header + clock + the stems input and output terminals, and with USB RETURN the return input and output terminals.
  const std::uint32_t ac_total = 9u + 8u + 17u + 12u + (ret ? 17u + 12u : 0u);

  // The UAC2 function: AudioControl + the stems stream, plus the return stream with USB RETURN.
  w.put({8, kTypeIad, layout::kIfAudioControl, ret ? std::uint8_t{3} : std::uint8_t{2}, kClassAudio, 0x00, kProtoUac2, kStrAudio});
  w.put({9, kTypeInterface, layout::kIfAudioControl, 0, 0, kClassAudio, kSubAudioControl, kProtoUac2, kStrAudio});
  // AC header: bcdADC 2.00, category 0x0A (pro audio), no controls.
  w.put({9, kTypeCsInterface, 0x01, 0x00, 0x02, 0x0A, lo8(ac_total), hi8(ac_total), 0x00});
  // Clock Source: internal fixed clock, frequency control read-only (one discrete rate, GET only).
  w.put({8, kTypeCsInterface, 0x0A, layout::kEntClock, 0x01, 0x01, 0x00, kStrClock});
  // Input Terminal "stems" (type 0x0603, line connector: external, usable as input or output), n channels.
  w.put({17, kTypeCsInterface, 0x02, layout::kEntStemsInput, 0x03, 0x06, 0x00, layout::kEntClock, lo8(n), 0x00, 0x00,
         0x00, 0x00, in_names, 0x00, 0x00, kStrAudio});
  // Output Terminal, USB streaming (0x0101), to the host.
  w.put({12, kTypeCsInterface, 0x03, layout::kEntStreamToHost, 0x01, 0x01, 0x00, layout::kEntStemsInput,
         layout::kEntClock, 0x00, 0x00, 0x00});
  if (ret) {
    // Input Terminal, USB streaming, from the host: 2 channels.
    w.put({17, kTypeCsInterface, 0x02, layout::kEntStreamFromHost, 0x01, 0x01, 0x00, layout::kEntClock,
           static_cast<std::uint8_t>(kReturnChannels), 0x00, 0x00, 0x00, 0x00, out_names, 0x00, 0x00, kStrAudio});
    // Output Terminal "return" (0x0603).
    w.put({12, kTypeCsInterface, 0x03, layout::kEntReturnOutput, 0x03, 0x06, 0x00, layout::kEntStreamFromHost,
           layout::kEntClock, 0x00, 0x00, 0x00});
  }
  write_audio_stream(w, cfg, true);
  if (ret) {
    write_audio_stream(w, cfg, false);
  }
}

// ASCII text to a string descriptor.
std::size_t write_string(const char* text, std::uint8_t* out, std::size_t cap) {
  std::size_t len = 0;
  while (text[len] != '\0' && len < 31u) {
    ++len;
  }
  const std::size_t total = 2u + 2u * len;
  if (len == 0 || total > cap) {
    return 0;
  }
  out[0] = static_cast<std::uint8_t>(total);
  out[1] = kTypeString;
  for (std::size_t i = 0; i < len; ++i) {
    out[2u + 2u * i] = static_cast<std::uint8_t>(text[i]);
    out[3u + 2u * i] = 0;
  }
  return total;
}

}  // namespace

std::size_t build_device_descriptor(const Config& cfg, std::uint8_t* out, std::size_t cap) noexcept {
  Writer w(out, cap);
  const std::uint16_t bcd = device_bcd(cfg.mode);
  // bcdUSB 2.00; class 0xEF/0x02/0x01 = Miscellaneous, Common, Interface Association (composite with IADs);
  // EP0 max packet 64; one configuration.
  w.put({18, kTypeDevice, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, lo8(kVendorId), hi8(kVendorId), lo8(kProductId), hi8(kProductId),
         lo8(bcd), hi8(bcd), kStrManufacturer, kStrProduct, kStrSerial, 1});
  return w.ok() ? w.size() : 0;
}

std::size_t build_config_descriptor(const Config& cfg, std::uint8_t* out, std::size_t cap) noexcept {
  Writer w(out, cap);
  const bool audio = cfg.mode == Mode::Audio;
  w.put({9, kTypeConfig, 0, 0, layout::interface_count(cfg.mode, cfg.usb_return), 1, 0, 0x80, kMaxPowerUnits});
  write_cdc(w, cfg.speed);
  if (audio) {
    write_audio(w, cfg);  // before MIDI: Linux takes the first AudioControl interface as the one holding the terminals
  }
  write_midi(w, cfg.mode, cfg.usb_return, cfg.speed);
  if (cfg.mode == Mode::Disk) {
    write_msc(w, cfg.speed);
  }
  w.patch16(2, static_cast<std::uint32_t>(w.size()));
  return w.ok() ? w.size() : 0;
}

std::uint32_t string_count(const Config& cfg) noexcept {
  return cfg.mode == Mode::Audio ? kStrChannelNames + profile_channels(cfg.profile) + (cfg.usb_return ? kReturnChannels : 0u)
                                 : (cfg.mode == Mode::Disk ? kStrDisk + 1u : static_cast<std::uint32_t>(kStrAudio));
}

std::size_t channel_name(Profile profile, std::uint32_t index, char* out) noexcept {
  const std::uint32_t n = profile_channels(profile);
  if (index >= n) {
    return 0;
  }
  std::size_t len = 0;
  const auto put = [&](char c) { out[len++] = c; };
  const auto put_text = [&](const char* s) {
    while (*s != '\0') {
      put(*s++);
    }
  };
  if (index >= n - 2u) {
    put_text(index == n - 2u ? "Master L" : "Master R");
  } else if (profile == Profile::P4S) {
    put('S');
    put(static_cast<char>('1' + static_cast<char>(index / 2u)));
    put(' ');
    put((index & 1u) == 0u ? 'L' : 'R');
  } else {
    put('S');
    put(static_cast<char>('1' + static_cast<char>(index)));
  }
  out[len] = '\0';
  return len;
}

std::size_t build_string_descriptor(const Config& cfg, std::uint8_t index, std::uint8_t* out,
                                    std::size_t cap) noexcept {
  if (index >= string_count(cfg)) {
    return 0;
  }
  switch (index) {
    case kStrLangId:
      if (cap < 4u) {
        return 0;
      }
      out[0] = 4;
      out[1] = kTypeString;
      out[2] = 0x09;  // 0x0409 US English
      out[3] = 0x04;
      return 4;
    case kStrManufacturer:
      return write_string(kManufacturerString, out, cap);
    case kStrProduct:
      return write_string(kProductString, out, cap);
    case kStrSerial: {
      const char* s = cfg.serial;
      if (s == nullptr || s[0] == '\0') {
        s = kSerialPlaceholder;
      }
      char buf[kMaxSerialChars + 1u];
      std::size_t n = 0;
      while (s[n] != '\0' && n < kMaxSerialChars) {
        buf[n] = s[n];
        ++n;
      }
      buf[n] = '\0';
      return write_string(buf, out, cap);
    }
    case kStrCdc:
      return write_string(kCdcString, out, cap);
    case kStrMidi:
      return write_string(kMidiString, out, cap);
    case kStrAudio:  // = kStrDisk
      return write_string(cfg.mode == Mode::Disk ? kDiskString : kAudioString, out, cap);
    case kStrClock:
      return write_string("44100 Hz clock", out, cap);
    default:
      break;
  }
  // Channel names: the IN stream's, then the return stream's two.
  const std::uint32_t n = profile_channels(cfg.profile);
  const std::uint32_t k = static_cast<std::uint32_t>(index) - kStrChannelNames;
  char name[10];
  if (k < n) {
    channel_name(cfg.profile, k, name);
  } else {
    const char* ret = (k - n) == 0u ? "Return L" : "Return R";
    std::size_t i = 0;
    for (; ret[i] != '\0'; ++i) {
      name[i] = ret[i];
    }
    name[i] = '\0';
  }
  return write_string(name, out, cap);
}

}  // namespace o3sdk::usb
