#include "pc_machine.hpp"
#include "android_ram_backing.hpp"
#include "bios_hard_disk.hpp"
#include "boot_profile.hpp"
#include "qssd_disk.hpp"
#include "c06_motherboard.hpp"
#include "c09_guest_trace.hpp"
#include "c09r_guest_vga_fabric.hpp"
#include "virtual_disk.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <optional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
struct Options {
    enum class Source { None, DemoX86, DemoX64, Iso, Disk, QssdCheckpoint, QssdCompact } source = Source::None;
    std::string iso_path;
    std::string disk_path;
    std::string qssd_path;
    std::uint64_t create_qssd_gib = 0;
    std::uint64_t create_disk_gib = 0;
    std::string evidence_path;
    std::string trace_path;
    std::string ram_spill_directory;
    std::uint64_t trace_max_events=25000;
    std::uint64_t steps = 200000;
    std::uint64_t ram_mib = 2048;
    std::uint64_t resident_mib = 256;
    std::uint64_t profile_every = 0, profile_from = 0, profile_until = 0;
    bool profile_timing = true;
    bool ether_arm64 = false;
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
    bool unprofiled = false;
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
        } else if (arg == "--qssd-checkpoint" || arg == "--qssd-compact") {
            if (out.source != Options::Source::None) throw std::invalid_argument("One action/boot source only");
            out.source = arg == "--qssd-checkpoint" ? Options::Source::QssdCheckpoint :
                                                        Options::Source::QssdCompact;
        } else if (arg == "--disk") out.disk_path = next();
        else if (arg == "--qssd-disk") out.qssd_path = next();
        else if (arg == "--create-qssd-gib") out.create_qssd_gib = positive(next(), 16384, "create-qssd-gib");
        else if (arg == "--create-disk-gib") out.create_disk_gib = positive(next(), 256, "create-disk-gib");
        else if (arg == "--steps") out.steps = positive(next(), 20'000'000'000, "steps");
        else if (arg == "--ram-mib") out.ram_mib = positive(next(), cosmic_pc::PcMachine::max_ram_mib(), "ram-mib");
        else if (arg == "--resident-mib") out.resident_mib = positive(next(), cosmic_pc::PcMachine::max_ram_mib(), "resident-mib");
        else if (arg == "--ram-spill-directory") out.ram_spill_directory = next();
        else if (arg == "--evidence") out.evidence_path = next();
        else if (arg == "--profile-every") out.profile_every = positive(next(), 100000000, "profile-every");
        else if (arg == "--profile-from") out.profile_from = positive(next(), 20'000'000'000, "profile-from");
        else if (arg == "--profile-until") out.profile_until = positive(next(), 20'000'000'000, "profile-until");
        else if (arg == "--profile-counts-only") unprofiled = true;
        else if (arg == "--ether-arm64") out.ether_arm64 = true;
        else if (arg == "--trace") out.trace_path = next();
        else if (arg == "--trace-max-events")
            out.trace_max_events = positive(next(), 2000000, "trace-max-events");
        else throw std::invalid_argument("Unknown option: " + arg);
    }
    if (out.source == Options::Source::None) throw std::invalid_argument("Select a boot source or --qssd-checkpoint/--qssd-compact");
    if (out.ether_arm64 && !out.profile_every)
        throw std::invalid_argument("--ether-arm64 requires --profile-every");
    out.profile_timing = !unprofiled;
    if ((unprofiled || out.profile_from || out.profile_until || !out.ram_spill_directory.empty()) && !out.profile_every)
        throw std::invalid_argument("Profile options require --profile-every");
    if (!out.profile_every && out.steps > 100000000)
        throw std::invalid_argument("steps out of supported bounds without boot profiling");
    if (out.profile_every) {
        if (out.profile_every < reverse_windows::BootProfile::kMinCheckpointSteps)
            throw std::invalid_argument("--profile-every requires at least 4096 steps");
        if (!out.trace_path.empty() || out.source == Options::Source::QssdCheckpoint ||
            out.source == Options::Source::QssdCompact)
            throw std::invalid_argument("--profile-every requires a guest without instruction tracing");
        if (out.profile_from >= out.steps)
            throw std::invalid_argument("--profile-from must precede the run budget");
        if (out.profile_until && (out.profile_until <= out.profile_from || out.profile_until > out.steps))
            throw std::invalid_argument("--profile-until must follow the profile start and fit the run budget");
    }
    if (out.source == Options::Source::QssdCheckpoint ||
        out.source == Options::Source::QssdCompact) {
        if (out.qssd_path.empty() || !out.disk_path.empty() ||
            out.create_qssd_gib || out.create_disk_gib)
            throw std::invalid_argument("QSSD maintenance requires only --qssd-disk PATH.rqvd");
    }
    if (out.ram_mib < 2) throw std::invalid_argument("Mundo X PC requires at least 2 MiB logical RAM");
    if (!out.trace_path.empty()) {
        if (out.source==Options::Source::QssdCompact ||
            out.source==Options::Source::QssdCheckpoint)
            throw std::invalid_argument("--trace needs a boot source, not offline QSSD maintenance");
        // Refuse to clobber an existing trace, especially an ISO or disk image.
        const auto trace = std::filesystem::weakly_canonical(out.trace_path);
        if (std::filesystem::exists(trace) || trace==std::filesystem::weakly_canonical(out.iso_path) ||
            (!out.disk_path.empty() && trace==std::filesystem::weakly_canonical(out.disk_path)) ||
            (!out.qssd_path.empty() && trace==std::filesystem::weakly_canonical(out.qssd_path)))
            throw std::invalid_argument("Trace path exists or aliases a guest ISO/disk; refuse overwrite");
    }
    if (out.create_disk_gib && out.disk_path.empty()) throw std::invalid_argument("--create-disk-gib requires --disk PATH.rvdisk");
    if (out.create_qssd_gib && out.qssd_path.empty()) throw std::invalid_argument("--create-qssd-gib requires --qssd-disk PATH.rqvd");
    if (!out.disk_path.empty() && !out.qssd_path.empty()) throw std::invalid_argument("Select one writable disk backend");
    if (out.source == Options::Source::Disk && out.disk_path.empty() && out.qssd_path.empty()) throw std::invalid_argument("--boot-hdd requires --disk or --qssd-disk");
    if (!out.evidence_path.empty()) {
        const auto evidence = std::filesystem::weakly_canonical(out.evidence_path);
        for (const auto* input : {&out.iso_path, &out.disk_path, &out.qssd_path, &out.trace_path}) {
            if (input->empty()) continue;
            if (evidence == std::filesystem::weakly_canonical(*input) ||
                (std::filesystem::exists(evidence) && std::filesystem::exists(*input) &&
                 std::filesystem::equivalent(evidence, *input)))
                throw std::invalid_argument("Evidence path aliases a guest medium or trace");
        }
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto opts = parse(argc, argv);
        if (opts.source == Options::Source::QssdCheckpoint) {
            reverse_windows::QssdDisk disk(opts.qssd_path);
            if (!disk.checkpoint()) throw std::runtime_error("QSSD index checkpoint could not be fsynced/published");
            const auto stats=disk.stats();
            std::cout << "{\"qssd_maintenance\":\"checkpoint\",\"logical_bytes\":"
                      <<stats.logical_bytes<<",\"journal_bytes\":"<<stats.physical_bytes
                      <<",\"commits\":"<<stats.commits<<",\"volume_generation\":"
                      <<disk.generation()<<",\"status\":\"published\"}\n";
            return 0;
        }
        if (opts.source == Options::Source::QssdCompact) {
            const auto results=reverse_windows::QssdDisk::compact(opts.qssd_path);
            std::cout << "{\"qssd_maintenance\":\"compact\",\"before_bytes\":"
                      <<results.before_bytes<<",\"after_bytes\":"<<results.after_bytes
                      <<",\"live_blocks\":"<<results.live_blocks
                      <<",\"volume_generation\":"<<results.generation
                      <<",\"checkpoint_published\":"
                      <<(results.checkpoint_saved?"true":"false")<<"}\n";
            return 0;
        }
        cosmic_pc::BootTiming::Duration iso_read_time{};
        bool profile_active = false;
        cosmic_pc::PcMachine pc(
            static_cast<std::size_t>(opts.ram_mib * 1024ULL * 1024ULL),
            static_cast<std::size_t>(opts.resident_mib * 1024ULL * 1024ULL));
        reverse_windows::AndroidRamBacking* ram_spill = nullptr;
        if (!opts.ram_spill_directory.empty() && pc.cpu.memory.budget_bytes() < pc.cpu.memory.size()) {
            const auto spill_budget = std::min(pc.cpu.memory.size(),
                reverse_windows::AndroidRamBacking::spill_budget(opts.ram_spill_directory));
            if (!spill_budget) throw std::runtime_error("No space for the requested RAM spill backing");
            auto backing = std::make_unique<reverse_windows::AndroidRamBacking>(
                opts.ram_spill_directory, pc.cpu.memory.size(), spill_budget);
            ram_spill = backing.get();
            pc.cpu.memory.attach_page_backing(std::move(backing));
        }
        std::optional<reverse_windows::BootProfile> profile;
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
        cosmic_pc::PcMachine::Reader read_iso;
        std::optional<cosmic_pc::BootPlan> optical_plan;
        std::uint64_t optical_bytes=0;
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
            const auto medium = std::make_shared<std::ifstream>(opts.iso_path, std::ios::binary);
            if (!*medium) throw std::runtime_error("ISO is not readable");
            medium->seekg(0, std::ios::end);
            const auto end = medium->tellg();
            if (end <= 0) throw std::runtime_error("Invalid ISO size");
            optical_bytes=static_cast<std::uint64_t>(end);
            read_iso=[medium,&iso_read_time,&profile_active](std::uint64_t offset, std::uint8_t* data, std::size_t size) {
                const auto start = profile_active ? cosmic_pc::BootTiming::Clock::now() : cosmic_pc::BootTiming::Clock::time_point{};
                medium->clear();
                medium->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                medium->read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
                if (profile_active) iso_read_time += cosmic_pc::BootTiming::Clock::now() - start;
                return static_cast<std::size_t>(medium->gcount()) == size;
            };
            // Independently audit El Torito metadata only when tracing.
            // Both passes read the same immutable local ISO; no rewrite/RISO.
            if (!opts.trace_path.empty())
                optical_plan=cosmic_pc::BootMedia(optical_bytes,read_iso).plan("iso");
            pc.mount(read_iso,optical_bytes,"iso");
        }

        std::unique_ptr<reverse_windows::BiosHardDisk> hdd;
        if (disk) hdd = std::make_unique<reverse_windows::BiosHardDisk>(pc, *disk);
        // One canonical guest CPU. The motherboard wraps its existing I/O hooks
        // after boot firmware and QSSD are mounted, and stays alive until exit.
        reverse_windows::PcMotherboardC06 motherboard(pc,disk.get(),read_iso,optical_bytes);
        const auto initial_disk_commits=qdisk ? qdisk->stats().commits : 0;
        if (opts.profile_every) {
            motherboard.enable_ps2_keyboard();
            pc.enable_boot_acceleration();
            if (opts.source == Options::Source::Iso) pc.enable_boot_hotspots(true, false);
            pc.cpu.enable_page_translation_cache();
            pc.cpu.enable_ether_arm64_blocks(opts.ether_arm64);
#if defined(MUNDO_X_PC_ETHER_NEXT)
            pc.cpu.enable_ether_next();
#endif
        }
        std::unique_ptr<reverse_windows::C09GuestTrace> trace;
        if (!opts.trace_path.empty()) {
            trace=std::make_unique<reverse_windows::C09GuestTrace>(
                pc,motherboard,qdisk,opts.trace_path,opts.trace_max_events);
            if (optical_plan) trace->note_boot_plan(*optical_plan,optical_bytes);
        }

        // The live guest framebuffer is the only source of pixels.
        // GPU-VIRTUAL selects exact updates; qSSD StateBus versions the plan.
        reverse_windows::GuestVgaFabricC09R vga_work(pc);
        auto vga_frame=vga_work.capture();
        std::uint64_t vga_captures=1;
        std::uint64_t vga_total_upload_bytes=vga_frame.transferred_bytes;
        std::uint64_t vga_baseline_bytes=4000;

        const bool demo = opts.source == Options::Source::DemoX86 ||
                          opts.source == Options::Source::DemoX64;
        auto platform = motherboard.evidence();
        cosmic_pc::BootTiming::Clock::time_point profile_start;
        std::uint64_t spill_read_start = 0, spill_write_start = 0;
        const auto begin_profile = [&] {
            if (opts.profile_timing) pc.enable_boot_timing();
            profile.emplace(opts.profile_every);
            profile->begin(pc,motherboard);
            if (ram_spill) {
                spill_read_start = ram_spill->read_us();
                spill_write_start = ram_spill->write_us();
            }
            profile_active = true;
            profile_start = cosmic_pc::BootTiming::Clock::now();
        };
        if (opts.profile_every && !opts.profile_from) begin_profile();
        while (platform.ticks < opts.steps && pc.status() != "fault" && !platform.powered_off) {
            if (opts.profile_until && pc.cpu.instructions == opts.profile_until) break;
            if (pc.cpu.halted && (demo || !pc.cpu.flag(cosmic_pc::Cpu::IF))) break;
            if (pc.waiting_key) {
                if (opts.source != Options::Source::DemoX86) break;
                pc.key('X', 0x2d);
            }
            const auto before = platform.ticks;
            const auto chunk = static_cast<unsigned>(
                std::min<std::uint64_t>(5000, opts.steps - before));
            auto slice = chunk;
            if (profile) slice = profile->budget(chunk,pc);
            else if (opts.profile_every)
                slice = static_cast<unsigned>(std::min<std::uint64_t>(chunk,opts.profile_from-pc.cpu.instructions));
            if (opts.profile_until)
                slice = static_cast<unsigned>(std::min<std::uint64_t>(slice,opts.profile_until-pc.cpu.instructions));
            pc.run(slice);
            if (opts.profile_every && !profile && pc.cpu.instructions==opts.profile_from) begin_profile();
            if (profile) profile->after_run(pc, motherboard);
            platform = motherboard.evidence();
            // The profiling loop excludes host-side frame capture.
            if (!opts.profile_every) {
                vga_frame=vga_work.capture();
                ++vga_captures;
                vga_total_upload_bytes+=vga_frame.transferred_bytes;
                vga_baseline_bytes+=4000;
            }
            if (platform.ticks == before && !pc.waiting_key) break;
        }
        std::ostringstream performance;
        performance << ",\"ether_arm64\":";
        reverse_windows::write_ether_arm64(performance, pc.cpu);
        if (opts.profile_every && !profile)
            throw std::runtime_error("Guest stopped before the requested profile start");
        if (profile) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                cosmic_pc::BootTiming::Clock::now() - profile_start).count();
            const auto optical_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(iso_read_time).count();
            performance << ",\"boot_profile\":";
            profile->write_json(performance, pc, motherboard);
            performance << ",\"profile_wall_ns\":" << elapsed
                        << ",\"profile_iso_read_ns\":" << optical_ns
                        << ",\"profile_until_steps\":" << opts.profile_until
                        << ",\"profile_until_reached\":"
                        << (opts.profile_until && pc.cpu.instructions == opts.profile_until ? "true" : "false")
                        << ",\"profile_source_bytes\":" << optical_bytes
                        << ",\"profile_ram_mib\":" << opts.ram_mib
                        << ",\"profile_resident_mib\":" << opts.resident_mib
                        << ",\"profile_initial_qssd_commits\":" << initial_disk_commits;
            if (ram_spill) {
                performance << ",\"profile_spill_read_us\":" << ram_spill->read_us() - spill_read_start
                            << ",\"profile_spill_write_us\":" << ram_spill->write_us() - spill_write_start;
            }
            if (opts.profile_timing) {
                performance << ",\"boot_time_samples\":";
                pc.boot_timing()->write_json(performance);
                performance << ",\"boot_code_regions\":";
                pc.boot_regions()->write_json(performance);
            }
            vga_frame=vga_work.capture();
            ++vga_captures;
            vga_total_upload_bytes+=vga_frame.transferred_bytes;
            vga_baseline_bytes+=4000;
        }
        if (trace) trace->finish();
        const auto final_json = pc.json();
        const auto* ata = motherboard.attached_ata();
        const auto ata_stats = ata ? ata->counters() : reverse_windows::AtaC07::Counters{};
        const std::string evidence = std::string("{\"schema\":\"reverse-windows-boot-evidence-v1\",") +
            "\"guest_kind\":" + cosmic_pc::PcMachine::quote(kind) +
            ",\"source\":\"original-Mundo-X-PcMachine\"" +
            ",\"windows10_boot_verified\":false" +
            ",\"c09r_gpu_vga_work\":{\"owner\":\"GPU-VIRTUAL-plus-qSSD-StateBus\",\"source\":\"real-guest-VGA\",\"frames\":" +
               std::to_string(vga_captures) + ",\"full_frame_bytes_baseline\":" +
               std::to_string(vga_baseline_bytes) + ",\"planned_host_update_bytes\":" +
               std::to_string(vga_total_upload_bytes) + ",\"latest_qssd_graphics_serial\":" +
               std::to_string(vga_frame.qssd_serial) +
               ",\"last_exact_mirror\":" + std::string(vga_frame.exact_mirror?"true":"false") +
               ",\"windows_graphics_driver_ready\":false}" +
            ",\"c09_trace\":{\"enabled\":" + std::string(trace?"true":"false") +
            ",\"schema\":\"reverse-windows-c09-trace-v1\",\"windows_pe_verified\":false}" +
            ",\"writable_hdd_attached\":" + std::string(disk ? "true" : "false") +
            ",\"bios_fixed_disks\":" + std::to_string(pc.cpu.read8(0x475)) +
            ",\"atapi_cdrom\":" + std::string(motherboard.optical_attached()?"true":"false") +
            ",\"storage_backend\":" + cosmic_pc::PcMachine::quote(qdisk ? "QSSD-journaled" : disk ? "raw" : "none") +
            ",\"qssd_commits\":" + std::to_string(qdisk ? qdisk->stats().commits : 0) +
            ",\"qssd_physical_bytes\":" + std::to_string(qdisk ? qdisk->stats().physical_bytes : 0) +
            ",\"hdd_sectors\":" + std::to_string(disk ? disk->sectors() : 0) +
            ",\"ide_c07\":{\"attached\":" + std::string(ata?"true":"false") +
               ",\"pci_class\":\"0101\",\"mode\":\"legacy-PIO\",\"dma_supported\":false,\"ata_commands\":" +
               std::to_string(ata_stats.commands) + ",\"read_sectors\":" +
               std::to_string(ata_stats.read_sectors) + ",\"write_sectors\":" +
               std::to_string(ata_stats.write_sectors) + ",\"flushes\":" +
               std::to_string(ata_stats.flushes) + ",\"irq14_delivered\":" +
               std::to_string(platform.ata_irqs) + "}" +
            ",\"motherboard_c06\":{\"pci_config\":true,\"acpi_tables\":true,\"apic_full\":false,\"irq_delivered\":" +
               std::to_string(platform.irq_delivered) + ",\"pit_irqs\":" +
               std::to_string(platform.pit_irqs) + ",\"apic_irqs\":" +
               std::to_string(platform.apic_irqs) + ",\"ticks\":" +
               std::to_string(platform.ticks) + "}" +
            performance.str() + ",\"machine\":" + final_json +
            ",\"vga_text\":" + cosmic_pc::PcMachine::quote(pc.screen()) + "}\n";
        std::cout << evidence;
        if (!opts.evidence_path.empty()) {
            std::ofstream file(opts.evidence_path, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("Cannot write evidence file");
            file << evidence;
            if (!file) throw std::runtime_error("Incomplete evidence write");
        }
        if (pc.status() == "fault") return 2;
        if (opts.profile_until) return pc.cpu.instructions == opts.profile_until ? 0 : 3;
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
