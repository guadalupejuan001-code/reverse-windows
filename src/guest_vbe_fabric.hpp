#pragma once

#include "pc_machine.hpp"
#include "qssd/graphics_work_state.hpp"
#include "trabajo_fisico.hpp"
#include <algorithm>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace reverse_windows {

class GuestVbeFabric {
 public:
  explicit GuestVbeFabric(cosmic_pc::PcMachine& machine) : machine_(machine) {
    machine_.watch_video_writes();
  }

  // The packet is [rectangle count, { x, y, width, height, packed ARGB... } ...].
  // It remains valid until capture() is called again. A no-change capture
  // returns an empty packet and does not publish a new graphics revision.
  const std::vector<std::uint32_t>& capture(unsigned width, unsigned height) {
    const auto info = machine_.video_info();
    if (!info.graphics || !width || !height || width > info.width || height > info.height)
      throw std::invalid_argument("Tamaño de presentación VBE inválido.");
    packet_.clear();
    const auto& writes = *machine_.cpu.memory.write_watch();
    const bool full = !initialized_ || writes.full() || width != width_ || height != height_;
    if (!full && !writes.pending()) return packet_;

    try {
      width_ = width;
      height_ = height;
      // Each captured row reuses the same nearest-neighbor source columns.
      if (source_width_ != info.width || sample_columns_.size() != width) {
        sample_columns_.resize(width);
        for (unsigned x = 0; x < width; ++x)
          sample_columns_[x] = x * info.width / width;
        source_width_ = info.width;
      }
      rows_.assign(height, xgpu::FramebufferSpan{width, 0});
      if (full) pixels_.resize(std::size_t(width) * height);
      ++stats_.captures;
      const auto page_count = (info.byte_size + kPageBytes - 1) / kPageBytes;
      const auto dirty = full ? page_count : writes.pages().size();
      stats_.dirty_pages += dirty;
      if (full || dirty > page_count / 2) {
        scan(info, 0, info.byte_size, full);
      } else {
        scan_pages(info, writes.pages());
      }

      auto plan = gpu_.plan(width, rows_);
      if (!plan.rectangles.empty()) publish(std::move(plan));
      initialized_ = true;
      machine_.cpu.memory.acknowledge_write_watch();
      return packet_;
    } catch (...) {
      initialized_ = false;
      throw;
    }
  }

  void require_full() noexcept { initialized_ = false; }

  void write_json(std::ostream& out) const {
    out << "{\"source\":\"real-guest-VBE\",\"owner\":\"GPU-VIRTUAL-plus-qSSD-StateBus\""
        << ",\"captures\":" << stats_.captures << ",\"frames\":" << stats_.frames
        << ",\"dirty_pages\":" << stats_.dirty_pages << ",\"read_bytes\":" << stats_.read_bytes
        << ",\"examined_pixels\":" << stats_.examined_pixels
        << ",\"sent_pixels\":" << stats_.sent_pixels << ",\"packet_bytes\":" << stats_.packet_bytes
        << ",\"rectangles\":" << stats_.rectangles
        << ",\"full_frames\":" << stats_.full_frames
        << ",\"qssd_graphics_serial\":" << stats_.serial
        << ",\"gpu_plan_revision\":" << stats_.gpu_revision << '}';
  }

 private:
  struct Stats {
    std::uint64_t captures = 0, frames = 0, full_frames = 0, dirty_pages = 0, read_bytes = 0;
    std::uint64_t examined_pixels = 0, sent_pixels = 0, packet_bytes = 0, rectangles = 0;
    std::uint64_t serial = 0, gpu_revision = 0;
  };
  static constexpr auto kPageBytes = cosmic_pc::MemoryFabric::kPageSize;
  cosmic_pc::PcMachine& machine_;
  xgpu::FramebufferWorkAuthority gpu_;
  qssd::GraphicsWorkMemory<xgpu::FramebufferWorkPlan> work_{4};
  Stats stats_;
  unsigned width_ = 0, height_ = 0, source_width_ = 0;
  bool initialized_ = false;
  std::vector<xgpu::FramebufferSpan> rows_;
  std::vector<unsigned> sample_columns_;
  std::vector<std::uint32_t> sorted_pages_;
  std::vector<std::uint8_t> source_;
  std::vector<std::uint32_t> pixels_, packet_;

