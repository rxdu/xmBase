/*
 * ring_buffer.hpp
 *
 * General-purpose, mutex-guarded circular buffer for the xMotion family. A
 * fixed-capacity (power-of-two) ring with single/burst read, peek, and an
 * optional overwrite-oldest-on-full policy. Thread-safe for concurrent
 * producer/consumer use via an internal mutex.
 *
 * This is the GENERAL container (byte/stream buffering, bounded queues). For the
 * hard-real-time logging hot path, the logging module has its own specialized
 * lock-free SPSC/MPSC rings (see logging/rt_logger*.hpp) — do not confuse the
 * two: use this one for general buffering, those for wait-free RT logging.
 *
 * Requirements:
 *  1. Capacity N must be a power of two.
 *  2. Usable capacity is N-1 (one slot is kept empty to distinguish full/empty).
 *
 * Indices are free-running and masked only on access, so occupancy/full/empty
 * math stays correct across wraps and across the overwrite path.
 *
 * Copyright (c) 2019-2026 Ruixiang Du (rdu)
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace xmotion {

template <typename T = uint8_t, std::size_t N = 1024>
class RingBuffer {
 public:
  explicit RingBuffer(bool enable_overwrite = true)
      : enable_overwrite_(enable_overwrite) {
    static_assert(
        (N != 0) && ((N & (N - 1)) == 0),
        "Size of ring buffer has to be 2^n, where n is a positive integer");
    size_mask_ = N - 1;
    write_index_ = 0;
    read_index_ = 0;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    write_index_ = 0;
    read_index_ = 0;
  }

  // --- size queries -------------------------------------------------------
  bool IsEmpty() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return (read_index_ == write_index_);
  }

  bool IsFull() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return ((write_index_ + 1) & size_mask_) == (read_index_ & size_mask_);
  }

  std::size_t GetFreeSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return N - 1 - ((write_index_ - read_index_) & size_mask_);
  }

  std::size_t GetOccupiedSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return (write_index_ - read_index_) & size_mask_;
  }

  std::array<T, N> GetBuffer() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_;
  }

  // --- burst read/write ---------------------------------------------------
  std::size_t Read(std::vector<T>& data, std::size_t btr) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Clamp to the caller's buffer so a too-small `data` can never overflow.
    if (btr > data.size()) btr = data.size();
    for (std::size_t i = 0; i < btr; ++i) {
      if (read_index_ == write_index_) return i;
      data[i] = buffer_[read_index_++ & size_mask_];
    }
    return btr;
  }

  // Copy up to btp not-yet-read elements without consuming them. Atomic: the
  // whole peek happens under a single lock, so it cannot tear against a
  // concurrent Write.
  std::size_t Peek(std::vector<T>& data, std::size_t btp) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    const std::size_t available = (write_index_ - read_index_) & size_mask_;
    const std::size_t n = std::min({btp, data.size(), available});
    for (std::size_t i = 0; i < n; ++i)
      data[i] = buffer_[(read_index_ + i) & size_mask_];
    return n;
  }

  std::size_t Write(const std::vector<T>& new_data, std::size_t btw) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (btw > new_data.size()) btw = new_data.size();  // never read OOB
    for (std::size_t i = 0; i < btw; ++i) {
      if (((write_index_ + 1) & size_mask_) == (read_index_ & size_mask_)) {
        if (!enable_overwrite_) return i;  // full, overwrite disabled
        ++read_index_;  // free-running; mask only on access (see header note)
      }
      buffer_[(write_index_++) & size_mask_] = new_data[i];
    }
    return btw;
  }

  // --- single read/write --------------------------------------------------
  std::size_t Read(T& data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (read_index_ == write_index_) return 0;  // empty
    data = buffer_[read_index_++ & size_mask_];
    return 1;
  }

  std::size_t PeekAt(T& data, std::size_t n) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Parenthesized: `>=` binds tighter than `&`, so `n >= (w - r) & mask` would
    // compare the unmasked difference and AND the bool with the mask.
    if (n >= ((write_index_ - read_index_) & size_mask_)) return 0;
    data = buffer_[(read_index_ + n) & size_mask_];
    return 1;
  }

  std::size_t Write(const T& new_data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (((write_index_ + 1) & size_mask_) == (read_index_ & size_mask_)) {
      if (!enable_overwrite_) return 0;  // full, overwrite disabled
      ++read_index_;  // free-running; mask only on access
    }
    buffer_[(write_index_++) & size_mask_] = new_data;
    return 1;
  }

 private:
  std::array<T, N> buffer_;
  std::size_t size_mask_;
  std::size_t write_index_ = 0;
  std::size_t read_index_ = 0;
  bool enable_overwrite_ = false;

  mutable std::mutex buffer_mutex_;
};

}  // namespace xmotion
