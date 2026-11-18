#pragma once

#include "pc_machine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace reverse_windows {

// An owner identifies one Prime vCPU. Each job has its own architectural
// x86 state and RAM. It never runs on an architectural Windows CPU instance.
class EtherX86Sessions {
 public:
  static constexpr unsigned kCodeAddress = 0x7000, kDataAddress = 0x9000;
  static constexpr unsigned kMappedBytes = 2U << 20;

  struct Job {
    Job() { registers[cosmic_pc::Cpu::ESP] = kCodeAddress; }
    std::vector<std::uint8_t> code, data;
    std::array<std::uint64_t, 16> registers{};
    unsigned step_limit = 0;
  };

  struct Result {
    std::array<std::uint64_t, 16> registers{};
    std::vector<std::uint8_t> data;
    std::uint64_t steps = 0;
    std::uint64_t projected_steps = 0;
    std::uint64_t rip = 0;
    std::uint32_t flags = 0;
    std::string fault;
    bool halted = false, budget_exhausted = false, cancelled = false;
  };

  struct Ticket {
    std::uint64_t id;
    std::future<Result> result;
  };

  struct Stats {
    std::uint64_t submitted = 0, completed = 0;
    unsigned in_flight = 0, peak_in_flight = 0;
  };

  EtherX86Sessions(unsigned owners, unsigned workers, unsigned max_per_owner)
      : owners_(owners), workers_(workers), max_per_owner_(max_per_owner) {
    for (auto& worker : workers_) {
      auto* slot = &worker;
      try {
        worker.thread = std::thread([this, slot] { run_worker(*slot); });
      } catch (const std::system_error&) {
        break;
      } catch (const std::bad_alloc&) {
        break;
      }
      ++worker_count_;
    }
  }

  ~EtherX86Sessions() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_.store(true, std::memory_order_relaxed);
    }
    for (auto& worker : workers_) worker.ready.notify_one();
    for (auto& worker : workers_)
      if (worker.thread.joinable()) worker.thread.join();
  }

  unsigned worker_count() const noexcept { return worker_count_; }

  // Admission uses an idle worker directly. An unavailable slot is reported
  // to the caller immediately and does not stall any accepted session.
  std::optional<Ticket> submit(unsigned owner, Job job) {
    if (owner >= owners_.size() || !job.step_limit || job.code.empty() ||
        job.code.size() > kDataAddress - kCodeAddress || job.data.size() > kMappedBytes - kDataAddress)
      return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& stats = owners_[owner];
    if (stats.in_flight >= max_per_owner_) return std::nullopt;
    auto end = workers_.begin() + worker_count_;
    auto worker = std::find_if(workers_.begin(), end, [](const Worker& slot) { return !slot.busy; });
    if (worker == end) return std::nullopt;
    std::promise<Result> promise;
    auto result = promise.get_future();
    worker->request.emplace(Request{owner, std::move(job), std::move(promise)});
    worker->busy = true;
    ++stats.submitted;
    ++stats.in_flight;
    stats.peak_in_flight = std::max(stats.peak_in_flight, stats.in_flight);
    worker->ready.notify_one();
    return Ticket{++next_id_, std::move(result)};
  }

  Stats stats(unsigned owner) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owner < owners_.size() ? owners_[owner] : Stats{};
  }

 private:
  struct Request {
    unsigned owner;
    Job job;
    std::promise<Result> completion;
  };

  struct Worker {
    std::thread thread;
    std::condition_variable ready;
    std::optional<Request> request;
    bool busy = false;
  };

  Result execute(Job job) const {
    Result result;
    if (stopping_.load(std::memory_order_relaxed)) {
      result.cancelled = true;
      return result;
    }
    cosmic_pc::PcMachine pc(4U << 20);
    pc.enable_boot_acceleration();
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    if (cpu.fault || !cpu.is_long_mode() || !cpu.halted)
      throw std::runtime_error("Could not initialize the private x86 execution context");

    cpu.halted = false;
    cpu.eflags = 2;
    cpu.eip = kCodeAddress;
    cpu.regs = job.registers;
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    cpu.memory.write_block(kCodeAddress, job.code.data(), job.code.size());
    cpu.write64(kDataAddress, 0);
    if (!job.data.empty()) cpu.memory.write_block(kDataAddress, job.data.data(), job.data.size());
    const auto first_step = cpu.instructions;
    using Clock = cosmic_pc::PcMachine::RunClock;
    while (!cpu.halted && !cpu.fault && cpu.instructions - first_step < job.step_limit) {
      if (stopping_.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        break;
      }
      const auto previous = cpu.instructions;
      const auto remaining = static_cast<unsigned>(job.step_limit - (previous - first_step));
      // Give proven x86 projections the entire budget. The time deadline
      // lets an ordinary interpreter workload return for cancellation.
      const auto deadline = Clock::now() + std::chrono::milliseconds(2);
      pc.run(remaining, deadline);
      if (cpu.instructions == previous && (pc.waiting_key || !pc.host_error.empty() ||
          Clock::now() < deadline)) break;
    }
    if (cpu.fault) result.fault = cpu.fault->describe();
    else if (!pc.host_error.empty()) result.fault = pc.host_error;
    else if (pc.waiting_key) result.fault = "Private x86 job requested BIOS keyboard input";
    result.registers = cpu.regs;
    result.steps = cpu.instructions - first_step;
    result.projected_steps = cpu.ether_arm64_stats().state_vector_steps;
    result.rip = cpu.eip;
    result.flags = cpu.eflags;
    result.halted = cpu.halted;
    result.budget_exhausted = !result.halted && result.fault.empty() &&
        !result.cancelled && result.steps == job.step_limit;
    result.data = std::move(job.data);
    if (!result.data.empty())
      cpu.memory.read_block(kDataAddress, result.data.data(), result.data.size());
    return result;
  }

  void run_worker(Worker& worker) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      worker.ready.wait(lock, [&] { return worker.request.has_value() || stopping_.load(std::memory_order_relaxed); });
      if (!worker.request) return;
      auto request = std::move(*worker.request);
      worker.request.reset();
      lock.unlock();
      Result result;
      std::exception_ptr failure;
      try {
        result = execute(std::move(request.job));
      } catch (...) {
        failure = std::current_exception();
      }
      lock.lock();
      auto& stats = owners_[request.owner];
      --stats.in_flight;
      ++stats.completed;
      worker.busy = false;
      if (failure) request.completion.set_exception(failure);
      else request.completion.set_value(std::move(result));
    }
  }

  std::vector<Stats> owners_;
  std::vector<Worker> workers_;
  mutable std::mutex mutex_;
  std::atomic<bool> stopping_{false};
  unsigned max_per_owner_, worker_count_ = 0;
  std::uint64_t next_id_ = 0;
};

} // namespace reverse_windows
