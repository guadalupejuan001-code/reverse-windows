#pragma once

#include "android_ram_backing.hpp"
#include "bios_hard_disk.hpp"
#include "c06_motherboard.hpp"
#include "boot_profile.hpp"
#include "c09r_guest_vga_fabric.hpp"
#include "ether_x86_check.hpp"
#include "guest_vbe_fabric.hpp"
#include "qssd_disk.hpp"
#include "storage_profile.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace reverse_windows {

class AndroidBootSession final {
 public:
  enum class BootSource { Iso=1, Disk=2, SelfTest=3 };
  enum class Step { Running=0, WaitingKey=1, Halted=2, Fault=3 };
  struct RamProfile {
    std::size_t physical_bytes;
    std::size_t backing_bytes;
  };
  struct VideoUpdate {
    unsigned width = 0, height = 0;
    std::size_t text_bytes = 0;
    const std::vector<std::uint32_t>* graphics = nullptr;
  };
  static constexpr unsigned kMaxVideoWidth = 960, kMaxVideoHeight = 720;
  static constexpr RamProfile kDefaultRamProfile{2048ULL * 1024 * 1024, 256ULL * 1024 * 1024};
  static constexpr cosmic_pc::CpuTopology kPrimeCpuTopology{8, 2};
  static constexpr std::uint64_t kPrimeRamBytes = 64ULL << 30;
  static constexpr std::uint64_t kMaxResidentBytes = 1024ULL * 1024 * 1024;
  static constexpr std::uint64_t kMinResidentBytes = 16ULL * 1024 * 1024;

  static RamProfile ram_profile(std::uint64_t available_bytes, std::uint64_t low_threshold,
                                bool low_memory, bool prime_ram = false) {
    constexpr std::uint64_t MiB = 1024 * 1024;
    const auto headroom = available_bytes > low_threshold ? available_bytes - low_threshold : 0;
    using Pc = cosmic_pc::PcMachine;
    const auto prime_space = Pc::high_ram_address_space(kPrimeRamBytes, true);
    const auto backing = page_floor(prime_ram ? headroom - std::min(headroom, prime_reserve(headroom, low_threshold))
                                              : headroom / 2);
    if (low_memory || backing < kMinResidentBytes)
      throw std::runtime_error("Android tiene poca RAM disponible para iniciar el equipo virtual.");
    auto physical = prime_ram ? prime_space : std::uint64_t(kDefaultRamProfile.physical_bytes);
    if (!prime_ram && backing >= 768 * MiB && Pc::max_ram_mib() >= 4096)
      physical = Pc::kLegacyPhysicalLimit;
    if (physical > std::numeric_limits<std::size_t>::max())
      throw std::runtime_error("El perfil de 64 GiB requiere un anfitrión de 64 bits.");
    return {static_cast<std::size_t>(physical),
            static_cast<std::size_t>(std::min(backing, resident_limit(physical)))};
  }

  void sample_ram_pressure(std::uint64_t available, std::uint64_t threshold, bool low_memory) {
    const auto& memory = pc_.cpu.memory;
    const auto headroom = available > threshold ? available - threshold : 0;
    const auto limit = resident_limit(memory.size());
    const bool prime_ram = high_ram_profile(memory.size());
    const bool background_trim = low_memory && available == 0 && threshold == 0;
    const auto floor = std::min(limit, kMinResidentBytes);
    const auto old_target = pressure_.target ? pressure_.target : memory.budget_bytes();
    auto target = floor;
    pressure_.reserve = 0;
    if (prime_ram && !background_trim) {
      auto occupied = std::min(memory.allocated_bytes(), limit);
      occupied += std::min(memory.backing_cache_bytes(), limit - occupied);
      // Estimate the available host surplus before this session's resident
      // payload. Releasing bytes should not cause the next sample to grow it.
      const auto missing = threshold > available ? threshold - available : 0;
      const auto max_growth = std::numeric_limits<std::uint64_t>::max() - occupied;
      const auto surplus = occupied + std::min(headroom, max_growth)
                         - std::min(occupied, missing);
      pressure_.reserve = prime_reserve(surplus, threshold);
      auto backing = surplus - std::min(surplus, pressure_.reserve);
      if (low_memory || missing) backing = std::min({backing, old_target, occupied});
      target = std::clamp(page_floor(backing), floor, limit);
    } else if (!low_memory) {
      const auto backing = std::min(headroom, 2 * limit) / 2 + memory.allocated_bytes() / 2;
      target = std::clamp(page_floor(backing), floor, limit);
    }
    board_->set_gpu_cache_enabled(!low_memory && (!prime_ram || headroom) && target > floor);
    pressure_.available = available;
    pressure_.threshold = threshold;
    pressure_.background_trim = background_trim;
    pressure_.low_memory = low_memory;
    // Reserve reductions take effect immediately. Small growth can wait for
    // another sample instead of invalidating the spill cache repeatedly.
    const auto delta = std::max(old_target, target) - std::min(old_target, target);
    if ((prime_ram && target < old_target) || low_memory || target == floor || delta >= 8 * 1024 * 1024)
      pressure_.target = target;
    else
      pressure_.target = old_target;
  }

  bool maintain_ram_budget() {
    if (!pressure_.target) return false;
    using Clock = cosmic_pc::SparseRam::ReclaimClock;
    using Adjustment = cosmic_pc::SparseRam::BudgetAdjustment;
    auto& memory = pc_.cpu.memory;
    const auto before = memory.allocated_bytes();
    const auto cache_before = memory.backing_cache_bytes();
    const auto encode_before = spill_.file ? spill_.file->codec_stats().encode.wall_ns : 0;
    const auto started = Clock::now();
    try {
      const auto clean_pages = spill_.file && high_ram_profile(memory.size()) ? kPrimeCleanReservePages : 0;
      pressure_.status = memory.adjust_payload_budget(pressure_.target, 128, started + kRamReclaimWindow,
                                                     clean_pages);
      pressure_.error.clear();
    } catch (const std::exception& error) {
      pressure_.status = Adjustment::Blocked;
      pressure_.error = error.what();
    }
    const auto elapsed = Clock::now() - started;
    if (spill_.file) pressure_.encode_ns += spill_.file->codec_stats().encode.wall_ns - encode_before;
    const auto after = memory.allocated_bytes();
    const auto cache_after = memory.backing_cache_bytes();
    pressure_.maintenance_time += elapsed;
    pressure_.last_maintenance = elapsed;
    pressure_.max_maintenance = std::max(pressure_.max_maintenance, elapsed);
    ++pressure_.maintenance_batches;
    pressure_.reclaimed_bytes += before - std::min(before, after);
    const auto resident_before = before + cache_before;
    const auto resident_after = after + cache_after;
    pressure_.last_released_bytes = resident_before - std::min(resident_before, resident_after);
    pressure_.released_bytes += pressure_.last_released_bytes;
    if (elapsed <= kRamReclaimWindow) pressure_.released_within_window += pressure_.last_released_bytes;
    return pressure_.status == Adjustment::Reclaiming;
  }

