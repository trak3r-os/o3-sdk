# The chunk container of an app's own files (`FORM`), version 1

Status: built with slice FA of M2c, 2026-10-29 (`docs/M2C-PLAN.md` decision D7) · Code: `kit/formats/include/o3sdk/formats/chunk_file.h`, `src/chunk_file.cpp` · Test: `tests/chunk_file_test.cpp`

The shape of track3r-os's settings and project files (`docs/formats/o3-project.md` there, sections 1 and 2), written again here as new code: an IFF-style `FORM`, big-endian, even-aligned chunks, a CRC-32 trailer. An app that wants versioned data in a file of its own (a pattern, a kit) writes it with `formats::chunk::Writer` into its own buffer and saves the bytes with `Files::save`; the kit's settings file (`o3sdk-settings.md`) is one. An app may also save raw bytes in any format it likes: the container is optional.

## 1. Conventions

- **Big-endian.** Every multi-byte integer is big-endian, as in IFF. A float is its IEEE-754 binary32 bit pattern, big-endian.
- **Chunk id**: four ASCII characters in `0x20..0x7E` (`"CRC "` has a space).
- **Chunk** = id (4) + payload length `n` (`u32`) + payload (`n` bytes) + one zero pad byte when `n` is odd, so every chunk starts at an even offset. The length does not include the pad.
- **Chunk version**: by convention the first `u16` of the payload of a known chunk, major in the high byte and minor in the low one. A reader refuses a newer major and reads a newer minor as far as it knows it. The container itself does not look at it; `Fields` is the helper that reads the payload.

## 2. Container

```
"FORM"  u32 length  <type: 4 characters>  chunk chunk ... chunk  "CRC " chunk
```

- `length` counts everything after the length field itself: the 4 bytes of the type and all the chunks with their headers and pads. It is even and at least 4. The file is `length + 8` bytes; bytes after that are ignored.
- The **`CRC `** chunk is last and ends exactly at the end of the `FORM`. Its payload is `u16 version` (`0x0100`) and `u32 crc`; a newer minor may carry more bytes after those, a newer major is refused. The CRC is CRC-32/ISO-HDLC (`formats/crc32.h`: the one of zlib, PNG and Ethernet; the check value of `"123456789"` is `0xCBF43926`) over every byte from offset 0 up to, not including, the header of the `CRC ` chunk. It therefore covers the `FORM` length.
- Chunks other than `CRC ` come in the order the file's type says; the container does not care. A reader that meets one it does not know skips it (and counts it, if it wants to say so).

## 3. What the reader checks

`Reader::open(data, n, type)` checks the whole container before a chunk is handed out, and no chunk of a file that fails is decoded:

| Error | When |
|---|---|
| `Truncated` | fewer than 12 bytes, or fewer bytes than `length + 8` |
| `BadMagic` | not `"FORM"`, or a type other than the one asked |
| `Malformed` | `length` odd or below 4; an id that is not printable; a chunk that runs past the end; a pad byte that is not zero; no `CRC ` chunk, or one that is not last, or shorter than 6 bytes, or of another major version |
| `BadCrc` | the CRC does not match |

`Writer` keeps room for the `CRC ` chunk (14 bytes) at all times: a write that would leave less is refused and `finish()` then answers 0. `finish()` sets the `FORM` length first, then computes the CRC, so the length is covered.

## 4. Where it is used

| File | Type | Chunks |
|---|---|---|
| `Apps/<app>/system.o3s` | `O3SK` | `SETS`, `APPV`, `LRNB` (`o3sdk-settings.md`) |
| HELLO's `riff.o3a` | `HRIF` | `RIFF` (`apps/hello/src/hello_app.cpp`) |
