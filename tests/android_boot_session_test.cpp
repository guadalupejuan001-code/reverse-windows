#include "android_boot_session.hpp"
#include "long_mode_guest.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
using Session = reverse_windows::AndroidBootSession;

class TestDiskDirectory {
 public:
  TestDiskDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "reverse-windows-android-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("Cannot create test directory");
    path = pattern;
  }
  ~TestDiskDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

class FileDescriptor {
 public:
  FileDescriptor(const std::filesystem::path& path, int mode)
      : value(::open(path.c_str(), mode | O_CLOEXEC)) {
    if (value < 0) throw std::runtime_error("Cannot open ISO fixture");
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  ~FileDescriptor() { ::close(value); }
  int value;
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::uint64_t counter(const std::string& json, const char* name) {
  const auto key = std::string("\"") + name + "\":";
  const auto pos = json.find(key);
  require(pos != std::string::npos, name);
  return std::stoull(json.substr(pos + key.size()));
}

std::string preparation_report(const std::string& json) {
  const auto begin = json.find("\"reclaim_preparation\":{");
  require(begin != std::string::npos, "Missing preparation diagnostics");
  const auto end = json.find('}', begin);
  require(end != std::string::npos, "Unterminated preparation diagnostics");
  return json.substr(begin, end - begin + 1);
}

void verify_ram_profile() {
  constexpr std::uint64_t mib = 1024 * 1024;
  const auto modest = Session::ram_profile(768 * mib, 256 * mib, false);
  require(modest.physical_bytes == 2048 * mib && modest.backing_bytes == 256 * mib,
          "RAM budget must leave headroom for Android");
  const auto ample = Session::ram_profile(2304 * mib, 256 * mib, false);
  require(ample.backing_bytes == 1024 * mib, "Available RAM must raise the backing budget");
  const auto abundant = Session::ram_profile(8192 * mib, 256 * mib, false);
  require(abundant.backing_bytes == 1024 * mib, "Resident budget must leave room for other host work");
  if (cosmic_pc::PcMachine::max_ram_mib() >= 4096) {
    require(ample.physical_bytes == 4096 * mib, "A device with sufficient RAM budget should select 4 GiB");
    const auto report = Session::check_hardware({}, -1, 0, {}, ample);
    require(report.find("\"ram_profile\":{\"address_space_bytes\":4294967296,") != std::string::npos &&
                report.find("\"backing_budget_bytes\":1073741824") != std::string::npos,
            "Hardware preflight must report its selected RAM profile and backing budget");
  }
  const auto prime = Session::ram_profile(4096 * mib, 256 * mib, false, true);
  const auto report = Session::check_hardware({}, -1, 0, {}, prime);
  require(counter(report, "ram_clock_hz") == 4'500'000'000'000ULL, "RAM clock in hardware preflight");
  require(counter(report, "usable_bytes") == Session::kPrimeRamBytes &&
              counter(report, "address_space_bytes") == prime.physical_bytes &&
              counter(report, "backing_budget_bytes") == 2880 * mib &&
              report.find("\"resident_policy\":\"android-surplus\"") != std::string::npos &&
              counter(report, "resident_bytes") < mib,
          "Prime must expose 64 GiB while reserving host memory");
  const auto phone = Session::ram_profile(768 * mib, 256 * mib, false, true);
  require(phone.backing_bytes == 384 * mib, "Prime must reserve 128 MiB above the Android threshold");
  const auto abundant_prime = Session::ram_profile(UINT64_MAX, 0, false, true);
  require(abundant_prime.backing_bytes == abundant_prime.physical_bytes,
          "Overflow must not limit backing on a host with abundant RAM");
  Session live({}, 0, Session::BootSource::SelfTest, -1, 0, phone);
  require(counter(live.diagnostic(), "ram_clock_hz") == 4'500'000'000'000ULL, "RAM clock in live telemetry");
  const auto used = counter(live.diagnostic(), "resident_bytes");
  constexpr std::uint64_t page = cosmic_pc::MemoryFabric::kPageSize;
  live.sample_ram_pressure(768 * mib - used, 256 * mib, false);
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(), "backing_budget_bytes") == phone.backing_bytes,
          "Accounting for already resident bytes must not grow the budget on a stable host");
  live.sample_ram_pressure(1024 * mib, 256 * mib, false);
  const auto surplus = 768 * mib + used;
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(), "backing_budget_bytes") == (surplus - surplus / 4) / page * page,
          "Prime must use only the live surplus left after the reserve");
  live.sample_ram_pressure(256 * mib, 256 * mib, false);
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(), "backing_budget_bytes") == 16 * mib &&
              counter(live.diagnostic(), "usable_bytes") == Session::kPrimeRamBytes,
          "Reaching the Android threshold must reclaim payload without reducing guest RAM");
  try {
    (void)Session::ram_profile(300 * mib, 256 * mib, true);
  } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("Starting a VM with Android memory pressure must be rejected");
}

#ifndef PRIME_PREVIOUS_ENGINE_WINDOW
// The historical engine-window benchmark compiles this harness against the
// 0.3.66 session, which predates the optional backing-read timing API.
void verify_ram_timing_option(const std::filesystem::path& directory) {
  constexpr std::uint64_t mib = 1024 * 1024;
  const auto profile = Session::ram_profile(768 * mib, 256 * mib, false, true);
  Session live({}, 0, Session::BootSource::SelfTest, -1, 0, profile, directory);
  const auto initial = live.diagnostic(false);
  require(initial.find("\"backing_timing_enabled\":false") != std::string::npos &&
              counter(initial, "backing_timed_attempts") == 0,
          "New sessions must leave detailed backing-read timers disabled");
  live.step(32);
  const auto before = live.diagnostic(false);
  const auto steps = live.session_steps();
  live.set_backing_read_timing_enabled(true);
  require(live.diagnostic(false).find("\"backing_timing_enabled\":true") != std::string::npos,
          "The owner must be able to enable backing timers between execution windows");
  live.set_backing_read_timing_enabled(false);
  const auto disabled = live.diagnostic(false);
  require(disabled.find("\"backing_timing_enabled\":false") != std::string::npos &&
              steps > 0 && live.session_steps() == steps &&
              counter(disabled, "resident_bytes") == counter(before, "resident_bytes") &&
              counter(disabled, "backing_budget_bytes") == counter(before, "backing_budget_bytes") &&
              counter(disabled, "forced_reclaims") == counter(before, "forced_reclaims") &&
              counter(disabled, "backing_read_attempts") == counter(before, "backing_read_attempts"),
          "Disabling timers must preserve the guest, its budget and accumulated counters");
  live.set_backing_read_timing_enabled(true);
  Session next({}, 0, Session::BootSource::SelfTest, -1, 0, profile, directory);
  require(next.diagnostic(false).find("\"backing_timing_enabled\":false") != std::string::npos,
          "A new session must not inherit the previous session's measurement mode");
  std::cout << "PRIME_RAM_TIMING_OPTION_PASS default_off=1 toggle=1 preserves_guest=1 next_session_off=1\n";
}
#endif

