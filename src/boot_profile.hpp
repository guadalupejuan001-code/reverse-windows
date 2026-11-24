#pragma once

#include "c06_motherboard.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <optional>
#include <ostream>
#include <stdexcept>

namespace reverse_windows {

inline void write_tsc_access(std::ostream& out, const cosmic_pc::Cpu::TscAccessStats& access) {
  out << "{\"rdtsc\":" << access.rdtsc
      << ",\"rdmsr_tsc\":" << access.read_tsc
      << ",\"wrmsr_tsc\":" << access.write_tsc
      << ",\"rdmsr_tsc_adjust\":" << access.read_adjust
      << ",\"wrmsr_tsc_adjust\":" << access.write_adjust << '}';
}

inline void write_ether_arm64(std::ostream& out, const cosmic_pc::Cpu& cpu) {
  const auto& stats = cpu.ether_arm64_stats();
  const auto space = cpu.ether_arm64_virtual_stats();
  out << "{\"host_supported\":" << (cpu.ether_arm64_host() ? "true" : "false")
      << ",\"enabled\":" << (cpu.ether_arm64_blocks_enabled() ? "true" : "false")
      << ",\"translated\":" << stats.translated << ",\"cache_hits\":" << stats.hits
      << ",\"executed_blocks\":" << stats.blocks << ",\"guest_steps\":" << stats.steps
      << ",\"expanded_blocks\":" << stats.expanded_blocks
      << ",\"largest_block_instructions\":" << stats.largest_block_instructions
      << ",\"block_instruction_limit\":" << cosmic_pc::ether_arm64::LongBlock::kMaxInstructions
      << ",\"ordinary_block_instruction_limit\":" << cosmic_pc::ether_arm64::Block::kMaxInstructions
      << ",\"long_block_instruction_limit\":" << cosmic_pc::ether_arm64::LongBlock::kMaxInstructions
      << ",\"long_blocks_executed\":" << stats.long_blocks_executed
      << ",\"largest_long_block_instructions\":" << stats.largest_long_block_instructions
      << ",\"long_block_steps\":" << stats.long_block_steps
      << ",\"long_block_compile_attempts\":" << stats.long_block_compile_attempts
      << ",\"long_block_rejections\":" << stats.long_block_rejections
      << ",\"long_block_cache_hits\":" << stats.long_block_cache_hits
      << ",\"long_block_deadline_deferrals\":" << stats.long_block_deadline_deferrals
      << ",\"protected32_guest_steps\":" << stats.protected32_steps
      << ",\"invalidated\":" << stats.invalidated << ",\"data_fallbacks\":" << stats.data_fallbacks
      << ",\"compiled_evictions\":" << stats.compiled_evictions
      << ",\"native_compile_ns\":" << stats.native_compile_ns
      << ",\"code_version_hits\":" << stats.code_version_hits
      << ",\"code_byte_checks\":" << stats.code_byte_checks
      << ",\"code_bytes_read\":" << stats.code_bytes_read
      << ",\"staged_ram_blocks\":" << stats.staged_ram_blocks
      << ",\"staged_ram_steps\":" << stats.staged_ram_steps
      << ",\"coalesced_ram_writes\":" << stats.coalesced_ram_writes
      << ",\"ram_add_guest_steps\":" << stats.ram_add_steps
      << ",\"on_bus_register_steps\":" << stats.on_bus_register_steps
      << ",\"off_bus_register_steps\":" << stats.off_bus_register_steps
      << ",\"register_bus_reacquisitions\":" << stats.register_bus_reacquisitions
      << ",\"register_bus_reacquire_samples\":" << stats.register_bus_reacquire_samples
      << ",\"register_bus_reacquire_contentions\":" << stats.register_bus_reacquire_contentions
      << ",\"register_bus_reacquire_wait_ns\":" << stats.register_bus_reacquire_wait_ns
      << ",\"resident_read_hits\":" << stats.resident_read_hits
      << ",\"ram_read_callbacks\":" << stats.ram_read_callbacks
      << ",\"repeated_loop_dispatches\":" << stats.repeated_loop_dispatches
      << ",\"repeated_loop_steps\":" << stats.repeated_loop_steps
      << ",\"state_vector_dispatches\":" << stats.state_vector_dispatches
      << ",\"state_vector_steps\":" << stats.state_vector_steps
      << ",\"protected32_state_vector_steps\":" << stats.protected32_state_vector_steps
      << ",\"rotate_guest_steps\":" << stats.rotate_steps
      << ",\"variable_rotate_guest_steps\":" << stats.variable_rotate_steps
      << ",\"x86_virtual_space\":{\"logical_bytes\":" << space.logical_bytes
      << ",\"cache_metadata_bytes\":" << space.cache_metadata_bytes
      << ",\"allocated_sets\":" << space.allocated_sets
      << ",\"translated_block_capacity\":" << space.translated_block_capacity << '}'
#if defined(REVERSE_MUNDO_X_SOURCE_SHA)
      << ",\"mundo_x_source_sha\":\"" REVERSE_MUNDO_X_SOURCE_SHA "\""
#endif
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
    start_time_ = Clock::now();
    initial_ = last_ = capture(pc, board, start_time_);
    next_ = initial_.steps + interval_;
    count_ = 0;
    first_ap_start_.reset();
  }

