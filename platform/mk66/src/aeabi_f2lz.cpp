// SPDX-License-Identifier: GPL-3.0-or-later
//
// __aeabi_f2lz, the run-time helper behind a float -> int64 conversion, by integer arithmetic (midi/f2lz.h says why): the toolchain's own is built on software doubles. Defining it here
// keeps libgcc's _fixsfdi member, and the double helpers it pulls in, out of every image. Only the image has it: the host has its own conversion.
#include <o3sdk/platform/mk66/midi/f2lz.h>

// The run-time helpers take their arguments by the base standard (the float arrives in r0, as the call sites in the objects load it), not by the hard-float variant (s0) that an ordinary
// function of this image has: pcs("aapcs") says so, and the disassembly of a call site and of this function is how it is checked.
extern "C" __attribute__((pcs("aapcs"))) long long __aeabi_f2lz(float f) {
  return o3sdk::platform::mk66::midi::float_to_int64(f);
}