void verify_ram_spill(const std::filesystem::path& directory) {
  using Backing = reverse_windows::AndroidRamBacking;
  constexpr auto page = Backing::kPageSize;
  constexpr auto mib = 1024 * 1024;
  const auto cache = directory / "cache";
  std::filesystem::create_directory(cache);
  const auto fallback = Session::check_hardware({}, -1, 0, {}, Session::kDefaultRamProfile,
                                               cache / "does-not-exist");
  require(fallback.find("\"spill_mode\":\"unavailable\"") != std::string::npos &&
              fallback.find("\"spill_limit_bytes\":0") != std::string::npos,
          "Hardware preflight must explain fallback when app cache is unavailable");
  // These large free-space requirements are for production hosts. Skip the
  // file-backed section only when the test runner's cache cannot support it.
  if (Backing::spill_budget(cache) < page) return;
  {
    Backing backing(cache, 4 * page, page);
    require(std::filesystem::is_empty(cache), "Guest RAM spill must not leave directory entries");
    std::array<std::uint8_t, page> original{}, restored{};
    original[0] = 0x81;
    original[page - 1] = 0x7e;
    backing.write(3, original.data());
    backing.read(3, restored.data());
    require(original == restored && backing.file_payload_bytes() == page,
            "RAM spill must restore both boundaries of the exact page");
    original[1] = 0xf2;
    backing.write(3, original.data());
    bool limit_reached = false;
    try {
      backing.write(2, original.data());
    } catch (const std::runtime_error&) {
      limit_reached = true;
    }
    backing.read(3, restored.data());
    require(limit_reached && original == restored && backing.file_payload_bytes() == page,
            "New slots must obey the spill budget without corrupting existing slots");
    backing.reset();
    require(backing.read_us() == 0 && backing.write_us() == 0,
            "Guest reset must clear spill timing along with its counters");
    bool old_page_released = false;
    try {
      backing.read(3, restored.data());
    } catch (const std::runtime_error&) {
      old_page_released = true;
    }
    require(old_page_released && backing.file_payload_bytes() == 0,
            "Guest reset must release the previous RAM contents");
    backing.write(2, original.data());
    backing.read(2, restored.data());
    require(original == restored, "Reset must allow fresh RAM slots");
  }
  {
    const Session::RamProfile small{2 * mib, 64 * page};
    Session demo({}, 0, Session::BootSource::SelfTest, -1, 0, small, cache);
    require(demo.diagnostic().find("\"spill_mode\":\"file\"") != std::string::npos &&
                demo.diagnostic().find("\"backing_budget_bytes\":262144") != std::string::npos,
            "The actual BIOS session must attach the optional spill without inflating its resident limit");
    constexpr auto threshold = 256 * mib;
    Session live({}, 0, Session::BootSource::SelfTest, -1, 0, Session::kDefaultRamProfile, cache);
    const auto boot = live.diagnostic();
    const auto usable = counter(boot, "usable_bytes");
    live.sample_ram_pressure(threshold + 8 * mib, threshold, true);
    require(!live.maintain_ram_budget() &&
                counter(live.diagnostic(), "backing_budget_bytes") == 16 * mib &&
                counter(live.diagnostic(), "usable_bytes") == usable,
            "Android pressure must lower the live backing budget without changing E820 RAM");
    live.sample_ram_pressure(threshold + 2 * Session::kMaxResidentBytes, threshold, false);
    require(!live.maintain_ram_budget() &&
                counter(live.diagnostic(), "backing_budget_bytes") == Session::kMaxResidentBytes &&
                counter(live.diagnostic(), "address_space_bytes") == 2048ULL * mib &&
                counter(live.diagnostic(), "usable_bytes") == usable,
            "Android headroom must restore the budget without switching the active guest to 4 GiB");
    Session::Step step = Session::Step::Running;
    for (unsigned i = 0; i < 128 && step == Session::Step::Running; ++i) step = live.step(128);
    std::array<std::uint8_t, 4000> cells{};
    require(step == Session::Step::WaitingKey && live.copy_text(cells.data(), cells.size()) == cells.size(),
            "The BIOS guest must still reach its keyboard prompt after pressure adjustments");
    std::cout << "PRIME_C04_ANDROID_PRESSURE_PASS low_mib=16 restored_mib=1024 guest_mib=2048\n";
    require(std::filesystem::is_empty(cache), "Session history must not retain temporary RAM files");
  }
  {
    constexpr std::size_t resident = 8 * page;
    using Pc = cosmic_pc::PcMachine;
    Pc pc(Pc::high_ram_address_space(Session::kPrimeRamBytes, true), resident);
    pc.cpu.a20_enabled = true;
    const auto address = pc.cpu.memory.size() - 20 * page;
    auto file = std::make_unique<Backing>(cache, pc.cpu.memory.size(), 32 * page);
    auto* file_stats = file.get();
    pc.cpu.memory.attach_page_backing(std::move(file));
    // Give each page too many changed words to keep it in compact RAM.
    for (unsigned index = 0; index < 20; ++index) {
      pc.cpu.memory.fill(address + index * page + 256, 0x58, 512);
      pc.cpu.write32(address + index * page, 0x50580000U + index);
      pc.cpu.write32(address + index * page + page - 4, 0xdeadc0deU - index);
    }
    const auto before = pc.cpu.memory.metrics();
    require(before.allocated_bytes <= resident && before.spilled_pages >= 12 &&
                before.backing_writes >= 12 && file_stats->file_payload_bytes() >= 12 * page,
            "PcMachine must cross the resident limit by evicting pages to the real file");
    using Adjustment = cosmic_pc::SparseRam::BudgetAdjustment;
    require(pc.cpu.memory.adjust_payload_budget(resident / 2, 2) == Adjustment::Reclaiming &&
                pc.cpu.memory.adjust_payload_budget(resident / 2, 2) == Adjustment::Applied &&
                pc.cpu.memory.budget_bytes() == resident / 2,
            "A populated guest must shrink in bounded batches on the real file");
    for (unsigned index = 0; index < 20; ++index) {
      require(pc.cpu.read32(address + index * page) == 0x50580000U + index &&
                  pc.cpu.read32(address + index * page + page - 4) == 0xdeadc0deU - index,
              "Guest bytes must survive real-file eviction and reload");
    }
    require(pc.cpu.memory.metrics().backing_reads >= 12, "Guest reads must reach the real spill file");
    require(pc.cpu.read32(address & UINT32_MAX) == 0, "A high RAM spill must never alias a low page");
    pc.cpu.memory.reset_zero();
    require(pc.cpu.memory.metrics().spilled_pages == 0 && file_stats->file_payload_bytes() == 0 &&
                pc.cpu.read32(address) == 0,
            "Guest RAM reset must discard the old file-backed image");

    // The same high guest addresses with few changes now fit in compact RAM.
    constexpr unsigned guest_pages = 20;
    constexpr auto compact_bytes = guest_pages * (sizeof(std::uint64_t) +
        2 * (sizeof(std::uint16_t) + sizeof(std::uint64_t)));
    for (unsigned index = 0; index < guest_pages; ++index) {
      pc.cpu.write32(address + index * page, 0x12120000U + index);
      pc.cpu.write32(address + index * page + page - 4, 0xaabbcc00U + index);
    }
    require(pc.cpu.memory.adjust_payload_budget(compact_bytes, guest_pages) == Adjustment::Applied,
            "Guest pressure must be able to reclaim the dense pages as exact pattern deltas");
    const auto compact = pc.cpu.memory.metrics();
    require(compact.pattern_delta_pages == guest_pages && compact.allocated_bytes == compact_bytes &&
                compact.pattern_delta_bytes == compact_bytes && compact.backing_writes == 0 &&
                file_stats->file_payload_bytes() == 0,
            "Sparse guest writes should fit the compact budget without touching QSSD");
    for (unsigned index = 0; index < guest_pages; ++index) {
      require(pc.cpu.read32(address + index * page) == 0x12120000U + index &&
                  pc.cpu.read32(address + index * page + page - 4) == 0xaabbcc00U + index,
              "Compact guest reads lost a change above 4 GiB");
    }
    require(pc.cpu.memory.adjust_payload_budget(0, guest_pages) == Adjustment::Applied &&
                pc.cpu.memory.metrics().pattern_delta_pages == 0 &&
                file_stats->file_payload_bytes() == guest_pages * page,
            "The QSSD fallback must materialize complete compact pages when needed");
    for (unsigned index = 0; index < guest_pages; ++index) {
      require(pc.cpu.read32(address + index * page) == 0x12120000U + index &&
                  pc.cpu.read32(address + index * page + 8) == 0 &&
                  pc.cpu.read32(address + index * page + page - 4) == 0xaabbcc00U + index,
              "Compact pages changed after transfer to QSSD");
    }
    std::cout << "PRIME_RAM_PATTERN_DELTA_GUEST_PASS high_address=" << address
              << " dense_payload_bytes=" << guest_pages * page
              << " compact_payload_bytes=" << compact_bytes << " initial_qssd_writes=0\n";
  }
  std::cout << "REVERSE_WINDOWS_ANDROID_RAM_SPILL_PASS anonymous_file=true resident_cap=true"
               " limit_failure_preserves_data=true reset_releases_slots=true\n";
  require(std::filesystem::is_empty(cache), "Temporary RAM must disappear on session close");
}

void verify_packed_ram64(const std::filesystem::path& directory) {
  using Backing = reverse_windows::AndroidRamBacking;
  using Pc = cosmic_pc::PcMachine;
  constexpr auto page = Backing::kPageSize;
  constexpr std::uint64_t mib = 1024 * 1024;
  constexpr auto threshold = 256 * mib;
  const auto cache = directory / "packed-ram-cache";
  std::filesystem::create_directory(cache);
  require(Backing::spill_budget(cache) >= 8 * page, "Packed RAM test requires a writable disk");

  const auto prime = Session::ram_profile(threshold + 600 * mib, threshold, false, true);
  Session live({}, 0, Session::BootSource::SelfTest, -1, 0, prime, cache);
  const auto initial = live.diagnostic();
  require(initial.find("\"spill_compression\":true") != std::string::npos &&
              counter(initial, "backing_budget_bytes") == 450 * mib &&
              counter(initial, "spill_cache_capacity_bytes") == 2 * mib,
          "A normal Prime session must attach the compressed QSSD store");
  live.sample_ram_pressure(0, 0, true);
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(), "backing_budget_bytes") == 16 * mib &&
              counter(live.diagnostic(), "spill_cache_capacity_bytes") == 128 * 1024 &&
              live.diagnostic().find("\"pressure_background_trim\":true") != std::string::npos,
          "Explicit background trim must still reclaim all but the floor");
  live.sample_ram_pressure(threshold + 600 * mib - 32 * 1024, threshold, false);
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(), "pressure_target_bytes") >= 449 * mib &&
              counter(live.diagnostic(), "usable_bytes") == Session::kPrimeRamBytes,
          "Prime must restore its resident budget with the same E820 map");

  std::array<std::uint8_t, page> raw{}, first{}, second{}, restored{};
  std::uint32_t random = 0x13579bdfU;
  for (auto& byte : raw) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    byte = std::uint8_t(random);
  }
  std::copy_n(raw.begin(), 1500, first.begin() + 256);
  second = first;
  second.back() = 0x8e;
  const auto end = prime.physical_bytes / page - 1;
  {
    Backing backing(cache, prime.physical_bytes, 2 * page, true);
    backing.write(end, first.data());
    backing.write(1, second.data());
    backing.write(0, raw.data());
    require(backing.packed_pages() == 2 && backing.file_payload_bytes() == 2 * page,
            "Two compressed pages and one raw page must fit in two QSSD blocks");
    bool full = false;
    try { backing.write(end, raw.data()); }
    catch (const std::runtime_error&) { full = true; }
    backing.read(end, restored.data());
    require(full && restored == first, "A full QSSD pool must keep the previous valid page");
    backing.read(1, restored.data());
    require(restored == second, "A failed expansion must preserve the neighboring record");
    backing.discard(1);
    backing.write(end, raw.data());
    backing.read(end, restored.data());
    require(restored == raw && !backing.packed_pages(), "A sole half can grow in its old block");
    auto changed_raw = raw;
    changed_raw.back() ^= 1;
    backing.write(end, changed_raw.data());
    backing.read(end, restored.data());
    require(restored == changed_raw, "A byte change must invalidate the exact raw candidate");
    backing.write(end, first.data());
    backing.write(2, second.data());
    backing.read(2, restored.data());
    require(restored == second && backing.packed_pages() == 2 &&
                backing.file_payload_bytes() == 2 * page,
            "Shrinking a raw record must make its other half available");
    const auto codec = backing.codec_stats();
    require(codec.encode_attempts == 6 && codec.encode_packed == 4 && codec.encode_raw == 2 &&
                codec.encode_bypassed == 2 && codec.decode_attempts == 3 &&
                codec.encode.wall_ns > 0 && codec.decode.wall_ns > 0,
            "Codec work must exclude raw reads and exact repeats of a rejected page");
    backing.reset();
    require(backing.file_payload_bytes() == 0 && backing.packed_pages() == 0,
            "Reset must clear every slot in the temporary compressed backing");
    backing.write(0, changed_raw.data());
    const auto after_reset = backing.codec_stats();
    require(backing.file_payload_bytes() == page && backing.packed_pages() == 0 &&
                after_reset.encode_attempts == 1 && after_reset.encode_raw == 1 &&
                after_reset.encode_bypassed == 0 && after_reset.decode_attempts == 0,
            "Reset must clear the compressed slot index and remembered codec work");
    std::cout << "PRIME_RAM_CODEC_SPLIT_PASS encode=" << codec.encode_attempts
              << " decode=" << codec.decode_attempts << " bypass=" << codec.encode_bypassed
              << " encode_ns=" << codec.encode.wall_ns << " decode_ns=" << codec.decode.wall_ns << '\n';
  }

  {
    // A deadline during compression must leave the uncommitted suffix in
    // the caller's RAM. The next write can group it with a new page.
    using Ram = cosmic_pc::SparseRam;
    Backing backing(cache, prime.physical_bytes, 3 * page, true);
    const Backing::PageWrite first_batch[]{{0, raw.data()}, {1, first.data()}};
    require(backing.write_pages(first_batch, 2, Ram::ReclaimClock::time_point::min()) == 1 &&
                backing.write_calls() == 1 && backing.write_batches() == 0,
            "An expired codec grant must commit only its first page");
    const Backing::PageWrite second_batch[]{{1, first.data()}, {end, second.data()}};
    require(backing.write_pages(second_batch, 2, Ram::ReclaimClock::time_point::max()) == 2 &&
                backing.write_calls() == 2 && backing.write_batches() == 1 &&
                backing.grouped_pages() == 2 && backing.file_payload_bytes() == 2 * page,
            "Adjacent packed halves must use one file write with correct accounting");
    backing.read(0, restored.data());
    require(restored == raw, "The first committed page must remain intact");
    backing.read(1, restored.data());
    require(restored == first, "The resumed page must match its caller's RAM");
    backing.read(end, restored.data());
    require(restored == second, "The other packed half must match its caller's RAM");
  }

  {
    // Store the two packed formats next to each other in one QSSD block.
    // The sparse page has 247 live words, the largest mask that fits.
    std::array<std::uint8_t, page> sparse{}, repeated;
    for (std::size_t word = 0; word < 246; ++word) {
      std::copy_n(raw.data() + word * 8, 8, sparse.data() + word * 16);
      sparse[word * 16] |= 1;
    }
    sparse.back() = 0x91;
    for (std::size_t offset = 0; offset < repeated.size(); offset += 128)
      std::copy_n(raw.data(), 128, repeated.data() + offset);

    using Ram = cosmic_pc::SparseRam;
    Backing backing(cache, prime.physical_bytes, 2 * page, true);
    const Backing::PageWrite mixed[]{{0, sparse.data()}, {1, repeated.data()}, {2, raw.data()}};
    require(backing.write_pages(mixed, 3, Ram::ReclaimClock::time_point::max()) == 3 &&
                backing.packed_pages() == 2 && backing.file_payload_bytes() == 2 * page,
            "Sparse, dictionary and raw pages must share the original file quota");
    for (const auto& write : mixed) {
      backing.read(write.page, restored.data());
      require(std::equal(restored.begin(), restored.end(), write.bytes),
              "Each mixed spill format must restore all 4096 bytes");
    }
    const auto codec = backing.codec_stats();
    require(codec.encode_sparse == 1 && codec.encode_deflate == 2 && codec.decode_sparse == 1 &&
                codec.encode_raw == 1 && codec.decode_attempts == 2,
            "Mixed spill metrics must distinguish sparse, deflate and raw pages");
  }

  Pc pc(prime.physical_bytes, 8 * page);
  pc.reserve_pci_mmio(true);
  pc.cpu.a20_enabled = true;
  auto backing = std::make_unique<Backing>(cache, prime.physical_bytes, 8 * page, true);
  auto* stats = backing.get();
  auto& memory = pc.cpu.memory;
  memory.reset_zero();
  memory.attach_page_backing(std::move(backing));
  using Adjustment = cosmic_pc::SparseRam::BudgetAdjustment;
  require(memory.adjust_payload_budget(2 * page, 0) == Adjustment::Applied,
          "The high-memory test must start with a two-page resident budget");
  const auto address = (end - 15) * page;
  for (unsigned i = 0; i < 16; ++i) {
    first[0] = std::uint8_t(i + 1);
    memory.write_block(address + i * page, first.data(), first.size());
  }
  require(memory.adjust_payload_budget(0, 16) == Adjustment::Applied &&
              memory.metrics().spilled_pages == 16 && stats->packed_pages() == 16 &&
              stats->file_payload_bytes() == 8 * page,
          "Mundo X must back sixteen distinct high RAM pages with eight storage blocks");
  for (unsigned i = 0; i < 16; ++i) {
    first[0] = std::uint8_t(i + 1);
    memory.read_block(address + i * page, restored.data(), restored.size());
    require(restored == first, "Every high guest page must round-trip byte for byte");
  }
  memory.fill(address, 0, page);
  require(stats->packed_pages() == 15 && pc.cpu.read32(address) == 0,
          "Zeroing a spilled guest page must release its slot");
  require(memory.adjust_payload_budget(2 * page, 0) == Adjustment::Applied,
          "Mundo X must be able to restore a two-page resident budget");
  memory.write_block(address - page, second.data(), second.size());
  require(memory.adjust_payload_budget(0, 16) == Adjustment::Applied &&
              stats->packed_pages() == 16 && stats->file_payload_bytes() == 8 * page,
          "New guest addresses must be able to reuse the released QSSD slot");
  memory.read_block(address - page, restored.data(), restored.size());
  require(restored == second, "Reusing a slot must preserve the new guest page");
  std::cout << "PRIME_RAM64_QSSD_PACKED_PASS guest_bytes=" << Session::kPrimeRamBytes
            << " highest_test_address=" << end * page
            << " guest_test_bytes=" << 16 * page << " qssd_test_bytes="
            << stats->file_payload_bytes() << " prime_phone_budget_mib=" << prime.backing_bytes / mib << '\n';
}