  bool prepare_ram_reclaim() {
    using Clock = cosmic_pc::SparseRam::ReclaimClock;
    using Adjustment = cosmic_pc::SparseRam::BudgetAdjustment;
    auto& memory = pc_.cpu.memory;
    const auto budget = memory.budget_bytes();
    if (!spill_.file || !high_ram_profile(memory.size()) || !pressure_.target ||
        pressure_.low_memory || pressure_.status != Adjustment::Applied ||
        memory.allocated_bytes() < budget - budget / 4) return false;
    const auto goal = std::min(kPrimeCleanReservePages, memory.allocated_pages() / 16);
    if (memory.clean_backing_pages() >= goal) return false;

    const auto start = Clock::now();
    const auto progress = memory.prepare_reclaim(goal, start + kRamPrepareWindow);
    const auto elapsed = Clock::now() - start;
    auto& prep = pressure_.preparation;
    ++prep.batches;
    prep.backed_pages += progress.backed_pages;
    prep.compact_up_to_2k += progress.compact_up_to_2k;
    prep.compact_up_to_3k += progress.compact_up_to_3k;
    prep.dense_pages += progress.dense_pages;
    prep.released_bytes += progress.released_bytes;
    prep.failures += progress.backing_failed;
    prep.max_time = std::max(prep.max_time, elapsed);
    if (elapsed > kRamPrepareWindow) ++prep.overruns;
    return !progress.backing_failed && elapsed <= kRamPrepareWindow && memory.clean_backing_pages() < goal;
  }

  AndroidBootSession(std::filesystem::path disk, unsigned create_gib,
                     BootSource source, int iso_fd=-1, std::uint64_t iso_bytes=0,
                     RamProfile profile=kDefaultRamProfile,
                     const std::filesystem::path& ram_cache={},
                     const std::filesystem::path& graphics_vram={},
                     const std::filesystem::path& data_disk={},
                     cosmic_pc::CpuTopology topology={}, bool active_guest_clock=false)
      : source_(source), use_active_guest_clock_(active_guest_clock && source != BootSource::SelfTest),
        pc_(profile.physical_bytes, profile.backing_bytes), vga_(pc_), vbe_(pc_) {
    if (disk.empty() && (create_gib || source != BootSource::SelfTest))
      throw std::invalid_argument("Selecciona o crea un disco QSSD para iniciar.");
    pc_.cpu.use_windows_x64_profile(source != BootSource::SelfTest);
    if (high_ram_profile(profile.physical_bytes))
      pc_.reserve_pci_mmio(true); // Match the installed RAM profile even without a graphics device.
    spill_ = configure_spill(pc_, ram_cache, source != BootSource::SelfTest);
    switch (source) {
      case BootSource::Iso:
        iso_ = std::make_shared<OpticalReader>(iso_fd, iso_bytes);
        pc_.mount([reader = iso_](auto offset, auto* target, auto count) {
          return reader->read(offset, target, count);
        }, iso_->size(), "iso");
        break;
      case BootSource::Disk:
        if (create_gib)
          throw std::invalid_argument("Un QSSD nuevo necesita arrancar primero desde una ISO.");
        break;
      case BootSource::SelfTest:
        pc_.boot_demo();
        break;
      default:
        throw std::invalid_argument("Origen de arranque desconocido.");
    }
    if (!disk.empty()) {
      if (create_gib) create_disk(disk, create_gib);
      disk_ = std::make_unique<QssdDisk>(disk);
    }
    if (!data_disk.empty()) data_disk_ = std::make_unique<QssdDisk>(data_disk);
    if (source == BootSource::Disk) {
      pc_.mount([this](auto offset, auto* target, auto count) {
        return disk_->read_bytes(offset, target, count);
      }, disk_->bytes(), "raw");
    }
    if (disk_) {
      bios_disk_ = std::make_unique<BiosHardDisk>(pc_, *disk_);
      initial_disk_commits_ = disk_->stats().commits;
    }
    if (iso_) {
      board_ = std::make_unique<PcMotherboardC06>(pc_, disk_.get(),
          [reader = iso_](auto offset, auto* target, auto count) {
            return reader->read(offset, target, count);
          }, iso_->size(), graphics_vram, data_disk_.get(), topology);
    } else {
      board_ = std::make_unique<PcMotherboardC06>(
          pc_, disk_.get(), AtapiCdrom::Reader{}, 0, graphics_vram, data_disk_.get(), topology);
    }
    board_->enable_ps2_keyboard();
    board_->enable_threaded_smp();
    if (source != BootSource::SelfTest) pc_.enable_guest_restart();
    pc_.enable_boot_acceleration();
    pc_.cpu.enable_page_translation_cache();
    pc_.cpu.enable_ether_arm64_blocks();
    if (source != BootSource::SelfTest) start_boot_profile();
#if defined(MUNDO_X_PC_ETHER_NEXT)
    pc_.cpu.enable_ether_next();
#endif
  }

  static void create_disk(const std::filesystem::path& disk, unsigned gib) {
    QssdDisk::create(disk, gib);
  }

