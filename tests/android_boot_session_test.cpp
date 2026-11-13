#include "android_boot_session.hpp"
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
    require(argc == 2 || argc == 3, "Pass the synthetic BIOS fixture and optional evidence path");
    const TestDiskDirectory dir;
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
                              "\"ram\":true", "\"vga\":true", "\"pci_ide\":true",
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
      require(progress.find("\"cpu_mode\":\"real16\"") != std::string::npos,
              "Guest BIOS prompt was reached outside real mode");
      require(progress.find("\"last_mode_from\":\"protected16\",\"last_mode_to\":\"real16\"") != std::string::npos,
              "Guest mode transition was missing from the boot diagnostic");
      for (const auto* marker : {"\"source\":\"iso\"", "\"status\":\"waiting_key\"",
                                 "\"handed_off\":true", "\"logical_bytes\":68719476736",
                                 "\"commits\":0", "\"identify_data_transfers\":1",
                                 "\"read_sectors\":0", "\"write_sectors\":0",
                                 "\"irq14\":1", "\"atapi_cdrom\":true"})
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
                 "qssd_logical_bytes=68719476736 bios_key=true guest_vga=true read_only_preflight=true "
                 "bios_write_ata_readback_bytes=512 mismatched_capacity_rejected=true "
                 "guest_restart=true persistent_reboot=true\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "REVERSE_WINDOWS_ANDROID_BIOS_ISO_QSSD_FAIL " << error.what() << '\n';
    return 1;
  }
}