void verify_deferred_ram_reclaim(const std::filesystem::path& directory) {
  using Backing = reverse_windows::AndroidRamBacking;
  using Ram = cosmic_pc::SparseRam;
  using Clock = Ram::ReclaimClock;
  using Adjustment = Ram::BudgetAdjustment;
  constexpr auto page = Backing::kPageSize;
  constexpr auto wait_limit = std::chrono::seconds(10);
  constexpr auto grant = std::chrono::milliseconds(3);
  constexpr std::size_t sparse_pages = 64, raw_pages = 128;
  constexpr std::size_t total_pages = sparse_pages + raw_pages;
  constexpr std::uint64_t capacity = (sparse_pages / 2 + raw_pages) * page;
  constexpr std::uint64_t roomier_capacity = 1024 * page;
  constexpr std::uint64_t logical = 2048 * page;
  constexpr auto cache_policy = Ram::BackingCachePolicy::Adaptive;
  const auto cache = directory / "deferred-ram-cache";
  std::filesystem::create_directory(cache);
  require(Backing::spill_budget(cache) >= roomier_capacity, "Deferred RAM test needs storage headroom");

  std::vector<std::uint8_t> input(total_pages * page);
  std::uint32_t random = 0x193ac6e5U;
  for (std::size_t index = 0; index < total_pages; ++index) {
    const auto bytes = index < sparse_pages ? 1450 : page;
    auto* first = input.data() + index * page;
    for (std::size_t byte = 0; byte < bytes; ++byte) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      first[byte] = std::uint8_t(random);
    }
  }
  const auto wait_for_worker = [&](const Backing& backing) {
    const auto deadline = Clock::now() + wait_limit;
    while (backing.staging_stats().pending_pages && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(backing.staging_stats().pending_pages == 0, "Deferred RAM work must settle");
  };
  const auto packed_write = [&](Backing& backing, std::uint64_t index, const std::uint8_t* source) {
    const Backing::PageWrite write{index, source};
    const auto deadline = Clock::now() + wait_limit;
    while (Clock::now() < deadline) {
      if (backing.write_pages(&write, 1, Clock::now() + grant) == 1) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Deferred backed page must make progress");
  };

  {
    Ram ram(logical, logical);
    auto backing = std::make_unique<Backing>(cache, logical, roomier_capacity, true, true);
    auto& store = *backing;
    ram.attach_page_backing(std::move(backing), cache_policy);
    ram.write_block(0, input.data(), page);
    const auto deadline = Clock::now() + wait_limit;
    auto result = Adjustment::Reclaiming;
    while (result == Adjustment::Reclaiming && Clock::now() < deadline)
      result = ram.adjust_payload_budget(0, 1, Clock::now() + grant);
    const auto enabled = store.staging_stats().enabled;
    require(result == Adjustment::Applied && ram.allocated_bytes() == 0 &&
                store.staging_stats().staged_pages == unsigned(enabled),
            "A finite single-page reclaim must use staging when the worker is available");
    std::array<std::uint8_t, page> restored;
    ram.read_block(0, restored.data(), restored.size());
    require(std::equal(restored.begin(), restored.end(), input.data()),
            "Raw staged RAM must be visible before compression finishes");
    wait_for_worker(store);
    const auto staged = store.staging_stats();
    require(staged.packed_pages == unsigned(enabled) && store.packed_pages() == 1 &&
                (staged.background_encode_ns > 0) == enabled && staged.failures == 0,
            "Deferred or synchronous packing must finish the single page");
    std::cout << "PRIME_DEFERRED_RAM_SINGLE_PASS worker_enabled=" << enabled << '\n';
  }

  for (const bool allow_deferred : {false, true}) {
    Ram ram(logical, logical);
    auto backing = std::make_unique<Backing>(cache, logical, capacity, true, allow_deferred);
    auto& store = *backing;
    ram.attach_page_backing(std::move(backing), cache_policy);
    ram.write_block(0, input.data(), input.size());
    const auto deadline = Clock::now() + wait_limit;
    std::vector<std::uint8_t> restored(input.size());
    auto result = Adjustment::Reclaiming;
    do {
      result = ram.adjust_payload_budget(0, 128, Clock::now() + grant);
      ram.read_block(0, restored.data(), restored.size());
      require(restored == input, "Spill must preserve all bytes during deferred compression");
      if (result == Adjustment::Reclaiming) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (result == Adjustment::Reclaiming && Clock::now() < deadline);
    require(result == Adjustment::Applied && ram.allocated_bytes() == 0 &&
                ram.spilled_pages() == total_pages,
            "Mixed RAM reclaim must fit at the exact packed capacity");
    wait_for_worker(store);
    const auto staged = store.staging_stats();
    require(store.file_payload_bytes() == capacity && store.packed_pages() == sparse_pages &&
                staged.failures == 0 &&
                (staged.enabled ? staged.staged_pages > 0 && staged.staged_pages < total_pages
                                : staged.staged_pages == 0) &&
                (allow_deferred || !staged.enabled),
            "Deferred packing must preserve the quota and single-core fallback");
    bool full = false;
    try { store.write(total_pages, input.data() + sparse_pages * page); }
    catch (const Backing::CapacityExceeded&) { full = true; }
    ram.read_block(0, restored.data(), restored.size());
    require(full && restored == input, "Quota exhaustion must preserve all backed RAM");
  }

  {
    Backing backing(cache, logical, roomier_capacity, true, true);
    std::array<std::uint8_t, page> restored;
    for (unsigned attempt = 0; attempt < 18; ++attempt) {
      // A pending worker must never publish a copy after the guest releases
      // its record and replaces that page or after the whole store is reset.
      packed_write(backing, 0, input.data());
      if (attempt % 3 == 0) backing.discard(0);
      if (attempt % 3 == 1) backing.reset();
      backing.write(0, input.data() + sparse_pages * page);
      // Follow the replacement with another job so an in-flight worker
      // must finish processing its older snapshot before we read it back.
      packed_write(backing, 1, input.data() + page);
      wait_for_worker(backing);
      backing.read(0, restored.data());
      require(std::equal(restored.begin(), restored.end(), input.data() + sparse_pages * page),
              "A worker snapshot must not overwrite a reused page after discard or reset");
      backing.discard(0);
      backing.discard(1);
    }
  }
  std::cout << "PRIME_DEFERRED_RAM_RECLAIM_PASS exact_capacity=1 single_page=1"
               " single_core_fallback=1 generation_checked=1\n";
}

void verify_shared_ram(const std::filesystem::path& directory) {
  using Backing = reverse_windows::AndroidRamBacking;
  using Ram = cosmic_pc::SparseRam;
  using CachePolicy = Ram::BackingCachePolicy;
  constexpr auto page = Backing::kPageSize;
  constexpr auto base = cosmic_pc::PcVideo::kFramebufferAddress;
  constexpr std::uint64_t mib = 1024 * 1024;
  constexpr unsigned rows = 96, rounds = 12;
  const auto cache = directory / "shared-ram-cache";
  std::filesystem::create_directory(cache);

  struct Result { std::uint64_t reads, hits; std::int64_t us; };
  const auto run = [&](CachePolicy policy) -> Result {
    cosmic_pc::PcMachine pc(2048 * mib, 64 * mib);
    auto& cpu = pc.cpu;
    auto& ram = cpu.memory;
    ram.reset_zero();
    cpu.a20_enabled = true;
    cosmic_pc::PcVideo video;
    cpu.set_reg16(cosmic_pc::Cpu::EAX, 0x4f02);
    cpu.set_reg16(cosmic_pc::Cpu::EBX, 0x4142);
    video.bios(cpu);
    require(video.info().graphics && video.info().pitch == page,
            "The VBE fixture needs one framebuffer page per row");
    auto qssd = std::make_unique<Backing>(cache, ram.size(), 2 * rows * page, true);
    ram.attach_page_backing(std::move(qssd), policy);

    std::array<std::uint8_t, page> original{}, disk_buffer{};
    std::array<std::uint8_t, rows * 4> pixels{};
    for (unsigned i = 128; i < 896; ++i) original[i] = std::uint8_t(i * 13);
    original[1] = 0xd4;
    original[2] = 0x3b;
    for (unsigned row = 0; row < rows; ++row) {
      original[0] = std::uint8_t(row);
      ram.write_block(base + row * page, original.data(), original.size());
    }
    const auto spilled = ram.adjust_payload_budget(0, rows);
    require(spilled == Ram::BudgetAdjustment::Applied && ram.metrics().spilled_pages == rows &&
                ram.adjust_payload_budget(64 * mib, 0) == Ram::BudgetAdjustment::Applied,
            "Shared RAM fixture must put each row in real QSSD backing");

    bool exact = true;
    const auto started = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) {
      for (unsigned row = 0; row < rows; ++row)
        exact &= cpu.read32(base + row * page) == (0x003bd400U | row);
      for (unsigned row = 0; row < rows; ++row) {
        original[0] = std::uint8_t(row);
        exact &= cpu.try_read_ram_block(base + row * page, disk_buffer.data(), page) && disk_buffer == original;
      }
      video.copy_rgba(ram, 0, 0, 1, rows, pixels.data(), pixels.size());
      for (unsigned row = 0; row < rows; ++row) {
        const auto* pixel = pixels.data() + 4 * row;
        exact &= pixel[0] == 0x3b && pixel[1] == 0xd4 && pixel[2] == row && pixel[3] == 255;
      }
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    require(exact, "CPU, BIOS transfers and VBE must see exactly the same guest bytes");
    const auto stats = ram.metrics();
    if (policy == CachePolicy::Adaptive) {
      require(stats.backing_cache_bytes == rows * page &&
                  ram.adjust_payload_budget(16 * mib, 0) == Ram::BudgetAdjustment::Applied &&
                  ram.backing_cache_bytes() > 0 && ram.backing_cache_bytes() <= 128 * 1024 &&
                  ram.metrics().backing_cache_capacity_bytes == 128 * 1024 &&
                  ram.allocated_bytes() + ram.backing_cache_bytes() <= ram.budget_bytes(),
              "Reducing the Android budget must retain valid decoded pages within the smaller budget");
    }
    original[0] = 0xa1;
    require(cpu.try_write_ram_block(base, original.data(), page) &&
                ram.metrics().backing_reads == stats.backing_reads && ram.metrics().backing_overwrite_pages == 1,
            "A full BIOS transfer into backed RAM must skip the old page");
    require(cpu.read32(base) == 0x003bd4a1, "A full BIOS transfer must publish its new bytes");
    if (policy == CachePolicy::Adaptive) {
      reverse_windows::SmpWorkers workers(2);
      std::array<std::array<std::uint8_t, page>, 2> restored;
      original[0] = 1;
      const auto before_workers = ram.metrics();
      const auto lanes = workers.try_run(2, [&](unsigned worker, unsigned) {
        ram.read_parallel_ram_block(base + page, restored[worker].data(), page);
      });
      require(lanes == 2, "Shared QSSD test needs two readers");
      const auto shared = ram.metrics();
      const auto worker_reads = shared.backing_reads - before_workers.backing_reads;
      const auto worker_hits = shared.backing_cache_hits - before_workers.backing_cache_hits;
      require(restored[0] == original && restored[1] == original &&
                  worker_reads <= 1 && worker_reads + worker_hits == 2,
              "Both workers must share exact decoded bytes with at most one real QSSD read");
    }
    return {stats.backing_reads, stats.backing_cache_hits, us};
  };

  const auto fixed = run(CachePolicy::Fixed);
  const auto adaptive = run(CachePolicy::Adaptive);
  require(fixed.reads == 3 * rows * rounds && adaptive.reads == rows &&
              adaptive.hits == 3 * rows * rounds - rows,
          "The adaptive cache must retrieve shared pages once across CPU, disk and video");
  std::cout << "PRIME_C04_SHARED_RAM_PASS fixed_reads=" << fixed.reads << " adaptive_reads=" << adaptive.reads
            << " cache_hits=" << adaptive.hits << " fixed_us=" << fixed.us << " adaptive_us=" << adaptive.us
            << " exact=1\n";
}

void run_until(Session& session, Session::Step expected, const char* message, unsigned budget = 64) {
  for (unsigned steps = 0; steps < 1024; steps += budget) {
    const auto status = session.step(budget);
    if (status == expected) return;
    if (status != Session::Step::Running) break;
  }
  throw std::runtime_error(std::string(message) + ": " + session.diagnostic());
}

using VgaCells = std::array<std::uint8_t, 4000>;

VgaCells capture_cells(Session& session) {
  VgaCells cells{};
  require(!session.video_info().graphics, "Expected guest BIOS text output");
  require(session.copy_text(cells.data(), cells.size()) == cells.size(),
          "Guest VGA text update missing");
  return cells;
}

std::string screen(const VgaCells& cells) {
  std::string output;
  output.reserve(cells.size() / 2);
  for (std::size_t i = 0; i < cells.size(); i += 2) output.push_back(char(cells[i]));
  return output;
}

void verify_real_mode_ps2(const std::filesystem::path& path, bool via_irq) {
  // Keep IF clear so an IRQ1-enabled controller can be polled too. The
  // motherboard regression exercises actual IRQ delivery and return.
  std::vector<std::uint8_t> code{0xfa}; // CLI
  if (via_irq) {
    const std::uint8_t configure[] = {0xb0, 0x60, 0xe6, 0x64, 0xb0, 0x05, 0xe6, 0x60};
    code.insert(code.end(), std::begin(configure), std::end(configure));
  } else {
    const std::uint8_t start[] = {0xb0, 0xf4, 0xe6, 0x60, 0xe4, 0x60};
    code.insert(code.end(), std::begin(start), std::end(start));
  }
  // Check real scan-set 2 bytes for an extended make and break.
  for (const std::uint8_t expected : {0xe0, 0x6b, 0xe0, 0xf0, 0x6b}) {
    const std::uint8_t entry[] = {
        0xe4, 0x64, 0xa8, 0x01, 0x74, 0xfa,       // Poll while the output buffer is empty.
        0xe4, 0x60, 0x3c, expected, 0x74, 0x02, 0xfa, 0xf4};
    code.insert(code.end(), std::begin(entry), std::end(entry));
  }
  // A separate Enter goes through a blocking BIOS call even though the
  // controller is configured. Neither the arrow nor Enter is delivered twice.
  const std::uint8_t wait[] = {
      0xb4, 0x00, 0xcd, 0x16, 0x3d, 0x0d, 0x1c, 0x74, 0x02, 0xfa, 0xf4,
      0xe4, 0x64, 0xa8, 0x01, 0x74, 0x02, 0xfa, 0xf4,
      0xb8, 'K', 0x0e, 0xcd, 0x10, 0xfa, 0xf4};
  code.insert(code.end(), std::begin(wait), std::end(wait));
  std::array<std::uint8_t, 512> boot{};
  std::copy(code.begin(), code.end(), boot.begin());
  boot[510] = 0x55;
  boot[511] = 0xaa;
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()) && disk.flush(), "Could not persist the keyboard test sector");
  }
  Session session(path, 0, Session::BootSource::Disk);
  require(session.step(64) == Session::Step::Running, "Guest stopped before its PS/2 poll");
  require(!session.key(0, 0xe04b, true) && !session.key(0, 0xe04b, false),
          "Raw PS/2 input reported a BIOS wakeup");
  run_until(session, Session::Step::WaitingKey, "Real-mode guest never received its PS/2 make and break");
  require(!session.key(0, 0x2a, true) && !session.key(0, 0x2a, false),
          "A modifier alone woke a waiting BIOS call");
  require(!session.key(13, 0x1c, false) && session.key(13, 0x1c, true) &&
              !session.key(13, 0x1c, false),
          "Only the BIOS make code may wake INT 16h");
  run_until(session, Session::Step::Halted, "Guest did not finish after receiving Enter through INT 16h");
  require(screen(capture_cells(session)).front() == 'K', "Keyboard input reached the wrong guest interface");
}

