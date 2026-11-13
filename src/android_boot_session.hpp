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
      pc_.cpu.write8(0x475, 1); // BIOS data area hard-disk count even when booted from CD.
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
#if defined(MUNDO_X_PC_ETHER_NEXT)
    pc_.cpu.enable_ether_next();
#endif
  }

  static void create_disk(const std::filesystem::path& disk, unsigned gib) {
    QssdDisk::create(disk, gib);
  }

  Step step(unsigned budget) {
    if (!is_stopped()) pc_.run(budget);
    if (pc_.cpu.fault || !pc_.host_error.empty()) return Step::Fault;
    if (pc_.waiting_key) return Step::WaitingKey;
    if (pc_.cpu.halted || board_->evidence().powered_off) return Step::Halted;
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
    out << ",\"atapi_cdrom\":" << (board_->optical_attached() ? "true" : "false")
        << ",\"ata\":{\"commands\":" << counts.commands
        << ",\"read_sectors\":" << counts.read_sectors
        << ",\"write_sectors\":" << counts.write_sectors << ",\"flushes\":" << counts.flushes
        << "},\"irq\":{\"ticks\":" << platform.ticks << ",\"delivered\":" << platform.irq_delivered
        << ",\"irq14\":" << platform.ata_irqs << "},\"vga\":{\"qssd_graphics_serial\":"
        << vga_serial_ << ",\"planned_update_bytes\":" << vga_transfer_bytes_
        << "},\"machine\":" << pc_.json() << '}';
    return out.str();
  }

 private:
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
  cosmic_pc::PcMachine pc_{2048ULL * 1024 * 1024, 256ULL * 1024 * 1024};
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
};
} // namespace reverse_windows