  static unsigned ceil_scale(unsigned value, unsigned to, unsigned from) noexcept {
    return static_cast<unsigned>((std::uint64_t(value) * to + from - 1) / from);
  }

  void scan_pages(const cosmic_pc::PcVideoInfo& info, const std::vector<std::uint32_t>& pages) {
    sorted_pages_ = pages;
    std::sort(sorted_pages_.begin(), sorted_pages_.end());
    auto it = sorted_pages_.begin();
    while (it != sorted_pages_.end()) {
      const auto begin = std::uint64_t(*it++) * kPageBytes;
      auto end = begin + kPageBytes;
      while (it != sorted_pages_.end() && std::uint64_t(*it) * kPageBytes == end) {
        ++it;
        end += kPageBytes;
      }
      scan(info, begin, std::min(end, info.byte_size), false);
    }
  }

  void scan(const cosmic_pc::PcVideoInfo& info, std::uint64_t begin, std::uint64_t end,
            bool full) {
    if (begin >= end) return;
    const auto first_y = ceil_scale(begin / info.pitch, height_, info.height);
    const auto last_y = ceil_scale((end - 1) / info.pitch + 1, height_, info.height);
    for (unsigned y = first_y; y < last_y; ++y) {
      // Invert the same floor mapping as the full-frame sampler. Only read
      // the destination samples whose source lies inside the dirty pages.
      const auto source_y = y * info.height / height_;
      const auto offset = std::uint64_t(source_y) * info.pitch;
      const auto first_source_x = begin > offset ? (begin - offset) / 4 : 0;
      const auto end_source_x = std::min<std::uint64_t>(info.width, (end - offset + 3) / 4);
      if (first_source_x >= end_source_x) continue;
      const auto first_x = ceil_scale(first_source_x, width_, info.width);
      const auto last_x = ceil_scale(end_source_x, width_, info.width);
      if (first_x == last_x) continue;
      const auto source_x = sample_columns_[first_x];
      const auto read_bytes = std::size_t(sample_columns_[last_x - 1] - source_x + 1) * 4;
      source_.resize(read_bytes);
      machine_.cpu.memory.read_block(info.physical_address + offset + source_x * 4,
                                     source_.data(), read_bytes);
      stats_.read_bytes += read_bytes;
      stats_.examined_pixels += last_x - first_x;
      auto& changed = rows_[y];
      auto* output = pixels_.data() + std::size_t(y) * width_;
      for (unsigned x = first_x; x < last_x; ++x) {
        const auto* bgrx = source_.data() + std::size_t(sample_columns_[x] - source_x) * 4;
        const auto argb = 0xff000000U | (std::uint32_t(bgrx[2]) << 16) |
            (std::uint32_t(bgrx[1]) << 8) | bgrx[0];
        if (!full && output[x] == argb) continue;
        output[x] = argb;
        changed.left = std::min(changed.left, x);
        changed.right = std::max(changed.right, x + 1);
      }
    }
  }

  void publish(xgpu::FramebufferWorkPlan plan) {
    packet_.reserve(plan.packet_words);
    packet_.push_back(static_cast<std::uint32_t>(plan.rectangles.size()));
    for (const auto& rect : plan.rectangles) {
      packet_.insert(packet_.end(), {rect.x, rect.y, rect.width, rect.height});
      for (unsigned y = rect.y; y < rect.y + rect.height; ++y) {
        const auto* start = pixels_.data() + std::size_t(y) * width_ + rect.x;
        packet_.insert(packet_.end(), start, start + rect.width);
      }
    }
    const auto completed = plan.revision;
    const auto view = work_.publish(std::move(plan), completed, completed);
    stats_.serial = view->serial;
    stats_.gpu_revision = completed;
    ++stats_.frames;
    stats_.full_frames += view->value->plan.full_frame;
    stats_.packet_bytes += packet_.size() * sizeof(packet_.front());
    stats_.sent_pixels += view->value->plan.pixels;
    stats_.rectangles += view->value->plan.rectangles.size();
  }
};

}  // namespace reverse_windows
