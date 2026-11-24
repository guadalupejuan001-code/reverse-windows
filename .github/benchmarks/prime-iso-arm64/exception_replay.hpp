#pragma once

#include "x86_cpu.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <vector>

class ExceptionReplay {
 public:
  ExceptionReplay() {
    if (active_ || cosmic_pc::Cpu::exception_event_hook)
      throw std::logic_error("Only one exception trace can run per native probe");
    active_ = this;
    cosmic_pc::Cpu::exception_event_hook = &receive;
  }

  ~ExceptionReplay() {
    cosmic_pc::Cpu::exception_event_hook = nullptr;
    active_ = nullptr;
  }

  ExceptionReplay(const ExceptionReplay&) = delete;
  ExceptionReplay& operator=(const ExceptionReplay&) = delete;

  // Called by the session owner between windows, once all vCPU workers joined.
  bool observed_nmi() const noexcept { return first_nmi_sequence_ != 0; }

  void write(const std::filesystem::path& path, std::uint64_t session_steps) const {
    const auto temporary = path.string() + ".tmp";
    std::ofstream out(temporary);
    out << "{\"schema\":\"prime-guest-exceptions-v1\",\"session_steps_at_capture\":" << session_steps
        << ",\"first_delivered_nmi_sequence\":" << first_nmi_sequence_
        << ",\"other_apic_events\":" << other_apic_events_
        << ",\"capacity_per_cpu_per_phase\":" << kCapacity
        << ",\"register_order\":[\"rax\",\"rcx\",\"rdx\",\"rbx\",\"rsp\",\"rbp\",\"rsi\",\"rdi\","
           "\"r8\",\"r9\",\"r10\",\"r11\",\"r12\",\"r13\",\"r14\",\"r15\"],\"through_first_nmi\":";
    write_phase(out, through_nmi_);
    out << ",\"after_first_nmi\":";
    write_phase(out, after_nmi_);
    out << "}\n";
    out.close();
    if (!out) throw std::runtime_error("Could not write guest exception evidence");
    std::filesystem::rename(temporary, path);
  }

 private:
  using Event = cosmic_pc::Cpu::ExceptionEvent;
  static constexpr unsigned kProcessors = 16;
  static constexpr unsigned kCapacity = 512;

  struct Recorded {
    std::uint64_t sequence = 0;
    Event event{};
  };

  struct PerCpu {
    std::uint64_t seen = 0;
    std::array<Recorded, kCapacity> recent{};
  };

  using Phase = std::array<PerCpu, kProcessors>;
  inline static ExceptionReplay* active_ = nullptr;
  std::mutex mutex_;
  std::uint64_t sequence_ = 0, first_nmi_sequence_ = 0, other_apic_events_ = 0;
  Phase through_nmi_{}, after_nmi_{};

  static void receive(const Event& event) noexcept {
    std::lock_guard<std::mutex> lock(active_->mutex_);
    active_->record(event);
  }

  void record(const Event& event) noexcept {
    if (event.apic_id >= kProcessors) {
      ++other_apic_events_;
      return;
    }
    auto& cpu = (observed_nmi() ? after_nmi_ : through_nmi_)[event.apic_id];
    const auto sequence = ++sequence_;
    cpu.recent[cpu.seen++ % kCapacity] = {sequence, event};
    if (!observed_nmi() && event.hardware && event.vector == 2 && event.delivered)
      first_nmi_sequence_ = sequence;
  }

  static void write_event(std::ostream& out, const Recorded& recorded) {
    const auto& event = recorded.event;
    out << "{\"sequence\":" << recorded.sequence << ",\"apic_id\":" << event.apic_id
        << ",\"vector\":" << event.vector << ",\"original_vector\":" << event.original_vector
        << ",\"error\":" << event.error << ",\"cr8\":" << event.cr8 << ",\"cs\":" << event.cs
        << ",\"opcode\":" << event.opcode << ",\"vcpu_steps\":" << event.steps
        << ",\"hardware\":" << (event.hardware ? "true" : "false")
        << ",\"delivered\":" << (event.delivered ? "true" : "false") << std::hex
        << ",\"rip\":\"0x" << event.rip << "\",\"handler_rip\":\"0x" << event.handler_rip
        << "\",\"linear\":\"0x" << event.linear << "\",\"cr2\":\"0x" << event.cr2
        << "\",\"cr3\":\"0x" << event.cr3 << "\",\"rflags\":\"0x" << event.rflags
        << "\",\"gs_base\":\"0x" << event.gs_base << "\",\"kernel_gs_base\":\"0x" << event.kernel_gs_base
        << "\",\"handler_rsp\":\"0x" << event.handler_rsp << "\",\"regs\":[";
    for (unsigned i = 0; i < event.regs.size(); ++i)
      out << (i ? ",\"0x" : "\"0x") << event.regs[i] << '"';
    out << "],\"instruction_hex\":\"" << std::setfill('0');
    for (unsigned i = 0; i < std::min<std::size_t>(event.byte_count, event.bytes.size()); ++i)
      out << std::setw(2) << unsigned(event.bytes[i]);
    out << "\"}" << std::dec;
  }

  static void write_phase(std::ostream& out, const Phase& phase) {
    std::vector<const Recorded*> ordered;
    out << "{\"seen_by_apic_id\":[";
    for (unsigned i = 0; i < phase.size(); ++i) {
      const auto& cpu = phase[i];
      if (i) out << ',';
      out << cpu.seen;
      const auto retained = std::min<std::uint64_t>(cpu.seen, kCapacity);
      for (unsigned slot = 0; slot < retained; ++slot) ordered.push_back(&cpu.recent[slot]);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Recorded* left, const Recorded* right) {
      return left->sequence < right->sequence;
    });
    out << "],\"events\":[";
    for (unsigned i = 0; i < ordered.size(); ++i) {
      if (i) out << ',';
      write_event(out, *ordered[i]);
    }
    out << "]}";
  }
};