constexpr unsigned kLongModeTestOffset = 0x160;

auto long_mode_test_sector(const std::uint8_t* guest, std::size_t size) {
  auto boot = cosmic_pc::long_mode_boot_disk();
  const std::array<std::uint8_t, 6> signature{0x48,0xb8,0x01,0,0,0};
  const auto start = std::search(boot.begin(), boot.end(), signature.begin(), signature.end());
  require(start != boot.end(), "Long-mode boot sector entry changed");
  // Redirect the canonical boot sector after it enables long mode.
  constexpr unsigned destination = 0x7c00 + kLongModeTestOffset;
  const std::uint8_t handoff[] = {0xbb,std::uint8_t(destination),std::uint8_t(destination >> 8),0,0,0xff,0xe3};
  std::copy(std::begin(handoff), std::end(handoff), start);
  require(size <= 510-kLongModeTestOffset, "Guest must fit before the boot signature");
  std::copy_n(guest, size, boot.begin() + kLongModeTestOffset);
  return boot;
}

template <std::size_t N>
auto long_mode_test_sector(const std::uint8_t (&guest)[N]) {
  static_assert(kLongModeTestOffset + N <= 510, "Guest must fit before the boot signature");
  return long_mode_test_sector(guest,N);
}

void measure_engine_window(std::uint64_t rounds, bool paced, bool mixed=false) {
  using Clock = cosmic_pc::PcMachine::RunClock;
  using namespace std::chrono;
  TestDiskDirectory dir;
  const auto path = dir.path / "engine-window.rqvd";
  auto checksum = rounds % 2 ? rounds * (rounds / 2 + 1) : (rounds / 2) * (rounds + 1);
  if (mixed) {
    checksum = 0;
    for (auto counter=rounds; counter; --counter) {
      checksum = (checksum + counter) ^ 0x13579bdf;
      checksum = (checksum << 5) | (checksum >> 59);
    }
  }
  std::vector<std::uint8_t> guest{0xfa,0x48,0xb9}; // CLI; MOV RCX,rounds.
  const auto imm64 = [&](std::uint64_t v) {
    for (unsigned i=0; i<8; ++i) guest.push_back(std::uint8_t(v >> (8*i)));
  };
  imm64(rounds);
  guest.insert(guest.end(),{0x31,0xc0}); // XOR EAX,EAX.
  const auto loop = guest.size();
  guest.insert(guest.end(),{0x48,0x01,0xc8}); // ADD RAX,RCX.
  if (mixed) guest.insert(guest.end(),{0x48,0x35,0xdf,0x9b,0x57,0x13,0x48,0xc1,0xc0,5});
  guest.insert(guest.end(),{0x48,0xff,0xc9,0x75}); // DEC RCX; JNZ loop.
  guest.push_back(std::uint8_t(loop-guest.size()-1));
  guest.insert(guest.end(),{0x48,0xba}); // MOV RDX,checksum.
  imm64(checksum);
  guest.insert(guest.end(),{0x48,0x39,0xd0,0x74,2,0x0f,0x0b,0xfa,0xf4}); // CMP; JE; UD2; CLI; HLT.
  const auto boot = long_mode_test_sector(guest.data(),guest.size());
  Session::create_disk(path, 1);
  { reverse_windows::QssdDisk disk(path); require(disk.write(0,1,boot.data()), "Write engine fixture"); }
  Session session(path,0,Session::BootSource::Disk,-1,0,Session::kDefaultRamProfile,
                  {},{},{},Session::kPrimeCpuTopology,true,Session::kPrimeCpuClock);
  for (unsigned i = 0; ; ++i) {
    const auto d = session.diagnostic(false);
    if (counter(d.substr(d.find("\"machine\":")), "ip") == 0x7c00 + kLongModeTestOffset + loop) break;
    require(i < 4000 && session.step(1) == Session::Step::Running, "Cannot enter the measured x64 loop");
  }
  const auto before = session.session_steps();
  std::uint64_t active_ns = 0, full = 0, windows = 0;
#ifdef PRIME_PREVIOUS_ENGINE_WINDOW
  std::uint64_t request = 4096;
#else
  for (const auto invalid : {nanoseconds::zero(), duration_cast<nanoseconds>(milliseconds(9))}) {
    bool rejected = false;
    try { session.step_engine_window(invalid); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && session.session_steps() == before, "An invalid time window retired work");
  }
#endif
  const auto begin = Clock::now();
  auto status = Session::Step::Running;
  while (status == Session::Step::Running && Clock::now()-begin < seconds(25)) {
    const auto turn = Clock::now();
    const auto steps = session.session_steps();
#ifdef PRIME_PREVIOUS_ENGINE_WINDOW
    status = session.step(unsigned(request), turn + milliseconds(8));
#else
    status = session.step_engine_window(milliseconds(8));
#endif
    const auto elapsed = std::max<std::int64_t>(1, duration_cast<nanoseconds>(Clock::now()-turn).count());
    active_ns += elapsed;
    const auto done = session.session_steps() - steps;
    ++windows;
#ifdef PRIME_PREVIOUS_ENGINE_WINDOW
    if (done && status == Session::Step::Running)
      request = std::clamp<std::uint64_t>((request + done * 8'000'000 / elapsed) / 2, 64, INT32_MAX);
#else
    require(done == session.last_engine_retired() && done <= 36'000'000'000ULL, "Incorrect 64-bit grant");
    require(session.engine_delay() <= milliseconds(8) &&
                (done == 36'000'000'000ULL || session.engine_delay() == nanoseconds::zero()),
            "An incomplete guest window requested pacing");
    if (done == 36'000'000'000ULL) {
      ++full;
      if (paced && status == Session::Step::Running) std::this_thread::sleep_for(session.engine_delay());
    }
#endif
  }
  const auto wall_ns = duration_cast<nanoseconds>(Clock::now()-begin).count();
  const auto retired = session.session_steps()-before;
  require(status == Session::Step::Halted && session.is_stopped() && retired == (mixed?5:3)*rounds+5,
          "The guest checksum, halt or exact retired count failed");
#ifndef PRIME_PREVIOUS_ENGINE_WINDOW
  const auto report = session.diagnostic();
  if (!mixed) require(counter(report,"register_batch_steps") > 0 && counter(report,"phase_projected_steps") > 0,
          "The production session never used register projection");
  if (cosmic_pc::Cpu::ether_arm64_host()) require(counter(report,"phase_ether_arm64_steps") > 0, "Native engine missing");
  require(session.step_engine_window(milliseconds(8)) == Session::Step::Halted &&
              session.last_engine_retired() == 0, "A stopped machine earned execution credit");
#endif
  std::cout << "PRIME_ENGINE_WINDOW_SAMPLE kind=" << (mixed ? "mix" : "sum") << " paced=" << paced << " steps=" << retired
            << " checksum=" << checksum << " windows=" << windows << " full=" << full
            << " active_ns=" << active_ns << " wall_ns=" << wall_ns << '\n';
}

void verify_late_prime_pressure(const std::filesystem::path& path) {
  constexpr std::uint64_t mib = 1024 * 1024;
  constexpr auto threshold = 64 * mib;
  constexpr std::uint64_t page = cosmic_pc::MemoryFabric::kPageSize;
  const std::uint8_t guest[] = {
      0xbf,0x08,0x30,0,0, 0xb8,0x83,0,0x20,0, 0xb9,0x20,0,0,0, // PDE[1..32], 2 MiB identity pages.
      0x48,0xab,0x05,0,0,0x20,0,0xe2,0xf7,                     // STOSQ; advance physical base; LOOP.
      0xbf,0,0,0x20,0, 0xb8,0x78,0x56,0x34,0x12, 0xbb,0,0x40,0,0,
      0xb9,0x28,0,0,0,0xf3,0x48,0xab,                          // 40 words per page, too many to compact.
      0x48,0x81,0xc7,0xc0,0x0e,0,0,0xff,0xcb,0x75,0xed,0xfa,0xf4}; // Next of 16384 pages; CLI; HLT.
  const auto boot = long_mode_test_sector(guest);
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()), "Could not persist the Prime pressure guest");
  }
  const auto profile = Session::ram_profile(512 * mib, threshold, false, true);
  Session live(path, 0, Session::BootSource::Disk, -1, 0, profile, path.parent_path());
  auto result = Session::Step::Running;
  for (unsigned slice = 0; result == Session::Step::Running && slice < 20; ++slice)
    result = live.step(50000);
  if (result != Session::Step::Halted)
    throw std::runtime_error("Prime pressure guest did not populate RAM: " + live.diagnostic());
  require(!live.prepare_ram_reclaim(), "Preparation must wait for the first Android pressure sample");
  const auto before = live.diagnostic(false);
  const auto resident = counter(before, "resident_bytes");
  require(resident >= 64 * mib && counter(before, "spill_writes") == 0,
          "Prime pressure guest must start with a populated resident working set");

  live.sample_ram_pressure(threshold, threshold, true);
  require(!live.prepare_ram_reclaim(), "Android pressure must take precedence over anticipatory work");
  const auto skipped = preparation_report(live.diagnostic(false));
  require(counter(skipped, "checks") == 2 && counter(skipped, "batches") == 0 &&
              counter(skipped, "skipped_no_target") == 1 &&
              counter(skipped, "skipped_low_memory") == 1 &&
              skipped.find("\"last_reason\":\"android-low-memory\"") != std::string::npos &&
              counter(skipped, "total_us") == 0,
          "Preparation diagnostics must distinguish eligibility checks from performed batches");
  require(live.maintain_ram_budget(), "Foreground pressure must reclaim in bounded batches");
  auto partial = live.diagnostic(false);
  // A short grant can expire while searching through hot pages before its
  // first write, especially when a test host is running the ARM64 binary.
  for (unsigned batch = 0; batch < 32 && counter(partial, "spill_writes") == 0; ++batch) {
    require(live.maintain_ram_budget(), "Foreground RAM search must remain resumable");
    partial = live.diagnostic(false);
  }
  const auto target = counter(partial, "pressure_target_bytes");
  const auto spills = counter(partial, "spill_writes");
  require(target == (resident - 32 * mib) / page * page &&
              spills != 0 && spills <= 128 &&
              counter(partial, "pressure_maintenance_target_us") == 3000 &&
              (counter(partial, "pressure_spill_encode_ns") > 0 ||
               counter(partial, "spill_deferred_staged_pages") > 0) &&
              counter(partial, "spill_write_calls") > 0 &&
              counter(partial, "pressure_reclaimed_bytes") >= spills * page &&
              counter(partial, "pressure_released_resident_bytes") ==
                  resident + counter(before, "spill_cache_bytes") -
                  counter(partial, "resident_bytes") - counter(partial, "spill_cache_bytes") &&
              counter(partial, "pressure_last_released_resident_bytes") > 0 &&
              counter(partial, "pressure_last_released_resident_bytes") <=
                  counter(partial, "pressure_released_resident_bytes") &&
              counter(partial, "pressure_first_released_bytes") > 0 &&
              partial.find("\"pressure_first_release_batch_us\":null") == std::string::npos &&
              partial.find("\"pressure_target_applied_us\":null") != std::string::npos &&
              counter(partial, "raw_backing_capacity_bytes") <= Session::kPrimeRamBytes &&
              partial.find("\"pressure_background_trim\":false") != std::string::npos,
          "Foreground pressure must reserve host headroom without targeting the 16 MiB floor");

  // A fresh surplus supersedes an unfinished reclaim before another page is evicted.
  live.sample_ram_pressure(512 * mib, threshold, false);
  require(!live.maintain_ram_budget() && !live.maintain_ram_budget(),
          "A fresh healthy reading must cancel stale pressure maintenance");
  const auto recovered = live.diagnostic(false);
  require(counter(recovered, "spill_writes") == spills &&
              counter(recovered, "pressure_target_bytes") >= 380 * mib &&
              counter(recovered, "pressure_first_released_bytes") == 0 &&
              recovered.find("\"pressure_first_release_batch_us\":null") != std::string::npos &&
              recovered.find("\"pressure_target_applied_us\":null") == std::string::npos &&
              counter(recovered, "usable_bytes") == Session::kPrimeRamBytes,
          "Recovering headroom must keep remaining guest pages and the original E820 map");

  require(!live.prepare_ram_reclaim(), "Low occupancy must not trigger anticipatory backing writes");
  const auto below = preparation_report(live.diagnostic(false));
  require(counter(below, "skipped_below_threshold") == 1 && counter(below, "batches") == 0 &&
              counter(below, "resident_bytes_at_check") < counter(below, "threshold_bytes") &&
              below.find("\"last_reason\":\"below-threshold\"") != std::string::npos,
          "An idle preparation check must explain its threshold and preserve the resident pages");

  live.sample_ram_pressure(threshold + 34 * mib, threshold, false);
  require(!live.maintain_ram_budget(), "Stable RAM budget must allow preparation before pressure");
  for (unsigned pass = 0; pass < 32; ++pass) {
    live.prepare_ram_reclaim();
    if (counter(live.diagnostic(false), "compact_up_to_2k")) break;
  }
  const auto prepared = live.diagnostic(false);
  const auto preparation = preparation_report(prepared);
  require(counter(prepared, "compact_up_to_2k") > 0 &&
              counter(prepared, "resident_bytes") < counter(recovered, "resident_bytes") &&
              counter(prepared, "usable_bytes") == Session::kPrimeRamBytes,
          "The live preparation route must compact 40-word pages before pressure");
  require(counter(preparation, "batches") > 0 && counter(preparation, "checks") >
              counter(preparation, "batches") && counter(preparation, "goal_pages") > 0 &&
              counter(preparation, "deficit_pages") == counter(preparation, "goal_pages") -
                  std::min(counter(preparation, "goal_pages"), counter(preparation, "clean_pages_after")) &&
              counter(preparation, "resident_bytes_at_check") >= counter(preparation, "threshold_bytes") &&
              counter(preparation, "total_us") >= counter(preparation, "last_us") &&
              counter(preparation, "empty_batches") <= counter(preparation, "batches"),
          "Prepared copies, reserve deficit and preparation time must describe the actual batch");

  live.sample_ram_pressure(UINT64_MAX, 0, false);
  require(!live.maintain_ram_budget() &&
              counter(live.diagnostic(false), "backing_budget_bytes") == profile.physical_bytes,
          "Live surplus accounting must saturate before adding resident bytes");
  std::cout << "PRIME_FOREGROUND_RAM_PRESSURE_PASS target_bytes=" << target
            << " first_batch_spills=" << spills
            << " maintenance_us=" << counter(partial, "pressure_maintenance_us")
            << " stale_reclaim_stopped=1 logical_gib=64\n";
}

