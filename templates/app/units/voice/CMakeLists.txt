# SPDX-License-Identifier: GPL-3.0-or-later
# VOICE: the template's one unit, a small monophonic instrument. The library is `unit_voice`; its header is
# <@APP_ID@/units/voice/voice.h>. A unit depends on o3-sdk's dsp layer only (o3sdk_unit links it); it is single precision
# (-fsingle-precision-constant) and warning-free under -Werror like the kit.
o3sdk_unit(voice SOURCES src/voice.cpp)