  static std::string check_hardware(const std::filesystem::path& disk, int iso_fd=-1,
                                    std::uint64_t iso_bytes=0, const std::string& iso_error={},
                                    RamProfile profile=kDefaultRamProfile,
                                    const std::filesystem::path& ram_cache={},
                                    cosmic_pc::CpuTopology topology={}) {
    cosmic_pc::PcMachine pc(profile.physical_bytes, profile.backing_bytes);
    if (profile.physical_bytes > cosmic_pc::PcMachine::kLegacyPhysicalLimit) pc.reserve_pci_mmio(true);
    const auto spill = configure_spill(pc, ram_cache);
    pc.boot_demo();
    auto& cpu = pc.cpu;
    cpu.use_windows_x64_profile();
    const auto cpuid = [&](std::uint32_t leaf) {
      constexpr unsigned address = 0x1000;
      cpu.reset(0, address);
      cpu.write16(address, 0xa20f); // CPUID
      cpu.regs[cosmic_pc::Cpu::EAX] = leaf;
      if (cpu.step() != cosmic_pc::StepResult::Executed)
        throw std::runtime_error("No se pudo consultar CPUID.");
      return cpu.regs;
    };
    const auto extended = cpuid(0x80000001U);
    const auto flag = [](std::uint64_t reg, unsigned bit) { return bool(reg & (1ULL << bit)); };
    pc.reset();
    pc.run(1500);
    std::array<std::uint8_t,4000> cells{};
    pc.copy_vga_text(cells.data(), cells.size());
    std::string text;
    for (std::size_t i=0;i<cells.size();i+=2) text += char(cells[i]);
    const bool ram = pc.waiting_key && text.find("ALU + RAM + lectura de disco: OK") != std::string::npos;
    const bool vga = pc.waiting_key && text.find("Pulsa una tecla:") != std::string::npos;
    pc.reset();
    bool pci_ide = false, local_apic = false;
    EtherX86Check x86_sessions;
    decltype(cpu.regs) basic{};
    {
      const std::array<std::uint8_t,2048> optical_sector{};
      PcMotherboardC06 board(pc, nullptr,
          [&](auto offset, auto* data, auto count) {
            if (offset > optical_sector.size() || count > optical_sector.size() - offset) return false;
            std::copy_n(optical_sector.data()+offset, count, data);
            return true;
          }, optical_sector.size(), {}, nullptr, topology);
      basic = cpuid(1);
      constexpr unsigned probe = 0x1000;
      cpu.reset(0, probe);
      cpu.write16(probe, 0x320f); // RDMSR
      cpu.regs[cosmic_pc::Cpu::ECX] = 0x1b;
      const auto base = cosmic_pc::PcMachine::kLocalApicAddress;
      local_apic = flag(basic[cosmic_pc::Cpu::EDX],9) &&
          cpu.step() == cosmic_pc::StepResult::Executed &&
          cpu.regs[cosmic_pc::Cpu::EDX] == 0 &&
          cpu.regs[cosmic_pc::Cpu::EAX] == (base | 0x900U) &&
          cpu.read32(AcpiC06::madt_address+36) == base &&
          (cpu.read32(base+0x30)&0xffU) == 0x14U;
      // Exercise guest I/O against the same motherboard used for ISO boots.
      constexpr std::array<std::uint8_t,16> pci_probe{{
          0x66,0xb8,0x08,0x10,0x00,0x80,0xba,0xf8,0x0c,0x66,0xef,
          0xba,0xfc,0x0c,0x66,0xed}};
      cpu.reset(0, 0x1000);
      cpu.memory.write_block(0x1000, pci_probe.data(), pci_probe.size());
      pci_ide = true;
      for (unsigned i=0;i<5 && pci_ide;++i) pci_ide = cpu.step() == cosmic_pc::StepResult::Executed;
      pci_ide = pci_ide && (cpu.regs[cosmic_pc::Cpu::EAX] >> 16) == 0x0101;
      if (cosmic_pc::Cpu::ether_arm64_host() &&
          topology.logical_processors() == kPrimeCpuTopology.logical_processors())
        x86_sessions = check_ether_x86_sessions(board);
    }

    auto optical_issue = iso_error;
    std::string disk_issue, ata_issue;
    const bool has_iso = iso_fd>=0 || !optical_issue.empty();
    const bool has_disk = !disk.empty();
    if (iso_fd>=0) {
      try {
        OpticalReader reader(iso_fd,iso_bytes);
        const auto plan = cosmic_pc::BootMedia(reader.size(),[&](auto off, auto* into, auto len) {
          return reader.read(off,into,len);
        }).plan("iso");
        const auto address = std::uint64_t(plan.load_segment)*16;
        if (address>pc.cpu.memory.size() || plan.image.size()>pc.cpu.memory.size()-address)
          throw std::runtime_error("La imagen de arranque excede la RAM.");
      } catch (const std::exception& error) { optical_issue = error.what(); }
    }
    if (has_disk) {
      try {
        QssdDisk volume(disk,QssdDisk::Access::ReadOnly);
        std::array<std::uint8_t,512> first{};
        if (!volume.read(0,1,first.data())) throw std::runtime_error("No se pudo leer el sector inicial.");
        try {
          check_ata_identify(pc, volume);
        } catch (const std::exception& error) { ata_issue = error.what(); }
      } catch (const std::exception& error) { disk_issue = error.what(); }
    }
    std::ostringstream out;
    out << std::boolalpha << "{\"schema\":\"reverse-windows-hardware-check-v1\",\"checks\":{"
        << "\"cpu_x64\":" << flag(extended[cosmic_pc::Cpu::EDX],29)
        << ",\"cpu_pae\":" << flag(basic[cosmic_pc::Cpu::EDX],6)
        << ",\"cpu_nx\":" << flag(extended[cosmic_pc::Cpu::EDX],20)
        << ",\"cpu_mmx\":" << flag(basic[cosmic_pc::Cpu::EDX],23)
        << ",\"cpu_sse\":" << flag(basic[cosmic_pc::Cpu::EDX],25)
        << ",\"cpu_sse2\":" << flag(basic[cosmic_pc::Cpu::EDX],26)
        << ",\"cpu_x87\":" << flag(basic[cosmic_pc::Cpu::EDX],0)
        << ",\"cpu_fxsr\":" << flag(basic[cosmic_pc::Cpu::EDX],24)
        << ",\"cpu_cx16\":" << flag(basic[cosmic_pc::Cpu::ECX],13)
        << ",\"cpu_lahf_sahf\":" << flag(extended[cosmic_pc::Cpu::ECX],0)
        << ",\"cpu_prefetchw\":" << flag(extended[cosmic_pc::Cpu::ECX],8)
        << ",\"lapic\":" << local_apic << ",\"ram\":" << ram
        << ",\"pci_ide\":" << pci_ide << ",\"vga\":" << vga << ",\"iso_boot\":";
    if (has_iso) out << optical_issue.empty(); else out << "null";
    out << ",\"qssd\":";
    if (has_disk) out << disk_issue.empty(); else out << "null";
    out << ",\"qssd_ata\":";
    if (!has_disk || !disk_issue.empty()) out << "null"; else out << ata_issue.empty();
    out << "},\"cpu_topology\":{\"sockets\":1,\"cores\":" << topology.cores
        << ",\"threads_per_core\":" << topology.threads_per_core
        << ",\"logical_processors\":" << ((basic[cosmic_pc::Cpu::EBX] >> 16) & 255U)
        << "},\"ram_profile\":";
    write_ram_profile(out, pc, spill);
    if (x86_sessions.requested_jobs) {
      out << ",\"ether_x86_sessions\":{\"passed\":" << x86_sessions.passed()
          << ",\"owners\":" << x86_sessions.owners << ",\"host_workers\":" << x86_sessions.host_workers
          << ",\"requested_jobs\":" << x86_sessions.requested_jobs
          << ",\"completed_jobs\":" << x86_sessions.completed_jobs
          << ",\"guest_steps\":" << x86_sessions.guest_steps
          << ",\"projected_steps\":" << x86_sessions.projected_steps
          << ",\"elapsed_ns\":" << x86_sessions.elapsed_ns
          << ",\"error\":" << cosmic_pc::PcMachine::quote(x86_sessions.error) << '}';
    }
    out << ",\"ready_for_windows10_install\":false,\"errors\":{"
        << "\"iso_boot\":" << cosmic_pc::PcMachine::quote(optical_issue)
        << ",\"qssd\":" << cosmic_pc::PcMachine::quote(disk_issue)
        << ",\"qssd_ata\":" << cosmic_pc::PcMachine::quote(ata_issue) << "}}";
    return out.str();
  }

  std::uint64_t session_steps() const noexcept { return previous_boot_steps_+board_->total_processor_steps(); }

  void configure_ether_x86_sessions(unsigned workers, unsigned max_sessions_per_vcpu) {
    board_->configure_ether_x86_sessions(workers, max_sessions_per_vcpu);
  }

  std::optional<EtherX86Sessions::Ticket> submit_ether_x86_session(unsigned vcpu, EtherX86Sessions::Job job) {
    return board_->submit_ether_x86_session(vcpu, std::move(job));
  }

  Step step(unsigned budget, cosmic_pc::PcMachine::RunClock::time_point deadline=
            cosmic_pc::PcMachine::RunClock::time_point::max()) {
    if (board_->faulted_cpu()) return Step::Fault;
    if (!is_stopped()) {
      if (use_active_guest_clock_ && pc_.cpu.is_long_mode() && !board_->active_clock_enabled())
        board_->enable_active_clock();
      const auto slice = boot_profile_ ? boot_profile_->budget(budget, pc_) : budget;
      board_->run(slice,deadline);
      if (boot_profile_) boot_profile_->after_run(pc_, *board_);
    }
    if (!pc_.cpu.fault && pc_.host_error.empty() &&
        pc_.guest_restart() != cosmic_pc::PcMachine::GuestRestart::None) {
      const auto steps = board_->total_processor_steps();
      const auto restart_count = restarts_;
      restart_guest();
      if (restarts_ != restart_count) previous_boot_steps_ += steps;
    }
    if (board_->faulted_cpu() || pc_.cpu.fault || !pc_.host_error.empty()) return Step::Fault;
    if (pc_.waiting_key) return Step::WaitingKey;
    if (is_stopped()) return Step::Halted;
    return Step::Running;
  }

  bool is_stopped() const noexcept {
    return board_->evidence().powered_off ||
        (pc_.cpu.halted && !pc_.cpu.flag(cosmic_pc::Cpu::IF) && !board_->secondary_can_run());
  }