void verify_prepared_ram_backing(const std::filesystem::path& directory) {
  using Ram = cosmic_pc::SparseRam;
  using Clock = Ram::ReclaimClock;
  constexpr std::size_t count = 256, bytes = count * Ram::kPageSize;
  Ram ram(2 * bytes, 2 * bytes);
  auto backing = std::make_unique<reverse_windows::AndroidRamBacking>(directory, ram.size(), 2 * bytes, true);
  auto* file = backing.get();
  ram.attach_page_backing(std::move(backing));
  std::vector<std::uint8_t> data(bytes), recovered(bytes);
  for (std::uint64_t i = 0; i < data.size() / sizeof(i); ++i)
    std::memcpy(data.data() + i * sizeof(i), &i, sizeof(i));
  ram.write_block(0, data.data(), data.size());
  for (unsigned pass = 0; pass < 32 && ram.clean_backing_pages() < count; ++pass)
    ram.prepare_reclaim(count, Clock::time_point::max());
  const auto writes = file->write_calls();
  require(ram.clean_backing_pages() == count && ram.allocated_bytes() == bytes &&
              file->codec_stats().encode_attempts == 0 && writes > 0,
          "Prime must prepare a raw file-backed reserve while keeping exact resident pages");

  const auto start = Clock::now();
  const auto first = ram.adjust_payload_budget(0, 128, start + std::chrono::milliseconds(3), count);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  const auto first_released = bytes - ram.allocated_bytes();
  auto status = first;
  for (unsigned pass = 0; status == Ram::BudgetAdjustment::Reclaiming && pass < 16; ++pass)
    status = ram.adjust_payload_budget(0, 128, Clock::now() + std::chrono::milliseconds(3), count);
  ram.read_block(0, recovered.data(), recovered.size());
  require(status == Ram::BudgetAdjustment::Applied && ram.metrics().backing_clean_reclaims == count &&
              file->write_calls() == writes && recovered == data,
          "Reclaim must drop the prepared copies without I/O writes and preserve every byte");
  std::cout << "PRIME_RAM_PREPARED_RECLAIM_PASS ready_pages=" << count
            << " first_released_bytes=" << first_released << " first_reclaim_us=" << elapsed
            << " writes_at_reclaim=0 exact=1\n";
}

