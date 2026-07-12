/*
 * @file thread_safe_queue.hpp
 * @date 10/10/24
 * @brief
 *
 * DEPRECATED-FOR-REMOVAL at xmBase 0.6.0 (ADR 0007): superseded by the
 * verified primitives in xmbase/concurrency/ (SpscQueue for SPSC rings;
 * callers needing a blocking multi-producer queue keep a local copy in the
 * owning component). New code must not add includes of this header.
 *
 * @copyright Copyright (c) 2024 Ruixiang Du (rdu)
 */
#ifndef XMBASE_CONTAINER_THREAD_SAFE_QUEUE_HPP
#define XMBASE_CONTAINER_THREAD_SAFE_QUEUE_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

namespace xmotion {
template <typename T, int N = 64>
class ThreadSafeQueue {
 public:
  ThreadSafeQueue() = default;
  ~ThreadSafeQueue() = default;

  // do not allow copy
  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  // allow move
  ThreadSafeQueue(ThreadSafeQueue&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    queue_ = std::move(other.queue_);
  }

  ThreadSafeQueue& operator=(ThreadSafeQueue&& other) noexcept {
    if (this != &other) {
      std::scoped_lock lock(mutex_, other.mutex_);
      queue_ = std::move(other.queue_);
    }
    return *this;
  }

  // push data into the queue
  void Push(const T& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() == N) queue_.pop();
    queue_.push(data);
    condition_.notify_one();  // Notify one waiting thread
  }

  // pop data from the queue (blocking)
  T Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !queue_.empty(); });
    T data = queue_.front();
    queue_.pop();
    return data;
  }

  // try to pop data from the queue (non-blocking)
  bool TryPop(T& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    data = queue_.front();
    queue_.pop();
    return true;
  }

 private:
  std::queue<T> queue_;
  std::mutex mutex_;
  std::condition_variable condition_;
};
}  // namespace xmotion

#endif  // XMBASE_CONTAINER_THREAD_SAFE_QUEUE_HPP
