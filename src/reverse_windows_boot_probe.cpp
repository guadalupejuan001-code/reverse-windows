#include "pc_machine.hpp"
#include "bios_hard_disk.hpp"
#include "qssd_disk.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
struct Options {
    enum class Source { None, DemoX86, DemoX64, Iso, Disk } source = Source::None;
    std::string iso_path;
    std::string disk_path;
    std::string qssd_path;
    std::uint64_t create_qssd_gib = 0;
    std::uint64_t create_disk_gib = 0;
    std::string evidence_path;
    std::uint64_t steps = 200000;
    std::uint64_t ram_mib = 2048;
    std::uint64_t resident_mib = 256;
};

std::uint64_t positive(const std::string& raw, std::uint64_t upper, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(raw, &consumed);
    if (raw.empty() || consumed != raw.size() || value == 0 || value > upper) {
        throw std::invalid_argument(std::string(name) + " out of supported bounds");
    }
    return value;
}

Options parse(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("Missing argument after " + arg);
            return argv[++i];
        };
        if (arg == "--demo-x86" || arg == "--demo-x64" || arg == "--iso" || arg == "--boot-hdd") {
            if (out.source != Options::Source::None) throw std::invalid_argument("One boot source only");
            if (arg == "--demo-x86") out.source = Options::Source::DemoX86;
            else if (arg == "--demo-x64") out.source = Options::Source::DemoX64;
            else if (arg == "--boot-hdd") out.source = Options::Source::Disk;
            else { out.source = Options::Source::Iso; out.iso_path = next(); }
        } else if (arg == "--disk") out.disk_path = next();
        else if (arg == "--qssd-disk") out.qssd_path = next();
        else if (arg == "--create-qssd-gib") out.create_qssd_gib = positive(next(), 256, "create-qssd-gib");
        else if (arg == "--create-disk-gib") out.create_disk_gib = positive(next(), 256, "create-disk-gib");
        else if (arg == "--steps") out.steps = positive(next(), 100000000, "steps");
        else if (arg == "--ram-mib") out.ram_mib = positive(next(), 2048, "ram-mib");
        else if (arg == "--resident-mib") out.resident_mib = positive(next(), 2048, "resident-mib");
        else if (arg == "--evidence") out.evidence_path = next();
        else throw std::invalid_argument("Unknown option: " + arg);
    }
    if (out.source == Options::Source::None) throw std::invalid_argument("Select --demo-x86, --demo-x64 or --iso FILE");
    if (out.ram_mib < 2) throw std::invalid_argument("Mundo X PC requires at least 2 MiB logical RAM");
    if (out.create_disk_gib && out.disk_path.empty()) throw std::invalid_argument("--create-disk-gib requires --disk PATH.rvdisk");
    if (out.create_qssd_gib && out.qssd_path.empty()) throw std::invalid_argument("--create-qssd-gib requires --qssd-disk PATH.rqvd");
    if (!out.disk_path.empty() && !out.qssd_path.empty()) throw std::invalid_argument("Select one writable disk backend");
    if (out.source == Options::Source::Disk && out.disk_path.empty() && out.qssd_path.empty()) throw std::invalid_argument("--boot-hdd requires --disk or --qssd-disk");
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parse(argc, argv);
        cosmic_pc::PcMachine pc(
            static_cast<std::size_t>(opts.ram_mib * 1024ULL * 1024ULL),
            static_cast<std::size_t>(opts.resident_mib * 1024ULL * 1024ULL));
        std::unique_ptr<reverse_windows::BlockDisk> disk;
        reverse_windows::QssdDisk* qdisk = nullptr;
        if (!opts.disk_path.empty()) {
            if (opts.create_disk_gib) reverse_windows::DiskImage::create(opts.disk_path, opts.create_disk_gib);
            disk = std::make_unique<reverse_windows::DiskImage>(opts.disk_path);
        } else if (!opts.qssd_path.empty()) {
            if (opts.create_qssd_gib) reverse_windows::QssdDisk::create(opts.qssd_path, opts.create_qssd_gib);
            auto candidate = std::make_unique<reverse_windows::QssdDisk>(opts.qssd_path);
            qdisk = candidate.get();
            disk = std::move(candidate);
        }
        std::shared_ptr<std::ifstream> medium;
        std::string kind;
        if (opts.source == Options::Source::DemoX86) {
            pc.boot_demo();
            kind = "synthetic-x86";
        } else if (opts.source == Options::Source::DemoX64) {
            pc.boot_long_mode_demo();
            kind = "synthetic-x64";
        } else if (opts.source == Options::Source::Disk) {
            kind = "persistent-HDD-raw-boot";
            pc.mount([&disk](std::uint64_t offset, std::uint8_t* target, std::size_t count) {
                return disk->read_bytes(offset, target, count);
            }, disk->bytes(), "raw");
        } else {
            kind = "user-ISO";
            medium = std::make_shared<std::ifstream>(opts.iso_path, std::ios::binary);
            if (!*medium) throw std::runtime_error("ISO is not readable");
            medium->seekg(0, std::ios::end);
            const auto end = medium->tellg();
            if (end <= 0) throw std::runtime_error("Invalid ISO size");
            pc.mount(
                [medium](std::uint64_t offset, std::uint8_t* data, std::size_t size) {
                    medium->clear();
                    medium->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                    medium->read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
                    return static_cast<std::size_t>(medium->gcount()) == size;
                }, static_cast<std::uint64_t>(end), "iso");
        }

        std::unique_ptr<reverse_windows::BiosHardDisk> hdd;
        if (disk) hdd = std::make_unique<reverse_windows::BiosHardDisk>(pc, *disk);

        while (pc.cpu.instructions < opts.steps && pc.status() != "fault" && !pc.cpu.halted) {
            if (pc.waiting_key) {
                if (opts.source != Options::Source::DemoX86) break;
                pc.key('X', 0x2d);
            }
            const auto before = pc.cpu.instructions;
            const auto chunk = static_cast<unsigned>(
                std::min<std::uint64_t>(5000, opts.steps - before));
            pc.run(chunk);
            if (pc.cpu.instructions == before && !pc.waiting_key) break;
        }
        const auto final_json = pc.json();
        const std::string evidence = std::string("{\"schema\":\"reverse-windows-boot-evidence-v1\",") +
            "\"guest_kind\":" + cosmic_pc::PcMachine::quote(kind) +
            ",\"source\":\"original-Mundo-X-PcMachine\"" +
            ",\"windows10_boot_verified\":false" +
            ",\"writable_hdd_attached\":" + std::string(disk ? "true" : "false") +
            ",\"storage_backend\":" + cosmic_pc::PcMachine::quote(qdisk ? "QSSD-journaled" : disk ? "raw" : "none") +
            ",\"qssd_commits\":" + std::to_string(qdisk ? qdisk->stats().commits : 0) +
            ",\"qssd_physical_bytes\":" + std::to_string(qdisk ? qdisk->stats().physical_bytes : 0) +
            ",\"hdd_sectors\":" + std::to_string(disk ? disk->sectors() : 0) +
            ",\"machine\":" + final_json +
            ",\"vga_text\":" + cosmic_pc::PcMachine::quote(pc.screen()) + "}\n";
        std::cout << evidence;
        if (!opts.evidence_path.empty()) {
            std::ofstream file(opts.evidence_path, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("Cannot write evidence file");
            file << evidence;
            if (!file) throw std::runtime_error("Incomplete evidence write");
        }
        if (pc.status() == "fault") return 2;
        if (opts.source == Options::Source::DemoX64)
            return pc.cpu.halted && std::string(pc.cpu.mode_name()) == "long64" ? 0 : 3;
        if (opts.source == Options::Source::DemoX86)
            return pc.cpu.halted ? 0 : 3;
        return 0;  // An ISO may pause at a budget or BIOS prompt; never report Windows boot.
    } catch (const std::exception& error) {
        std::cerr << "REVERSE_WINDOWS_PROBE_ERROR: " << error.what() << '\n';
        return 2;
    }
}
