#pragma once

#include "qssd/packed_transient_pages.hpp"
#include "sparse_ram.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <zlib.h>

namespace reverse_windows {

class AndroidRamBacking final : public cosmic_pc::SparseRam::PageBacking {
 public:
  static constexpr auto kPageSize = qssd::TransientPages::kPageSize;
  static_assert(kPageSize == cosmic_pc::SparseRam::kPageSize);

  static std::uint64_t spill_budget(const std::filesystem::path& directory) {
    return qssd::TransientPages::spill_budget(directory);
  }

  AndroidRamBacking(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                    std::uint64_t spill_bytes, bool pack_pages = false) {
    if (pack_pages) packed_ = std::make_unique<Packed>(directory, logical_bytes, spill_bytes);
    else raw_ = std::make_unique<qssd::TransientPages>(directory, logical_bytes, spill_bytes);
  }

  void read(std::uint64_t index, std::uint8_t* page) override {
    if (packed_) packed_->read(index, page);
    else raw_->read(index, page);
  }

  void write(std::uint64_t index, const std::uint8_t* page) override {
    if (packed_) packed_->write(index, page);
    else raw_->write(index, page);
  }

  void discard(std::uint64_t index) noexcept override {
    if (packed_) packed_->pages.discard(index);
  }

  void reset() noexcept override {
    if (packed_) packed_->pages.reset();
    else raw_->reset();
  }

  bool compression_enabled() const noexcept { return bool(packed_); }
  std::uint64_t packed_pages() const noexcept { return packed_ ? packed_->pages.packed_pages() : 0; }
  std::uint64_t file_payload_bytes() const noexcept {
    return packed_ ? packed_->pages.file_payload_bytes() : raw_->file_payload_bytes();
  }
  std::uint64_t read_us() const noexcept {
    return packed_ ? packed_->pages.read_us() : raw_->read_us();
  }
  std::uint64_t write_us() const noexcept {
    return packed_ ? packed_->pages.write_us() : raw_->write_us();
  }

 private:
  struct Packed {
    Packed(const std::filesystem::path& directory, std::uint64_t logical_bytes,
           std::uint64_t spill_bytes) : pages(directory, logical_bytes, spill_bytes) {
      if (::deflateInit2(&encoder, Z_BEST_SPEED, Z_DEFLATED, 12, 5, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("No se pudo iniciar la compresión del respaldo de RAM.");
    }
    ~Packed() { ::deflateEnd(&encoder); }

    void write(std::uint64_t index, const std::uint8_t* page) {
      if (::deflateReset(&encoder) != Z_OK)
        throw std::runtime_error("No se pudo preparar la compresión de RAM.");
      encoder.next_in = const_cast<std::uint8_t*>(page);
      encoder.avail_in = kPageSize;
      encoder.next_out = buffer.data();
      encoder.avail_out = buffer.size();
      const auto status = ::deflate(&encoder, Z_FINISH);
      if (status == Z_STREAM_END) {
        std::fill(buffer.begin() + encoder.total_out, buffer.end(), 0);
        pages.write(index, buffer.data(), true);
      } else if ((status == Z_OK || status == Z_BUF_ERROR) && encoder.avail_out == 0) {
        pages.write(index, page, false);
      } else {
        throw std::runtime_error("No se pudo comprimir la página de RAM.");
      }
    }

    void read(std::uint64_t index, std::uint8_t* page) {
      if (!pages.read(index, page, buffer.data())) return;
      uLongf restored = kPageSize;
      if (::uncompress(page, &restored, buffer.data(), buffer.size()) != Z_OK ||
          restored != kPageSize)
        throw std::runtime_error("La página comprimida del respaldo de RAM es inválida.");
    }

    qssd::PackedTransientPages pages;
    z_stream encoder{};
    std::array<std::uint8_t, qssd::PackedTransientPages::kHalfPageSize> buffer{};
  };

  std::unique_ptr<qssd::TransientPages> raw_;
  std::unique_ptr<Packed> packed_;
};

} // namespace reverse_windows
