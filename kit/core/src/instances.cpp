// SPDX-License-Identifier: GPL-3.0-or-later
//
// Explicit instantiation of the shared input queue. It makes the ring compile (and show up in the size
// report) on every target, including mk66 where no test binary exists. Costs nothing at link time:
// the members are weak, per-function sections that --gc-sections drops unless something uses them.
#include <o3sdk/core/input_event.h>

namespace o3sdk::core {

template class SpscRing<InputEvent, kInputQueueCapacity>;

}  // namespace o3sdk::core