  bool key(unsigned ascii, unsigned scancode, bool pressed) {
    if (ascii > 255 || (scancode > 255 && (scancode & ~255U) != 0xe000U))
      throw std::invalid_argument("Código de teclado BIOS inválido.");
    if (is_stopped() || pc_.cpu.fault || !pc_.host_error.empty()) return false;
    const bool was_waiting = pc_.waiting_key;
    auto& held_route = key_routes_[(scancode & 0xffU) + (scancode > 0xff ? 256 : 0)];
    auto route = held_route;
    // Keep a key's make, repeat and break on the same interface even when
    // the guest changes mode or switches between BIOS and controller input.
    if (pressed && route == KeyRoute::None)
      route = pc_.cpu.is_real_mode() && (was_waiting || !board_->guest_uses_ps2_scancodes())
          ? KeyRoute::Bios : KeyRoute::Ps2;
    if (!pressed) held_route = KeyRoute::None;
    if (route == KeyRoute::Bios) {
      pc_.key_event(static_cast<std::uint8_t>(ascii), scancode, pressed);
      if (pressed) held_route = route;
    } else if (route == KeyRoute::Ps2 && scancode && board_->inject_ps2_key(scancode, pressed)) {
      if (pressed) held_route = route;
    }
    return was_waiting && !pc_.waiting_key;
  }

  void flush() {
    const bool boot_synced = !disk_ || disk_->flush();
    const bool data_synced = !data_disk_ || data_disk_->flush();
    if (!boot_synced) throw std::runtime_error("No se pudo sincronizar el QSSD de arranque.");
    if (!data_synced) throw std::runtime_error("No se pudo sincronizar el QSSD de datos.");
  }

  cosmic_pc::PcVideoInfo video_info() const noexcept { return pc_.video_info(); }
  bool video_pending() const noexcept { return pc_.cpu.memory.write_watch()->pending(); }

  std::size_t copy_text(std::uint8_t* target, std::size_t capacity) {
    if (!target || capacity < presented_text_.size())
      throw std::invalid_argument("Buffer VGA insuficiente.");
    if (pc_.video_info().graphics) return 0;
    const auto& watch = *pc_.cpu.memory.write_watch();
    if (text_visible_ && !watch.pending()) return 0;
    const bool new_mode = watch.full();
    const auto frame = vga_.capture();
    const bool refresh = !text_visible_ || new_mode || frame.changed_cells;
    if (frame.full_snapshot) {
      presented_text_ = frame.complete;
    } else {
      for (const auto& cell : frame.updates) {
        presented_text_[cell.index * 2] = cell.character;
        presented_text_[cell.index * 2 + 1] = cell.attribute;
      }
    }
    vga_serial_ = frame.qssd_serial;
    vga_transfer_bytes_ = frame.transferred_bytes;
    text_visible_ = true;
    vbe_.require_full();
    pc_.cpu.memory.acknowledge_write_watch();
    if (!refresh) return 0;
    std::memcpy(target, presented_text_.data(), presented_text_.size());
    return presented_text_.size();
  }

  VideoUpdate capture_video(std::uint8_t* text, std::size_t capacity) {
    const auto info = pc_.video_info();
    if (!info.graphics)
      return {info.width, info.height, copy_text(text, capacity), nullptr};
    text_visible_ = false;
    auto width = info.width;
    auto height = info.height;
    if (width > kMaxVideoWidth || height > kMaxVideoHeight) {
      if (std::uint64_t(width) * kMaxVideoHeight >= std::uint64_t(height) * kMaxVideoWidth) {
        height = std::max(1U, height * kMaxVideoWidth / width);
        width = kMaxVideoWidth;
      } else {
        width = std::max(1U, width * kMaxVideoHeight / height);
        height = kMaxVideoHeight;
      }
    }
    const auto& packet = vbe_.capture(width, height);
    return {width, height, 0, packet.empty() ? nullptr : &packet};
  }

  void retry_video() noexcept {
    text_visible_ = false;
    vbe_.require_full();
    pc_.cpu.memory.invalidate_write_watch();
  }