void verify_active_guest_clock(const std::filesystem::path& path) {
  const std::uint8_t guest[]{
      0xb9,0,0x10,0,0,          // MOV ECX,4096
      0x0f,0x31,0x90,0xe2,0xfb, // RDTSC; NOP; LOOP
      0xfa,0xf4};
  const auto boot = long_mode_test_sector(guest);
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()), "Could not write the clock fixture");
  }
  Session session(path, 0, Session::BootSource::Disk, -1, 0, Session::kDefaultRamProfile,
                  {}, {}, {}, Session::kPrimeCpuTopology, true, Session::kPrimeCpuClock);
  require(session.diagnostic(false).find("\"clock_source\":\"guest-cycles\"") != std::string::npos,
          "Firmware must start with the deterministic clock");
  for (unsigned slice = 0; slice < 1024 && !session.is_stopped(); ++slice) {
    const auto deadline = cosmic_pc::PcMachine::RunClock::now() + std::chrono::milliseconds(8);
    if (session.step(32, deadline) == Session::Step::Fault)
      throw std::runtime_error(session.diagnostic());
  }
  const auto result = session.diagnostic(false);
  require(session.is_stopped() &&
              result.find("\"clock_source\":\"host-active-monotonic\"") != std::string::npos,
          "The x64 guest must complete with the active monotonic clock");
  constexpr std::uint64_t second = 1'000'000'000, hz = cosmic_pc::PcMachine::kVirtualCyclesPerSecond;
  const auto elapsed = counter(result, "active_clock_elapsed_ns");
  const auto ticks = counter(result, "active_clock_ticks");
  require(counter(result, "guest_tsc_hz") == 4'500'000'000'000ULL,
          "The Android TSC must start at the experimental 4.5 THz rate");
  require(ticks > 0 && ticks == elapsed / second * hz + elapsed % second * hz / second,
          "Active clock ticks must follow the declared oscillator rate");
  std::cout << "PRIME_ANDROID_ACTIVE_CLOCK_PASS deterministic_firmware=1 long64=1 ticks=" << ticks << '\n';
}

void verify_arm64_session(const std::filesystem::path& path) {
  const std::uint8_t guest[]{
      0x0f,0x09,                              // WBINVD at CPL0, then continue.
      0x48,0xb8,0x83,0,0,0,0x10,0,0,0,      // 2 MiB page starting at physical 64 GiB.
      0x48,0x89,0x04,0x25,0x08,0x30,0,0,    // Install it as PDE[1].
      0xbf,0,0,0x20,0, 0xb9,8,0,0,0,         // Linear 2 MiB, eight links.
      0x48,0x8d,0x47,8, 0x48,0xab,0xe2,0xf8, // LEA next address; STOSQ; LOOP.
      0xbe,0,0,0x20,0,                       // ESI = first chain node.
      0x41,0xbe,0x40,0,0x20,0,               // R14D = end of chain.
      0x48,0x8b,0x36,0x49,0x3b,0xf6,0x75,0xf8,
      0xfa,0xf4};
  const auto boot = long_mode_test_sector(guest);
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()) && disk.flush(), "Could not persist long-mode test sector");
  }
  const auto prime = Session::ram_profile(768ULL << 20, 256ULL << 20, false, true);
  Session session(path, 0, Session::BootSource::Disk, -1, 0, prime, path.parent_path());
  for (unsigned steps = 0; steps < 6000 && !session.is_stopped(); steps += 256) {
    if (session.step(256) == Session::Step::Fault)
      throw std::runtime_error(session.diagnostic());
  }
  const auto info = session.diagnostic(false);
  require(session.is_stopped() && info.find("\"cpu_mode\":\"long64\"") != std::string::npos,
          "The Android native session did not execute the long-mode QSSD guest");
  require(counter(info, "wbinvd_retired") == 1, "The Android session did not continue after WBINVD");
  require(counter(info, "usable_bytes") == Session::kPrimeRamBytes &&
              counter(info, "page_translation_hits") > 0,
          "The Android guest must use cached translations into its 64 GiB profile");
  std::cout << "PRIME_C10_WBINVD_SESSION_PASS count=1 resume=1\n";
  const auto arm64 = info.find("\"ether_arm64\":{");
  require(arm64 != std::string::npos, "C03 counts are missing from the Android diagnostic");
  const auto ether = info.substr(arm64);
  const auto native_steps = counter(ether, "guest_steps");
  require(cosmic_pc::Cpu::ether_arm64_host() ? native_steps >= 12 : native_steps == 0,
          "Android native session selected the wrong launch path");
  require(counter(info, "phase_ether_arm64_steps") == native_steps &&
              counter(info, "phase_projected_steps") == 0 &&
              counter(info, "phase_x86_steps") == counter(info, "session_steps"),
          "Live Android metrics must report the whole phase and its native and projected subsets");
  require(counter(info, "translated_steps") == native_steps &&
              counter(info, "translated_steps") + counter(info, "batched_steps") +
                  counter(info, "interpreted_steps") == counter(info, "phase_x86_steps"),
          "Coverage must account for all guest steps in the phase without counting subsets twice");
  const auto loop_dispatches = counter(ether, "repeated_loop_dispatches");
  const auto loop_steps = counter(ether, "repeated_loop_steps");
  require(loop_steps <= native_steps && ((loop_dispatches == 0) == (loop_steps == 0)),
          "Android diagnostics must report repeated loops within native retired steps");
  require(counter(ether, "state_vector_steps") == 0 && counter(ether, "state_vector_dispatches") == 0,
          "The pointer loop must stay on the guarded RAM path");
  const auto version_hits = counter(ether, "code_version_hits");
  const auto byte_checks = counter(ether, "code_byte_checks");
  const auto code_bytes = counter(ether, "code_bytes_read");
  if (cosmic_pc::Cpu::ether_arm64_host()) {
    require(version_hits > 0 && byte_checks > 0 && code_bytes > 0,
            "Android native session did not reuse a validated code page");
  } else {
    require(version_hits == 0 && byte_checks == 0 && code_bytes == 0,
            "Non-ARM64 session unexpectedly used the native code guard");
  }
