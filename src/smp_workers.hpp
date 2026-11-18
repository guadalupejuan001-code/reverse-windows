#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace reverse_windows {

// Each job owns one host thread, including the participating VM thread.
// Workers share a start and a join at the boundary of a dispatch.
class SmpWorkers {
 public:
  explicit SmpWorkers(unsigned maximum) : maximum_(maximum) {}
  ~SmpWorkers() { shutdown(); }

  unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()) + 1; }

  unsigned prepare(unsigned count) {
    const auto total = std::min(count, maximum_);
    try {
      while (size() < total) {
        const auto index = size();
        workers_.emplace_back([this, index] { worker(index); });
      }
    } catch (const std::system_error&) {
      maximum_ = size();
    }
    return std::min(count, size());
  }

  void run(unsigned count, const std::function<void(unsigned)>& task) {
    if (count > size()) throw std::logic_error("SMP dispatch requires one worker per job");
    if (count < 2) {
      if (count) task(0);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &task;
      participants_ = count;
      remaining_ = count - 1;
      failure_ = {};
      ++generation_;
    }
    ready_.notify_all();
    execute(0, task);
    std::unique_lock<std::mutex> lock(mutex_);
    finished_.wait(lock, [this] { return remaining_ == 0; });
    task_ = nullptr;
    if (failure_) std::rethrow_exception(failure_);
  }

 private:
  void execute(unsigned index, const std::function<void(unsigned)>& task) {
    try {
      task(index);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_) failure_ = std::current_exception();
    }
  }
  void worker(unsigned index) {
    std::uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      ready_.wait(lock, [&] { return stopping_ || (generation_ != seen && index < participants_); });
      if (stopping_) return;
      seen = generation_;
      const auto* task = task_;
      lock.unlock();
      execute(index, *task);
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
  const std::function<void(unsigned)>* task_ = nullptr;
  unsigned maximum_, participants_ = 0, remaining_ = 0;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::exception_ptr failure_;
};

} // namespace reverse_windows
