#pragma once

#include "graphics_memory.hpp"
#include "guest_triangle_work.hpp"
#include "pc_machine.hpp"
#include "qssd/graphics_work_state.hpp"
#include "relaciones_gpu.hpp"
#ifdef REVERSE_WINDOWS_GLES_GUEST
#include "regraphics/guest_renderer.hpp"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

namespace reverse_windows {

class GuestGpuC08 {
 public:
  static constexpr unsigned pci_device = 3;
  static constexpr std::uint32_t pci_id = (RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID;
  // The board reserves this entire range in E820, including on a 4-GiB guest.
  static constexpr std::uint32_t pci_window_start = RWGPU_BAR_DEFAULT;
  static constexpr std::uint32_t pci_window_bytes = RWGPU_BAR_WINDOW_BYTES;
  static constexpr std::uint32_t pci_window_end = pci_window_start + pci_window_bytes;
  static_assert(pci_window_start == cosmic_pc::PcMachine::kPciMmioBase &&
                pci_window_bytes == cosmic_pc::PcMachine::kPciMmioBytes &&
                pci_window_end <= cosmic_pc::PcMachine::kIoApicAddress);

  GuestGpuC08(cosmic_pc::PcMachine& machine, const std::filesystem::path& vram)
      : pc_(machine), memory_(vram) {}

  void reset() noexcept {
    pci_command_ = 0;
    bar_ = pci_window_start;
    params_.fill(0);
    direct_params_.fill(0);
    status_ = RWGPU_IDLE;
    completed_ = 0;
    result_ = 0;
    pending_ = {};
    probe_bar_ = false;
    stage_.clear();
    memory_.reset();
    resident_download_bytes_ = 0;
    gpu_ns_.reset();
    renderer_name_.clear();
    hardware_ = false;
    host_ns_ = backend_ns_ = 0;
    source_triangles_ = drawn_triangles_ = vertex_bytes_ = presented_bytes_ = 0;
#ifdef REVERSE_WINDOWS_GLES_GUEST
    color_.clear();
#endif
  }

  std::uint32_t config_word(unsigned offset) const noexcept {
    switch (offset) {
      case 0: return pci_id;
      case 4: return pci_command_;
      case 8: return 0x03020001; // 3D controller, independent of the boot display.
      case 0x10: return probe_bar_ ? ~(RWGPU_BAR_BYTES - 1) : bar_;
      case 0x2c: return pci_id;
      default: return 0;
    }
  }
  bool config_write(unsigned offset, unsigned bytes, std::uint32_t value) noexcept {
    const auto old_bar = bar_, old_decode = pci_command_;
    const bool old_probe = probe_bar_;
    if (offset == 4) pci_command_ = value & 6U; // Memory Space and Bus Master.
    if (offset == 0x10 && bytes == 4 && !(pci_command_ & 2U)) {
      probe_bar_ = value == UINT32_MAX;
      if (!probe_bar_) {
        const auto base = value & ~(RWGPU_BAR_BYTES - 1);
        bar_ = (base >= pci_window_start && base <= pci_window_end - RWGPU_BAR_BYTES) ? base : 0;
      }
    }
    return old_bar != bar_ || old_decode != pci_command_ || old_probe != probe_bar_;
  }
  bool claims(std::uint64_t address, std::size_t bytes = 1) const noexcept {
    return bytes && (pci_command_ & 2) && bar_ && !probe_bar_ &&
        address < std::uint64_t(bar_) + RWGPU_BAR_BYTES &&
        (address >= bar_ || bytes > bar_ - address);
  }
  std::uint8_t read(std::uint64_t address) const noexcept {
    const auto offset = unsigned(address - bar_);
    if (offset >= RWGPU_STAGE_OFFSET && offset - RWGPU_STAGE_OFFSET < stage_.size())
      return stage_[offset - RWGPU_STAGE_OFFSET];
    return std::uint8_t(word(offset & ~3u) >> ((offset & 3) * 8));
  }
  bool write(std::uint64_t address, std::uint8_t data) {
    const auto offset = unsigned(address - bar_);
    if (offset >= RWGPU_STAGE_OFFSET && offset - RWGPU_STAGE_OFFSET < RWGPU_STAGE_BYTES) {
      if (stage_.empty()) {
        try { stage_.resize(RWGPU_STAGE_BYTES); }
        catch (const std::bad_alloc&) { status_ = RWGPU_FAILED; return true; }
      }
      stage_[offset - RWGPU_STAGE_OFFSET] = data;
      pending_ = {};
      return true;
    }
    const auto reg = offset & ~3u;
    if (!command_register(reg) && !direct_register(reg)) {
      pending_ = {};
      return true;
    }
    if (offset % 4 == 0) pending_ = {reg, 0, 0};
    if (reg != pending_.reg || offset % 4 != pending_.written) {
      pending_ = {};
      return true;
    }
    pending_.value |= std::uint32_t(data) << (8 * pending_.written++);
    if (pending_.written == 4) {
      const auto value = pending_.value;
      pending_ = {};
      if (reg == RWGPU_REG_DOORBELL) dispatch(value);
      else if (command_register(reg)) params_[(reg - RWGPU_REG_OFFSET_LO) / 4] = value;
      else direct_params_[(reg - RWGPU_REG_AUX_LO) / 4] = value;
    }
    return true;
  }

  void set_vram_cache_enabled(bool enabled) noexcept {
    memory_.set_cache_enabled(enabled);
    if (!enabled) std::vector<std::uint8_t>().swap(transfer_);
  }
  std::uint64_t vram_journal_bytes() const { return memory_.journal_bytes(); }
  std::uint64_t resident_download_bytes() const noexcept { return resident_download_bytes_; }

  void write_json(std::ostream& out) const {
    out << "{\"logical_vram_bytes\":" << GpuVram::kBytes
        << ",\"qssd_backing_bytes\":" << memory_.journal_bytes()
        << ",\"qssd_vram_commits\":" << memory_.commits()
        << ",\"stage_bytes\":" << stage_.size()
        << ",\"resident_vram_bytes\":" << memory_.resident_bytes()
        << ",\"resident_download_bytes\":" << resident_download_bytes_
        << ",\"uploads\":" << commands_[RWGPU_UPLOAD] << ",\"downloads\":" << commands_[RWGPU_DOWNLOAD]
        << ",\"vram_copies\":" << commands_[RWGPU_COPY] << ",\"index_sums\":" << commands_[RWGPU_SUM_INDICES]
        << ",\"status\":" << status_ << ",\"ticket\":" << completed_
        << ",\"writes\":" << commands_[RWGPU_WRITE] << ",\"reads\":" << commands_[RWGPU_READ]
        << ",\"draw_requests\":" << commands_[RWGPU_DRAW] + commands_[RWGPU_DRAW_VRAM] + commands_[RWGPU_DRAW_RAM]
        << ",\"vram_draw_requests\":" << commands_[RWGPU_DRAW_VRAM]
        << ",\"ram_draw_requests\":" << commands_[RWGPU_DRAW_RAM] << ",\"draws\":" << draws_
        << ",\"color_reads\":" << commands_[RWGPU_READ_COLOR] + commands_[RWGPU_READ_COLOR_RAM]
        << ",\"color_dma_reads\":" << commands_[RWGPU_READ_COLOR_RAM]
        << ",\"source_triangles\":" << source_triangles_ << ",\"submitted_triangles\":" << drawn_triangles_
        << ",\"vertex_bytes\":" << vertex_bytes_ << ",\"presented_bytes\":" << presented_bytes_
        << ",\"graphics_serial\":" << work_.snapshot()->serial
        << ",\"renderer\":" << cosmic_pc::PcMachine::quote(renderer_name_)
        << ",\"hardware_reported\":" << (hardware_ ? "true" : "false")
        << ",\"submit_and_present_ns\":" << host_ns_ << ",\"render_and_readback_ns\":" << backend_ns_
        << ",\"gpu_query_ns\":";
    if (gpu_ns_) out << *gpu_ns_; else out << "null";
    out << '}';
  }

 private:
  struct Partial { unsigned reg = UINT32_MAX, value = 0, written = 0; };
  cosmic_pc::PcMachine& pc_;
  GpuVram memory_;
  xgpu::GuestTriangleWork planner_;
  using RenderPlan = std::shared_ptr<const xgpu::GuestTrianglePlan>;
  qssd::GraphicsWorkMemory<RenderPlan> work_;
  std::array<std::uint64_t, RWGPU_DRAW_RAM + 1> commands_{};
  std::uint64_t draws_ = 0, source_triangles_ = 0, drawn_triangles_ = 0;
  std::uint64_t vertex_bytes_ = 0, presented_bytes_ = 0, host_ns_ = 0, backend_ns_ = 0;
  std::uint64_t result_ = 0, resident_download_bytes_ = 0;
  std::optional<std::uint64_t> gpu_ns_;
  bool hardware_ = false;
  std::string renderer_name_;
  std::array<std::uint32_t, 7> params_{};
  std::array<std::uint32_t, 3> direct_params_{};
  std::uint32_t pci_command_ = 0, bar_ = pci_window_start, status_ = RWGPU_IDLE, completed_ = 0;
  Partial pending_{};
  bool probe_bar_ = false;
  std::vector<std::uint8_t> stage_, transfer_;
#ifdef REVERSE_WINDOWS_GLES_GUEST
  regraphics::GuestRenderer renderer_;
  std::vector<std::uint8_t> bgrx_, color_;
  std::vector<xgpu::GuestVertex> vertices_;
#endif

  static bool command_register(unsigned reg) noexcept {
    return reg >= RWGPU_REG_OFFSET_LO && reg <= RWGPU_REG_DOORBELL;
  }
  static bool direct_register(unsigned reg) noexcept {
    return reg >= RWGPU_REG_AUX_LO && reg <= RWGPU_REG_LENGTH_HI;
  }
  static std::uint64_t wide(std::uint32_t lo, std::uint32_t hi) noexcept {
    return (std::uint64_t(hi) << 32) | lo;
  }

  std::uint32_t word(unsigned reg) const noexcept {
    switch (reg) {
      case RWGPU_REG_MAGIC: return RWGPU_MAGIC;
      case RWGPU_REG_ABI: return RWGPU_ABI;
      case RWGPU_REG_LOGICAL_LO: return static_cast<std::uint32_t>(GpuVram::kBytes);
      case RWGPU_REG_LOGICAL_HI: return static_cast<std::uint32_t>(GpuVram::kBytes >> 32);
      case RWGPU_REG_CAPS:
        return RWGPU_CAP_VRAM | RWGPU_CAP_DIRECT_VRAM | RWGPU_CAP_SUM_INDICES
#ifdef REVERSE_WINDOWS_GLES_GUEST
            | RWGPU_CAP_TRIANGLES | RWGPU_CAP_DRAW_VRAM | RWGPU_CAP_COLOR_DMA | RWGPU_CAP_DRAW_RAM
#endif
            ;
      case RWGPU_REG_STATUS: return status_;
      case RWGPU_REG_COMPLETED: return completed_;
      case RWGPU_REG_FORMAT: return sizeof(RwGpuVertex);
      case RWGPU_REG_MODE_WIDTH: return pc_.video_info().graphics ? pc_.video_info().width : 0;
      case RWGPU_REG_MODE_HEIGHT: return pc_.video_info().graphics ? pc_.video_info().height : 0;
      case RWGPU_REG_VRAM_BATCH_BYTES: return RWGPU_VRAM_BATCH_BYTES;
      case RWGPU_REG_DIRECT_MAX: return GpuVram::kTransferBytes;
      case RWGPU_REG_BUS_MASTER: return (pci_command_ & 4U) != 0;
      case RWGPU_REG_RESULT_LO: return std::uint32_t(result_);
      case RWGPU_REG_RESULT_HI: return std::uint32_t(result_ >> 32);
      default:
        if (command_register(reg) && reg != RWGPU_REG_DOORBELL)
          return params_[(reg - RWGPU_REG_OFFSET_LO) / 4];
        return direct_register(reg) ? direct_params_[(reg - RWGPU_REG_AUX_LO) / 4] : 0;
    }
  }

  void dispatch(std::uint32_t op) noexcept {
    const auto ticket = params_.back();
    auto next_ticket = completed_ + 1;
    if (!next_ticket) next_ticket = 1;
    if (ticket != next_ticket) { status_ = RWGPU_INVALID; return; }
    result_ = 0;
    try {
      if (op >= RWGPU_WRITE && op <= RWGPU_DRAW_RAM) ++commands_[op];
      const auto offset = wide(params_[0], params_[1]);
      const auto auxiliary = wide(direct_params_[0], direct_params_[1]);
      const auto count = wide(params_[2], direct_params_[2]);
      switch (op) {
        case RWGPU_WRITE: case RWGPU_READ: status_ = staged_transfer(op, offset, params_[2]); break;
        case RWGPU_DRAW: status_ = draw(op, offset, params_[2], 0); break;
        case RWGPU_DRAW_VRAM: case RWGPU_DRAW_RAM:
          status_ = count > UINT32_MAX ? unsigned(RWGPU_INVALID) : draw(op, auxiliary, params_[2], offset);
          break;
        case RWGPU_READ_COLOR: status_ = read_color(offset, params_[2]); break;
        case RWGPU_READ_COLOR_RAM: status_ = read_color_ram(offset, auxiliary, count); break;
        case RWGPU_UPLOAD: case RWGPU_DOWNLOAD: case RWGPU_COPY:
          status_ = direct_transfer(op, offset, auxiliary, count);
          break;
        case RWGPU_SUM_INDICES: status_ = sum_indices(count); break;
        default: status_ = RWGPU_BAD_COMMAND;
      }
    } catch (const std::invalid_argument&) {
      status_ = RWGPU_INVALID;
    } catch (...) {
      status_ = RWGPU_FAILED;
    }
    completed_ = ticket;
  }

  static unsigned result_status(GpuVram::Result result) noexcept {
    if (result == GpuVram::Result::Complete) return RWGPU_OK;
    return result == GpuVram::Result::InvalidRange ? RWGPU_INVALID : RWGPU_FAILED;
  }

  unsigned staged_transfer(unsigned op, std::uint64_t offset, unsigned bytes) {
    if (!bytes || bytes > RWGPU_VRAM_BATCH_BYTES || bytes % GpuVram::kPageBytes ||
        offset % GpuVram::kPageBytes || !GpuVram::valid_range(offset, bytes) ||
        (op == RWGPU_WRITE && stage_.empty())) return RWGPU_INVALID;
    if (stage_.empty()) stage_.resize(RWGPU_STAGE_BYTES);
    return result_status(op == RWGPU_READ ? memory_.read(offset, stage_.data(), bytes)
                                         : memory_.write(offset, stage_.data(), bytes));
  }

  bool ordinary_ram(std::uint64_t address, std::size_t size) const {
    const auto& cpu = pc_.cpu;
    const auto top = std::min(cpu.memory.size(), cosmic_pc::PcMachine::kIoApicAddress);
    if (address > top || size > top - address) return false;
    if (!size) return true;
    const auto overlaps = [&](std::uint64_t base, std::uint64_t length) {
      return address < base + length && base < address + size;
    };
    using Video = cosmic_pc::PcVideo;
    return !overlaps(0xa0000, 0x60000) && !overlaps(pci_window_start, pci_window_bytes) &&
           (!Video::available(cpu.memory) || !overlaps(Video::kFramebufferAddress, Video::kApertureBytes)) &&
           (!cpu.memory.has_window() || !overlaps(cpu.memory.window_base(), cpu.memory.window_bytes())) &&
           (!cpu.mmio_range_claim_hook || !cpu.mmio_range_claim_hook(address, size));
  }

  unsigned direct_transfer(unsigned op, std::uint64_t source, std::uint64_t destination,
                           std::uint64_t count) {
    if (op != RWGPU_COPY && !(pci_command_ & 4U)) return RWGPU_UNAVAILABLE;
    if (count > GpuVram::kTransferBytes) return RWGPU_INVALID;
    const bool from_ram = op == RWGPU_UPLOAD, to_ram = op == RWGPU_DOWNLOAD;
    const auto bytes = std::size_t(count);
    if ((!to_ram && bytes > GpuVram::kTransferBytes - destination % GpuVram::kPageBytes) ||
        !(from_ram ? ordinary_ram(source, bytes) : GpuVram::valid_range(source, bytes)) ||
        !(to_ram ? ordinary_ram(destination, bytes) : GpuVram::valid_range(destination, bytes)))
      return RWGPU_INVALID;
    if (!bytes) return RWGPU_OK;
    if (to_ram) {
      if (const auto* data = memory_.resident_read(source, bytes)) {
        pc_.cpu.memory.write_block(destination, data, bytes);
        resident_download_bytes_ += count;
        result_ = count;
        return RWGPU_OK;
      }
    }
    transfer_.resize(bytes);
    if (from_ram) {
      pc_.cpu.memory.read_block(source, transfer_.data(), bytes);
    } else if (const auto result = memory_.read(source, transfer_.data(), bytes);
               result != GpuVram::Result::Complete) {
      return result_status(result);
    }
    if (to_ram) {
      pc_.cpu.memory.write_block(destination, transfer_.data(), bytes);
    } else if (const auto result = memory_.write(destination, transfer_.data(), bytes);
               result != GpuVram::Result::Complete) {
      return result_status(result);
    }
    result_ = count;
    return RWGPU_OK;
  }

  unsigned sum_indices(std::uint64_t count) {
    if (count > xgpu::domain_limit) return RWGPU_INVALID;
    if (count) {
      xgpu::ValueReader indices(xgpu::field_root(xgpu::Field(count, xgpu::Mode({0, 1}))));
      result_ = indices.sum_unsigned(0, count).value;
    }
    return RWGPU_OK;
  }

  unsigned read_color(std::uint64_t offset, unsigned bytes) {
#ifdef REVERSE_WINDOWS_GLES_GUEST
    if (color_.empty()) return RWGPU_UNAVAILABLE;
    if (!bytes || bytes > RWGPU_STAGE_BYTES || offset > color_.size() || bytes > color_.size() - offset)
      return RWGPU_INVALID;
    if (stage_.empty()) stage_.resize(RWGPU_STAGE_BYTES);
    std::memcpy(stage_.data(), color_.data() + offset, bytes);
    return RWGPU_OK;
#else
    (void)offset; (void)bytes;
    return RWGPU_UNAVAILABLE;
#endif
  }

  unsigned read_color_ram(std::uint64_t offset, std::uint64_t destination, std::uint64_t count) {
#ifdef REVERSE_WINDOWS_GLES_GUEST
    if (color_.empty() || !(pci_command_ & 4U)) return RWGPU_UNAVAILABLE;
    if (!count || count > GpuVram::kTransferBytes || offset > color_.size() ||
        count > color_.size() - offset || !ordinary_ram(destination, std::size_t(count)))
      return RWGPU_INVALID;
    pc_.cpu.memory.write_block(destination, color_.data() + offset, std::size_t(count));
    result_ = count;
    return RWGPU_OK;
#else
    (void)offset; (void)destination; (void)count;
    return RWGPU_UNAVAILABLE;
#endif
  }

  unsigned draw(unsigned op, std::uint64_t flags, unsigned bytes, std::uint64_t offset) {
#ifdef REVERSE_WINDOWS_GLES_GUEST
    static_assert(sizeof(RwGpuVertex) == sizeof(xgpu::GuestVertex) &&
                  RWGPU_MAX_VERTICES == xgpu::GuestTriangleWork::max_vertices);
    const auto start = std::chrono::steady_clock::now();
    const auto display = pc_.video_info();
    const auto width = params_[3], height = params_[4], clear = params_[5];
    const auto present = flags == RWGPU_DRAW_PRESENT_VBE;
    if (bytes % (3 * sizeof(RwGpuVertex)) ||
        bytes > RWGPU_MAX_VERTICES * sizeof(RwGpuVertex) ||
        flags > RWGPU_DRAW_PRESENT_VBE || !width || !height ||
        (present && (!display.graphics || width != display.width || height != display.height)) ||
        width > RWGPU_MAX_WIDTH || height > RWGPU_MAX_HEIGHT) return RWGPU_INVALID;
    if (op == RWGPU_DRAW_RAM && !(pci_command_ & 4U)) return RWGPU_UNAVAILABLE;
    if ((op == RWGPU_DRAW_RAM && !ordinary_ram(offset, bytes)) ||
        (op == RWGPU_DRAW_VRAM && !GpuVram::valid_range(offset, bytes)) ||
        (op == RWGPU_DRAW && bytes > stage_.size())) return RWGPU_INVALID;
    vertices_.resize(bytes / sizeof(RwGpuVertex));
    auto* vertex_data = reinterpret_cast<std::uint8_t*>(vertices_.data());
    if (op == RWGPU_DRAW_RAM) {
      pc_.cpu.memory.read_block(offset, vertex_data, bytes);
    } else if (op == RWGPU_DRAW_VRAM) {
      const auto read = memory_.read(offset, vertex_data, bytes);
      if (read != GpuVram::Result::Complete) return result_status(read);
    } else if (bytes) {
      std::memcpy(vertices_.data(), stage_.data(), bytes);
    }
    const auto plan = std::make_shared<const xgpu::GuestTrianglePlan>(planner_.plan(vertices_));
    auto receipt = renderer_.render(*plan, width, height, clear, bgrx_);
    // Return the actual readback to the canonical guest display. C07 observes
    // this physical write the same way as BIOS or ordinary guest pixel stores.
    if (present) pc_.cpu.memory.write_block(display.physical_address, bgrx_.data(), bgrx_.size());
    work_.publish(plan, plan->revision, plan->revision);
    bgrx_.swap(color_);
    ++draws_;
    source_triangles_ = plan->source_triangles;
    drawn_triangles_ = plan->vertices.size() / 3;
    vertex_bytes_ = receipt.vertex_bytes;
    presented_bytes_ = present ? color_.size() : 0;
    renderer_name_ = std::move(receipt.renderer);
    hardware_ = receipt.hardware_reported;
    backend_ns_ = receipt.cpu_wall_ns;
    gpu_ns_ = receipt.gpu_query_ns;
    host_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    return RWGPU_OK;
#else
    (void)op; (void)flags; (void)bytes; (void)offset;
    return RWGPU_UNAVAILABLE;
#endif
  }
};

}  // namespace reverse_windows
