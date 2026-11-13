#pragma once

#include "bios_hard_disk.hpp"
#include "c06_motherboard.hpp"
#include "c09r_guest_vga_fabric.hpp"
#include "qssd_disk.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace reverse_windows {

class AndroidBootSession final {
 public:
  enum class BootSource { Iso=1, Disk=2, SelfTest=3 };
  enum class Step { Running=0, WaitingKey=1, Halted=2, Fault=3 };

  AndroidBootSession(std::filesystem::path disk, unsigned create_gib,
                     BootSource source, int iso_fd=-1, std::uint64_t iso_bytes=0)
      : source_(source), vga_(pc_) {
    if (disk.empty() && (create_gib || source != BootSource::SelfTest))
      throw std::invalid_argument("Selecciona o crea un disco QSSD para iniciar.");
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
    if (source == BootSource::Disk) {
      pc_.mount([this](auto offset, auto* target, auto count) {
        return disk_->read_bytes(offset, target, count);
      }, disk_->bytes(), "raw");
    }
    if (disk_) {
      bios_disk_ = std::make_unique<BiosHardDisk>(pc_, *disk_);
    }
    if (iso_) {
      board_ = std::make_unique<PcMotherboardC06>(pc_, disk_.get(),
          [reader = iso_](auto offset, auto* target, auto count) {
            return reader->read(offset, target, count);
          }, iso_->size());
    } else {
      board_ = std::make_unique<PcMotherboardC06>(pc_, disk_.get());
    }
    board_->enable_ps2_keyboard();
    if (source != BootSource::SelfTest) pc_.enable_guest_restart();
#if defined(MUNDO_X_PC_ETHER_NEXT)
    pc_.cpu.enable_ether_next();
#endif
  }

  static void create_disk(const std::filesystem::path& disk, unsigned gib) {
    QssdDisk::create(disk, gib);
  }

