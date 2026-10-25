# SPDX-License-Identifier: GPL-3.0-or-later
# hal: contracts only (Display, Input, Clock, the interrupt priority table, the audio device, the read-only SD card storage). Headers only; depends on core
# alone: the one sample-rate constant they need is core/audio_clock.h's (ADR 0012 am. 2).
o3sdk_library(hal DEPENDS core)