  std::string diagnostic(bool detailed = true) const {
    const auto platform = board_->evidence();
    const auto interrupts = board_->interrupt_state();
    const auto* secondary_fault = board_->faulted_cpu();
    auto status = platform.powered_off ? "powered_off" : secondary_fault ? "fault" : pc_.status();
    if (status == "halted" && board_->secondary_can_run()) status = "running";
    const auto* ata = board_->attached_ata();
    const auto counts = ata ? ata->counters() : AtaC07::Counters{};
    std::ostringstream out;
    out << "{\"schema\":\"reverse-windows-android-session-v1\",\"source\":\""
        << source_name() << "\",\"status\":\"" << status << "\",\"iso_bytes\":"
        << (iso_ ? iso_->size() : 0)
        << ",\"iso_read_us\":" << (iso_ ? iso_->read_us() : 0)
        << ",\"input\":\"bios-int16+ps2-irq1\",\"disk\":";
    if (disk_) {
      const auto stats = disk_->stats();
      out << "{\"logical_bytes\":" << stats.logical_bytes
          << ",\"physical_bytes\":" << stats.physical_bytes
          << ",\"commits\":" << stats.commits << ",\"initial_commits\":" << initial_disk_commits_
          << ",\"generation\":" << disk_->generation() << '}';
    } else {
      out << "null";
    }
    if (data_disk_) {
      const auto stats = data_disk_->stats();
      const auto activity = board_->attached_data_ata()->counters();
      out << ",\"data_disk\":{\"logical_bytes\":" << stats.logical_bytes
          << ",\"physical_bytes\":" << stats.physical_bytes << ",\"commits\":" << stats.commits
          << ",\"ata_identify_data_transfers\":" << activity.identify_data_transfers
          << ",\"ata_read_sectors\":" << activity.read_sectors
          << ",\"ata_write_sectors\":" << activity.write_sectors << '}';
    }
    out << ",\"storage_io\":";
    write_storage_profile(out, pc_, bios_disk_.get(), ata, disk_.get());
    const auto topology = pc_.cpu.topology();
    const auto execution = board_->cpu_execution_totals();
    const auto startup = board_->startup_ipis();
    const auto& threaded = board_->threaded_smp_stats();
    out << ",\"cpu_topology\":{\"sockets\":1,\"cores\":" << topology.cores
        << ",\"threads_per_core\":" << topology.threads_per_core
        << ",\"logical_processors\":" << topology.logical_processors()
        << ",\"started\":" << board_->processors_started()
        << ",\"host_workers\":" << board_->worker_count()
        << ",\"threaded_execution\":" << (board_->threaded_smp_enabled() ? "true" : "false")
        << ",\"host_workers_last_dispatch\":" << threaded.last_workers
        << ",\"host_workers_peak_dispatch\":" << threaded.peak_workers
        << ",\"threaded_dispatches\":" << threaded.dispatches
        << ",\"threaded_guest_steps\":" << threaded.guest_steps
        << ",\"phase_x86_steps\":" << execution.guest_steps
        << ",\"phase_ether_arm64_steps\":" << execution.ether_steps
        << ",\"phase_projected_steps\":" << execution.projected_steps
        << ",\"vcpus_with_projected_steps\":" << execution.processors_with_projected_steps
        << ",\"parallel_register_batches\":" << board_->parallel_register_batches()
        << ",\"parallel_register_steps\":" << board_->parallel_register_steps()
        << ",\"parallel_register_continuations\":" << board_->parallel_register_continuations()
        << ",\"parallel_ram_batches\":" << board_->parallel_ram_batches()
        << ",\"parallel_ram_steps\":" << board_->parallel_ram_steps()
        << ",\"longest_parallel_register_slice\":" << board_->longest_parallel_register_slice()
        << ",\"startup_ipis\":{\"init_icr_writes\":" << startup.init_icr_writes
        << ",\"sipi_icr_writes\":" << startup.sipi_icr_writes
        << ",\"init_targets_queued\":" << startup.init_targets_queued
        << ",\"sipi_targets_queued\":" << startup.sipi_targets_queued
        << ",\"sipi_targets_applied\":" << startup.sipi_targets_applied << '}';
    if (detailed) {
      out << ",\"steps_by_logical_processor\":[";
      for (unsigned id = 0; id < topology.logical_processors(); ++id)
        out << (id ? "," : "") << board_->processor_steps(id);
      out << "],\"ether_arm64_steps_by_logical_processor\":[";
      for (unsigned id = 0; id < topology.logical_processors(); ++id)
        out << (id ? "," : "") << board_->processor_ether_arm64_steps(id);
      out << "],\"state_vector_steps_by_logical_processor\":[";
      for (unsigned id = 0; id < topology.logical_processors(); ++id)
        out << (id ? "," : "") << board_->processor_state_vector_steps(id);
      out << "],\"state_vector_interpreter_steps_by_logical_processor\":[";
      for (unsigned id = 0; id < topology.logical_processors(); ++id)
        out << (id ? "," : "") << board_->processor_state_vector_interpreter_steps(id);
      out << ']';
    }
    out << '}';
    if (const auto workers = board_->ether_x86_session_workers()) {
      out << ",\"ether_x86_sessions\":{\"host_workers\":" << workers << ",\"by_vcpu\":[";
      for (unsigned id = 0; id < topology.logical_processors(); ++id) {
        const auto stats = board_->ether_x86_session_stats(id);
        out << (id ? "," : "") << "{\"id\":" << id
            << ",\"submitted\":" << stats.submitted << ",\"completed\":" << stats.completed
            << ",\"in_flight\":" << stats.in_flight << ",\"peak_in_flight\":" << stats.peak_in_flight << '}';
      }
      out << "]}";
    }
    const auto* faulted_cpu = secondary_fault ? secondary_fault : &pc_.cpu;
    if (faulted_cpu->fault) {
      out << ",\"cpu_fault\":{\"apic_id\":" << faulted_cpu->apic_id() << ",\"message\":"
          << cosmic_pc::PcMachine::quote(faulted_cpu->fault->describe());
      if (faulted_cpu->fault->idt_attempt_count) {
        out << ",\"idt_attempts\":";
        cosmic_pc::PcMachine::write_idt_attempts(out, *faulted_cpu->fault);
      }
      if (detailed && faulted_cpu->fault->interrupt_origin) {
        out << ",\"interrupt_origin\":";
        cosmic_pc::PcMachine::write_interrupt_origin(out, *faulted_cpu->fault->interrupt_origin);
      }
      out << '}';
    }
    out << ",\"ram_profile\":";
    write_ram_profile(out, pc_, spill_, &pressure_);
    const auto gate_work = pc_.quantum_session->gate_work_stats();
    const auto& boot_work = pc_.boot_acceleration();
    const auto batched_steps = boot_work.rep_elements + boot_work.shift_loop_steps +
        boot_work.zero_loop_steps + boot_work.scan_loop_steps + boot_work.counter_loop_steps +
        boot_work.affine_ram_rotate_steps + boot_work.movntdq_store_steps + execution.state_vector_interpreter_steps;
    const auto accelerated_steps = execution.ether_steps + batched_steps;
    out << ",\"ether_coverage\":{\"phase_steps\":" << execution.guest_steps
        << ",\"translated_steps\":" << execution.ether_steps
        << ",\"batched_steps\":" << batched_steps
        << ",\"state_vector_interpreter_steps\":" << execution.state_vector_interpreter_steps
        << ",\"projected_steps\":" << execution.projected_steps
        << ",\"interpreted_steps\":";
    if (accelerated_steps <= execution.guest_steps) out << execution.guest_steps - accelerated_steps;
    else out << "null";
    out << '}';
    out << ",\"ether_next\":{\"quantum_gates_in\":" << gate_work.input_gates
        << ",\"quantum_gates_run\":" << gate_work.executed_gates
        << ",\"quantum_gates_cancelled\":" << gate_work.cancelled_gates
        << ",\"pullback_cache_hits\":" << gate_work.pullback_hits
        << ",\"boot_rep_batches\":" << boot_work.batches
        << ",\"boot_rep_elements\":" << boot_work.rep_elements
        << ",\"boot_rep_bytes\":" << boot_work.bytes
        << ",\"boot_rep_direct_page_copy_bytes\":" << boot_work.rep_direct_page_copy_bytes
        << ",\"boot_paged_rep_elements\":" << boot_work.paged_rep_elements
        << ",\"boot_shift_loop_batches\":" << boot_work.shift_loop_batches
        << ",\"boot_shift_loop_steps\":" << boot_work.shift_loop_steps
        << ",\"boot_zero_loop_batches\":" << boot_work.zero_loop_batches
        << ",\"boot_zero_loop_steps\":" << boot_work.zero_loop_steps
        << ",\"boot_scan_loop_steps\":" << boot_work.scan_loop_steps
        << ",\"boot_counter_loop_batches\":" << boot_work.counter_loop_batches
        << ",\"boot_counter_loop_steps\":" << boot_work.counter_loop_steps
        << ",\"boot_wide_multiply_loop_batches\":" << boot_work.wide_multiply_loop_batches
        << ",\"boot_wide_multiply_loop_steps\":" << boot_work.wide_multiply_loop_steps
        << ",\"boot_rotate_xor_loop_batches\":" << boot_work.rotate_xor_loop_batches
        << ",\"boot_rotate_xor_loop_steps\":" << boot_work.rotate_xor_loop_steps
        << ",\"boot_affine_ram_rotate_batches\":" << boot_work.affine_ram_rotate_batches
        << ",\"boot_affine_ram_rotate_steps\":" << boot_work.affine_ram_rotate_steps
        << ",\"boot_movntdq_store_batches\":" << boot_work.movntdq_store_batches
        << ",\"boot_movntdq_store_steps\":" << boot_work.movntdq_store_steps;
#if defined(MUNDO_X_PC_ETHER_NEXT)
    const auto ether = pc_.cpu.ether_next_stats();
    out << ",\"neon_requested\":" << (pc_.cpu.ether_next_enabled() ? "true" : "false")
        << ",\"arm64_host\":" << (pc_.cpu.ether_next_native_host() ? "true" : "false")
        << ",\"neon_guest_instructions\":" << ether.native_instructions
        << ",\"cache_hits\":" << ether.cache.hits << ",\"cache_misses\":" << ether.cache.misses
        << ",\"cache_fallbacks\":" << ether.cache.fallbacks;
#endif
    out << '}';
    out << ",\"ether_arm64\":";
    write_ether_arm64(out, pc_.cpu);
    if (detailed) {
      if (const auto& hotspots = pc_.boot_hotspots()) {
        out << ",\"boot_hotspots\":";
        hotspots->write_json(out);
      }
      if (const auto& hotspots = pc_.boot_scalar_hotspots()) {
        out << ",\"boot_scalar_hotspots\":";
        hotspots->write_json(out);
      }
      if (boot_profile_) {
        out << ",\"boot_profile_phase\":" << restarts_
            << ",\"boot_profile_session_start_steps\":" << previous_boot_steps_;
        out << ",\"boot_profile\":";
        boot_profile_->write_json(out, pc_, *board_);
        out << ",\"boot_time_samples\":";
        pc_.boot_timing()->write_json(out);
        out << ",\"boot_code_regions\":";
        pc_.boot_regions()->write_json(out);
        if (previous_boot_profile_)
          out << ",\"previous_boot_profile\":" << *previous_boot_profile_;
      }
    }
    const auto& mode = pc_.boot_mode_progress();
    out << ",\"boot_progress\":{\"handed_off\":" << (pc_.cpu.instructions ? "true" : "false")
        << ",\"session_steps\":" << session_steps()
        << ",\"checkpoint_interval_steps\":" << (boot_profile_ ? BootProfile::kDefaultCheckpointSteps : 0)
        << ",\"mode_changes\":" << mode.changes << ",\"last_mode_from\":\"" << mode.from
        << "\",\"last_mode_to\":\"" << mode.to << "\",\"last_mode_step\":" << mode.step
        << ",\"restarts\":" << restarts_ << ",\"last_restart\":\"" << last_restart_ << "\"}";
    out << ",\"atapi_cdrom\":" << (board_->optical_attached() ? "true" : "false");
    if (const auto* optical = board_->attached_atapi()) {
      const auto& cd = optical->activity();
      out << ",\"atapi\":{\"packets\":" << cd.packets
          << ",\"read_sectors\":" << cd.read_sectors
          << ",\"rejected_packets\":" << cd.rejected_packets
          << ",\"last_packet\":" << unsigned(cd.last_packet)
          << ",\"last_rejected_packet\":" << unsigned(cd.last_rejected_packet)
          << ",\"last_sense\":" << unsigned(cd.last_sense)
          << ",\"last_asc\":" << unsigned(cd.last_asc)
          << ",\"read_bytes\":" << cd.read_sectors * AtapiCdrom::sector_bytes << '}';
    }
    const auto rtc = board_->rtc_evidence();
    out << ",\"rtc\":{\"selected_register\":" << rtc.selected_register
        << ",\"nmi_masked\":" << (rtc.nmi_masked ? "true" : "false")
        << ",\"register_a\":" << rtc.register_a
        << ",\"register_b\":" << rtc.register_b
        << ",\"register_c\":" << rtc.register_c
        << ",\"irq8_assertions\":" << rtc.irq8_assertions
        << ",\"data_writes\":" << rtc.data_writes
        << "},\"ata\":{\"commands\":" << counts.commands
        << ",\"identify_data_transfers\":" << counts.identify_data_transfers
        << ",\"read_sectors\":" << counts.read_sectors
        << ",\"write_sectors\":" << counts.write_sectors << ",\"flushes\":" << counts.flushes
        << "},\"irq\":{\"ticks\":" << platform.ticks << ",\"delivered\":" << platform.irq_delivered
        << ",\"irq14\":" << platform.ata_irqs << ",\"lapic_delivered\":" << platform.apic_irqs
        << ",\"sci_delivered\":" << platform.sci_irqs
        << ",\"virtual_clock_hz\":" << cosmic_pc::PcMachine::kVirtualCyclesPerSecond
        << ",\"clock_source\":\"" << (board_->active_clock_enabled() ? "host-active-monotonic" : "guest-cycles") << '"'
        << ",\"active_clock_elapsed_ns\":" << board_->active_clock_elapsed_ns()
        << ",\"active_clock_ticks\":" << board_->active_clock_ticks()
        << ",\"halted_with_if\":" << (pc_.cpu.halted && pc_.cpu.flag(cosmic_pc::Cpu::IF) ? "true" : "false")
        << ",\"lapic_global_enabled\":" << (interrupts.global_enabled ? "true" : "false")
        << ",\"lapic_software_enabled\":" << (interrupts.software_enabled ? "true" : "false")
        << ",\"lapic_pending_vector\":" << interrupts.pending
        << ",\"lapic_in_service_vector\":" << interrupts.in_service
        << ",\"lapic_priority\":" << interrupts.priority
        << ",\"lapic_timer_count\":" << interrupts.timer_count
        << ",\"ioapic_select\":" << interrupts.ioapic_select
        << ",\"ioapic_id\":" << interrupts.ioapic_id
        << "},\"vga\":{\"qssd_graphics_serial\":"
        << vga_serial_ << ",\"planned_update_bytes\":" << vga_transfer_bytes_
        << "},\"vbe\":";
    vbe_.write_json(out);
    if (const auto* gpu = board_->attached_gpu()) {
      out << ",\"guest_gpu\":";
      gpu->write_json(out);
    }
    out << ",\"machine\":" << pc_.json() << '}';
    return out.str();
  }

