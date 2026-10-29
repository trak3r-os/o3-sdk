# SPDX-License-Identifier: GPL-3.0-or-later
# formats: byte-stream codecs over core::ByteSource / ByteSink (docs/M2C-PLAN.md, slice FS), ported from track3r-os formats/ and formats/wav: the CRC-32 the save
# layer verifies a file with, the Progress a resumable job step returns, the sample probe (a WAV's channel count from its header) and the resumable WAV reader
# (a sample into a caller's int16 buffer, a slice at a time). Depends on core only; app depends on it (core <- formats <- app). No heap, no double, no printf.
o3sdk_library(formats SOURCES src/crc32.cpp src/sample_probe.cpp src/wav_importer.cpp src/chunk_file.cpp DEPENDS core)
# The single-precision rule (cmake/O3sdkTargets.cmake): a layer outside its list says so itself, as track3r-os's formats library did.
target_compile_options(formats PRIVATE -fsingle-precision-constant)
target_compile_options(formats_hdrcheck PRIVATE -fsingle-precision-constant)
