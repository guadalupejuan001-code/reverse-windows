#include "c06_motherboard.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::AcpiC06;
using reverse_windows::PcMotherboardC06;

namespace {
constexpr auto kLapic = PcMotherboardC06::kLocalApic;
constexpr unsigned kProcessors = 16;
constexpr unsigned kSharedCounter = 0x3200;
constexpr std::uint8_t kStartupVector = 8;

void require(bool condition, const std::string& problem) {
    if (!condition) throw std::runtime_error(problem);
}

std::uint64_t topology_leaf(Cpu& cpu, unsigned level) {
    cpu.reset(0, 0x7100);
    cpu.a20_enabled = true;
    cpu.write16(0x7100, 0xa20f);
    cpu.regs[Cpu::EAX] = 0x0b;
    cpu.regs[Cpu::ECX] = level;
    require(cpu.step() == cosmic_pc::StepResult::Executed, "CPUID on the boot CPU");
    return cpu.regs[Cpu::EAX] | (cpu.regs[Cpu::EBX] << 8) | (cpu.regs[Cpu::ECX] << 24);
}

void check_acpi(Cpu& cpu) {
    constexpr auto madt = AcpiC06::madt_address;
    const auto size = cpu.read32(madt + 4);
    require(size == AcpiC06::madt_bytes + 8 * (kProcessors - 1), "MADT has 16 processor records");
    unsigned sum = 0;
    for (unsigned i = 0; i < size; ++i) sum += cpu.read8(madt + i);
    require((sum & 255U) == 0, "MADT checksum");
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto record = madt + 44 + id * 8U;
        require(cpu.read8(record) == 0 && cpu.read8(record + 1) == 8 &&
                cpu.read8(record + 2) == id && cpu.read8(record + 3) == id &&
                cpu.read32(record + 4) == 1, "MADT enabled APIC ID " + std::to_string(id));
    }
    require(cpu.read8(madt + 44 + 8 * kProcessors + 2) == kProcessors, "unique I/O APIC ID");
    const auto dsdt = cpu.read32(AcpiC06::fadt_address + 40);
    require(dsdt == AcpiC06::smp_dsdt_address, "FADT points to the SMP DSDT");
    std::string aml;
    sum = 0;
    for (unsigned i = 0; i < cpu.read32(dsdt + 4); ++i) {
        const auto byte = cpu.read8(dsdt + i);
        aml += char(byte);
        sum += byte;
    }
    require((sum & 255U) == 0, "DSDT checksum");
    std::size_t cursor = 0;
    for (unsigned id = 0; id < kProcessors; ++id) {
        cursor = aml.find("ACPI0007", cursor);
        require(cursor != std::string::npos &&
                aml.substr(cursor + 10, 4) == "_UID" &&
                std::uint8_t(aml[cursor + 15]) == id, "DSDT device UID " + std::to_string(id));
        cursor += 16;
    }
}

void install_trampoline(Cpu& boot) {
    // Executed by all APs from the page selected by the startup IPI. Index the
    // result area by CPUID APIC ID and atomically increment one shared word.
    constexpr std::uint8_t startup[] = {
        0x66,0xb8,0x0b,0x00,0x00,0x00, 0x66,0x31,0xc9, 0x0f,0xa2, // CPUID.0B.0
        0x89,0xd6, 0xc1,0xe6,0x02,                               // SI = APIC ID * 4
        0x66,0x89,0x84,0x00,0x30, 0x66,0x89,0x9c,0x40,0x30,        // shift; SMT count
        0x66,0xb8,0x0b,0x00,0x00,0x00, 0x66,0xb9,0x01,0x00,0x00,0x00,0x0f,0xa2,
        0x66,0x89,0x84,0x80,0x30, 0x66,0x89,0x9c,0xc0,0x30,        // shift; package count
        0x66,0x89,0x94,0x00,0x31,                                 // APIC ID
        0x66,0xb9,0x1b,0x00,0x00,0x00, 0x0f,0x32,                 // RDMSR IA32_APIC_BASE
        0x66,0x89,0x84,0x40,0x31,
        0xf0,0x66,0xff,0x06,0x00,0x32,                             // LOCK INC [0x3200]
        0xfa,0xf4                                                  // CLI; HLT
    };
    boot.memory.write_block(std::uint32_t(kStartupVector) << 12, startup, sizeof(startup));
}