 private:
  static constexpr auto kRamReclaimWindow = std::chrono::milliseconds(3);
  static constexpr auto kRamPrepareWindow = std::chrono::microseconds(750);
  static constexpr std::size_t kPrimeCleanReservePages = (32 * 1024 * 1024) / cosmic_pc::SparseRam::kPageSize;

  static bool high_ram_profile(std::uint64_t physical) noexcept {
    return physical > cosmic_pc::PcMachine::kLegacyPhysicalLimit;
  }

  static std::uint64_t resident_limit(std::uint64_t physical) noexcept {
    return high_ram_profile(physical)
        ? physical : std::min(physical, kMaxResidentBytes);
  }

  static std::uint64_t page_floor(std::uint64_t bytes) noexcept {
    constexpr auto page = cosmic_pc::MemoryFabric::kPageSize;
    return bytes / page * page;
  }

  static std::uint64_t prime_reserve(std::uint64_t surplus, std::uint64_t threshold) noexcept {
    constexpr std::uint64_t pressure_reserve_max = 256ULL << 20;
    const auto pressure_reserve = std::clamp(threshold / 2, kMinResidentBytes, pressure_reserve_max);
    return std::max(surplus / 4, pressure_reserve);
  }

  struct RamPressure {
    struct Preparation {
      std::uint64_t batches = 0, backed_pages = 0, failures = 0, overruns = 0;
      std::uint64_t compact_up_to_2k = 0, compact_up_to_3k = 0, dense_pages = 0, released_bytes = 0;
      std::chrono::steady_clock::duration max_time{};
    } preparation;
    std::uint64_t target = 0;
    std::uint64_t available = 0, threshold = 0, reserve = 0;
    bool low_memory = false;
    bool background_trim = false;
    cosmic_pc::SparseRam::BudgetAdjustment status = cosmic_pc::SparseRam::BudgetAdjustment::Applied;
    std::string error;
    std::chrono::steady_clock::duration maintenance_time{}, last_maintenance{}, max_maintenance{};
    std::uint64_t maintenance_batches = 0, reclaimed_bytes = 0, encode_ns = 0;
    std::uint64_t last_released_bytes = 0, released_bytes = 0, released_within_window = 0;
  };

  struct SpillInfo {
    AndroidRamBacking* file = nullptr; // Owned by the same PcMachine's MemoryFabric.
    std::uint64_t limit = 0;
    std::string state = "disabled";
    std::string detail;
  };

  static SpillInfo configure_spill(cosmic_pc::PcMachine& pc, const std::filesystem::path& directory,
                                  bool defer_pressure_packing = false) {
    SpillInfo info;
    if (directory.empty()) return info;
    auto& memory = pc.cpu.memory;
    const auto packed = high_ram_profile(memory.size());
    if (!packed && memory.budget_bytes() >= memory.size()) return info;
    info.limit = std::min(AndroidRamBacking::spill_budget(directory), memory.size());
    if (!info.limit) {
      info.state = "unavailable";
      info.detail = "No hay espacio disponible para el respaldo temporal de RAM.";
      return info;
    }
    try {
      auto file = std::make_unique<AndroidRamBacking>(directory, memory.size(), info.limit, packed,
                                                      defer_pressure_packing);
      auto* file_stats = file.get();
      using CachePolicy = cosmic_pc::SparseRam::BackingCachePolicy;
      if (packed) memory.attach_page_backing(std::move(file), CachePolicy::Adaptive, kPrimeCleanReservePages);
      else memory.attach_page_backing(std::move(file), CachePolicy::Fixed);
      info.file = file_stats;
      info.state = "file";
    } catch (const std::exception& error) {
      info.state = "unavailable";
      info.detail = error.what();
      info.limit = 0;
    }
    return info;
  }

