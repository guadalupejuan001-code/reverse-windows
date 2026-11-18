#pragma once

#include "qssd/packed_transient_pages.hpp"
#include "sparse_ram.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#if defined(__linux__)
#include <sched.h>
#endif
#include <zlib.h>

namespace reverse_windows {

class AndroidRamBacking final : public cosmic_pc::SparseRam::PageBacking {
 public:
  static constexpr auto kPageSize = qssd::TransientPages::kPageSize;
  static_assert(kPageSize == cosmic_pc::SparseRam::kPageSize);

  struct CodecTiming {
    std::uint64_t wall_ns = 0;
  };

  struct CodecStats {
    std::uint64_t encode_attempts = 0, encode_packed = 0, encode_raw = 0, encode_bypassed = 0;
    std::uint64_t decode_attempts = 0;
    std::uint64_t encode_sparse = 0, encode_deflate = 0, decode_sparse = 0;
    CodecTiming encode, decode;
  };

  struct StagingStats {
    bool enabled = false;
    std::uint64_t staged_pages = 0, pending_pages = 0, packed_pages = 0, failures = 0;
    std::uint64_t yielded_batches = 0, background_encode_ns = 0, foreground_settled_pages = 0;
  };

  static std::uint64_t spill_budget(const std::filesystem::path& directory) {
    return qssd::TransientPages::spill_budget(directory);
  }

  AndroidRamBacking(const std::filesystem::path& directory, std::uint64_t logical_bytes,
                    std::uint64_t spill_bytes, bool pack_pages = false, bool defer_pressure_packing = false) {
    if (pack_pages)
      packed_ = std::make_unique<Packed>(directory, logical_bytes, spill_bytes, defer_pressure_packing);
    else raw_ = std::make_unique<qssd::TransientPages>(directory, logical_bytes, spill_bytes);
  }

  void read(std::uint64_t index, std::uint8_t* page) override {
    if (packed_) packed_->read(index, page);
    else raw_->read(index, page);
  }

  void write(std::uint64_t index, const std::uint8_t* page) override {
    if (!packed_) {
      raw_->write(index, page);
      return;
    }
    try {
      packed_->write(index, page);
    } catch (const qssd::PackedTransientPages::CapacityExceeded& error) {
      throw CapacityExceeded(error.what());
    }
  }

  bool supports_grouped_writes() const noexcept override { return true; }

  void begin_paced_reclaim() noexcept override {
    if (packed_) packed_->reclaim_active = true;
  }

  void end_paced_reclaim() noexcept override {
    if (!packed_) return;
    packed_->reclaim_active = false;
    if (std::exchange(packed_->notify_after_reclaim, false)) packed_->ready.notify_one();
  }

  std::size_t write_pages(const PageWrite* writes, std::size_t count,
                          cosmic_pc::SparseRam::ReclaimClock::time_point deadline) override {
    if (!count || count > kMaxWritePages) throw std::invalid_argument("Tanda de respaldo de RAM inválida.");
    if (packed_) {
      count = packed_->write_pages(writes, count, deadline);
    } else {
      std::array<qssd::TransientPages::PageWrite, kMaxWritePages> pages;
      for (std::size_t i = 0; i < count; ++i) pages[i] = {writes[i].page, writes[i].bytes};
      raw_->write_pages(pages.data(), count);
    }
    if (count > 1) {
      ++write_batches_;
      grouped_pages_ += count;
    }
    return count;
  }

  void discard(std::uint64_t index) noexcept override {
    if (packed_) packed_->discard(index);
  }

  void reset() noexcept override {
    if (packed_) packed_->reset();
    else raw_->reset();
    write_batches_ = grouped_pages_ = 0;
  }

  bool compression_enabled() const noexcept { return bool(packed_); }
  std::uint64_t packed_pages() const noexcept {
    return packed_ ? packed_->file_counter(&qssd::PackedTransientPages::packed_pages) : 0;
  }
  std::uint64_t file_payload_bytes() const noexcept {
    return packed_ ? packed_->file_counter(&qssd::PackedTransientPages::file_payload_bytes) : raw_->file_payload_bytes();
  }
  std::uint64_t read_us() const noexcept {
    return packed_ ? packed_->file_counter(&qssd::PackedTransientPages::read_us) : raw_->read_us();
  }
  std::uint64_t write_us() const noexcept {
    return packed_ ? packed_->file_counter(&qssd::PackedTransientPages::write_us) : raw_->write_us();
  }
  std::uint64_t write_calls() const noexcept {
    return packed_ ? packed_->file_counter(&qssd::PackedTransientPages::write_calls) : raw_->write_calls();
  }
  std::uint64_t write_batches() const noexcept { return write_batches_; }
  std::uint64_t grouped_pages() const noexcept { return grouped_pages_; }
  CodecStats codec_stats() const noexcept { return packed_ ? packed_->stats : CodecStats{}; }
  StagingStats staging_stats() const noexcept { return packed_ ? packed_->staging_stats() : StagingStats{}; }