#if defined(REVERSE_MUNDO_X_SOURCE_SHA)
  require(ether.find("\"mundo_x_source_sha\":\"" REVERSE_MUNDO_X_SOURCE_SHA "\"") != std::string::npos,
          "Android native session source SHA does not match its CMake lock");
#endif
  std::cout << "PRIME_C03_NATIVE_SESSION_PASS translated_steps=" << native_steps << '\n';
}

void verify_idt_diagnostic(const std::filesystem::path& path) {
  const std::uint8_t guest[] = {0x0f,0x01,0x1c,0x25,0xd0,0x7d,0,0, 0xcc}; // LIDT [0x7dd0]; INT3.
  auto boot = long_mode_test_sector(guest);
  const std::uint8_t idtr[] = {0xff,0x0f,0x00,0x50,0x01,0,0,0,0,0}; // 256 entries at 0x15000.
  std::copy(std::begin(idtr), std::end(idtr), boot.begin() + 0x1d0);
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()) && disk.flush(), "Could not persist IDT test sector");
  }
  Session session(path, 0, Session::BootSource::Disk, -1, 0, Session::kDefaultRamProfile,
                  {}, {}, {}, Session::kPrimeCpuTopology);
  require(session.step(6000) == Session::Step::Fault,
          "INT3 failed to reach the terminal IDT diagnostic after entering long mode");
  const auto live = session.diagnostic(false);
  require(live.find("\"cpu_fault\":{\"apic_id\":0,") != std::string::npos &&
              live.find("opcode=cc") != std::string::npos &&
              live.find("\"idt_attempts\":[{\"source\":\"software\",\"vector\":3,") != std::string::npos &&
              live.find("\"entry_address_hex\":\"0000000000015030\",\"entry_size\":16,"
                        "\"bytes_read\":16,\"gate_bytes_hex\":\"00000000000000000000000000000000\"") !=
                  std::string::npos,
          "Immediate Android fault must include the CPU, vector and raw 64-bit gate");
  require(live.find("\"source\":\"exception\",\"vector\":13,") != std::string::npos &&
              live.find("\"source\":\"exception\",\"vector\":8,") != std::string::npos &&
              counter(live, "checkpoint_interval_steps") == 50'000'000,
          "Android fault must carry nested IDT failures and the profile interval");
  const auto exported = session.diagnostic(true);
  require(exported.find("\"interrupt_origin\":") != std::string::npos &&
              exported.find("\"last_lidt\":{") != std::string::npos &&
              exported.find("\"base_hex\":\"0000000000015000\"") != std::string::npos &&
              exported.find("\"code_hex\":\"CC") != std::string::npos &&
              exported.find("\"stack_hex\":\"") != std::string::npos,
          "Android export must preserve LIDT, faulting code and readable stack");
  std::cout << "PRIME_ANDROID_IDT_DIAGNOSTIC_PASS vector=3 gate_bytes=16 nested_vectors=13,8\n";
}

void write_evidence(const std::filesystem::path& path, const VgaCells& cells,
                    const std::string& before, const std::string& after) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  require(output.is_open(), "Cannot create boot evidence");
  output << "{\"schema\":\"reverse-windows-c03-synthetic-boot-v1\",\"columns\":80,\"rows\":25,"
            "\"vga_cells_hex\":\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : cells) output << hex[byte >> 4] << hex[byte & 15];
  output << "\",\"before_restart\":" << before << ",\"after_restart\":" << after << "}\n";
  output.close();
  require(bool(output), "Cannot write complete boot evidence");
}

std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), "Cannot read ISO fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void verify_riso_boot(const std::filesystem::path& original, const std::filesystem::path& archive) {
  const TestDiskDirectory dir;
  const auto archive_size = std::filesystem::file_size(archive);
  const auto original_size = std::filesystem::file_size(original);
  std::unique_ptr<Session> session;
  {
    const FileDescriptor fd(archive,O_RDONLY);
    const auto hardware = Session::check_hardware({},fd.value,archive_size);
    require(hardware.find("\"iso_boot\":true") != std::string::npos,
            "Android hardware check must accept the REISO01 boot catalog");
    session = std::make_unique<Session>(dir.path / "riso.rqvd",1,Session::BootSource::Iso,
                                        fd.value,archive_size);
  }
  require(session->step(40) != Session::Step::Fault, "Android failed to boot the REISO01 fixture");
  const auto report = session->diagnostic();
  require(report.find("\"iso_container\":\"REISO01\"") != std::string::npos &&
              counter(report,"iso_bytes") == original_size &&
              counter(report,"iso_file_bytes") == archive_size &&
              report.find("\"atapi_cdrom\":true") != std::string::npos &&
              report.find("\"regs_hex\":[\"0000000000001234\"") != std::string::npos,
          "Android must mount the expanded CD-ROM geometry and execute the original x86 boot code");
  std::cout << "REVERSE_WINDOWS_ANDROID_REISO_BOOT_PASS original_bytes=" << original_size
            << " archive_bytes=" << archive_size << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--measure-engine-window") {
      measure_engine_window(100'000'000ULL, false);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--sustain-engine-window") {
      measure_engine_window(1'500'000'000'000ULL, true);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--measure-engine-mix") {
      measure_engine_window(250'000ULL, false, true);
      return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--riso") {
      verify_riso_boot(argv[2],argv[3]);
      return 0;
    }
    verify_ram_profile();
    require(argc == 2 || argc == 3, "Pass the synthetic BIOS fixture and optional evidence path");
    const TestDiskDirectory dir;
#ifndef PRIME_PREVIOUS_ENGINE_WINDOW
    verify_ram_timing_option(dir.path);
