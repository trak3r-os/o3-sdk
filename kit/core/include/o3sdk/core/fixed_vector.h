// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <type_traits>

namespace o3sdk::core {

// Vector with a compile-time capacity N and inline storage: no heap, no exceptions.
// Overflow is reported by the return value (the item is not stored), never by UB.
// T must be trivially copyable and destructible; all N slots are value-initialised up front, which keeps
// the class free of placement-new and manual lifetime management.
// Not thread-safe; for ISR <-> main hand-over use SpscRing.
template <typename T, std::size_t N>
class FixedVector {
  static_assert(N > 0, "FixedVector capacity must be at least 1");
  static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value,
                "FixedVector holds trivial types only");

 public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;

  static constexpr std::size_t capacity() noexcept { return N; }
  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0; }
  constexpr bool full() const noexcept { return size_ == N; }

  // Appends a copy of `item`; false (nothing stored) when full.
  constexpr bool push_back(const T& item) noexcept {
    if (size_ == N) {
      return false;
    }
    items_[size_] = item;
    ++size_;
    return true;
  }

  // Drops the last item; false when already empty.
  constexpr bool pop_back() noexcept {
    if (size_ == 0) {
      return false;
    }
    --size_;
    return true;
  }

  constexpr void clear() noexcept { size_ = 0; }

  // Element access. Precondition: i < size() (unchecked, like std::vector).
  constexpr T& operator[](std::size_t i) noexcept { return items_[i]; }
  constexpr const T& operator[](std::size_t i) const noexcept { return items_[i]; }
  // Precondition: !empty().
  constexpr T& front() noexcept { return items_[0]; }
  constexpr const T& front() const noexcept { return items_[0]; }
  constexpr T& back() noexcept { return items_[size_ - 1]; }
  constexpr const T& back() const noexcept { return items_[size_ - 1]; }

  constexpr T* data() noexcept { return items_; }
  constexpr const T* data() const noexcept { return items_; }
  constexpr iterator begin() noexcept { return items_; }
  constexpr iterator end() noexcept { return items_ + size_; }
  constexpr const_iterator begin() const noexcept { return items_; }
  constexpr const_iterator end() const noexcept { return items_ + size_; }

 private:
  T items_[N]{};
  std::size_t size_ = 0;
};

}  // namespace o3sdk::core
