#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#ifdef __ANDROID__
#include <pthread.h>
#include <unistd.h>
#include <vector>
#endif

namespace reverse_windows {

// The caller participates. A lane can execute several independent jobs.
// Nested work can fall back to the caller without dispatching a second pool.
class SmpWorkers {
 public:
  using Task = std::function<void(unsigned lane, unsigned lanes)>;

  explicit SmpWorkers(unsigned maximum) : maximum_(std::max(1U, maximum)) {}
  ~SmpWorkers() { shutdown(); }

  unsigned size() const noexcept { return live_lanes_.load(std::memory_order_acquire); }

#ifdef __ANDROID__
  // The VM thread runs lane zero. Only include native lanes actually created.
  std::vector<int> android_thread_ids() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> ids{gettid()};
    for (auto& worker : workers_) {
      const int tid = pthread_gettid_np(worker.thread.native_handle());
      if (tid > 0) ids.push_back(tid);
    }
    return ids;
  }
#endif

  // Returns zero without executing the task when another dispatch owns the
  // pool or there is only enough capacity for serial work.
  unsigned try_run(unsigned wanted, const Task& task) {
    if (wanted < 2 || busy_.exchange(true, std::memory_order_acquire)) return 0;
    struct Release {
      std::atomic<bool>& busy;
      ~Release() { busy.store(false, std::memory_order_release); }
    } release{busy_};
    const auto count = prepare(wanted);
    if (count < 2) return 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &task;
      participants_ = count;
      remaining_ = count - 1;
      failure_ = {};
      ++generation_;
    }
    for (unsigned index = 1; index < count; ++index) workers_[index - 1].ready.notify_one();
    execute(0, count, task);
    std::unique_lock<std::mutex> lock(mutex_);
    finished_.wait(lock, [this] { return remaining_ == 0; });
    task_ = nullptr;
    if (failure_) std::rethrow_exception(failure_);
    return count;
  }

 private:
  struct Worker {
    std::condition_variable ready;
    std::thread thread;
  };

  unsigned prepare(unsigned wanted) {
    const auto total = std::min(wanted, maximum_);
    if (size() >= total) return total;
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      while (size() < total) {
        const auto index = size();
        auto& lane = workers_.emplace_back();
        try {
          lane.thread = std::thread([this, slot = &lane, index] { run_worker(*slot, index); });
        } catch (...) {
          workers_.pop_back();
          throw;
        }
        live_lanes_.store(index + 1, std::memory_order_release);
      }
    } catch (const std::system_error&) {
      maximum_ = size();
    } catch (const std::bad_alloc&) {
      maximum_ = size();
    }
    return std::min(wanted, size());
  }

  void execute(unsigned index, unsigned count, const Task& task) {
    try {
      task(index, count);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_) failure_ = std::current_exception();
    }
  }
  void run_worker(Worker& lane, unsigned index) {
    std::uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      lane.ready.wait(lock, [&] { return stopping_ || (generation_ != seen && index < participants_); });
      if (stopping_) return;
      seen = generation_;
      const auto* task = task_;
      const auto count = participants_;
      lock.unlock();
      execute(index, count, *task);
      lock.lock();
      if (--remaining_ == 0) finished_.notify_one();
    }
  }
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    for (auto& worker : workers_) worker.ready.notify_one();
    for (auto& worker : workers_) worker.thread.join();
  }

  std::mutex mutex_;
  std::condition_variable finished_;
  std::deque<Worker> workers_;
  const Task* task_ = nullptr;
  std::atomic<bool> busy_{false};
  std::atomic<unsigned> live_lanes_{1};
  unsigned maximum_, participants_ = 0, remaining_ = 0;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
  std::exception_ptr failure_;
};

} // namespace reverse_windows
