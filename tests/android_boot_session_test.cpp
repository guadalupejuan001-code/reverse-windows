#include "android_boot_session.hpp"
#include "long_mode_guest.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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
  try {
    (void)Session::ram_profile(300 * mib, 256 * mib, true);
  } catch (const std::runtime_error&) { return; }
  throw std::runtime_error("Starting a VM with Android memory pressure must be rejected");
}

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
    constexpr std::uint64_t address = 0x100000;
    constexpr std::size_t resident = 8 * page;
    cosmic_pc::PcMachine pc(2 * mib, resident);
    auto file = std::make_unique<Backing>(cache, pc.cpu.memory.size(), 32 * page);
    auto* file_stats = file.get();
    pc.cpu.memory.attach_page_backing(std::move(file));
    // Non-repeating writes to 20 distinct guest pages cross the real limit.
    for (unsigned index = 0; index < 20; ++index) {
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
    pc.cpu.memory.reset_zero();
    require(pc.cpu.memory.metrics().spilled_pages == 0 && file_stats->file_payload_bytes() == 0 &&
                pc.cpu.read32(address) == 0,
            "Guest RAM reset must discard the old file-backed image");
  }
  std::cout << "REVERSE_WINDOWS_ANDROID_RAM_SPILL_PASS anonymous_file=true resident_cap=true"
               " limit_failure_preserves_data=true reset_releases_slots=true\n";
  require(std::filesystem::is_empty(cache), "Temporary RAM must disappear on session close");
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

void verify_arm64_session(const std::filesystem::path& path) {
  auto boot = cosmic_pc::long_mode_boot_disk();
  const std::array<std::uint8_t, 6> signature{0x48,0xb8,0x01,0,0,0};
  const auto start = std::search(boot.begin(), boot.end(), signature.begin(), signature.end());
  require(start != boot.end(), "Long-mode boot sector entry changed");
  // Redirect the canonical boot sector after it enables long mode.
  constexpr unsigned loop_offset = 0x160, destination = 0x7c00 + loop_offset;
  const std::uint8_t handoff[] = {0xbb,std::uint8_t(destination),std::uint8_t(destination >> 8),0,0,0xff,0xe3};
  std::copy(std::begin(handoff), std::end(handoff), start);
  const std::uint8_t guest[]{
      0xbe,0xa0,0x7d,0,0,                    // ESI = first chain node.
      0x41,0xbe,0xe0,0x7d,0,0,               // R14D = end of chain.
      0x48,0x8b,0x36,0x49,0x3b,0xf6,0x75,0xf8,
      0xfa,0xf4};
  std::copy(std::begin(guest), std::end(guest), boot.begin() + loop_offset);
  for (unsigned offset = 0x1a0; offset < 0x1e0; offset += 8) {
    boot[offset] = std::uint8_t(0x7c00 + offset + 8);
    boot[offset + 1] = std::uint8_t((0x7c00 + offset + 8) >> 8);
  }
  Session::create_disk(path, 1);
  {
    reverse_windows::QssdDisk disk(path);
    require(disk.write(0, 1, boot.data()) && disk.flush(), "Could not persist long-mode test sector");
  }
  Session session(path, 0, Session::BootSource::Disk);
  for (unsigned steps = 0; steps < 6000 && !session.is_stopped(); steps += 256) {
    if (session.step(256) == Session::Step::Fault)
      throw std::runtime_error(session.diagnostic());
  }
  const auto info = session.diagnostic();
  require(session.is_stopped() && info.find("\"cpu_mode\":\"long64\"") != std::string::npos,
          "The Android native session did not execute the long-mode QSSD guest");
  const auto arm64 = info.find("\"ether_arm64\":{");
  require(arm64 != std::string::npos, "C03 counts are missing from the Android diagnostic");
  const auto native_steps = counter(info.substr(arm64), "guest_steps");
  require(cosmic_pc::Cpu::ether_arm64_host() ? native_steps >= 12 : native_steps == 0,
          "Android native session selected the wrong launch path");
  std::cout << "PRIME_C03_NATIVE_SESSION_PASS translated_steps=" << native_steps << '\n';
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
}  // namespace

int main(int argc, char** argv) {
  try {
    verify_ram_profile();
    require(argc == 2 || argc == 3, "Pass the synthetic BIOS fixture and optional evidence path");
    const TestDiskDirectory dir;
    verify_ram_spill(dir.path);
    verify_real_mode_ps2(dir.path / "keyboard-irq.rqvd", true);
    verify_real_mode_ps2(dir.path / "keyboard-poll.rqvd", false);
    verify_arm64_session(dir.path / "ether-arm64.rqvd");
    const auto disk = dir.path / "android-test.rqvd";
    const auto fixture = dir.path / "boot.iso";
    std::filesystem::copy_file(argv[1], fixture);
    const auto original = bytes(fixture);
    VgaCells frame{};
    std::string progress, restarted;
    const auto missing = Session::check_hardware({});
    for (const auto* marker : {"\"iso_boot\":null", "\"qssd\":null", "\"qssd_ata\":null", "\"cpu_x64\":true",
                              "\"cpu_mmx\":true", "\"cpu_sse\":true", "\"cpu_sse2\":true",
                              "\"cpu_x87\":false", "\"cpu_fxsr\":true",
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
      auto session = [&] {
        FileDescriptor iso(fixture, O_RDONLY);
        return Session(disk, 64, Session::BootSource::Iso, iso.value, original.size());
      }();
      run_until(session, Session::Step::WaitingKey,
                "El Torito guest did not detect QSSD and return to the real-mode BIOS prompt", 1);
      frame = capture_cells(session);
      progress = session.diagnostic();
      require(counter(progress, "session_steps") == counter(progress, "instructions") &&
                  counter(progress, "session_steps") > 0,
              "Before the first restart, session and CPU steps must match");
      require(progress.find("\"boot_profile\":{") != std::string::npos &&
                  progress.find("\"boot_time_samples\":{") != std::string::npos &&
                  progress.find("\"boot_code_regions\":{") != std::string::npos &&
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
                                 "\"lapic_pending_vector\":0", "\"lapic_timer_count\":0"})
        require(progress.find(marker) != std::string::npos, marker);
      const auto text = screen(frame);
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
      Session reboot(disk, 0, Session::BootSource::Disk);
      run_until(reboot, Session::Step::Halted, "QSSD guest did not complete its boot");
      require(screen(capture_cells(reboot)).find("QSSD REBOOT OK") != std::string::npos,
              "QSSD did not boot the MBR installed by the ISO guest");
      require(reboot.diagnostic().find("\"source\":\"qssd\"") != std::string::npos,
              "Reboot diagnostic did not identify QSSD");
      const auto disk_boot = reboot.diagnostic();
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
