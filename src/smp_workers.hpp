#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace reverse_windows {

// One synchronous dispatch at a time. The VM thread participates and waits
// for all workers before it touches the machine again.
class SmpWorkers {
 public:
  explicit SmpWorkers(unsigned maximum) {
    const auto total = std::min(maximum, std::max(1U, std::thread::hardware_concurrency()));
    try {
      for (unsigned i = 1; i < total; ++i)
        workers_.emplace_back([this] { worker(); });
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~SmpWorkers() { shutdown(); }

  unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()) + 1; }

  void run(unsigned count, const std::function<void(unsigned)>& task) {
    if (count <= 1 || workers_.empty()) {
      for (unsigned i = 0; i < count; ++i) task(i);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &task;
      count_ = count;
      next_.store(0, std::memory_order_relaxed);
      remaining_ = workers_.size();
      failure_ = {};
      ++generation_;
    }
    ready_.notify_all();
    execute(count, task);
    std::unique_lock<std::mutex> lock(mutex_);
    finished_.wait(lock, [this] { return remaining_ == 0; });
    task_ = nullptr;
    if (failure_) std::rethrow_exception(failure_);
  }

 private:
  void execute(unsigned count, const std::function<void(unsigned)>& task) {
    try {
      for (;;) {
        const auto slot = next_.fetch_add(1, std::memory_order_relaxed);
        if (slot >= count) return;
        task(slot);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_) failure_ = std::current_exception();
    }
  }
  void worker() {
    std::uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      ready_.wait(lock, [&] { return stopping_ || generation_ != seen; });
      if (stopping_) return;
      seen = generation_;
      const auto* task = task_;
      const auto count = count_;
      lock.unlock();
      execute(count, *task);
      lock.lock();
      if (--remaining_ == 0) finished_.notify_one();
    }
  }
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  std::mutex mutex_;
  std::condition_variable ready_, finished_;
  std::vector<std::thread> workers_;
  std::atomic<unsigned> next_{0};
  const std::function<void(unsigned)>* task_ = nullptr;
  unsigned count_ = 0;
  std::size_t remaining_ = 0;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::exception_ptr failure_;
};

} // namespace reverse_windows