 private:
  static bool host_can_defer() noexcept {
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    return ::sched_getaffinity(0, sizeof(allowed), &allowed) == 0 && CPU_COUNT(&allowed) >= 2;
#else
    return false;
#endif
  }

  class CodecTimer {
   public:
    explicit CodecTimer(CodecTiming& timing) noexcept
        : timing_(timing), started_(Clock::now()) {}
    ~CodecTimer() {
      timing_.wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_).count();
    }

   private:
    using Clock = std::chrono::steady_clock;
    CodecTiming& timing_;
    const Clock::time_point started_;
  };

  struct Packed {
    using Buffer = std::array<std::uint8_t, qssd::PackedTransientPages::kHalfPageSize>;
    // A zero tag, Adler-32, one presence bit per 8-byte word, then only the
    // nonzero words. The tag cannot be the first byte of a zlib stream.
    static constexpr std::uint8_t kSparseTag = 0;
    static constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
    static constexpr std::size_t kMaskOffset = 1 + sizeof(std::uint32_t);
    static constexpr std::size_t kMaskBytes = kPageSize / (8 * kWordBytes);
    static constexpr std::size_t kLiteralOffset = kMaskOffset + kMaskBytes;

    using Clock = cosmic_pc::SparseRam::ReclaimClock;
    using FileCounter = std::uint64_t (qssd::PackedTransientPages::*)() const noexcept;

    Packed(const std::filesystem::path& directory, std::uint64_t logical_bytes,
           std::uint64_t spill_bytes, bool defer) : pages(directory, logical_bytes, spill_bytes) {
      if (!start_encoder(encoder))
        throw std::runtime_error("No se pudo iniciar la compresión del respaldo de RAM.");
      if (defer && host_can_defer() && start_encoder(background_encoder)) {
        try {
          worker = std::thread([this] { pack_loop(); });
          staging.enabled = true;
        } catch (...) {
          ::deflateEnd(&background_encoder);
        }
      }
    }

    ~Packed() {
      if (staging.enabled) {
        {
          std::lock_guard<std::mutex> lock(io_mutex);
          stopping = true;
        }
        ready.notify_one();
        worker.join();
        ::deflateEnd(&background_encoder);
      }
      ::deflateEnd(&encoder);
    }

    static bool start_encoder(z_stream& state) noexcept {
      return ::deflateInit2(&state, Z_BEST_SPEED, Z_DEFLATED, 12, 5, Z_DEFAULT_STRATEGY) == Z_OK;
    }

    std::uint64_t file_counter(FileCounter counter) const noexcept {
      std::lock_guard<std::mutex> lock(io_mutex);
      return (pages.*counter)();
    }

    StagingStats staging_stats() const noexcept {
      std::lock_guard<std::mutex> lock(io_mutex);
      auto snapshot = staging;
      snapshot.yielded_batches = yielded_batches;
      return snapshot;
    }

    void write(std::uint64_t index, const std::uint8_t* page) {
      Buffer target;
      const auto packed = encode_page(page, target);
      std::unique_lock<std::mutex> lock(io_mutex);
      settle_for_reserve(lock, 2);
      pages.write(index, packed ? target.data() : page, packed);
      retire(index);
    }

    void discard(std::uint64_t index) noexcept {
      std::lock_guard<std::mutex> lock(io_mutex);
      pages.discard(index);
      retire(index);
    }