  static void write_ram_profile(std::ostream& out, const cosmic_pc::PcMachine& pc,
                                const SpillInfo& spill, const RamPressure* pressure = nullptr) {
    const auto metrics = pc.cpu.memory.metrics();
    const auto compaction = pc.ram_compaction_stats();
    const auto codec = spill.file ? spill.file->codec_stats() : AndroidRamBacking::CodecStats{};
    const auto deferred = spill.file ? spill.file->staging_stats() : AndroidRamBacking::StagingStats{};
    const auto usable = pc.usable_ram_bytes();
    const auto resident_cap = std::min(usable, metrics.budget_bytes);
    const auto raw_backing_capacity = resident_cap + std::min(usable - resident_cap, spill.limit);
    using Adjustment = cosmic_pc::SparseRam::BudgetAdjustment;
    out << "{\"address_space_bytes\":" << pc.cpu.memory.size()
        << ",\"usable_bytes\":" << usable
        << ",\"backing_budget_bytes\":" << pc.cpu.memory.budget_bytes()
        << ",\"raw_backing_capacity_bytes\":" << raw_backing_capacity
        << ",\"resident_policy\":\""
        << (high_ram_profile(pc.cpu.memory.size())
            ? "android-surplus" : "android-half-surplus") << "\""
        << ",\"resident_bytes\":" << metrics.allocated_bytes
        << ",\"resident_metadata_bytes\":" << metrics.metadata_bytes
        << ",\"page_state_vectors\":" << metrics.page_state_vectors
        << ",\"pages_per_state_vector\":" << cosmic_pc::SparseRam::kPagesPerTable
        << ",\"pattern_pages\":" << metrics.pattern_pages
        << ",\"sparse_word_pages\":" << metrics.sparse_word_pages
        << ",\"pattern_delta_pages\":" << metrics.pattern_delta_pages
        << ",\"pattern_delta_bytes\":" << metrics.pattern_delta_bytes
        << ",\"compaction_passes\":" << compaction.passes
        << ",\"compaction_pages\":" << compaction.pages_compacted
        << ",\"compaction_interval_steps\":" << compaction.interval_steps;
    if (pressure && pressure->target) {
      const auto status = pressure->status == Adjustment::Applied ? "applied" :
                          pressure->status == Adjustment::Reclaiming ? "reclaiming" : "blocked";
      out << ",\"pressure_target_bytes\":" << pressure->target
          << ",\"pressure_status\":" << cosmic_pc::PcMachine::quote(status)
          << ",\"pressure_host_available_bytes\":" << pressure->available
          << ",\"pressure_host_threshold_bytes\":" << pressure->threshold
          << ",\"pressure_host_reserve_bytes\":" << pressure->reserve
          << ",\"pressure_background_trim\":" << (pressure->background_trim ? "true" : "false")
          << ",\"android_low_memory\":" << (pressure->low_memory ? "true" : "false")
          << ",\"pressure_error\":" << cosmic_pc::PcMachine::quote(pressure->error)
          << ",\"pressure_maintenance_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(pressure->maintenance_time).count()
          << ",\"pressure_maintenance_target_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(kRamReclaimWindow).count()
          << ",\"pressure_last_maintenance_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(pressure->last_maintenance).count()
          << ",\"pressure_max_maintenance_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(pressure->max_maintenance).count()
          << ",\"pressure_maintenance_batches\":" << pressure->maintenance_batches
          << ",\"pressure_spill_encode_ns\":" << pressure->encode_ns
          << ",\"pressure_reclaimed_bytes\":" << pressure->reclaimed_bytes
          << ",\"pressure_last_released_resident_bytes\":" << pressure->last_released_bytes
          << ",\"pressure_released_resident_bytes\":" << pressure->released_bytes
          << ",\"pressure_within_window_released_bytes\":" << pressure->released_within_window
          << ",\"reclaim_preparation\":{\"batches\":" << pressure->preparation.batches
          << ",\"backed_pages\":" << pressure->preparation.backed_pages
          << ",\"released_bytes\":" << pressure->preparation.released_bytes
          << ",\"compact_up_to_2k\":" << pressure->preparation.compact_up_to_2k
          << ",\"compact_2k_to_3k\":" << pressure->preparation.compact_up_to_3k
          << ",\"dense_pages\":" << pressure->preparation.dense_pages
          << ",\"failures\":" << pressure->preparation.failures
          << ",\"overruns\":" << pressure->preparation.overruns
          << ",\"window_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(kRamPrepareWindow).count()
          << ",\"max_us\":"
          << std::chrono::duration_cast<std::chrono::microseconds>(pressure->preparation.max_time).count()
          << '}';
    }
    out
        << ",\"spill_mode\":" << cosmic_pc::PcMachine::quote(spill.state)
        << ",\"spill_backend\":" << (spill.file ? "\"qssd-transient\"" : "null")
        << ",\"spill_error\":" << cosmic_pc::PcMachine::quote(spill.detail)
        << ",\"spill_limit_bytes\":" << spill.limit
        << ",\"spilled_bytes\":" << metrics.backing_bytes
        << ",\"spill_file_payload_bytes\":" << (spill.file ? spill.file->file_payload_bytes() : 0)
        << ",\"spill_compression\":" << (spill.file && spill.file->compression_enabled() ? "true" : "false")
        << ",\"spill_packed_pages\":" << (spill.file ? spill.file->packed_pages() : 0)
        << ",\"spill_reads\":" << metrics.backing_reads
        << ",\"spill_writes\":" << metrics.backing_writes
        << ",\"spill_write_calls\":" << (spill.file ? spill.file->write_calls() : 0)
        << ",\"spill_grouped_batches\":" << (spill.file ? spill.file->write_batches() : 0)
        << ",\"spill_grouped_pages\":" << (spill.file ? spill.file->grouped_pages() : 0)
        << ",\"spill_deferred_enabled\":" << (deferred.enabled ? "true" : "false")
        << ",\"spill_deferred_staged_pages\":" << deferred.staged_pages
        << ",\"spill_deferred_pending_pages\":" << deferred.pending_pages
        << ",\"spill_deferred_packed_pages\":" << deferred.packed_pages
        << ",\"spill_deferred_failures\":" << deferred.failures
        << ",\"spill_deferred_yielded_batches\":" << deferred.yielded_batches
        << ",\"spill_deferred_background_encode_ns\":" << deferred.background_encode_ns
        << ",\"spill_deferred_foreground_settled_pages\":" << deferred.foreground_settled_pages
        << ",\"spill_clean_resident_pages\":" << metrics.backing_clean_pages
        << ",\"spill_clean_reclaims\":" << metrics.backing_clean_reclaims
        << ",\"spill_cache_hits\":" << metrics.backing_cache_hits
        << ",\"spill_cache_bytes\":" << metrics.backing_cache_bytes
        << ",\"spill_cache_capacity_bytes\":" << metrics.backing_cache_capacity_bytes
        << ",\"spill_overwrite_pages\":" << metrics.backing_overwrite_pages
        << ",\"spill_read_us\":" << (spill.file ? spill.file->read_us() : 0)
        << ",\"spill_write_us\":" << (spill.file ? spill.file->write_us() : 0)
        << ",\"spill_codec\":{\"encode_attempts\":" << codec.encode_attempts
        << ",\"encode_packed\":" << codec.encode_packed
        << ",\"encode_raw\":" << codec.encode_raw
        << ",\"encode_bypassed\":" << codec.encode_bypassed
        << ",\"encode_sparse\":" << codec.encode_sparse
        << ",\"encode_deflate\":" << codec.encode_deflate
        << ",\"decode_attempts\":" << codec.decode_attempts
        << ",\"decode_sparse\":" << codec.decode_sparse
        << ",\"encode_wall_ns\":" << codec.encode.wall_ns
        << ",\"decode_wall_ns\":" << codec.decode.wall_ns << "}}";
  }