void test_eight_cores_sixteen_threads() {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
    check_acpi(pc.cpu);
    require(topology_leaf(pc.cpu,0) == (1ULL | (2ULL << 8) | (0x100ULL << 24)), "SMT topology");
    require(topology_leaf(pc.cpu,1) == (4ULL | (16ULL << 8) | (0x201ULL << 24)), "package topology");
    require(topology_leaf(pc.cpu,2) == (2ULL << 24), "end of topology enumeration");

    pc.cpu.reset(0, 0x7200);
    pc.cpu.a20_enabled = true;
    pc.cpu.write16(0x7200, 0xfeeb);  // BSP runs a bounded spin until the test ends.
    install_trampoline(pc.cpu);
    board.run(32);
    require(board.processors_started() == 1 && pc.cpu.read32(kSharedCounter) == 0,
            "only BSP runs before INIT/SIPI");
    pc.cpu.write32(kLapic + 0x300, 0x000cc500); // INIT asserted, all excluding BSP
    pc.cpu.write32(kLapic + 0x300, 0x000c8500); // INIT level deassert
    pc.cpu.write32(kLapic + 0x300, 0x000c0600 | kStartupVector);
    pc.cpu.write32(kLapic + 0x300, 0x000c0600 | (kStartupVector + 1)); // first SIPI wins
    const auto start_steps = board.total_processor_steps();
    for (unsigned i = 0; i < 64; ++i) board.run(1);
    require(board.processors_started() == kProcessors, "small budgets do not starve any AP");
    board.run(640); // The workers contend for the same RAM word.
    require(board.total_processor_steps() <= start_steps + 704,
            "a VM step budget is shared by its processors");
    require(!board.faulted_cpu(), "all startup guest code must execute");
    require(board.processors_started() == kProcessors && pc.cpu.read32(kSharedCounter) == 15,
            "all 15 APs ran x86 LOCK INC on one shared RAM word");
    require(board.worker_count() >= 1 && board.worker_count() <= 8, "bounded host worker count");
    for (unsigned id = 1; id < kProcessors; ++id) {
        const auto offset = id * 4;
        require(pc.cpu.read32(0x3000 + offset) == 1 && pc.cpu.read32(0x3040 + offset) == 2 &&
                pc.cpu.read32(0x3080 + offset) == 4 && pc.cpu.read32(0x30c0 + offset) == 16 &&
                pc.cpu.read32(0x3100 + offset) == id &&
                pc.cpu.read32(0x3140 + offset) == (kLapic | 0x800U) &&
                board.processor_steps(id) > 0, "independent CPU state and APIC ID " + std::to_string(id));
    }

    const auto ap1_steps = board.processor_steps(1);
    pc.cpu.write32(kLapic + 0x310, 1U << 24);
    pc.cpu.write32(kLapic + 0x300, 0x600U | kStartupVector);
    board.run(64);
    require(pc.cpu.read32(kSharedCounter) == 15 && board.processor_steps(1) == ap1_steps,
            "repeat SIPI must not reboot a running AP");

    pc.cpu.write32(kLapic + 0x300, 0xc500U);
    pc.cpu.write32(kLapic + 0x300, 0x600U | kStartupVector);
    board.run(128);
    require(pc.cpu.read32(kSharedCounter) == 16 && board.processor_steps(1) > ap1_steps,
            "a directed INIT followed by SIPI restarts just the addressed AP");
    pc.reset();
    require(board.processors_started() == 1 && pc.cpu.read32(kSharedCounter) == 0,
            "machine reset clears AP execution and shared RAM");
    check_acpi(pc.cpu);
}
void test_secondary_interrupts() {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1});
    auto& cpu = pc.cpu;
    cpu.reset(0,0x7200);
    cpu.a20_enabled = true;
    cpu.write16(0x7200,0xfeeb);

    // Flat GDT and protected-mode IDT used by the application processor.
    cpu.write16(0x2800,23);
    cpu.write32(0x2802,0x2810);
    cpu.write16(0x2806,0x07ff);
    cpu.write32(0x2808,0x1000);
    cpu.write64(0x2818,0x00cf9a000000ffffULL);
    cpu.write64(0x2820,0x00cf92000000ffffULL);
    cpu.write64(0x1208,0x00008e0000089200ULL); // Fixed interprocessor IRQ.
    cpu.write64(0x1210,0x00008e0000089230ULL); // LAPIC timer IRQ.
    // Switch the AP to 32-bit mode, read its own LAPIC ID, configure its
    // timer and wait with IF set, then acknowledge IRQs and return via IRET.
    constexpr std::uint8_t startup[] = {
        0xfa,0x0f,0x01,0x16,0x00,0x28,0x0f,0x20,0xc0,0x0c,0x01,0x0f,
        0x22,0xc0,0x66,0xea,0x20,0x90,0x00,0x00,0x08,0x00
    };
    constexpr std::uint8_t protected_init[] = {
        0x66,0xb8,0x10,0x00,0x8e,0xd8,0x8e,0xd0,0xbc,0x00,0x62,0x00,
        0x00,0x0f,0x01,0x1d,0x06,0x28,0x00,0x00,0xa1,0x20,0x00,0xe0,
        0xfe,0xc1,0xe8,0x18,0xa3,0x00,0x34,0x00,0x00,0xc7,0x05,0xf0,
        0x00,0xe0,0xfe,0xff,0x01,0x00,0x00,0xc7,0x05,0x20,0x03,0xe0,
        0xfe,0x42,0x00,0x00,0x00,0xc7,0x05,0xe0,0x03,0xe0,0xfe,0x0b,
        0x00,0x00,0x00,0xc7,0x05,0x80,0x03,0xe0,0xfe,0x28,0x00,0x00,
        0x00,0xfb,0xf4,0xeb,0xfd
    };
    constexpr std::uint8_t ipi_handler[] = {
        0xff,0x05,0x04,0x34,0x00,0x00,0xc7,0x05,0xb0,0x00,0xe0,0xfe,
        0x00,0x00,0x00,0x00,0xcf
    };
    constexpr std::uint8_t timer_handler[] = {
        0xff,0x05,0x08,0x34,0x00,0x00,0xc7,0x05,0xb0,0x00,0xe0,0xfe,
        0x00,0x00,0x00,0x00,0xcf
    };
    cpu.memory.write_block(0x9000,startup,sizeof(startup));
    cpu.memory.write_block(0x9020,protected_init,sizeof(protected_init));
    cpu.memory.write_block(0x9200,ipi_handler,sizeof(ipi_handler));
    cpu.memory.write_block(0x9230,timer_handler,sizeof(timer_handler));
    cpu.write32(kLapic + 0x310,1U << 24);
    cpu.write32(kLapic + 0x300,0x0609U);
    board.run(256);
    if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
    require(cpu.read32(0x3400) == 1, "AP-specific MMIO LAPIC identity");
    require(cpu.read32(0x3408) == 1, "the AP's own timer interrupted HLT");
    cpu.write32(kLapic + 0x300,0x41U);
    board.run(64);
    if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
    require(cpu.read32(0x3404) == 1 && cpu.read32(0x3408) == 1 &&
            board.evidence().apic_irqs == 2, "directed IPI, EOI, and independent AP timer");
}

void test_ether_worker_block() {
    if (!Cpu::ether_arm64_host()) return;
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault, "long-mode worker fixture");
    cpu.halted = false;
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    constexpr unsigned address = 0x7000;
    constexpr std::uint8_t block[] = {0x48,0x89,0xd8,0x48,0xff,0xc0,0xeb,0x00};
    cpu.memory.write_block(address,block,sizeof(block)); // MOV RAX,RBX; INC RAX; JMP.
    cpu.regs[Cpu::EBX] = 41;
    std::mutex bus;
    std::unique_lock<std::mutex> lock(bus);
    unsigned steps = 0;
    for (unsigned attempt = 0; attempt < 4 && !steps; ++attempt) {
        cpu.eip = address;
        steps = cpu.try_ether_arm64_block(3,&lock);
    }
    require(steps == 3 && cpu.regs[Cpu::EAX] == 42 && lock.owns_lock(),
            "the private ARM64 block retires and returns with the bus lock held");
}
} // namespace

int main() {
    try {
        test_eight_cores_sixteen_threads();
        test_secondary_interrupts();
        test_ether_worker_block();
        std::cout << "SMP_PASS cores=8 logical_processors=16 aps_executed=15 atomic_shared_ram=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
