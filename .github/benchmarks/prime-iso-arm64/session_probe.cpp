#include "android_boot_session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

using Session = reverse_windows::AndroidBootSession;
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Nanoseconds = std::chrono::nanoseconds;

struct GuestSlice {
  std::uint64_t elapsed_ms, duration_ns, guest_steps, retired;
};
struct MaintenanceSlice {
  std::uint64_t elapsed_ms, duration_ns, next_delay_ms;
  bool sampled_pressure, pending_reclaim, preparing;
};

template <typename Unit>
std::uint64_t count_since(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<Unit>(end - start).count();
}

std::uint64_t process_cpu_ns() {
  timespec stamp{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &stamp) != 0)
    throw std::runtime_error("Could not read process CPU time");
  return std::uint64_t(stamp.tv_sec) * 1'000'000'000 + stamp.tv_nsec;
}

std::uint64_t peak_rss_bytes() {
  struct rusage resources {};
  if (getrusage(RUSAGE_SELF, &resources) != 0)
    throw std::runtime_error("Could not read process RSS");
  return std::uint64_t(resources.ru_maxrss) * 1024;
}

std::string_view checkpoint_field(std::string_view diagnostic, std::string_view prefix) {
  const auto field = diagnostic.find(prefix);
  if (field == std::string_view::npos) return "unavailable";
  const auto start = field + prefix.size();
  const auto finish = diagnostic.find('"', start);
  return finish == std::string_view::npos ? "unavailable" : diagnostic.substr(start, finish - start);
}

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: session_probe ISO OUTPUT_DIR RETIRED_TARGET TIMEOUT_S AVAILABLE_MIB THRESHOLD_MIB\n";
    return 1;
  }
  const std::filesystem::path run_dir(argv[2]);
  const auto target = std::stoull(argv[3]);
  const auto run_time = std::chrono::seconds(std::stoull(argv[4]));
  try {
    const auto available_mib = std::stoull(argv[5]);
    const auto threshold_mib = std::stoull(argv[6]);
    if (!available_mib || !threshold_mib || available_mib > (1ULL << 30) || threshold_mib > (1ULL << 30))
      throw std::invalid_argument("Invalid host memory inputs for the pressure sample");
    const auto available = available_mib << 20;
    const auto threshold = threshold_mib << 20;
    if (!std::filesystem::create_directory(run_dir))
      throw std::runtime_error("The session output directory already exists");
    const auto ram_cache = run_dir / "ram";
    if (!std::filesystem::create_directory(ram_cache))
      throw std::runtime_error("Could not create temporary RAM backing directory");
    const auto ram = Session::ram_profile(available, threshold, false, true);
    const int fd = ::open(argv[1], O_CLOEXEC | O_RDONLY);
    if (fd < 0) throw std::runtime_error("Could not open the read-only optical image");
    struct Close { int fd; ~Close() { ::close(fd); } } optical{fd};
    Session session(run_dir / "system.rqvd", 64, Session::BootSource::Iso, fd, 5550497792ULL,
        ram, ram_cache, run_dir / "vram.rqvd", {}, Session::kPrimeCpuTopology, true, Session::kPrimeCpuClock);
    std::ofstream snapshots(run_dir / "snapshots.jsonl");
    if (!snapshots) throw std::runtime_error("Could not open the snapshot log");
    std::ofstream milestones(run_dir / "milestones.jsonl");
    if (!milestones) throw std::runtime_error("Could not open the milestone log");
    std::vector<std::uint64_t> milestone_steps{350'000'000, 400'000'000, 700'000'000};
    for (std::uint64_t point = 750'000'000; point <= 1'000'000'000; point += 50'000'000)
      milestone_steps.push_back(point);
    for (std::uint64_t point = 1'500'000'000; point <= 8'000'000'000; point += 500'000'000)
      milestone_steps.push_back(point);
    for (std::uint64_t point = 8'200'000'000; point <= 8'650'000'000; point += 50'000'000)
      milestone_steps.push_back(point);
    std::size_t next_milestone = 0;
    std::vector<GuestSlice> windows;
    windows.reserve(60000);
    std::vector<MaintenanceSlice> maintenance;
    maintenance.reserve(4096);
    const auto start = Clock::now();
    const auto cpu_start = process_cpu_ns();
    std::uint64_t engine_elapsed_ns = 0;
    auto status = Session::Step::Running;
    std::uint64_t next_sample = 0, next_maintenance = 0, next_snapshot = 0;
    bool reclaim_pending = false;
    while (status == Session::Step::Running && session.session_steps() < target &&
           Clock::now() - start < run_time) {
      const auto due_at = Clock::now();
      const auto uptime = count_since<Milliseconds>(start, due_at);
      if (uptime >= next_maintenance) {
        const bool sampled = uptime >= next_sample;
        if (sampled) {
          session.sample_ram_pressure(available, threshold, false);
          reclaim_pending = session.maintain_ram_budget();
          next_sample = uptime + 10000;
        } else if (reclaim_pending) {
          reclaim_pending = session.maintain_ram_budget();
        }
        bool preparing = false;
        if (reclaim_pending) {
          next_sample = std::min(next_sample, uptime + 1000);
          next_maintenance = count_since<Milliseconds>(start, Clock::now()) + 16;
        } else {
          preparing = session.prepare_ram_reclaim();
          next_maintenance = std::min(next_sample, count_since<Milliseconds>(start, Clock::now()) +
                                                    (preparing ? 50U : 250U));
        }
        maintenance.push_back({uptime, count_since<Nanoseconds>(due_at, Clock::now()),
                               next_maintenance - std::min(next_maintenance, uptime),
                               sampled, reclaim_pending, preparing});
      }
      if (uptime >= next_snapshot) {
        snapshots << "{\"elapsed_ms\":" << uptime << ",\"session\":" << session.diagnostic(false) << "}\n";
        if (!snapshots) throw std::runtime_error("Could not write a session snapshot");
        next_snapshot = uptime + 1000;
      }
      const auto retired_before = session.session_steps();
      const auto slice_start = Clock::now();
      status = session.step_engine_window(Milliseconds(8));
      const auto slice_end = Clock::now();
      const auto retired_after = session.session_steps();
      const auto slice_ns = count_since<Nanoseconds>(slice_start, slice_end);
      engine_elapsed_ns += slice_ns;
      windows.push_back({count_since<Milliseconds>(start, slice_end), slice_ns, retired_after,
                         retired_after - retired_before});
      while (next_milestone < milestone_steps.size() && retired_after >= milestone_steps[next_milestone]) {
        const auto threshold = milestone_steps[next_milestone++];
        const auto elapsed_ns = count_since<Nanoseconds>(start, slice_end);
        const auto diagnostic = session.diagnostic(false);
        milestones << "{\"target_steps\":" << threshold
                   << ",\"elapsed_ns\":" << elapsed_ns
                   << ",\"process_cpu_ns\":" << process_cpu_ns() - cpu_start
                   << ",\"engine_window_ns\":" << engine_elapsed_ns
                   << ",\"process_peak_rss_bytes\":" << peak_rss_bytes()
                   << ",\"session\":" << diagnostic << "}\n";
        milestones.flush();
        if (!milestones) throw std::runtime_error("Could not write the milestone log");
        std::cout << "PRIME_ISO_PROGRESS threshold=" << threshold << " guest_steps=" << retired_after
                  << " mode=" << checkpoint_field(diagnostic, "\"cpu_mode\":\"")
                  << " rip=" << checkpoint_field(diagnostic, "\"rip_hex\":\"")
                  << " elapsed_ms=" << elapsed_ns / 1'000'000 << '\n' << std::flush;
      }
    }
    const auto elapsed_ms = count_since<Milliseconds>(start, Clock::now());
    milestones.close();
    if (!milestones) throw std::runtime_error("Could not finish the milestone log");
    snapshots.close();
    if (!snapshots) throw std::runtime_error("Could not finish the snapshot log");
    std::ofstream window_log(run_dir / "windows.csv");
    window_log << "elapsed_ms,duration_ns,guest_steps,retired\n";
    for (const auto& row : windows)
      window_log << row.elapsed_ms << ',' << row.duration_ns << ',' << row.guest_steps << ',' << row.retired << '\n';
    window_log.close();
    if (!window_log) throw std::runtime_error("Could not write the guest slice timing log");
    std::ofstream maintenance_log(run_dir / "maintenance.csv");
    maintenance_log << "elapsed_ms,duration_ns,next_delay_ms,sampled_pressure,pending_reclaim,preparing\n";
    for (const auto& row : maintenance)
      maintenance_log << row.elapsed_ms << ',' << row.duration_ns << ',' << row.next_delay_ms << ','
                      << row.sampled_pressure << ',' << row.pending_reclaim << ',' << row.preparing << '\n';
    maintenance_log.close();
    if (!maintenance_log) throw std::runtime_error("Could not write the maintenance timing log");
    std::array<std::uint8_t,4000> text{};
    const auto video = session.capture_video(text.data(), text.size());
    std::ofstream report(run_dir / "session.json");
    report << session.diagnostic(true) << '\n';
    if (!report) throw std::runtime_error("Could not write session evidence");
    std::cout << "PRIME_NATIVE_REAL_ISO status=" << int(status) << " guest_steps="
              << session.session_steps() << " host_elapsed_ms=" << elapsed_ms << " slices=" << windows.size()
              << " maintenance_calls=" << maintenance.size()
              << " available_mib=" << available_mib << " threshold_mib=" << threshold_mib
              << " initial_budget_bytes=" << ram.backing_bytes
              << " screen=" << video.width << 'x' << video.height << " text_bytes=" << video.text_bytes
              << " graphics_words=" << (video.graphics ? video.graphics->size() : 0) << '\n';
    if (video.text_bytes) {
      std::ofstream output(run_dir / "text-screen.txt");
      for (unsigned row=0; row<25; ++row) {
        std::string line;
        for (unsigned col=0; col<80; ++col) {
          const auto ch = text[(row*80+col)*2];
          line += ch >= 32 && ch <= 126 ? char(ch) : ' ';
        }
        const auto last = line.find_last_not_of(' ');
        if (last != std::string::npos) output << line.substr(0,last+1);
        output << '\n';
      }
      if (!output) throw std::runtime_error("Could not write text screen evidence");
    }
    session.flush();
    return status == Session::Step::Fault ? 2 : 0;
  } catch (const std::exception& error) {
    std::cerr << "PRIME_NATIVE_REAL_ISO_ERROR " << error.what() << '\n';
    return 3;
  }
}
