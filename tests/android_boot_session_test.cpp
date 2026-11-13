#include "android_boot_session.hpp"
#include <array>
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

void run_until(Session& session, Session::Step expected, const char* message) {
  for (unsigned chunk = 0; chunk < 10; ++chunk) {
    const auto status = session.step(64);
    if (status == expected) return;
    if (status != Session::Step::Running) break;
  }
  throw std::runtime_error(std::string(message) + ": " + session.diagnostic());
}

std::string screen(Session& session) {
  std::array<std::uint8_t, 4000> cells{};
  require(!session.video_info().graphics, "Expected guest BIOS text output");
  require(session.copy_text(cells.data(), cells.size()) == cells.size(),
          "Guest VGA text update missing");
  std::string output;
  output.reserve(cells.size() / 2);
  for (std::size_t i = 0; i < cells.size(); i += 2) output.push_back(char(cells[i]));
  return output;
}

std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), "Cannot read ISO fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "Pass the synthetic BIOS installation fixture");
    const TestDiskDirectory dir;
    const auto disk = dir.path / "android-test.rqvd";
    const auto fixture = dir.path / "boot.iso";
    std::filesystem::copy_file(argv[1], fixture);
    const auto original = bytes(fixture);
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
        return Session(disk, 1, Session::BootSource::Iso, iso.value, original.size());
      }();
      run_until(session, Session::Step::WaitingKey,
                "El Torito guest did not check IA32_TSC and return to the real-mode BIOS prompt");
      require(session.diagnostic().find("\"cpu_mode\":\"real16\"") != std::string::npos,
              "Guest BIOS prompt was reached outside real mode");
      require(screen(session).find("ISO -> QSSD") != std::string::npos,
              "Guest-generated VGA prompt missing");
      session.key(0x0d, 0x1c, false);
      require(session.step(10) == Session::Step::WaitingKey, "Key release advanced BIOS prompt");
      session.key(0x0d, 0x1c, true);
      run_until(session, Session::Step::Halted, "Guest did not complete the BIOS write");
      require(screen(session).find("MBR SAVED") != std::string::npos,
              "Guest did not confirm the QSSD MBR write");
      require(session.diagnostic().find("\"commits\":1") != std::string::npos,
              "QSSD diagnostic did not record the write");
      session.flush();
    }
    require(bytes(fixture) == original, "Guest changed the read-only ISO");
    {
      Session reboot(disk, 0, Session::BootSource::Disk);
      run_until(reboot, Session::Step::Halted, "QSSD guest did not complete its boot");
      require(screen(reboot).find("QSSD REBOOT OK") != std::string::npos,
              "QSSD did not boot the MBR installed by the ISO guest");
      require(reboot.diagnostic().find("\"source\":\"qssd\"") != std::string::npos,
              "Reboot diagnostic did not identify QSSD");
    }
    std::cout << "REVERSE_WINDOWS_ANDROID_BIOS_ISO_QSSD_PASS "
                 "synthetic_iso=true protected32_ia32_tsc=true protected16_to_real16=true "
                 "bios_key=true guest_vga=true persistent_reboot=true\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "REVERSE_WINDOWS_ANDROID_BIOS_ISO_QSSD_FAIL " << error.what() << '\n';
    return 1;
  }
}