    std::size_t write_pages(const PageWrite* writes, std::size_t count,
                            Clock::time_point deadline) {
      // Leave part of the pressure window for submitting the prepared run.
      constexpr auto write_margin = std::chrono::microseconds(250);
      std::array<qssd::PackedTransientPages::PageWrite, kMaxWritePages> prepared;
      const bool paced = deadline != Clock::time_point::max();
      {
        std::unique_lock<std::mutex> lock(io_mutex, std::defer_lock);
        if (!acquire_write_lock(lock, paced)) return yield_batch();
        // Stage only when the file still has room for a complete subsequent
        // group and a separate block for an atomic raw-to-packed move.
        if (paced && staging.enabled && !staging.failures && Clock::now() < deadline &&
            staging.pending_pages + count <= pending.size() &&
            pages.room_for_raw(count + kMaxWritePages + 1)) {
          for (std::size_t i = 0; i < count; ++i)
            prepared[i] = {writes[i].page, writes[i].bytes, false};
          pages.write_pages(prepared.data(), count);
          enqueue(writes, count);
          lock.unlock();
          // Reclaim all the groups that fit this grant before waking a
          // sleeping encoder so it does not contend with the same grant.
          if (reclaim_active) notify_after_reclaim = true;
          else ready.notify_one();
          return count;
        }
        if (staging.pending_pages && !pages.room_for_raw(count + 1)) {
          if (paced) return yield_batch();
          settle_for_reserve(lock, count + 1);
        }
      }

      // The worker only replaces raw pages with smaller records, so room
      // checked above remains available while this caller encodes its prefix.
      std::array<Buffer, kMaxWritePages> buffers;
      std::size_t processed = 0;
      do {
        const auto& write = writes[processed];
        auto& target = buffers[processed];
        const auto packed = encode_page(write.bytes, target);
        prepared[processed++] = {write.page, packed ? target.data() : write.bytes, packed};
      } while (processed < count && (!paced || Clock::now() + write_margin < deadline));
      std::unique_lock<std::mutex> lock(io_mutex, std::defer_lock);
      if (!acquire_write_lock(lock, paced)) return yield_batch();
      pages.write_pages(prepared.data(), processed);
      return processed;
    }

    bool encode_page(const std::uint8_t* page, Buffer& target) {
      // The same bytes cannot encode differently with a freshly reset encoder.
      if (has_raw_page && std::memcmp(page, raw_page.data(), kPageSize) == 0) {
        ++stats.encode_bypassed;
        return false;
      }
      const auto packed = encode(page, target, encoder, stats);
      if (!packed) {
        std::memcpy(raw_page.data(), page, kPageSize);
        has_raw_page = true;
      }
      return packed;
    }

    static bool encode(const std::uint8_t* page, Buffer& target, z_stream& encoder, CodecStats& stats) {
      ++stats.encode_attempts;
      const CodecTimer timer(stats.encode);
      if (encode_sparse(page, target)) {
        ++stats.encode_sparse;
        ++stats.encode_packed;
        return true;
      }
      ++stats.encode_deflate;
      if (::deflateReset(&encoder) != Z_OK)
        throw std::runtime_error("No se pudo preparar la compresión de RAM.");
      encoder.next_in = const_cast<std::uint8_t*>(page);
      encoder.avail_in = kPageSize;
      encoder.next_out = target.data();
      encoder.avail_out = target.size();
      const auto status = ::deflate(&encoder, Z_FINISH);
      if (status == Z_STREAM_END) {
        ++stats.encode_packed;
        std::fill(target.begin() + encoder.total_out, target.end(), 0);
        return true;
      }
      if ((status == Z_OK || status == Z_BUF_ERROR) && encoder.avail_out == 0) {
        ++stats.encode_raw;
        return false;
      }
      throw std::runtime_error("No se pudo comprimir la página de RAM.");
    }

    static bool encode_sparse(const std::uint8_t* page, Buffer& target) noexcept {
      std::size_t literal = kLiteralOffset;
      for (std::size_t group = 0; group < kMaskBytes; ++group) {
        std::uint8_t mask = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
          std::uint64_t word;
          std::memcpy(&word, page + (group * 8 + bit) * kWordBytes, kWordBytes);
          if (!word) continue;
          if (target.size() - literal < kWordBytes) return false;
          mask |= std::uint8_t(1u << bit);
          std::memcpy(target.data() + literal, &word, kWordBytes);
          literal += kWordBytes;
        }
        target[kMaskOffset + group] = mask;
      }
      target.front() = kSparseTag;
      const auto checksum = static_cast<std::uint32_t>(::adler32(1, page, kPageSize));
      std::memcpy(target.data() + 1, &checksum, sizeof(checksum));
      std::fill(target.begin() + literal, target.end(), 0);
      return true;
    }

    static bool decode_sparse(const Buffer& source, std::uint8_t* page) noexcept {
      std::size_t literal = kLiteralOffset;
      std::memset(page, 0, kPageSize);
      for (std::size_t group = 0; group < kMaskBytes; ++group) {
        const auto mask = source[kMaskOffset + group];
        if (!mask) continue;
        for (unsigned bit = 0; bit < 8; ++bit) {
          if (!(mask & (1u << bit))) continue;
          if (source.size() - literal < kWordBytes) return false;
          std::memcpy(page + (group * 8 + bit) * kWordBytes, source.data() + literal, kWordBytes);
          literal += kWordBytes;
        }
      }
      std::uint32_t checksum;
      std::memcpy(&checksum, source.data() + 1, sizeof(checksum));
      return checksum == ::adler32(1, page, kPageSize);
    }

