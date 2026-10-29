// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Storage for one object that is too big for a test's stack (the Cortex-M4 target suites run on a 60 KiB stack with a 32 KiB guard, and the
// audio Engine is 14 KiB): `static StaticSlot<Engine> slot; Engine& e = slot.make(&cycles);` builds a fresh object in the same place each time.
// The objects here are trivially destructible, so the previous one is simply built over.
//
// A type that takes an external list store (seq::Sequencer, E-SONG-07: `T::OrderStoreType`, `attach_order_store`) gets one in the slot, attached at every make().

#include <new>
#include <type_traits>

namespace o3sdk::test {

namespace detail {
template <typename T, typename = void>
struct SlotStore {
  void attach(T&) noexcept {}
};
template <typename T>
struct SlotStore<T, std::void_t<typename T::OrderStoreType>> {
  typename T::OrderStoreType store;
  void attach(T& t) noexcept { t.attach_order_store(store); }
};
}  // namespace detail

template <typename T>
class StaticSlot {
 public:
  template <typename... Args>
  T& make(Args&&... args) {
    T& t = *new (storage_) T(static_cast<Args&&>(args)...);
    extra_.attach(t);
    return t;
  }

 private:
  alignas(T) unsigned char storage_[sizeof(T)];
  detail::SlotStore<T> extra_;
};

}  // namespace o3sdk::test
