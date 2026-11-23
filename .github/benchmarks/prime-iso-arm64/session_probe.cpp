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

std::string_view checkpoint_counter(std::string_view diagnostic, std::string_view prefix) {
  const auto field = diagnostic.find(prefix);
  if (field == std::string_view::npos) return "unavailable";
  const auto start = field + prefix.size();
  const auto finish = diagnostic.find_first_not_of("0123456789", start);
  return finish == start ? "unavailable" : diagnostic.substr(start, finish - start);
}

void save_full_frame(const Session::VideoUpdate& frame, const std::filesystem::path& path,
                     std::uint64_t* last_signature = nullptr) {
  if (!frame.graphics || !frame.width || !frame.height) return;
  const auto& packet = *frame.graphics;
  const auto pixels = std::size_t(frame.width) * frame.height;
  if (packet.empty() || packet[0] > pixels) throw std::runtime_error("Invalid frame rectangle count");
  std::vector<std::uint8_t> rgb(pixels * 3), covered(pixels);
  std::size_t at = 1;
  for (unsigned i = 0; i < packet[0]; ++i) {
    if (packet.size() - at < 4) throw std::runtime_error("Incomplete frame rectangle");
    const auto x = packet[at++], y = packet[at++], width = packet[at++], height = packet[at++];
    if (!width || !height || x >= frame.width || y >= frame.height || width > frame.width - x ||
        height > frame.height - y || std::size_t(width) * height > packet.size() - at)
      throw std::runtime_error("Out-of-range frame rectangle");
    for (unsigned row = 0; row < height; ++row) {
      for (unsigned column = 0; column < width; ++column) {
        const auto pixel = packet[at++];
        const auto index = std::size_t(y + row) * frame.width + x + column;
        rgb[index * 3] = std::uint8_t(pixel >> 16);
        rgb[index * 3 + 1] = std::uint8_t(pixel >> 8);
        rgb[index * 3 + 2] = std::uint8_t(pixel);
        covered[index] = 1;
      }
    }
  }
  if (at != packet.size() || std::find(covered.begin(), covered.end(), 0) != covered.end())
    throw std::runtime_error("Requested full frame has missing RGB pixels");
  if (last_signature) {
    // Preserve meaningful changes to the upper screen even when the lower
    // part contains the animated loading indicator.
    auto signature = (std::uint64_t(frame.width) << 32) | frame.height;
    for (std::size_t pixel = 0; pixel < pixels * 2 / 3; pixel += 19)
      for (unsigned color = 0; color < 3; ++color)
        signature = (signature ^ rgb[pixel * 3 + color]) * UINT64_C(1099511628211);
    if (signature == *last_signature) return;
    *last_signature = signature;
  }
  const auto temporary = path.string() + ".tmp";
  std::ofstream out(temporary, std::ios::binary);
  out << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
  out.close();
  if (!out) throw std::runtime_error("Could not write the terminal RGB frame");
  std::filesystem::rename(temporary, path);
}