    void read(std::uint64_t index, std::uint8_t* page) {
      Buffer buffer;
      {
        std::lock_guard<std::mutex> lock(io_mutex);
        if (!pages.read(index, page, buffer.data())) return;
      }
      ++stats.decode_attempts;
      const CodecTimer timer(stats.decode);
      if (buffer.front() == kSparseTag) {
        ++stats.decode_sparse;
        if (decode_sparse(buffer, page)) return;
      } else {
        uLongf restored = kPageSize;
        if (::uncompress(page, &restored, buffer.data(), buffer.size()) == Z_OK && restored == kPageSize)
          return;
      }
      throw std::runtime_error("La página comprimida del respaldo de RAM es inválida.");
    }

    void reset() noexcept {
      std::lock_guard<std::mutex> lock(io_mutex);
      pages.reset();
      stats = {};
      has_raw_page = false;
      pending = {};
      staging = StagingStats{staging.enabled};
      yielded_batches = next_job = 0;
    }

    struct Pending {
      std::uint64_t page = 0, serial = 0;
      bool retried = false;
    };

    bool acquire_write_lock(std::unique_lock<std::mutex>& lock, bool paced) const {
      if (paced && staging.enabled) return lock.try_lock();
      lock.lock();
      return true;
    }

    std::size_t yield_batch() noexcept {
      ++yielded_batches;
      return 0;
    }

    void enqueue(const PageWrite* writes, std::size_t count) noexcept {
      auto* entry = pending.data();
      for (std::size_t i = 0; i < count; ++i) {
        while (entry->serial) ++entry;
        if (++next_serial == 0) ++next_serial;
        *entry++ = {writes[i].page, next_serial};
      }
      staging.pending_pages += count;
      staging.staged_pages += count;
    }

    void retire(std::uint64_t page) noexcept {
      if (!staging.pending_pages) return;
      for (auto& item : pending) {
        if (!item.serial || item.page != page) continue;
        item = {};
        --staging.pending_pages;
        return;
      }
    }

    Pending& take_pending() noexcept {
      while (!pending[next_job].serial) next_job = (next_job + 1) % pending.size();
      auto& item = pending[next_job];
      next_job = (next_job + 1) % pending.size();
      return item;
    }

    void settle_for_reserve(std::unique_lock<std::mutex>& lock, std::size_t blocks) {
      // True allocation-time writes are synchronous. Recover only as many
      // queued slots as needed instead of waiting for worker scheduling.
      auto remaining = staging.pending_pages * 2;
      while (remaining && staging.pending_pages && !pages.room_for_raw(blocks)) {
        --remaining;
        pack_pending(lock, take_pending(), encoder, false);
      }
    }

    void pack_loop() noexcept {
      std::unique_lock<std::mutex> lock(io_mutex);
      for (;;) {
        ready.wait(lock, [&] { return stopping || staging.pending_pages; });
        if (stopping) return;
        pack_pending(lock, take_pending(), background_encoder, true);
      }
    }

    void pack_pending(std::unique_lock<std::mutex>& lock, Pending& item, z_stream& state, bool background) {
      const auto job = item;
      std::array<std::uint8_t, kPageSize> raw;
      Buffer encoded;
      bool failed = false, packed = false;
      CodecStats delta;
      try {
        if (!pages.read(job.page, raw.data(), encoded.data())) {
          lock.unlock();
          try {
            packed = encode(raw.data(), encoded, state, background ? delta : stats);
          } catch (...) {
            failed = true;
          }
          lock.lock();
          if (item.serial != job.serial || stopping) return;
          if (packed && pages.pack_raw_copy(job.page, encoded.data())) ++staging.packed_pages;
        }
      } catch (...) {
        failed = true;
      }
      if (background) staging.background_encode_ns += delta.encode.wall_ns;
      else ++staging.foreground_settled_pages;
      if (failed) {
        ++staging.failures;
        // A transient failure can otherwise strand an ordinary compressible
        // page in a full raw block and consume capacity on the next reclaim.
        if (!item.retried) {
          item.retried = true;
          return;
        }
      }
      item = {};
      --staging.pending_pages;
    }

    qssd::PackedTransientPages pages;
    mutable std::mutex io_mutex;
    std::condition_variable ready;
    std::array<Pending, 128> pending{};
    StagingStats staging;
    std::size_t next_job = 0;
    std::uint64_t next_serial = 0, yielded_batches = 0;
    bool stopping = false, reclaim_active = false, notify_after_reclaim = false;
    z_stream background_encoder{};
    std::thread worker;
    CodecStats stats;
    z_stream encoder{};
    std::array<std::uint8_t, kPageSize> raw_page;
    bool has_raw_page = false;
  };

  std::unique_ptr<qssd::TransientPages> raw_;
  std::unique_ptr<Packed> packed_;
  std::uint64_t write_batches_ = 0, grouped_pages_ = 0;
};

} // namespace reverse_windows
