#pragma once

#include "pc_machine.hpp"
#include "qssd/graphics_work_state.hpp"
#include "trabajo_fisico.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
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
        contiguous_end_.clear();
        // The Android 1024x768 boot surface presents at 960x720. Its nearest-
        // neighbor samples consist of runs of 15 consecutive source pixels.
        if (width < info.width && std::uint64_t(info.width-width)*8 <= width) {
          contiguous_end_.resize(width);
          contiguous_end_.back() = width;
          for (unsigned x=width-1; x>0; --x)
            contiguous_end_[x-1] = sample_columns_[x-1]+1 == sample_columns_[x] ? contiguous_end_[x] : x;
        }
        source_width_ = info.width;
      }
      rows_.assign(height, xgpu::FramebufferSpan{width, 0});
      if (full) pixels_.resize(std::size_t(width) * height);
      ++stats_.captures;
      const auto page_count = (info.byte_size + kPageBytes - 1) / kPageBytes;
      const auto dirty = full ? page_count : writes.pages().size();
      stats_.dirty_pages += dirty;
      if (full) {
        scan(info, 0, info.byte_size, true);
      } else {
        scan_pages(info, writes);
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
        << ",\"scanout_cache_bytes\":" << pixels_.size() * sizeof(pixels_[0])
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
  std::vector<unsigned> sample_columns_, contiguous_end_;
  std::vector<std::uint32_t> sorted_pages_;
  std::vector<std::uint8_t> source_;
  std::vector<std::uint32_t> pixels_, packet_;

  static unsigned ceil_scale(unsigned value, unsigned to, unsigned from) noexcept {
    return static_cast<unsigned>((std::uint64_t(value) * to + from - 1) / from);
  }

  void scan_pages(const cosmic_pc::PcVideoInfo& info, const cosmic_pc::MemoryFabric::WriteWatch& writes) {
    sorted_pages_ = writes.pages();
    if (!std::is_sorted(sorted_pages_.begin(), sorted_pages_.end()))
      std::sort(sorted_pages_.begin(), sorted_pages_.end());
    std::uint64_t begin = 0, end = 0;
    for (const auto page : sorted_pages_) {
      const auto offset = std::uint64_t(page) * kPageBytes;
      const auto bounds = writes.bounds(page);
      const auto next = offset + bounds.begin;
      if (next != end) {
        scan(info, begin, end, false);
        begin = next;
      }
      end = offset + bounds.end;
    }
    scan(info, begin, end, false);
  }

  void scan(const cosmic_pc::PcVideoInfo& info, std::uint64_t begin, std::uint64_t end,
            bool full) {
    if (begin >= end) return;
    const auto first_y = ceil_scale(begin / info.pitch, height_, info.height);
    const auto last_y = ceil_scale((end - 1) / info.pitch + 1, height_, info.height);
    for (unsigned y = first_y; y < last_y; ++y) {
      // Invert the same floor mapping as the full-frame sampler. Only read
      // the destination samples whose source pixel intersects the written bytes.
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
      auto* output = pixels_.data() + std::size_t(y) * width_;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      if (width_ == info.width) {
        scan_native_row(source_.data(), output+first_x, first_x, last_x, full, rows_[y]);
        continue;
      }
      // Full scaled rows use one uninterrupted gather. Segmenting them only
      // pays off on dirty captures where equal runs can be skipped.
      if (!full && !contiguous_end_.empty()) {
        for (unsigned x=first_x; x<last_x;) {
          const auto end_x = std::min(contiguous_end_[x], last_x);
          const auto* input = source_.data() + 4*(sample_columns_[x]-source_x);
          scan_native_row(input, output+x, x, end_x, full, rows_[y]);
          x = end_x;
        }
        continue;
      }
#endif
      const auto sample = [&](unsigned x) {
        const auto* bgrx = source_.data() + std::size_t(sample_columns_[x] - source_x) * 4;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        std::uint32_t pixel;
        std::memcpy(&pixel, bgrx, sizeof(pixel));
        return pixel | 0xff000000U;
#else
        return 0xff000000U | (std::uint32_t(bgrx[2]) << 16) |
            (std::uint32_t(bgrx[1]) << 8) | bgrx[0];
#endif
      };
      if (full) {
        for (unsigned x = first_x; x < last_x; ++x) output[x] = sample(x);
        rows_[y] = {first_x, last_x};
        continue;
      }
      auto changed = rows_[y];
      for (unsigned x = first_x; x < last_x; ++x) {
        const auto argb = sample(x);
        if (output[x] == argb) continue;
        output[x] = argb;
        changed.left = std::min(changed.left, x);
        changed.right = std::max(changed.right, x + 1);
      }
      rows_[y] = changed;
    }
  }

  // At native width incoming BGRX and cached ARGB have identical color bits.
  // Ignore X, skip unchanged pixel pairs, and copy a contiguous interior span.
  static void scan_native_row(const std::uint8_t* input, std::uint32_t* output, unsigned x, unsigned end, bool full,
                       xgpu::FramebufferSpan& row) {
    unsigned left = 0, right = end-x;
    const auto pixel = [&](unsigned i) {
      std::uint32_t value;
      std::memcpy(&value, input+4*i, sizeof(value));
      return value | 0xff000000U;
    };
    if (!full) {
      const auto equal_pair = [&](unsigned i) {
        std::uint64_t incoming, previous;
        std::memcpy(&incoming, input+4*i, sizeof(incoming));
        std::memcpy(&previous, output+i, sizeof(previous));
        return ((incoming ^ previous) & UINT64_C(0x00ffffff00ffffff)) == 0;
      };
      while (right-left >= 2 && equal_pair(left)) left += 2;
      if (left < right && pixel(left) == output[left]) ++left;
      if (left == right) return;
      while (right-left >= 2 && equal_pair(right-2)) right -= 2;
      if (pixel(right-1) == output[right-1]) --right;
    }
    for (unsigned i=left; i<right; ++i) output[i] = pixel(i);
    row.left = std::min(row.left, x+left);
    row.right = std::max(row.right, x+right);
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
