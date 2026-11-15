#pragma once

#include "c08_gpu_dma.h"
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
#include <functional>
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
  static_assert(RWGPU_STAGE_OFFSET % 4 == 0 && RWGPU_STAGE_BYTES % 4 == 0);

  using Msi = std::function<bool(std::uint32_t, std::uint16_t)>;
  GuestGpuC08(cosmic_pc::PcMachine& machine, const std::filesystem::path& vram, Msi msi = {})
      : pc_(machine), memory_(vram), msi_(std::move(msi)) {}

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
    msi_address_ = msi_data_ = 0;
    msi_enabled_ = false;
    reset_engine();
    gpu_ns_.reset();
    renderer_name_.clear();
    hardware_ = false;
    host_ns_ = backend_ns_ = 0;
    alpha_draw_ticket_ = 0;
    source_triangles_ = drawn_triangles_ = vertex_bytes_ = presented_bytes_ = 0;
#ifdef REVERSE_WINDOWS_GLES_GUEST
    color_.clear();
#endif
  }

  std::uint32_t config_word(unsigned offset) const noexcept {
    switch (offset) {
      case 0: return pci_id;
      case 4: return pci_command_ | (msi_ ? 0x00100000U : 0U); // PCI status capabilities bit.
      case 8: return 0x03020001; // 3D controller, independent of the boot display.
      case 0x10: return probe_bar_ ? ~(RWGPU_BAR_BYTES - 1) : bar_;
      case 0x2c: return pci_id;
      case 0x34: return msi_ ? RWGPU_PCI_MSI_CAP : 0;
      case RWGPU_PCI_MSI_CAP: return msi_ ? 0x05U | (std::uint32_t(msi_enabled_) << 16) : 0;
      case RWGPU_PCI_MSI_CAP + 4: return msi_address_;
      case RWGPU_PCI_MSI_CAP + 8: return msi_data_;
      default: return 0;
    }
  }
  bool config_write(unsigned offset, unsigned bytes, std::uint32_t value) noexcept {
    if (msi_ && offset >= RWGPU_PCI_MSI_CAP && offset < RWGPU_PCI_MSI_CAP + 12) {
      const auto reg = offset & ~3U;
      const auto shift = (offset & 3U) * 8;
      const auto mask = (bytes == 4 ? UINT32_MAX : (1U << (8 * bytes)) - 1) << shift;
      const auto merged = (config_word(reg) & ~mask) | ((value << shift) & mask);
      if (reg == RWGPU_PCI_MSI_CAP) msi_enabled_ = (merged & 0x10000U) != 0;
      else if (reg == RWGPU_PCI_MSI_CAP + 4) msi_address_ = merged & ~3U;
      else msi_data_ = std::uint16_t(merged);
      if (irq_status_) send_msi();
      return false; // The target address for BAR accesses did not change.
    }
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
  // The board routes only aligned dwords within the decoded BAR here.
  std::uint32_t read32(std::uint64_t address) const noexcept {
    const auto offset = unsigned(address - bar_);
    if (offset >= RWGPU_STAGE_OFFSET && offset - RWGPU_STAGE_OFFSET < stage_.size()) {
      const auto index = offset - RWGPU_STAGE_OFFSET;
      std::uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(stage_[index + i]) << (i * 8);
      return value;
    }
    return word(offset);
  }
  bool write32(std::uint64_t address, std::uint32_t value) {
    const auto offset = unsigned(address - bar_);
    if (offset >= RWGPU_STAGE_OFFSET && offset - RWGPU_STAGE_OFFSET < RWGPU_STAGE_BYTES) {
      // The byte path owns first allocation and reports allocation failures.
      if (stage_.empty()) return false;
      const auto index = offset - RWGPU_STAGE_OFFSET;
      for (unsigned i = 0; i < 4; ++i) stage_[index + i] = std::uint8_t(value >> (i * 8));
      pending_ = {};
      return true;
    }
    pending_ = {};
    commit_register(offset, value);
    return true;
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
    if (!command_register(reg) && !direct_register(reg) &&
        reg != RWGPU_REG_IRQ_ACK && reg != RWGPU_REG_ENGINE_RESET) {
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
      commit_register(reg, value);
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
        << ",\"draw_requests\":" << commands_[RWGPU_DRAW] + commands_[RWGPU_DRAW_VRAM] +
                                     commands_[RWGPU_DRAW_RAM] + commands_[RWGPU_DRAW_READBACK]
        << ",\"vram_draw_requests\":" << commands_[RWGPU_DRAW_VRAM]
        << ",\"ram_draw_requests\":" << commands_[RWGPU_DRAW_RAM] << ",\"draws\":" << draws_
        << ",\"alpha_draw_ticket\":" << alpha_draw_ticket_
        << ",\"color_reads\":" << commands_[RWGPU_READ_COLOR] + commands_[RWGPU_READ_COLOR_RAM]
        << ",\"color_dma_reads\":" << commands_[RWGPU_READ_COLOR_RAM]
        << ",\"frame_dma_requests\":" << commands_[RWGPU_DRAW_READBACK]
        << ",\"frame_dma_completions\":" << frame_dma_completions_
        << ",\"frame_dma_bytes\":" << frame_dma_bytes_
        << ",\"scheduler_fence\":" << fence_last_
        << ",\"scheduler_submissions\":" << commands_[RWGPU_SUBMIT_DMA]
        << ",\"scheduler_faults\":" << scheduler_faults_
        << ",\"scheduler_draw_ticket\":" << scheduled_draw_ticket_
        << ",\"scheduler_upload_bytes\":" << paging_upload_bytes_
        << ",\"scheduler_download_bytes\":" << paging_download_bytes_
        << ",\"scheduler_msi\":" << msi_messages_
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
  Msi msi_;
  xgpu::GuestTriangleWork planner_;
  using RenderPlan = std::shared_ptr<const xgpu::GuestTrianglePlan>;
  qssd::GraphicsWorkMemory<RenderPlan> work_;
  std::array<std::uint64_t, RWGPU_PREEMPT + 1> commands_{};
  std::uint64_t draws_ = 0, source_triangles_ = 0, drawn_triangles_ = 0;
  std::uint64_t vertex_bytes_ = 0, presented_bytes_ = 0, host_ns_ = 0, backend_ns_ = 0;
  std::uint64_t result_ = 0, resident_download_bytes_ = 0;
  std::uint64_t frame_dma_completions_ = 0, frame_dma_bytes_ = 0;
  std::uint64_t scheduler_faults_ = 0, msi_messages_ = 0;
  std::uint64_t paging_upload_bytes_ = 0, paging_download_bytes_ = 0;
  std::optional<std::uint64_t> gpu_ns_;
  bool hardware_ = false;
  std::string renderer_name_;
  std::array<std::uint32_t, 7> params_{};
  std::array<std::uint32_t, 3> direct_params_{};
  std::uint32_t pci_command_ = 0, bar_ = pci_window_start, status_ = RWGPU_IDLE, completed_ = 0;
  std::uint32_t alpha_draw_ticket_ = 0;
  std::uint32_t msi_address_ = 0;
  std::uint16_t msi_data_ = 0;
  bool msi_enabled_ = false;
  std::uint32_t fence_last_ = 0, fence_preempt_ = 0, preempt_last_ = 0, fence_fault_ = 0;
  std::uint32_t irq_status_ = 0, scheduled_draw_ticket_ = 0;
  Partial pending_{};
  bool probe_bar_ = false;
  std::vector<std::uint8_t> stage_, transfer_, dma_commands_;
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
  void commit_register(unsigned reg, std::uint32_t value) {
    if (reg == RWGPU_REG_DOORBELL) dispatch(value);
    else if (reg == RWGPU_REG_IRQ_ACK) irq_status_ &= ~value;
    else if (reg == RWGPU_REG_ENGINE_RESET) { if (value == 1) reset_engine(); }
    else if (command_register(reg)) params_[(reg - RWGPU_REG_OFFSET_LO) / 4] = value;
    else if (direct_register(reg)) direct_params_[(reg - RWGPU_REG_AUX_LO) / 4] = value;
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
            | RWGPU_CAP_TRIANGLES | RWGPU_CAP_DRAW_VRAM | RWGPU_CAP_COLOR_DMA |
              RWGPU_CAP_DRAW_RAM | RWGPU_CAP_COLOR_ALPHA | RWGPU_CAP_DRAW_READBACK |
              (msi_ ? RWGPU_CAP_WDDM_SCHEDULER : 0)
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
      case RWGPU_REG_FENCE_LAST: return fence_last_;
      case RWGPU_REG_IRQ_STATUS: return irq_status_;
      case RWGPU_REG_FENCE_PREEMPT: return fence_preempt_;
      case RWGPU_REG_PREEMPT_LAST: return preempt_last_;
      case RWGPU_REG_FENCE_FAULT: return fence_fault_;
      case RWGPU_REG_SCHEDULED_DRAW_TICKET: return scheduled_draw_ticket_;
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
      if (op >= RWGPU_WRITE && op <= RWGPU_PREEMPT) ++commands_[op];
      const auto offset = wide(params_[0], params_[1]);
      const auto auxiliary = wide(direct_params_[0], direct_params_[1]);
      const auto count = wide(params_[2], direct_params_[2]);
      switch (op) {
        case RWGPU_WRITE: case RWGPU_READ: status_ = staged_transfer(op, offset, params_[2]); break;
        case RWGPU_DRAW: status_ = draw(op, offset, params_[2], 0); break;
        case RWGPU_DRAW_VRAM: case RWGPU_DRAW_RAM:
          status_ = count > UINT32_MAX ? unsigned(RWGPU_INVALID) : draw(op, auxiliary, params_[2], offset);
          break;
        case RWGPU_DRAW_READBACK:
          status_ = count > UINT32_MAX ? unsigned(RWGPU_INVALID) :
              draw(RWGPU_DRAW_RAM, RWGPU_DRAW_COLOR_ALPHA, params_[2], offset, auxiliary);
          break;
        case RWGPU_READ_COLOR: status_ = read_color(offset, params_[2]); break;
        case RWGPU_READ_COLOR_RAM: status_ = read_color_ram(offset, auxiliary, count); break;
        case RWGPU_UPLOAD: case RWGPU_DOWNLOAD: case RWGPU_COPY:
          status_ = direct_transfer(op, offset, auxiliary, count);
          break;
        case RWGPU_SUM_INDICES: status_ = sum_indices(count); break;
        case RWGPU_SUBMIT_DMA: status_ = execute_dma(offset, auxiliary, count); break;
        case RWGPU_PREEMPT:
          status_ = msi_ && auxiliary <= UINT32_MAX && !offset && !count ? RWGPU_OK : RWGPU_INVALID;
          break;
        default: status_ = RWGPU_BAD_COMMAND;
      }
    } catch (const std::invalid_argument&) {
      status_ = RWGPU_INVALID;
    } catch (...) {
      status_ = RWGPU_FAILED;
    }
    completed_ = ticket;
    // The PCI ticket and any DMA writes are visible before raising the MSI.
    if (op == RWGPU_SUBMIT_DMA && direct_params_[1] == 0) {
      if (status_ == RWGPU_OK) {
        fence_last_ = direct_params_[0];
        irq_status_ |= RWGPU_IRQ_COMPLETE;
      } else {
        if (!(irq_status_ & RWGPU_IRQ_FAULT)) fence_fault_ = direct_params_[0];
        irq_status_ |= RWGPU_IRQ_FAULT;
        ++scheduler_faults_;
      }
      send_msi();
    }
    if (op == RWGPU_PREEMPT && status_ == RWGPU_OK) {
      fence_preempt_ = direct_params_[0];
      preempt_last_ = fence_last_;
      irq_status_ |= RWGPU_IRQ_PREEMPT;
      send_msi();
    }
  }

  void reset_engine() noexcept {
    fence_last_ = fence_preempt_ = preempt_last_ = fence_fault_ = irq_status_ = scheduled_draw_ticket_ = 0;
  }

  void send_msi() noexcept {
    if (msi_enabled_ && msi_ && msi_(msi_address_, msi_data_)) ++msi_messages_;
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
    using Pc = cosmic_pc::PcMachine;
    const auto top = cpu.memory.size();
    if (address > top || size > top - address) return false;
    if (!size) return true;
    const auto overlaps = [&](std::uint64_t base, std::uint64_t length) {
      return address < base + length && base < address + size;
    };
    using Video = cosmic_pc::PcVideo;
    return !overlaps(Pc::kConventionalRamEnd, Pc::kExtendedRamStart - Pc::kConventionalRamEnd) &&
           !overlaps(Pc::kIoApicAddress, Pc::kLegacyPhysicalLimit - Pc::kIoApicAddress) &&
           !overlaps(pci_window_start, pci_window_bytes) &&
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

  template <class T>
  static T dma_field(const std::uint8_t* data) noexcept {
    T value;
    std::memcpy(&value, data, sizeof(value));
    return value;
  }

  unsigned execute_dma(std::uint64_t address, std::uint64_t fence, std::uint64_t bytes) {
    if (!(pci_command_ & 4U) || !msi_) return RWGPU_UNAVAILABLE;
    if (irq_status_ & RWGPU_IRQ_FAULT) return RWGPU_FAILED;
    if (fence > UINT32_MAX || bytes > RWGPU_DMA_BUFFER_BYTES || (bytes & 7U) ||
        (bytes && !ordinary_ram(address, std::size_t(bytes)))) return RWGPU_INVALID;
    if (!bytes) return RWGPU_OK; // Scheduler signal packets have no GPU work.
    dma_commands_.resize(std::size_t(bytes));
    pc_.cpu.memory.read_block(address, dma_commands_.data(), dma_commands_.size());
    for (std::size_t at = 0; at < dma_commands_.size();) {
      const auto* record = dma_commands_.data() + at;
      const auto header = dma_field<RwGpuDmaHeader>(record);
      if (header.record_bytes < sizeof(header) || (header.record_bytes & 7U) ||
          header.record_bytes > dma_commands_.size() - at) return RWGPU_INVALID;
      const auto status = execute_dma_record(header, record);
      if (status != RWGPU_OK) return status;
      at += header.record_bytes;
    }
    return RWGPU_OK;
  }

  unsigned execute_dma_record(RwGpuDmaHeader header, const std::uint8_t* record) {
    switch (header.opcode) {
      case RWGPU_DMA_UPLOAD: case RWGPU_DMA_DOWNLOAD:
        return paging_transfer(header, record);
      case RWGPU_DMA_COPY: {
        if (header.record_bytes != sizeof(RwGpuDmaCopy)) return RWGPU_INVALID;
        const auto copy = dma_field<RwGpuDmaCopy>(record);
        return copy.reserved ? unsigned(RWGPU_INVALID) :
            direct_transfer(RWGPU_COPY, copy.source, copy.destination, copy.bytes);
      }
      case RWGPU_DMA_FILL: {
        if (header.record_bytes != sizeof(RwGpuDmaFill)) return RWGPU_INVALID;
        const auto fill = dma_field<RwGpuDmaFill>(record);
        if (!valid_vram_write(fill.destination, fill.bytes) || (fill.bytes & 3U)) return RWGPU_INVALID;
        transfer_.resize(fill.bytes);
        for (std::size_t at = 0; at < fill.bytes; at += sizeof(fill.pattern))
          std::memcpy(transfer_.data() + at, &fill.pattern, sizeof(fill.pattern));
        return result_status(memory_.write(fill.destination, transfer_.data(), fill.bytes));
      }
      case RWGPU_DMA_DRAW:
        return scheduled_draw(header, record);
      default:
        return RWGPU_BAD_COMMAND;
    }
  }

  static bool valid_vram_write(std::uint64_t address, std::size_t bytes) {
    return bytes && bytes <= GpuVram::kTransferBytes - address % GpuVram::kPageBytes &&
           GpuVram::valid_range(address, bytes);
  }

  unsigned paging_transfer(RwGpuDmaHeader header, const std::uint8_t* record) {
    if (header.record_bytes < sizeof(RwGpuDmaTransfer)) return RWGPU_INVALID;
    const auto job = dma_field<RwGpuDmaTransfer>(record);
    const bool upload = header.opcode == RWGPU_DMA_UPLOAD;
    if (!job.ranges || job.ranges > RWGPU_DMA_MAX_RANGES || !job.bytes ||
        header.record_bytes != sizeof(job) + job.ranges * sizeof(RwGpuDmaRange) ||
        (upload ? !valid_vram_write(job.vram, job.bytes)
                : (job.bytes > GpuVram::kTransferBytes || !GpuVram::valid_range(job.vram, job.bytes))))
      return RWGPU_INVALID;
    const auto range = [&](unsigned index) {
      return dma_field<RwGpuDmaRange>(record + sizeof(job) + index * sizeof(RwGpuDmaRange));
    };
    std::size_t bytes = 0;
    for (unsigned i = 0; i < job.ranges; ++i) {
      const auto part = range(i);
      if (part.reserved || !part.bytes || part.bytes > job.bytes - bytes ||
          !ordinary_ram(part.ram, part.bytes)) return RWGPU_INVALID;
      bytes += part.bytes;
    }
    if (bytes != job.bytes) return RWGPU_INVALID;
    if (upload) {
      transfer_.resize(bytes);
      std::size_t at = 0;
      for (unsigned i = 0; i < job.ranges; ++i) {
        const auto part = range(i);
        pc_.cpu.memory.read_block(part.ram, transfer_.data() + at, part.bytes);
        at += part.bytes;
      }
      const auto result = memory_.write(job.vram, transfer_.data(), bytes);
      if (result == GpuVram::Result::Complete) paging_upload_bytes_ += bytes;
      return result_status(result);
    }
    const auto* resident = memory_.resident_read(job.vram, bytes);
    const auto* source = resident;
    if (!source) {
      transfer_.resize(bytes);
      const auto result = memory_.read(job.vram, transfer_.data(), bytes);
      if (result != GpuVram::Result::Complete) return result_status(result);
      source = transfer_.data();
    }
    auto& ram = pc_.cpu.memory;
    const auto pins = ram.pin_writes();
    for (unsigned i = 0; i < job.ranges; ++i) {
      const auto part = range(i);
      ram.prepare_write(part.ram, part.bytes);
    }
    std::size_t at = 0;
    for (unsigned i = 0; i < job.ranges; ++i) {
      const auto part = range(i);
      ram.write_block(part.ram, source + at, part.bytes);
      at += part.bytes;
    }
    paging_download_bytes_ += bytes;
    if (resident) resident_download_bytes_ += bytes;
    return RWGPU_OK;
  }

  unsigned scheduled_draw(RwGpuDmaHeader header, const std::uint8_t* data) {
#ifdef REVERSE_WINDOWS_GLES_GUEST
    if (header.record_bytes < sizeof(RwGpuDmaDraw)) return RWGPU_INVALID;
    const auto job = dma_field<RwGpuDmaDraw>(data);
    if (job.vertices > RWGPU_MAX_VERTICES || job.vertices % 3 ||
        header.record_bytes != rwgpu_dma_draw_bytes(job.vertices) ||
        !job.width || job.width > RWGPU_MAX_WIDTH || !job.height || job.height > RWGPU_MAX_HEIGHT ||
        !valid_vram_write(job.destination, std::size_t(job.width) * job.height * 4)) return RWGPU_INVALID;
    vertices_.resize(job.vertices);
    if (job.vertices) std::memcpy(vertices_.data(), data + sizeof(job), job.vertices * sizeof(RwGpuVertex));
    const auto result = render_frame(job.width, job.height, job.clear_rgba, false, true, {}, job.destination);
    if (result == RWGPU_OK) scheduled_draw_ticket_ = params_.back();
    return result;
#else
    (void)header; (void)data;
    return RWGPU_UNAVAILABLE;
#endif
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

  unsigned draw(unsigned op, std::uint64_t flags, unsigned bytes, std::uint64_t offset,
                std::optional<std::uint64_t> dma_out = {}) {
#ifdef REVERSE_WINDOWS_GLES_GUEST
    static_assert(sizeof(RwGpuVertex) == sizeof(xgpu::GuestVertex) &&
                  RWGPU_MAX_VERTICES == xgpu::GuestTriangleWork::max_vertices);
    const auto display = pc_.video_info();
    const auto width = params_[3], height = params_[4], clear = params_[5];
    const auto present = flags == RWGPU_DRAW_PRESENT_VBE;
    const auto alpha = flags == RWGPU_DRAW_COLOR_ALPHA;
    if (bytes % (3 * sizeof(RwGpuVertex)) ||
        bytes > RWGPU_MAX_VERTICES * sizeof(RwGpuVertex) ||
        flags > RWGPU_DRAW_COLOR_ALPHA || !width || !height ||
        (present && (!display.graphics || width != display.width || height != display.height)) ||
        width > RWGPU_MAX_WIDTH || height > RWGPU_MAX_HEIGHT) return RWGPU_INVALID;
    if (op == RWGPU_DRAW_RAM && !(pci_command_ & 4U)) return RWGPU_UNAVAILABLE;
    if ((op == RWGPU_DRAW_RAM && !ordinary_ram(offset, bytes)) ||
        (op == RWGPU_DRAW_VRAM && !GpuVram::valid_range(offset, bytes)) ||
        (op == RWGPU_DRAW && bytes > stage_.size())) return RWGPU_INVALID;
    const auto pixels = std::size_t(width) * height * 4;
    if (dma_out && !ordinary_ram(*dma_out, pixels)) return RWGPU_INVALID;
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
    return render_frame(width, height, clear, present, alpha, dma_out, {});
#else
    (void)op; (void)flags; (void)bytes; (void)offset; (void)dma_out;
    return RWGPU_UNAVAILABLE;
#endif
  }

#ifdef REVERSE_WINDOWS_GLES_GUEST
  unsigned render_frame(unsigned width, unsigned height, unsigned clear, bool present, bool alpha,
                        std::optional<std::uint64_t> dma_out, std::optional<std::uint64_t> vram_out) {
    const auto start = std::chrono::steady_clock::now();
    const auto pixels = std::size_t(width) * height * 4;
    // Preflight the entire DMA destination before GPU work. The pins keep
    // prepared pages resident until the frame is copied.
    const auto pins = pc_.cpu.memory.pin_writes();
    if (dma_out) pc_.cpu.memory.prepare_write(*dma_out, pixels);
    const auto plan = std::make_shared<const xgpu::GuestTrianglePlan>(planner_.plan(vertices_));
    auto receipt = renderer_.render(*plan, width, height, clear, bgrx_, alpha);
    if (bgrx_.size() != pixels) return RWGPU_FAILED;
    // Return the actual readback to the canonical guest display. C07 observes
    // this physical write the same way as BIOS or ordinary guest pixel stores.
    if (present) pc_.cpu.memory.write_block(pc_.video_info().physical_address, bgrx_.data(), bgrx_.size());
    if (vram_out) {
      const auto result = memory_.write(*vram_out, bgrx_.data(), pixels);
      if (result != GpuVram::Result::Complete) return result_status(result);
    }
    work_.publish(plan, plan->revision, plan->revision);
    // The DMA destination is resident and pinned, so this copy cannot need
    // backing I/O or grow SparseRam after publishing the completed work.
    if (dma_out) pc_.cpu.memory.write_block(*dma_out, bgrx_.data(), pixels);
    bgrx_.swap(color_);
    ++draws_;
    if (dma_out) {
      result_ = pixels;
      ++frame_dma_completions_;
      frame_dma_bytes_ += pixels;
    }
    if (alpha) alpha_draw_ticket_ = params_.back();
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
  }
#endif
};

}  // namespace reverse_windows