  static std::string check_hardware(const std::filesystem::path& disk, int iso_fd=-1,
                                    std::uint64_t iso_bytes=0, const std::string& iso_error={}) {
    cosmic_pc::PcMachine pc(kRamBytes, kRamBudgetBytes);
    pc.boot_demo();
    auto& cpu = pc.cpu;
    const auto cpuid = [&](std::uint32_t leaf) {
      constexpr unsigned address = 0x1000;
      cpu.reset(0, address);
      cpu.write16(address, 0xa20f); // CPUID
      cpu.regs[cosmic_pc::Cpu::EAX] = leaf;
      if (cpu.step() != cosmic_pc::StepResult::Executed)
        throw std::runtime_error("No se pudo consultar CPUID.");
      return cpu.regs;
    };
    const auto basic = cpuid(1), extended = cpuid(0x80000001U);
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
    bool pci_ide = false;
    {
      const std::array<std::uint8_t,2048> optical_sector{};
      PcMotherboardC06 board(pc, nullptr,
          [&](auto offset, auto* data, auto count) {
            if (offset > optical_sector.size() || count > optical_sector.size() - offset) return false;
            std::copy_n(optical_sector.data()+offset, count, data);
            return true;
          }, optical_sector.size());
      // Exercise guest I/O against the same motherboard used for ISO boots.
      constexpr std::array<std::uint8_t,16> pci_probe{{
          0x66,0xb8,0x08,0x10,0x00,0x80,0xba,0xf8,0x0c,0x66,0xef,
          0xba,0xfc,0x0c,0x66,0xed}};
      cpu.reset(0, 0x1000);
      cpu.memory.write_block(0x1000, pci_probe.data(), pci_probe.size());
      pci_ide = true;
      for (unsigned i=0;i<5 && pci_ide;++i) pci_ide = cpu.step() == cosmic_pc::StepResult::Executed;
      pci_ide = pci_ide && (cpu.regs[cosmic_pc::Cpu::EAX] >> 16) == 0x0101;
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
        if (address>kRamBytes || plan.image.size()>kRamBytes-address)
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
        << ",\"ram\":" << ram << ",\"pci_ide\":" << pci_ide << ",\"vga\":" << vga << ",\"iso_boot\":";
    if (has_iso) out << optical_issue.empty(); else out << "null";
    out << ",\"qssd\":";
    if (has_disk) out << disk_issue.empty(); else out << "null";
    out << ",\"qssd_ata\":";
    if (!has_disk || !disk_issue.empty()) out << "null"; else out << ata_issue.empty();
    out << "},\"ready_for_windows10_install\":false,\"errors\":{"
        << "\"iso_boot\":" << cosmic_pc::PcMachine::quote(optical_issue)
        << ",\"qssd\":" << cosmic_pc::PcMachine::quote(disk_issue)
        << ",\"qssd_ata\":" << cosmic_pc::PcMachine::quote(ata_issue) << "}}";
    return out.str();
  }

  Step step(unsigned budget) {
    if (!is_stopped()) pc_.run(budget);
    if (!pc_.cpu.fault && pc_.host_error.empty() &&
        pc_.guest_restart() != cosmic_pc::PcMachine::GuestRestart::None) restart_guest();
    if (pc_.cpu.fault || !pc_.host_error.empty()) return Step::Fault;
    if (pc_.waiting_key) return Step::WaitingKey;
    if (is_stopped()) return Step::Halted;
    return Step::Running;
  }

  bool is_stopped() const noexcept {
    return board_->evidence().powered_off ||
        (pc_.cpu.halted && !pc_.cpu.flag(cosmic_pc::Cpu::IF));
  }

  void key(unsigned ascii, unsigned scancode, bool pressed) {
    if (ascii > 255 || (scancode > 255 && (scancode & ~255U) != 0xe000U))
      throw std::invalid_argument("Código de teclado BIOS inválido.");
    if (board_->evidence().powered_off) return;
    if (pc_.cpu.is_real_mode()) {
      const auto scan = static_cast<std::uint8_t>(scancode);
      const bool modifier = scan == 0x2a || scan == 0x36 || scan == 0x1d || scan == 0x38;
      if (pressed && (ascii || (scancode && !modifier)))
        pc_.key(static_cast<std::uint8_t>(ascii), scan);
    } else if (scancode) {
      (void)board_->inject_ps2_key(scancode, pressed);
    }
  }

  void flush() {
    if (disk_ && !disk_->flush()) throw std::runtime_error("No se pudo sincronizar QSSD.");
  }

  cosmic_pc::PcVideoInfo video_info() const noexcept { return pc_.video_info(); }

  std::size_t copy_text(std::uint8_t* target, std::size_t capacity) {
    if (!target || capacity < presented_text_.size())
      throw std::invalid_argument("Buffer VGA insuficiente.");
    if (pc_.video_info().graphics) {
      text_visible_ = false;
      return 0;
    }
    const auto frame = vga_.capture();
    const bool refresh = !text_visible_ || frame.changed_cells;
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
    if (!refresh) return 0;
    std::memcpy(target, presented_text_.data(), presented_text_.size());
    return presented_text_.size();
  }

  void copy_graphics(std::uint32_t* target, std::size_t capacity,
                     unsigned width, unsigned height) {
    const auto info = pc_.video_info();
    if (!info.graphics || !target || !width || !height ||
        width > info.width || height > info.height || width > capacity / height)
      throw std::invalid_argument("Tamaño de presentación VBE inválido.");
    text_visible_ = false;
    video_row_.resize(std::size_t(info.width) * 4);
    for (unsigned y = 0; y < height; ++y) {
      const auto source_y = y * info.height / height;
      pc_.copy_video_rgba(0, source_y, info.width, 1, video_row_.data(), video_row_.size());
      for (unsigned x = 0; x < width; ++x) {
        const auto i = std::size_t(x * info.width / width) * 4;
        target[std::size_t(y) * width + x] = 0xff000000U |
            (std::uint32_t(video_row_[i]) << 16U) |
            (std::uint32_t(video_row_[i + 1]) << 8U) | video_row_[i + 2];
      }
    }
  }

  std::string diagnostic() const {
    const auto platform = board_->evidence();
    const auto status = platform.powered_off ? "powered_off" : pc_.status();
    const auto* ata = board_->attached_ata();
    const auto counts = ata ? ata->counters() : AtaC07::Counters{};
    std::ostringstream out;
    out << "{\"schema\":\"reverse-windows-android-session-v1\",\"source\":\""
        << source_name() << "\",\"status\":\"" << status << "\",\"iso_bytes\":"
        << (iso_ ? iso_->size() : 0) << ",\"input\":\"bios-int16+ps2-irq1\",\"disk\":";
    if (disk_) {
      const auto stats = disk_->stats();
      out << "{\"logical_bytes\":" << stats.logical_bytes
          << ",\"physical_bytes\":" << stats.physical_bytes
          << ",\"commits\":" << stats.commits << ",\"generation\":" << disk_->generation() << '}';
    } else {
      out << "null";
    }
    const auto gate_work = pc_.quantum_session->gate_work_stats();
    out << ",\"ether_next\":{\"quantum_gates_in\":" << gate_work.input_gates
        << ",\"quantum_gates_run\":" << gate_work.executed_gates
        << ",\"quantum_gates_cancelled\":" << gate_work.cancelled_gates
        << ",\"pullback_cache_hits\":" << gate_work.pullback_hits;
#if defined(MUNDO_X_PC_ETHER_NEXT)
    const auto ether = pc_.cpu.ether_next_stats();
    out << ",\"neon_requested\":" << (pc_.cpu.ether_next_enabled() ? "true" : "false")
        << ",\"arm64_host\":" << (pc_.cpu.ether_next_native_host() ? "true" : "false")
        << ",\"neon_guest_instructions\":" << ether.native_instructions
        << ",\"cache_hits\":" << ether.cache.hits << ",\"cache_misses\":" << ether.cache.misses
        << ",\"cache_fallbacks\":" << ether.cache.fallbacks;
#endif
    out << '}';
    const auto& mode = pc_.boot_mode_progress();
    out << ",\"boot_progress\":{\"handed_off\":" << (pc_.cpu.instructions ? "true" : "false")
        << ",\"mode_changes\":" << mode.changes << ",\"last_mode_from\":\"" << mode.from
        << "\",\"last_mode_to\":\"" << mode.to << "\",\"last_mode_step\":" << mode.step
        << ",\"restarts\":" << restarts_ << ",\"last_restart\":\"" << last_restart_ << "\"}";
    out << ",\"atapi_cdrom\":" << (board_->optical_attached() ? "true" : "false")
        << ",\"ata\":{\"commands\":" << counts.commands
        << ",\"identify_data_transfers\":" << counts.identify_data_transfers
        << ",\"read_sectors\":" << counts.read_sectors
        << ",\"write_sectors\":" << counts.write_sectors << ",\"flushes\":" << counts.flushes
        << "},\"irq\":{\"ticks\":" << platform.ticks << ",\"delivered\":" << platform.irq_delivered
        << ",\"irq14\":" << platform.ata_irqs << "},\"vga\":{\"qssd_graphics_serial\":"
        << vga_serial_ << ",\"planned_update_bytes\":" << vga_transfer_bytes_
        << "},\"machine\":" << pc_.json() << '}';
    return out.str();
  }

 private:
  static constexpr std::uint64_t kRamBytes = 2048ULL*1024*1024;
  static constexpr std::uint64_t kRamBudgetBytes = 256ULL*1024*1024;

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
      text_visible_=false;
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
    bool read(std::uint64_t offset, std::uint8_t* target, std::size_t count) const {
      if (offset > size_ || count > size_ - offset) return false;
      while (count) {
        const auto n = ::pread(fd_, target, count, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        const auto got = static_cast<std::size_t>(n);
        target += got;
        count -= got;
        offset += got;
      }
      return true;
    }
   private:
    int fd_ = -1;
    std::uint64_t size_ = 0;
  };

  const char* source_name() const noexcept {
    switch (source_) {
      case BootSource::Iso: return "iso";
      case BootSource::Disk: return "qssd";
      case BootSource::SelfTest: return "self-test";
    }
    return "unknown";
  }

  BootSource source_;
  cosmic_pc::PcMachine pc_{kRamBytes,kRamBudgetBytes};
  std::shared_ptr<OpticalReader> iso_;
  std::unique_ptr<QssdDisk> disk_;
  std::unique_ptr<BiosHardDisk> bios_disk_;
  std::unique_ptr<PcMotherboardC06> board_;
  GuestVgaFabricC09R vga_;
  std::array<std::uint8_t, 4000> presented_text_{};
  std::vector<std::uint8_t> video_row_;
  bool text_visible_ = false;
  std::uint64_t vga_serial_ = 0;
  std::uint64_t vga_transfer_bytes_ = 0;
  std::uint64_t restarts_ = 0;
  unsigned rapid_restarts_ = 0;
  const char* last_restart_ = "";
};
} // namespace reverse_windows
