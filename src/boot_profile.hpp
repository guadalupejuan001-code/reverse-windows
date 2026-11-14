#pragma once

#include "c06_motherboard.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <ostream>
#include <stdexcept>

namespace reverse_windows {

inline void write_ether_arm64(std::ostream& out, const cosmic_pc::Cpu& cpu) {
  const auto& stats = cpu.ether_arm64_stats();
  out << "{\"host_supported\":" << (cpu.ether_arm64_host() ? "true" : "false")
      << ",\"enabled\":" << (cpu.ether_arm64_blocks_enabled() ? "true" : "false")
      << ",\"translated\":" << stats.translated << ",\"cache_hits\":" << stats.hits
      << ",\"executed_blocks\":" << stats.blocks << ",\"guest_steps\":" << stats.steps
      << ",\"invalidated\":" << stats.invalidated << ",\"data_fallbacks\":" << stats.data_fallbacks
      << '}';
}

// Compare the same logical guest progress across runs. CPU and board
// state at each boundary help detect execution paths that diverge.
class BootProfile {
 public:
  static constexpr std::uint64_t kDefaultCheckpointSteps = 50'000'000;
  static constexpr std::uint64_t kMinCheckpointSteps = 4096;

  explicit BootProfile(std::uint64_t checkpoint_steps)
      : interval_(checkpoint_steps), next_(checkpoint_steps) {
    if (checkpoint_steps < kMinCheckpointSteps)
      throw std::invalid_argument("Boot profile checkpoint interval is below 4096 steps");
  }

  unsigned budget(unsigned requested, const cosmic_pc::PcMachine& pc) const noexcept {
    return pc.cpu.instructions < next_
        ? static_cast<unsigned>(std::min<std::uint64_t>(requested, next_ - pc.cpu.instructions))
        : 0;
  }

  void begin(const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board) noexcept {
    initial_ = last_ = capture(pc, board);
    next_ = initial_.steps + interval_;
    count_ = 0;
  }

  void after_run(const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board) noexcept {
    if (pc.cpu.instructions != next_) return;
    const auto current = capture(pc, board);
    const auto scalar = current.steps - last_.steps - (current.batched - last_.batched);
    points_[count_ % points_.size()] = {current, last_.steps, scalar, current.timing.since(last_.timing)};
    last_ = current;
    ++count_;
    next_ += interval_;
  }

  void write_json(std::ostream& out, const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board) const {
    out << "{\"checkpoint_interval_steps\":" << interval_ << ",\"checkpoints_seen\":" << count_
        << ",\"start\":";
    write(out, initial_);
    out << ",\"current\":";
    write(out, capture(pc, board));
    out << ",\"checkpoints\":[";
    const auto first = count_ > points_.size() ? count_ - points_.size() : 0;
    for (auto i = first; i < count_; ++i) {
      if (i != first) out << ',';
      const auto& p = points_[i % points_.size()];
      out << "{\"from_steps\":" << p.from << ",\"scalar_steps_in_interval\":" << p.scalar
          << ",\"state\":";
      write(out, p.state);
      out << ",\"timing_in_interval\":";
      p.timing.write_json(out);
      out << '}';
    }
    out << "]}";
  }

 private:
  struct State {
    std::uint64_t steps = 0, ip = 0, cr3 = 0, ticks = 0, irq = 0;
    std::uint64_t disk_reads = 0, ata_commands = 0, atapi_packets = 0;
    std::uint64_t page_walks = 0, page_hits = 0, instruction_page_hits = 0, batched = 0;
    std::uint64_t resident_bytes = 0, spill_reads = 0, spill_writes = 0, neon_steps = 0;
    std::uint64_t mode_changes = 0;
    std::uint32_t flags = 0;
    std::uint16_t cs = 0;
    const char* mode = "unknown";
    std::array<std::uint64_t,16> regs{};
    cosmic_pc::BootTiming::Counters timing;
  };
  struct Point {
    State state{};
    std::uint64_t from = 0, scalar = 0;
    cosmic_pc::BootTiming::Counters timing;
  };

  static State capture(const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board) noexcept {
    const auto& cpu = pc.cpu;
    const auto board_state = board.evidence();
    const auto& work = pc.boot_acceleration();
    State out;
    out.steps = cpu.instructions;
    out.ip = cpu.eip;
    out.cr3 = cpu.cr3;
    out.cs = cpu.segments[cosmic_pc::Cpu::CS];
    out.mode = cpu.mode_name();
    out.flags = cpu.eflags;
    out.regs = cpu.regs;
    out.ticks = board_state.ticks;
    out.irq = board_state.irq_delivered;
    out.disk_reads = pc.disk_reads;
    if (const auto* ata = board.attached_ata()) out.ata_commands = ata->counters().commands;
    if (const auto* optical = board.attached_atapi()) out.atapi_packets = optical->activity().packets;
    out.page_walks = cpu.page_walks;
    out.page_hits = cpu.page_translation_hits;
    out.instruction_page_hits = cpu.instruction_page_hits;
    const auto memory = cpu.memory.metrics();
    out.resident_bytes = memory.allocated_bytes;
    out.spill_reads = memory.backing_reads;
    out.spill_writes = memory.backing_writes;
    out.batched = work.rep_elements + work.shift_loop_steps + work.zero_loop_steps +
        work.scan_loop_steps + work.arm64_block_steps;
    out.mode_changes = pc.boot_mode_progress().changes;
#if defined(MUNDO_X_PC_ETHER_NEXT)
    out.neon_steps = cpu.ether_next_stats().native_instructions;
#endif
    if (const auto& timing = pc.boot_timing()) out.timing = timing->counters();
    return out;
  }

  static void write(std::ostream& out, const State& s) {
    out << "{\"steps\":" << s.steps << ",\"mode\":\"" << s.mode << "\",\"cs\":" << s.cs
        << ",\"ip_hex\":\"" << std::hex << std::setw(16) << std::setfill('0') << s.ip
        << "\",\"cr3_hex\":\"" << std::setw(16) << s.cr3
        << std::dec << std::setfill(' ') << "\",\"flags\":" << s.flags
        << ",\"board_ticks\":" << s.ticks << ",\"irq_delivered\":" << s.irq
        << ",\"bios_disk_reads\":" << s.disk_reads << ",\"ata_commands\":" << s.ata_commands
        << ",\"atapi_packets\":" << s.atapi_packets << ",\"page_walks\":" << s.page_walks
        << ",\"page_translation_hits\":" << s.page_hits
        << ",\"instruction_page_hits\":" << s.instruction_page_hits
        << ",\"ram_resident_bytes\":" << s.resident_bytes << ",\"spill_reads\":" << s.spill_reads
        << ",\"spill_writes\":" << s.spill_writes << ",\"batched_steps\":" << s.batched
        << ",\"neon_steps\":" << s.neon_steps
        << ",\"mode_changes\":" << s.mode_changes
        << ",\"run_ns\":" << s.timing.ns(cosmic_pc::BootTimePart::Run)
        << ",\"sampled_iterations\":" << s.timing.iterations << ",\"registers\":[";
    for (std::size_t i = 0; i < s.regs.size(); ++i) {
      if (i) out << ',';
      out << '"' << std::hex << s.regs[i] << std::dec << '"';
    }
    out << "]}";
  }

  const std::uint64_t interval_;
  std::uint64_t next_, count_ = 0;
  State initial_{}, last_{};
  std::array<Point,8> points_{};
};

} // namespace reverse_windows