  void after_run(const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board) noexcept {
    const bool checkpoint = pc.cpu.instructions == next_;
    const bool first_ap_start = !first_ap_start_ &&
        board.startup_ipis().sipi_targets_applied > initial_.sipi_targets_applied;
    if (!checkpoint && !first_ap_start) return;
    const auto current = capture(pc, board);
    if (first_ap_start) first_ap_start_ = current;
    if (!checkpoint) return;
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
    out << ",\"first_ap_start_observed\":";
    if (first_ap_start_) write(out, *first_ap_start_);
    else out << "null";
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
  using Clock = cosmic_pc::PcMachine::RunClock;
  struct State {
    std::uint64_t steps = 0, ip = 0, cr3 = 0, ticks = 0, irq = 0;
    std::uint64_t total_processor_steps = 0, ether_arm64_steps = 0, rtc_irq8_delivered = 0;
    std::uint64_t host_elapsed_ns = 0, sipi_targets_applied = 0;
    unsigned started_vcpus = 0;
    cosmic_pc::Cpu::TscAccessStats tsc_access;
    std::uint64_t disk_reads = 0, ata_commands = 0, atapi_packets = 0;
    std::uint64_t page_walks = 0, page_hits = 0, instruction_page_hits = 0, batched = 0;
    std::uint64_t instruction_page_reuses = 0;
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

  State capture(const cosmic_pc::PcMachine& pc, const PcMotherboardC06& board,
                Clock::time_point now = Clock::now()) const noexcept {
    const auto& cpu = pc.cpu;
    const auto board_state = board.evidence();
    const auto execution = board.cpu_execution_totals();
    const auto& work = pc.boot_acceleration();
    State out;
    out.steps = cpu.instructions;
    out.total_processor_steps = execution.guest_steps;
    out.ether_arm64_steps = execution.ether_steps;
    out.tsc_access = execution.tsc_access;
    out.started_vcpus = board.processors_started();
    out.rtc_irq8_delivered = board.rtc_evidence().irq8_delivered;
    out.sipi_targets_applied = board.startup_ipis().sipi_targets_applied;
    // Wall time since this profile phase began, including time between board runs.
    out.host_elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time_).count();
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
    out.instruction_page_reuses = cpu.instruction_page_reuses;
    const auto memory = cpu.memory.metrics();
    out.resident_bytes = memory.allocated_bytes;
    out.spill_reads = memory.backing_reads;
    out.spill_writes = memory.backing_writes;
    out.batched = work.rep_elements + work.shift_loop_steps + work.zero_loop_steps +
        work.scan_loop_steps + work.forward_copy_loop_steps + work.arm64_block_steps + work.counter_loop_steps +
        work.affine_ram_rotate_steps + work.movntdq_store_steps + cpu.state_vector_interpreter_stats().steps;
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
        << ",\"total_processor_steps\":" << s.total_processor_steps
        << ",\"ether_arm64_steps\":" << s.ether_arm64_steps
        << ",\"started_vcpus\":" << s.started_vcpus
        << ",\"rtc_irq8_delivered\":" << s.rtc_irq8_delivered
        << ",\"sipi_targets_applied\":" << s.sipi_targets_applied
        << ",\"host_elapsed_ns\":" << s.host_elapsed_ns
        << ",\"bios_disk_reads\":" << s.disk_reads << ",\"ata_commands\":" << s.ata_commands
        << ",\"atapi_packets\":" << s.atapi_packets << ",\"page_walks\":" << s.page_walks
        << ",\"page_translation_hits\":" << s.page_hits
        << ",\"instruction_page_hits\":" << s.instruction_page_hits
        << ",\"instruction_page_reuses\":" << s.instruction_page_reuses
        << ",\"ram_resident_bytes\":" << s.resident_bytes << ",\"spill_reads\":" << s.spill_reads
        << ",\"spill_writes\":" << s.spill_writes << ",\"batched_steps\":" << s.batched
        << ",\"neon_steps\":" << s.neon_steps
        << ",\"mode_changes\":" << s.mode_changes
        << ",\"run_ns\":" << s.timing.ns(cosmic_pc::BootTimePart::Run)
        << ",\"sampled_iterations\":" << s.timing.iterations << ",\"tsc_access\":";
    write_tsc_access(out, s.tsc_access);
    out << ",\"registers\":[";
    for (std::size_t i = 0; i < s.regs.size(); ++i) {
      if (i) out << ',';
      out << '"' << std::hex << s.regs[i] << std::dec << '"';
    }
    out << "]}";
  }

  const std::uint64_t interval_;
  std::uint64_t next_, count_ = 0;
  Clock::time_point start_time_{};
  std::optional<State> first_ap_start_;
  State initial_{}, last_{};
  std::array<Point,8> points_{};
};

} // namespace reverse_windows