  static void check_ata_identify(cosmic_pc::PcMachine& pc, QssdDisk& volume) {
    constexpr std::uint16_t code = 0x1000, data = 0x1800;
    constexpr std::uint8_t identify_probe[]{
        0xfa, 0xfc,                        // CLI; CLD
        0xba,0xf6,0x01, 0xb0,0xe0, 0xee, // Select primary master.
        0x42, 0xb0,0xec, 0xee, 0xec,      // Issue IDENTIFY DEVICE and read status.
        0x24,0x89, 0x3c,0x08, 0x75,0x0b, // Require DRQ, no BSY or ERR.
        0xba,0xf0,0x01,                   // DX = ATA data register.
        0xbf,0x00,0x18, 0xb9,0x00,0x01, // DI = data; CX = 256 words.
        0xf3,0x6d,                       // REP INSW from the real ATA device.
        0xf4};                            // Halt; only the full response counts.
    PcMotherboardC06 board(pc, &volume);
    auto& cpu = pc.cpu;
    cpu.reset(0, code);
    cpu.memory.write_block(code, identify_probe, sizeof(identify_probe));
    pc.run(300);
    const auto counts = board.attached_ata()->counters();
    if (cpu.fault || !cpu.halted || counts.identify_data_transfers != 1 ||
        (cpu.read16(data + 83 * 2) & 0xc400U) != 0x4400U ||
        cpu.read64(data + 100 * 2) != volume.sectors())
      throw std::runtime_error("El invitado de prueba no confirmó la capacidad ATA del QSSD.");
  }

  void start_boot_profile() {
    pc_.enable_boot_hotspots();
    pc_.enable_boot_timing();
    boot_profile_.emplace(BootProfile::kDefaultCheckpointSteps);
    boot_profile_->begin(pc_, *board_);
  }

  std::string completed_boot_profile() const {
    std::ostringstream out;
    out << "{\"source\":\"" << source_name() << "\",\"phase\":" << restarts_
        << ",\"session_start_steps\":" << previous_boot_steps_
        << ",\"boot_profile\":";
    boot_profile_->write_json(out, pc_, *board_);
    out << ",\"boot_time_samples\":";
    pc_.boot_timing()->write_json(out);
    out << ",\"boot_code_regions\":";
    pc_.boot_regions()->write_json(out);
    out << ",\"ether_arm64\":";
    write_ether_arm64(out, pc_.cpu);
    out << '}';
    return out.str();
  }

  void restart_guest() {
    using Restart = cosmic_pc::PcMachine::GuestRestart;
    switch (pc_.guest_restart()) {
      case Restart::A20Port: last_restart_="port92"; break;
      case Restart::KeyboardController: last_restart_="i8042"; break;
      case Restart::BiosBootstrap: last_restart_="int19"; break;
      case Restart::None: return;
    }
    try {
      flush();
      std::array<std::uint8_t,512> sector{};
      if (!disk_ || !disk_->read(0,1,sector.data()))
        throw std::runtime_error("No se pudo leer el QSSD durante el reinicio.");
      const bool bootable = sector[510]==0x55 && sector[511]==0xaa &&
          std::any_of(sector.begin(),sector.begin()+440,[](std::uint8_t byte) { return byte!=0; });
      const auto next = bootable ? BootSource::Disk : BootSource::Iso;
      if (next==BootSource::Iso && !iso_)
        throw std::runtime_error("El QSSD no contiene un sector de arranque.");
      if (next!=source_ || pc_.cpu.instructions>=65536) rapid_restarts_=0;
      else if (++rapid_restarts_>3)
        throw std::runtime_error("El medio solicitó reinicios repetidos al arrancar.");

      auto completed = boot_profile_ ? std::optional<std::string>(completed_boot_profile()) : std::nullopt;
      if (next==BootSource::Disk) {
        pc_.mount([this](auto off, auto* into, auto count) {
          return disk_->read_bytes(off,into,count);
        },disk_->bytes(),"raw");
      } else {
        pc_.mount([reader=iso_](auto off, auto* into, auto count) {
          return reader->read(off,into,count);
        },iso_->size(),"iso");
      }
      source_=next;
      ++restarts_;
      key_routes_.fill(KeyRoute::None);
      text_visible_=false;
      if (boot_profile_) {
        previous_boot_profile_ = std::move(completed);
        boot_profile_->begin(pc_, *board_);
      }
    } catch (const std::exception& error) { pc_.host_error=error.what(); }
  }
  class OpticalReader {
   public:
    OpticalReader(int fd, std::uint64_t expected_size) {
      if (fd < 0) throw std::invalid_argument("Selecciona una ISO local.");
      const int mode = ::fcntl(fd, F_GETFL);
      if (mode == -1 || (mode & O_ACCMODE) != O_RDONLY)
        throw std::invalid_argument("El descriptor de la ISO debe ser de solo lectura.");
      const int owned = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
      if (owned < 0) throw std::system_error(errno, std::generic_category(), "Duplicar ISO");
      fd_ = owned;
      try {
        struct stat info {};
        if (::fstat(fd_, &info) != 0)
          throw std::system_error(errno, std::generic_category(), "Consultar ISO");
        if (info.st_size > 0) size_ = static_cast<std::uint64_t>(info.st_size);
        else size_ = expected_size;
        std::uint8_t first = 0;
        if (!size_ || size_ > std::uint64_t(std::numeric_limits<off_t>::max()) ||
            (info.st_size > 0 && expected_size && expected_size != size_) ||
            !read(0, &first, 1))
          throw std::invalid_argument("La ISO necesita un archivo local con tamaño y lectura aleatoria.");
      } catch (...) {
        ::close(fd_);
        throw;
      }
    }
    OpticalReader(const OpticalReader&) = delete;
    OpticalReader& operator=(const OpticalReader&) = delete;
    ~OpticalReader() { ::close(fd_); }
    std::uint64_t size() const noexcept { return size_; }
    std::uint64_t read_us() const noexcept {
      return std::chrono::duration_cast<std::chrono::microseconds>(read_time_).count();
    }
    bool read(std::uint64_t offset, std::uint8_t* target, std::size_t count) const {
      if (offset > size_ || count > size_ - offset) return false;
      const auto started = std::chrono::steady_clock::now();
      while (count) {
        const auto n = ::pread(fd_, target, count, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        const auto got = static_cast<std::size_t>(n);
        target += got;
        count -= got;
        offset += got;
      }
      read_time_ += std::chrono::steady_clock::now() - started;
      return true;
    }
   private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
    mutable std::chrono::steady_clock::duration read_time_{};
  };

  const char* source_name() const noexcept {
    switch (source_) {
      case BootSource::Iso: return "iso";
      case BootSource::Disk: return "qssd";
      case BootSource::SelfTest: return "self-test";
    }
    return "unknown";
  }

  enum class KeyRoute : std::uint8_t { None, Bios, Ps2 };
  BootSource source_;
  const bool use_active_guest_clock_;
  std::array<KeyRoute, 512> key_routes_{};
  std::optional<BootProfile> boot_profile_;
  std::optional<std::string> previous_boot_profile_;
  cosmic_pc::PcMachine pc_;
  SpillInfo spill_;
  RamPressure pressure_;
  std::shared_ptr<OpticalReader> iso_;
  std::unique_ptr<QssdDisk> disk_;
  std::unique_ptr<QssdDisk> data_disk_;
  std::unique_ptr<BiosHardDisk> bios_disk_;
  std::unique_ptr<PcMotherboardC06> board_;
  GuestVgaFabricC09R vga_;
  GuestVbeFabric vbe_;
  std::array<std::uint8_t, 4000> presented_text_{};
  bool text_visible_ = false;
  std::uint64_t vga_serial_ = 0;
  std::uint64_t vga_transfer_bytes_ = 0;
  std::uint64_t initial_disk_commits_ = 0;
  std::uint64_t restarts_ = 0;
  std::uint64_t previous_boot_steps_ = 0;
  unsigned rapid_restarts_ = 0;
  const char* last_restart_ = "";
};
} // namespace reverse_windows