void save_wall_observation(Session& session, const std::filesystem::path& dir,
                           std::uint64_t target_ms, std::uint64_t elapsed_ns,
                           std::uint64_t cpu_ns, std::uint64_t engine_ns) {
  const auto label = "wall-" + std::to_string(target_ms);
  std::array<std::uint8_t, 4000> text{};
  try {
    session.retry_video();
    const auto frame = session.capture_video(text.data(), text.size());
    save_full_frame(frame, dir / (label + ".ppm"));
    if (frame.text_bytes) {
      std::ofstream out(dir / (label + ".txt"));
      for (std::size_t i = 0; i < frame.text_bytes / 2; ++i) {
        const auto ch = text[i * 2];
        out << (ch >= 32 && ch <= 126 ? char(ch) : ' ');
        if (i % 80 == 79) out << '\n';
      }
      if (!out) throw std::runtime_error("Could not write text screen snapshot");
    }
  } catch (const std::exception& error) {
    std::cerr << "PRIME_WALL_FRAME_ERROR " << target_ms << ' ' << error.what() << '\n';
  }
  const auto temporary = dir / (label + ".json.tmp");
  std::ofstream out(temporary);
  out << "{\"wall_target_ms\":" << target_ms
      << ",\"target_steps\":" << session.session_steps()
      << ",\"elapsed_ns\":" << elapsed_ns
      << ",\"process_cpu_ns\":" << cpu_ns
      << ",\"engine_window_ns\":" << engine_ns
      << ",\"process_peak_rss_bytes\":" << peak_rss_bytes()
      << ",\"session\":" << session.diagnostic(true) << "}\n";
  out.close();
  if (!out) throw std::runtime_error("Could not write wall observation");
  std::filesystem::rename(temporary, dir / (label + ".json"));
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
    for (const auto point : {9'000'000'000ULL, 9'250'000'000ULL, 9'500'000'000ULL})
      milestone_steps.push_back(point);
    const auto detailed_end = std::min<std::uint64_t>(target, 11'500'000'000ULL);
    for (std::uint64_t point = 9'550'000'000ULL; point <= detailed_end; point += 50'000'000ULL)
      milestone_steps.push_back(point);
    for (std::uint64_t point = 12'000'000'000ULL; point <= target; point += 500'000'000ULL)
      milestone_steps.push_back(point);
    constexpr std::array<std::uint64_t, 4> frame_checkpoints{
        11'500'000'000ULL, 20'000'000'000ULL, 30'000'000'000ULL, 37'000'000'000ULL};
    constexpr std::array<std::uint64_t, 3> wall_observations{720000, 1080000, 1320000};
    std::size_t next_milestone = 0, next_wall_observation = 0;
    std::array<std::uint8_t, 4000> text{};
    const auto start = Clock::now();
    const auto cpu_start = process_cpu_ns();
    std::uint64_t engine_elapsed_ns = 0, windows = 0, maintenance_calls = 0;
    auto status = Session::Step::Running;
    std::uint64_t next_sample = 0, next_maintenance = 0, next_snapshot = 0, next_video = 0;
    std::uint64_t video_signature = 0;
    bool reclaim_pending = false;
    while (status == Session::Step::Running && Clock::now() - start < run_time) {
      const auto due_at = Clock::now();
      const auto uptime = count_since<Milliseconds>(start, due_at);
      while (next_wall_observation < wall_observations.size() &&
             uptime >= wall_observations[next_wall_observation]) {
        const auto point = wall_observations[next_wall_observation++];
        save_wall_observation(session, run_dir, point, count_since<Nanoseconds>(start, due_at),
                              process_cpu_ns() - cpu_start, engine_elapsed_ns);
      }
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
        ++maintenance_calls;
      }
      if (uptime >= next_snapshot) {
        snapshots << "{\"elapsed_ms\":" << uptime << ",\"session\":" << session.diagnostic(false) << "}\n";
        if (!snapshots) throw std::runtime_error("Could not write a session snapshot");
        next_snapshot = uptime + (session.session_steps() < 11'500'000'000ULL ? 1000U : 30000U);
      }
      if (uptime >= 600000 && uptime >= next_video) {
        next_video = uptime + 10000;
        try {
          session.retry_video();
          save_full_frame(session.capture_video(text.data(), text.size()),
                          run_dir / "display-latest.ppm", &video_signature);
        } catch (const std::exception& error) {
          std::cerr << "PRIME_VIDEO_OBSERVATION_ERROR " << error.what() << '\n';
        }
      }
      const auto slice_start = Clock::now();
      status = session.step_engine_window(Milliseconds(8));
      const auto slice_end = Clock::now();
      const auto retired_after = session.session_steps();
      const auto slice_ns = count_since<Nanoseconds>(slice_start, slice_end);
      engine_elapsed_ns += slice_ns;
      ++windows;
      while (next_milestone < milestone_steps.size() && retired_after >= milestone_steps[next_milestone]) {
        const auto threshold = milestone_steps[next_milestone++];
        const auto elapsed_ns = count_since<Nanoseconds>(start, slice_end);
        const auto diagnostic = session.diagnostic(threshold >= 9'500'000'000ULL);
        if (std::find(frame_checkpoints.begin(), frame_checkpoints.end(), threshold) != frame_checkpoints.end()) {
          try {
            session.retry_video();
            save_full_frame(session.capture_video(text.data(), text.size()),
                            run_dir / ("screen-" + std::to_string(threshold) + ".ppm"));
          } catch (const std::exception& error) {
            std::cerr << "PRIME_MILESTONE_FRAME_ERROR " << threshold << ' ' << error.what() << '\n';
          }
        }
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
                  << " streaming_fill_bsp_steps=" << checkpoint_counter(diagnostic, "\"boot_streaming_sse_fill_steps\":")
                  << " streaming_fill_ap_steps=" << checkpoint_counter(diagnostic, "\"secondary_streaming_sse_fill_steps\":")
                  << " elapsed_ms=" << elapsed_ns / 1'000'000 << '\n' << std::flush;
      }
    }
    const auto elapsed_ms = count_since<Milliseconds>(start, Clock::now());
    milestones.close();
    if (!milestones) throw std::runtime_error("Could not finish the milestone log");
    snapshots.close();
    if (!snapshots) throw std::runtime_error("Could not finish the snapshot log");
    session.retry_video();
    const auto video = session.capture_video(text.data(), text.size());
    save_full_frame(video, run_dir / "terminal.ppm");
    std::ofstream report(run_dir / "session.json");
    report << session.diagnostic(true) << '\n';
    if (!report) throw std::runtime_error("Could not write session evidence");
    std::cout << "PRIME_NATIVE_REAL_ISO status=" << int(status) << " guest_steps="
              << session.session_steps() << " host_elapsed_ms=" << elapsed_ms << " slices=" << windows
              << " maintenance_calls=" << maintenance_calls
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
    std::ofstream completion(run_dir / "wall-completion.json");
    completion << "{\"elapsed_ms\":" << elapsed_ms << ",\"native_status\":" << int(status)
               << ",\"process_cpu_ns\":" << process_cpu_ns() - cpu_start
               << ",\"engine_window_ns\":" << engine_elapsed_ns
               << ",\"process_peak_rss_bytes\":" << peak_rss_bytes()
               << ",\"requested_wall_ms\":" << std::chrono::duration_cast<Milliseconds>(run_time).count()
               << "}\n";
    if (!completion) throw std::runtime_error("Could not write wall completion evidence");
    session.flush();
    return status == Session::Step::Fault ? 2 : 0;
  } catch (const std::exception& error) {
    std::cerr << "PRIME_NATIVE_REAL_ISO_ERROR " << error.what() << '\n';
    return 3;
  }
}
