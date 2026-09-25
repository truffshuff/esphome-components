#pragma once

#include "esphome/core/helpers.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace esphome::nimble_base {

/// Fixed-capacity event ring that hands NimBLE host-task callbacks to the
/// ESPHome main loop.
///
/// Every NimBLE callback runs on the host task, and nothing it touches in
/// ESPHome (API connections, listeners, components) is thread-safe, so the
/// callbacks only copy into a ring and the owning component's loop() handles
/// the event. This is the same producer/consumer split esp32_ble uses for
/// Bluedroid events and rp2 uses for BTstack IRQs.
///
/// - Storage is allocated once in init() via RAMAllocator (PSRAM first, then
///   internal) - producers never allocate.
/// - Producers (host task, occasionally the main loop itself for synthesized
///   completions) fill the tail slot in place under the lock. A full ring
///   drops the event and counts it; it never overwrites.
/// - The single consumer (main loop) peeks the head slot, handles it outside
///   the lock and then releases it. Producers only write the tail slot and a
///   full ring refuses pushes, so the slot being handled is never written
///   concurrently, and a handler may push to the same ring without deadlock.
template<typename T, uint16_t N> class EventRing {
 public:
  bool init() {
    if (this->slots_ != nullptr)
      return true;
    RAMAllocator<T> allocator;
    T *slots = allocator.allocate(N);
    if (slots == nullptr)
      return false;
    for (uint16_t i = 0; i < N; i++)
      new (&slots[i]) T();
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->slots_ = slots;
    return true;
  }

  /// Producer: fill the next free slot in place. False when full or not initialised.
  template<typename F> bool push(F &&fill) {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->slots_ == nullptr || this->count_ >= N) {
      this->dropped_++;
      return false;
    }
    fill(this->slots_[this->tail_]);
    this->tail_ = (this->tail_ + 1) % N;
    this->count_++;
    return true;
  }

  /// Consumer: the oldest event, or nullptr. Valid until pop_front().
  T *front() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    return this->count_ == 0 ? nullptr : &this->slots_[this->head_];
  }

  /// Consumer: release the slot returned by front().
  void pop_front() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->count_ == 0)
      return;
    this->head_ = (this->head_ + 1) % N;
    this->count_--;
  }

  /// Consumer: drop everything queued (e.g. events for a link that is gone).
  void clear() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    this->head_ = this->tail_;
    this->count_ = 0;
  }

  bool empty() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    return this->count_ == 0;
  }

  /// Events refused since the last call.
  uint32_t take_dropped() {
    std::lock_guard<std::mutex> lock(this->mutex_);
    uint32_t dropped = this->dropped_;
    this->dropped_ = 0;
    return dropped;
  }

 protected:
  T *slots_{nullptr};
  std::mutex mutex_;
  uint16_t head_{0};
  uint16_t tail_{0};
  uint16_t count_{0};
  uint32_t dropped_{0};
};

}  // namespace esphome::nimble_base