#endif
    verify_ram_spill(dir.path);
    verify_packed_ram64(dir.path);
    verify_deferred_ram_reclaim(dir.path);
    verify_prepared_ram_backing(dir.path);
    verify_shared_ram(dir.path);
    verify_real_mode_ps2(dir.path / "keyboard-irq.rqvd", true);
    verify_real_mode_ps2(dir.path / "keyboard-poll.rqvd", false);
    verify_late_prime_pressure(dir.path / "ram-pressure.rqvd");
    verify_active_guest_clock(dir.path / "active-clock.rqvd");
    verify_arm64_session(dir.path / "ether-arm64.rqvd");
    verify_idt_diagnostic(dir.path / "idt-diagnostic.rqvd");
    const auto disk = dir.path / "android-test.rqvd";
    const auto data_disk = dir.path / "android-data.rqvd";
    const auto vram = dir.path / "android-test-vram.rqvd";
    const auto prime = Session::ram_profile(768ULL << 20, 256ULL << 20, false, true);
    const auto fixture = dir.path / "boot.iso";
    std::filesystem::copy_file(argv[1], fixture);
    const auto original = bytes(fixture);
    VgaCells frame{};
    std::string progress, restarted;
    const auto missing = Session::check_hardware({});
    for (const auto* marker : {"\"iso_boot\":null", "\"qssd\":null", "\"qssd_ata\":null", "\"cpu_x64\":true",
                              "\"cpu_mmx\":true", "\"cpu_sse\":true", "\"cpu_sse2\":true",
                              "\"cpu_x87\":true", "\"cpu_fxsr\":true", "\"ether_quantum_block\":true",
                              "\"lapic\":true", "\"ram\":true", "\"vga\":true", "\"pci_ide\":true",
                              "\"ready_for_windows10_install\":false"})
      require(missing.find(marker) != std::string::npos, marker);
    {
      bool rejected = false;
      try {
        FileDescriptor writable(fixture, O_RDWR);
        Session denied(disk, 1, Session::BootSource::Iso, writable.value, original.size());
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      require(rejected && !std::filesystem::exists(disk),
              "Writable ISO descriptor was accepted or mutated the disk");
    }
    {
      const auto invalid = dir.path / "invalid.iso";
      std::ofstream(invalid) << "No El Torito image";
      bool rejected = false;
      try {
        FileDescriptor iso(invalid, O_RDONLY);
        Session denied(disk, 64, Session::BootSource::Iso, iso.value);
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      require(rejected && !std::filesystem::exists(disk), "Invalid ISO created a disk before validation");
    }
    {
      using reverse_windows::QssdDisk;
      Session::create_disk(data_disk, QssdDisk::kMaxLogicalBytes / QssdDisk::kGiB);
      auto session = [&] {
        FileDescriptor iso(fixture, O_RDONLY);
        return Session(disk, 64, Session::BootSource::Iso, iso.value, original.size(),
                       prime, dir.path, vram, data_disk, Session::kPrimeCpuTopology, true, Session::kPrimeCpuClock);
      }();
      run_until(session, Session::Step::WaitingKey,
                "El Torito guest did not detect QSSD and return to the real-mode BIOS prompt", 1);
      frame = capture_cells(session);
      progress = session.diagnostic();
      require(progress.find("\"clock_source\":\"host-active-monotonic\"") != std::string::npos &&
                  counter(progress, "active_clock_ticks") > 0 && counter(progress, "guest_tsc_hz") == 4'500'000'000'000ULL,
              "A protected32 ISO must activate the Prime clock without entering long mode");
      require(progress.find("\"cpuid_vendor\":\"GenuineIntel\"") != std::string::npos,
              "ISO startup did not select the Windows x64 processor profile");
      require(progress.find("\"cores\":8,\"threads_per_core\":2,\"logical_processors\":16,\"started\":1") !=
                  std::string::npos, "Prime Android ISO boot uses the requested CPU topology");
      require(counter(progress, "host_workers_capacity") == 8,
              "Prime Android must retain a separate eight-worker host limit");
      const auto live = session.diagnostic(false);
      for (const char* field : {"\"ata\":{", "\"disk\":{", "\"data_disk\":{"}) {
        const auto begin = live.find(field);
        require(begin != std::string::npos, "Missing storage device in live telemetry");
        const auto entry = live.substr(begin, live.find('}', begin) - begin);
        require(counter(entry, "clock_hz") == 4'500'000'000'000ULL && counter(entry, "clock_cycles") > 0,
                "ATA and both QSSDs must expose their active clocks");
      }
      require(live.size() < progress.size() && live.find("\"boot_profile\":") == std::string::npos &&
                  live.find("\"status\":\"waiting_key\"") != std::string::npos &&
                  counter(live, "session_steps") == counter(progress, "session_steps"),
              "Live status must preserve guest progress without serializing detailed profiles");
      const auto atapi = live.find("\"atapi\":{");
      require(atapi != std::string::npos &&
                  counter(live.substr(atapi), "read_bytes") ==
                      counter(live.substr(atapi), "read_sectors") * reverse_windows::AtapiCdrom::sector_bytes,
              "Live CD-ROM byte metrics must account for full ATAPI sectors");
      require(counter(progress, "session_steps") == counter(progress, "instructions") &&
                  counter(progress, "session_steps") > 0,
              "Before the first restart, session and CPU steps must match");
      require(progress.find("\"boot_profile\":{") != std::string::npos &&
                  progress.find("\"boot_time_samples\":{") != std::string::npos &&
                  progress.find("\"boot_code_regions\":{") != std::string::npos &&
                  progress.find("\"instruction_page_reuses\":") != std::string::npos &&
                  progress.find("\"sampled_profile_ns\":") != std::string::npos &&
                  progress.find("\"sampled_arm64_execute_ns\":") != std::string::npos &&
                  progress.find("\"sampled_bulk_execute_ns\":") != std::string::npos &&
                  counter(progress, "sampled_windows") > 0 &&
                  counter(progress, "initial_commits") == 0,
              "ISO profiling must report activity and the initial disk state");
      require(progress.find("\"cpu_mode\":\"real16\"") != std::string::npos,
              "Guest BIOS prompt was reached outside real mode");
      require(progress.find("\"last_mode_from\":\"protected16\",\"last_mode_to\":\"real16\"") != std::string::npos,
              "Guest mode transition was missing from the boot diagnostic");
      for (const auto* marker : {"\"source\":\"iso\"", "\"status\":\"waiting_key\"",
                                 "\"handed_off\":true", "\"logical_bytes\":68719476736",
                                 "\"commits\":0", "\"identify_data_transfers\":1",
                                 "\"read_sectors\":0", "\"write_sectors\":0",
                                 "\"irq14\":1", "\"atapi_cdrom\":true",
                                 "\"atapi\":{\"packets\":0,\"read_sectors\":0,\"rejected_packets\":0",
                                 "\"last_rejected_packet\":0", "\"halted_with_if\":false",
                                 "\"lapic_global_enabled\":true", "\"lapic_software_enabled\":false",
                                 "\"lapic_pending_vector\":0", "\"lapic_timer_count\":0",
                                 "\"lapic_in_service_delivery_id\":0", "\"lapic_in_service_since_tick\":0",
                                 "\"irq8_acknowledgements\":0", "\"irq8_delivered\":0",
                                 "\"irq8_redirection_low\":65536", "\"lapic_vectors\":[]",
                                 "\"secondary_rep_elements\":0", "\"secondary_rep_bytes\":0",
                                 "\"secondary_scan_loop_steps\":0", "\"secondary_shift_loop_steps\":0",
                                 "\"secondary_zero_loop_steps\":0", "\"secondary_wide_multiply_loop_steps\":0",
                                 "\"cooperative_loop_blocks\":0", "\"worker_loop_steps\":0"})
        require(progress.find(marker) != std::string::npos, marker);
      const auto text = screen(frame);
      require(progress.find("\"data_disk\":{\"logical_bytes\":9007199254740992,\"physical_bytes\":64,") !=
                  std::string::npos, "Session did not report the attached sparse 8 PiB data disk");
      require(counter(progress, "usable_bytes") == Session::kPrimeRamBytes &&
                  counter(progress, "logical_vram_bytes") == 120000000000ULL &&
                  counter(progress, "resident_bytes") <= prime.backing_bytes &&
                  progress.find("\"spill_backend\":\"qssd-transient\"") != std::string::npos,
              "Prime ISO must boot with 64 GiB, bounded RAM backing and the PrimeVGC attached");
      const auto direct_copy_bytes = counter(progress, "boot_rep_direct_page_copy_bytes");
      require(direct_copy_bytes % cosmic_pc::MemoryFabric::kPageSize == 0 &&
                  direct_copy_bytes <= counter(progress, "boot_rep_bytes"),
              "The Android diagnostic must report a bounded count of direct page copies");
      require(text.compare(0, 14, "EL TORITO BIOS") == 0 &&
                  text.compare(80, 20, "QSSD 64 GiB DETECTED") == 0 &&
                  text.compare(3 * 80, 32, "ENTER: WRITE TEST MBR AND REBOOT") == 0,
              "Guest-generated VGA boot, discovery and confirmation lines missing");
      for (std::size_t column = 0; column < 20; ++column)
        require(frame[2 * (80 + column) + 1] == 0x0a, "Guest BIOS green text attribute missing");
      session.key(0x0d, 0x1c, false);
      require(session.step(10) == Session::Step::WaitingKey, "Key release advanced BIOS prompt");
      session.key(0x0d, 0x1c, true);
      run_until(session, Session::Step::Halted, "Guest did not reboot from the QSSD it had written");
      require(screen(capture_cells(session)).find("QSSD REBOOT OK") != std::string::npos,
              "Guest-requested restart did not execute QSSD boot code");
      restarted = session.diagnostic();
      require(restarted.find("\"clock_source\":\"guest-cycles\"") != std::string::npos &&
                  counter(restarted, "active_clock_elapsed_ns") == 0,
              "A reboot to a real-mode disk must resume deterministic firmware timing");
      require(counter(restarted, "session_steps") >=
                  counter(progress, "session_steps") + counter(restarted, "instructions"),
              "Guest reboot must preserve the accumulated session steps");
      require(counter(restarted, "iso_read_us") >= counter(progress, "iso_read_us"),
              "Optical read timing must cover the session across the guest reboot");
      const auto previous = restarted.find("\"previous_boot_profile\":{\"source\":\"iso\",\"phase\":0,");
      require(previous != std::string::npos &&
                  counter(restarted, "boot_profile_phase") == 1 &&
                  counter(restarted, "boot_profile_session_start_steps") ==
                      counter(restarted, "session_steps") - counter(restarted, "instructions") &&
                  counter(restarted.substr(previous), "sampled_windows") > 0,
              "Reboot must retain the ISO timing and start a new QSSD profile");
      for (const auto* marker : {"\"commits\":1", "\"source\":\"qssd\"", "\"restarts\":1",
                                 "\"last_restart\":\"port92\"", "\"atapi_cdrom\":true",
                                 "\"read_sectors\":1", "\"write_sectors\":0"})
        require(restarted.find(marker) != std::string::npos, marker);
      session.flush();
    }
    require(bytes(fixture) == original, "Guest changed the read-only ISO");
    {
      reverse_windows::QssdDisk reopened(disk, reverse_windows::QssdDisk::Access::ReadOnly);
      std::array<std::uint8_t, 512> persisted{};
      constexpr std::size_t cd_payload = 29 * 2048;
      require(original.size() >= cd_payload + persisted.size() && reopened.read(0, 1, persisted.data()) &&
                  std::memcmp(persisted.data(), original.data() + cd_payload, persisted.size()) == 0,
              "Reopened QSSD did not preserve the exact CD-sourced sector");
    }
    {
      std::ofstream append(disk, std::ios::binary | std::ios::app);
      append.put('\0');
    }
    const auto with_tail = bytes(disk);
    {
      FileDescriptor iso(fixture, O_RDONLY);
      const auto readiness = Session::check_hardware(disk, iso.value, original.size());
      require(readiness.find("\"iso_boot\":true,\"qssd\":true") != std::string::npos &&
                  readiness.find("\"qssd_ata\":true") != std::string::npos,
              "Hardware check did not read the committed media");
    }
    require(bytes(disk)==with_tail, "Preflight modified the QSSD or recovered its tail in place");
    {
      Session reboot(disk, 0, Session::BootSource::Disk, -1, 0, prime, dir.path, vram, data_disk,
                     Session::kPrimeCpuTopology);
      run_until(reboot, Session::Step::Halted, "QSSD guest did not complete its boot");
      require(screen(capture_cells(reboot)).find("QSSD REBOOT OK") != std::string::npos,
              "QSSD did not boot the MBR installed by the ISO guest");
      require(reboot.diagnostic().find("\"source\":\"qssd\"") != std::string::npos,
              "Reboot diagnostic did not identify QSSD");
      const auto disk_boot = reboot.diagnostic();
      require(counter(disk_boot, "logical_processors") == 16,
              "Direct QSSD boot keeps all 16 logical processors available");
      require(disk_boot.find("\"data_disk\":{\"logical_bytes\":9007199254740992,\"physical_bytes\":64,") !=
                  std::string::npos, "Direct boot did not reopen the data disk");
      require(counter(disk_boot, "usable_bytes") == Session::kPrimeRamBytes &&
                  counter(disk_boot, "logical_vram_bytes") == 120000000000ULL,
              "Direct QSSD boot must keep the RAM and graphics profile used by ISO boot");
      std::cout << "PRIME_UNIFIED_SESSION_PASS ram=" << counter(disk_boot, "usable_bytes")
                << " vram=" << counter(disk_boot, "logical_vram_bytes")
                << " ram_metadata=" << counter(progress, "resident_metadata_bytes")
                << " ram_resident=" << counter(progress, "resident_bytes") << '\n';
      require(counter(disk_boot, "initial_commits") == 1,
              "Disk-only session must report the already committed sector as initial state");
      require(counter(disk_boot, "boot_profile_phase") == 0 &&
                  counter(disk_boot, "boot_profile_session_start_steps") == 0 &&
                  counter(disk_boot, "sampled_windows") > 0 &&
                  disk_boot.find("\"previous_boot_profile\"") == std::string::npos,
              "A direct QSSD boot must be timed independently of ISO sessions");
    }
    {
      const auto wrong_disk = dir.path / "unexpected-capacity.rqvd";
      FileDescriptor iso(fixture, O_RDONLY);
      {
        Session wrong(wrong_disk, 1, Session::BootSource::Iso, iso.value, original.size());
        run_until(wrong, Session::Step::Halted, "Guest accepted a QSSD with a different capacity");
        const auto stopped = wrong.diagnostic();
        require(screen(capture_cells(wrong)).find("QSSD 64 GiB DETECTED") == std::string::npos &&
                    stopped.find("\"commits\":0") != std::string::npos &&
                    stopped.find("\"write_sectors\":0") != std::string::npos &&
                    stopped.find("\"identify_data_transfers\":0") != std::string::npos,
                "Failed EDD discovery claimed success or touched the wrong QSSD");
      }
      const auto readiness = Session::check_hardware(wrong_disk, iso.value, original.size());
      if (readiness.find("\"qssd_ata\":true") == std::string::npos)
        throw std::runtime_error("Preflight rejected the 1 GiB QSSD at its actual capacity: " + readiness);
    }
    if (argc == 3) write_evidence(argv[2], frame, progress, restarted);
    std::cout << "REVERSE_WINDOWS_ANDROID_BIOS_ISO_QSSD_PASS "
                 "synthetic_iso=true protected32_ia32_tsc=true protected32_lock_or=true "
                 "protected16_to_real16=true bios_edd=true pci_ide=true ata_identify_words=256 irq14=1 "
                 "qssd_logical_bytes=68719476736 bios_key=true bios_key_store_0500=true "
                 "real_mode_ps2_irq_enabled=true real_mode_ps2_explicit_polling=true "
                 "guest_vga=true read_only_preflight=true "
                 "bios_write_ata_readback_bytes=512 mismatched_capacity_rejected=true "
                 "guest_restart=true persistent_reboot=true\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "REVERSE_WINDOWS_ANDROID_BIOS_ISO_QSSD_FAIL " << error.what() << '\n';
    return 1;
  }
}
