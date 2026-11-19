#include "c06_motherboard.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::AcpiC06;
using reverse_windows::PcMotherboardC06;

namespace {
constexpr auto kLapic = PcMotherboardC06::kLocalApic;
constexpr unsigned kProcessors = 16;
constexpr unsigned kSharedCounter = 0x3200;
constexpr std::uint8_t kStartupVector = 8;
constexpr unsigned kSmpEntry = (kStartupVector << 12) + 0x100;
constexpr unsigned kStartFlag = 0x4400, kJobs = 0x4500, kJobBytes = 32;

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
        0x88,0xd0,0xe6,0xe9,                                       // OUT 0xE9, APIC ID.
        0x66,0xb9,0x1b,0x00,0x00,0x00, 0x0f,0x32,                 // RDMSR IA32_APIC_BASE
        0x66,0x89,0x84,0x40,0x31,
        0xf0,0x66,0xff,0x06,0x00,0x32,                             // LOCK INC [0x3200]
        0xfa,0xf4                                                  // CLI; HLT
    };
    boot.memory.write_block(std::uint32_t(kStartupVector) << 12, startup, sizeof(startup));
}

void test_eight_cores_sixteen_threads(bool threaded, unsigned worker_limit = kProcessors) {
    std::array<std::thread::id, kProcessors> hosts{};
    hosts[0] = std::this_thread::get_id();
    PcMachine pc(4U << 20);
    pc.boot_demo();
    pc.cpu.out_hook = [&](std::uint16_t port, unsigned width, std::uint32_t value) {
        if (port != 0xe9 || width != 1 || value == 0 || value >= kProcessors) return false;
        hosts[value] = std::this_thread::get_id();
        return true;
    };
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
    if (threaded) board.enable_threaded_smp(worker_limit);
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
    const auto initial_irqs = board.evidence().irq_delivered;
    pc.cpu.write32(kLapic + 0x300, 0x000cc500); // INIT asserted, all excluding BSP
    pc.cpu.write32(kLapic + 0x300, 0x000c8500); // INIT level deassert
    pc.cpu.write32(kLapic + 0x300, 0x000c0600 | kStartupVector);
    pc.cpu.write32(kLapic + 0x300, 0x000c0600 | (kStartupVector + 1)); // first SIPI wins
    const auto queued = board.startup_ipis();
    require(queued.init_icr_writes == 2 && queued.init_targets_queued == kProcessors - 1 &&
                queued.sipi_icr_writes == 2 && queued.sipi_targets_queued == kProcessors - 1 &&
                queued.sipi_targets_applied == 0,
            "startup counters separate ICR writes, queued targets, and AP starts");
    const auto start_steps = board.total_processor_steps();
    for (unsigned i = 0; i < 64; ++i) board.run(1);
    require(board.processors_started() == kProcessors, "small budgets do not starve any AP");
    board.run(640); // The workers contend for the same RAM word.
    require(board.total_processor_steps() <= start_steps + 704,
            "a VM step budget is shared by its processors");
    require(!board.faulted_cpu(), "all startup guest code must execute");
    require(board.processors_started() == kProcessors && pc.cpu.read32(kSharedCounter) == 15,
            "all 15 APs ran x86 LOCK INC on one shared RAM word");
    require(board.startup_ipis().sipi_targets_applied == kProcessors - 1 &&
                board.evidence().irq_delivered == initial_irqs,
            "applied SIPIs count separately from delivered maskable IRQs");
    require(board.worker_count() >= 1 && board.worker_count() <= kProcessors, "bounded host worker count");
    for (unsigned id = 1; id < kProcessors; ++id) {
        const auto offset = id * 4;
        require(hosts[id] != std::thread::id{}, "each AP must reach the guest I/O probe");
        require(pc.cpu.read32(0x3000 + offset) == 1 && pc.cpu.read32(0x3040 + offset) == 2 &&
                pc.cpu.read32(0x3080 + offset) == 4 && pc.cpu.read32(0x30c0 + offset) == 16 &&
                pc.cpu.read32(0x3100 + offset) == id &&
                pc.cpu.read32(0x3140 + offset) == (kLapic | 0x800U) &&
                board.processor_steps(id) > 0, "independent CPU state and APIC ID " + std::to_string(id));
    }
    const std::set<std::thread::id> distinct_hosts(hosts.begin(), hosts.end());
    const auto& dispatch = board.threaded_smp_stats();
    if (threaded) {
        require(distinct_hosts.size() == worker_limit && board.worker_count() == worker_limit &&
                    dispatch.peak_workers == worker_limit && dispatch.dispatches && dispatch.guest_steps,
                "All 16 real-mode vCPUs must use the bounded host pool without cached ARM64 loops");
        std::cout << "SMP_THREADED_INTERPRETER_PASS workers=" << distinct_hosts.size()
                  << " shared_atomic_counter=" << pc.cpu.read32(kSharedCounter) << '\n';
    } else {
        require(distinct_hosts.size() == 1 && !dispatch.dispatches,
                "the default scheduler keeps the deterministic serial startup path");
    }

    const auto ap1_steps = board.processor_steps(1);
    pc.cpu.write32(kLapic + 0x310, 1U << 24);
    pc.cpu.write32(kLapic + 0x300, 0x600U | kStartupVector);
    board.run(64);
    require(pc.cpu.read32(kSharedCounter) == 15 && board.processor_steps(1) == ap1_steps,
            "repeat SIPI must not reboot a running AP");
    const auto duplicate = board.startup_ipis();
    require(duplicate.sipi_icr_writes == 3 && duplicate.sipi_targets_queued == kProcessors - 1 &&
                duplicate.sipi_targets_applied == kProcessors - 1,
            "SIPI to a running AP counts only as an ICR write");

    pc.cpu.write32(kLapic + 0x300, 0xc500U);
    pc.cpu.write32(kLapic + 0x300, 0x600U | kStartupVector);
    board.run(128);
    require(pc.cpu.read32(kSharedCounter) == 16 && board.processor_steps(1) > ap1_steps,
            "a directed INIT followed by SIPI restarts just the addressed AP");
    const auto restarted = board.startup_ipis();
    require(restarted.init_icr_writes == 3 && restarted.init_targets_queued == kProcessors &&
                restarted.sipi_icr_writes == 4 && restarted.sipi_targets_queued == kProcessors &&
                restarted.sipi_targets_applied == kProcessors,
            "one directed AP restart adds one queued and one applied SIPI");
    pc.reset();
    require(board.processors_started() == 1 && pc.cpu.read32(kSharedCounter) == 0,
            "machine reset clears AP execution and shared RAM");
    const auto reset = board.startup_ipis();
    require(reset.init_icr_writes == 0 && reset.sipi_icr_writes == 0 && reset.init_targets_queued == 0 &&
                reset.sipi_targets_queued == 0 && reset.sipi_targets_applied == 0,
            "a machine reset begins a new phase for the startup counters");
    require(!dispatch.dispatches && !dispatch.guest_steps && !dispatch.last_workers && !dispatch.peak_workers,
            "a machine reset clears per-phase thread evidence");
    check_acpi(pc.cpu);
}
// Enter the flat 32-bit code segment at 0008:9020 from a real-mode startup page.
void install_protected32_entry(Cpu& cpu, unsigned address) {
    cpu.write16(0x2800,23);
    cpu.write32(0x2802,0x2810);
    cpu.write64(0x2818,0x00cf9a000000ffffULL);
    cpu.write64(0x2820,0x00cf92000000ffffULL);
    constexpr std::uint8_t startup[] = {
        0xfa, 0x0f,0x01,0x16,0x00,0x28,               // CLI; LGDT [2800h]
        0x0f,0x20,0xc0, 0x0c,0x01, 0x0f,0x22,0xc0,   // Set CR0.PE.
        0x66,0xea,0x20,0x90,0x00,0x00,0x08,0x00};    // Far jump into code32.
    cpu.memory.write_block(address,startup,sizeof(startup));
}

void test_secondary_interrupts(bool threaded) {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1});
    if (threaded) board.enable_threaded_smp();
    auto& cpu = pc.cpu;
    cpu.reset(0,0x7200);
    cpu.a20_enabled = true;
    cpu.write16(0x7200,0xfeeb);

    install_protected32_entry(cpu, 0x9000);
    cpu.write16(0x2806,0x07ff);
    cpu.write32(0x2808,0x1000);
    cpu.write64(0x1208,0x00008e0000089200ULL); // Fixed interprocessor IRQ.
    cpu.write64(0x1210,0x00008e0000089230ULL); // LAPIC timer IRQ.
    // Switch the AP to 32-bit mode, read its own LAPIC ID, configure its
    // timer and wait with IF set, then acknowledge IRQs and return via IRET.
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

void test_bsp_irq_during_ap_rep() {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    pc.enable_boot_acceleration();
    bool armed = false;
    pc.cpu.out_hook = [&](std::uint16_t port, unsigned width, std::uint32_t) {
        if (port != 0xe9 || width != 1) return false;
        armed = true;
        return true;
    };
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1});
    auto& cpu = pc.cpu;
    cpu.reset(0, 0x7200);
    cpu.a20_enabled = true;
    cpu.eflags = Cpu::IF | 2;
    cpu.regs[Cpu::ESP] = 0x7000;
    cpu.write16(0x7200, 0xfeeb);
    cpu.write32(0x51 * 4, 0x7400); // BSP real-mode IRQ vector and IRET.
    cpu.write8(0x7400, 0xcf);
    cpu.out_hook(0x21, 1, 0xff);
    cpu.out_hook(0xa1, 1, 0xff);
    cpu.write32(kLapic + 0xf0, 0x1ff);
    install_protected32_entry(cpu, 0x9000);
    constexpr std::uint8_t code[] = {
        0x66,0xb8,0x10,0,0x8e,0xd8,0x8e,0xc0,       // Flat data and extra segments.
        0xbb,0,0,0xe0,0xfe, 0xc7,0x83,0xf0,0,0,0,0xff,1,0,0,
        0xbe,0,0x50,0,0, 0xbf,0,0x51,0,0, 0xb9,16,0,0,0,
        0xfc,0xfb,0x90,0xe6,0xe9,                     // Mark entry to the measured grant.
        0xc7,0x83,0,3,0,0,0x51,0,0,0,                // Interrupt BSP; AP's IRR stays clear.
        0xf3,0xa4,0xfa,0xf4
    };
    cpu.memory.write_block(0x9020, code, sizeof(code));
    for (unsigned i = 0; i < 16; ++i) cpu.write8(0x5000 + i, std::uint8_t(0x71 + i));
    cpu.write32(kLapic + 0x310, 1U << 24);
    cpu.write32(kLapic + 0x300, 0x609);
    for (unsigned i = 0; !armed && i < 512; ++i) board.run(1);
    require(armed, "AP did not arm the directed-IRQ fixture");
    board.run(64);
    require(!board.faulted_cpu() && board.interrupt_state().pending == 0x51,
            "BSP IRQ must be pending throughout the AP's grant");
    for (unsigned i = 0; i < 16; ++i)
        require(cpu.read8(0x5100 + i) == 0x71 + i, "AP copy while BSP IRQ pending");
#ifdef PRIME_R101_BASELINE
    const auto elements = 0;
#else
    const auto elements = board.secondary_rep_elements();
#endif
#if !defined(PRIME_R101_BASELINE) && !defined(PRIME_R102_BASELINE)
    // The first scalar iteration decodes REP. The remaining 15 are eligible.
    require(elements == 15, "cross-vCPU REP batched " + std::to_string(elements) + " of 15 eligible elements");
#endif
    board.run(64);
    require(board.evidence().apic_irqs == 1 && !board.interrupt_state().pending,
            "the directed BSP interrupt was lost");
    std::cout << "PRIME_BSP_IRQ_AP_REP_PASS bytes=16 batched=" << elements << " delivered=1\n";
}

// Fifteen application processors enter HLT with interrupts enabled. The BSP
// stays in a deterministic real-mode JMP loop with external interrupts masked.
void measure_sleeping_siblings(bool threaded) {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    auto& cpu = pc.cpu;
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
    if (threaded) board.enable_threaded_smp(8);
    cpu.reset(0, 0x7200);
    cpu.a20_enabled = true;
    cpu.write16(0x7200, 0xfeeb);
    constexpr std::uint8_t ap_code[] = {0xfb, 0x90, 0xf4}; // STI; NOP; HLT.
    cpu.memory.write_block(kStartupVector << 12, ap_code, sizeof(ap_code));
    cpu.write32(kLapic + 0x300, 0x000c0600 | kStartupVector);
    board.run(8192);
    require(board.processors_started() == 16 && !board.faulted_cpu(), "15 APs reached their SIPI code");
    const auto before = board.processor_steps(0);
    const auto total = board.total_processor_steps();
    constexpr unsigned budget = 32768;
    const auto start = PcMachine::RunClock::now();
    board.run(budget);
    const auto retired = board.processor_steps(0) - before;
    require(retired && board.total_processor_steps() - total == retired, "a sleeping AP executed guest code");
#if !defined(PRIME_R101_BASELINE) && !defined(PRIME_R102_BASELINE)
    require(retired == budget, "sleeping processors consumed the BSP's instruction budget");
    for (unsigned id = 1; id < 16; ++id)
        require(std::string(board.processor_state(id)) == "halted", "sleeping AP telemetry");
#endif
    unsigned slices = 1;
    while (board.processor_steps(0) - before < budget && slices < 32) { board.run(budget); ++slices; }
    require(board.processor_steps(0) - before == budget, "equal-work BSP comparison");
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(PcMachine::RunClock::now() - start).count();
    std::cout << "PRIME_IDLE_APS_PASS sleeping=15 threaded=" << threaded << " budget=" << budget
              << " retired=" << retired << " slices=" << slices << " equal_work_ns=" << ns << '\n';
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
    require(cpu.ether_arm64_stats().off_bus_register_steps == steps,
            "the retired register work must have run outside the bus lock");

    constexpr unsigned loop = address + 32;
    constexpr std::uint8_t loop_code[] = {
        0x48,0x83,0xc0,7, 0x48,0xff,0xc6, // ADD RAX,7; INC RSI.
        0x48,0xff,0xc2, 0x48,0xff,0xc2, 0x48,0xff,0xc2,
        0x48,0xff,0xc2, 0x48,0xff,0xc2, 0x48,0xff,0xc2,
        0x48,0xff,0xc2, 0x48,0xff,0xc2, 0x48,0xff,0xc2,
        0x48,0xff,0xc2, 0x48,0xff,0xc2, 0x48,0xff,0xc2, // 12 INC RDX.
        0x83,0xe9,1, 0x75,0xd0 // SUB ECX,1; JNZ.
    };
    constexpr unsigned rounds = 256, loop_steps = rounds * 16;
    cpu.memory.write_block(loop, loop_code, sizeof(loop_code));
    const auto prepare = [&](bool irq_enabled) {
        cpu.eip = loop;
        cpu.eflags = 2 | (irq_enabled ? Cpu::IF : 0);
        cpu.regs[Cpu::EAX] = 0;
        cpu.regs[Cpu::ECX] = rounds;
        cpu.regs[Cpu::EDX] = 0;
        cpu.regs[Cpu::ESI] = 0;
    };
    const auto before = cpu.ether_arm64_stats().private_loop_steps;
    steps = 0;
    for (unsigned attempt = 0; attempt < 4 && !steps; ++attempt) {
        prepare(false);
        steps = cpu.try_ether_arm64_block(loop_steps, &lock);
    }
    require(steps == loop_steps && cpu.regs[Cpu::EAX] == rounds * 7 && cpu.regs[Cpu::ECX] == 0 &&
            cpu.regs[Cpu::EDX] == rounds * 12 && cpu.regs[Cpu::ESI] == rounds &&
            cpu.eip == loop + sizeof(loop_code) && lock.owns_lock() &&
            cpu.ether_arm64_stats().private_loop_steps == before + steps,
            "the worker must retire 4096 private steps within one grant");

    const auto native_regs = cpu.regs;
    const auto native_flags = cpu.eflags;
    prepare(false);
    for (unsigned i = 0; i < steps; ++i)
        require(cpu.step() == cosmic_pc::StepResult::Executed, "scalar loop failed");
    require(cpu.regs == native_regs && cpu.eflags == native_flags && cpu.eip == loop + sizeof(loop_code),
            "the private rounds changed the scalar result");

    prepare(true);
    require(cpu.try_ether_arm64_block(loop_steps, &lock) == 16 && cpu.regs[Cpu::ECX] == rounds - 1 &&
            cpu.eip == loop && lock.owns_lock(), "the worker must return for IRQ polling with IF set");

    prepare(true);
    require(cpu.ether_arm64_register_loop_length() == 16 &&
            cpu.try_ether_arm64_register_loop(loop_steps, lock) == loop_steps && lock.owns_lock() &&
            cpu.regs == native_regs && cpu.eflags == (native_flags | Cpu::IF) &&
            cpu.eip == loop + sizeof(loop_code), "a motherboard-bounded register dispatch retires the whole budget");

    constexpr unsigned long_rounds = 8193, long_steps = long_rounds * 16;
    Cpu::EtherRegisterWork work;
    prepare(false);
    cpu.regs[Cpu::ECX] = long_rounds;
    require(cpu.prepare_ether_arm64_register_loop(long_steps, work), "prepare a continuous register loop");
    work.run(Cpu::EtherRegisterWork::Clock::now());
    require(cpu.retire_ether_arm64_register_loop(work) == 0 && cpu.regs[Cpu::ECX] == long_rounds,
            "an expired deadline retires nothing");
    require(cpu.prepare_ether_arm64_register_loop(long_steps, work), "prepare again after a deadline");
    work.run(Cpu::EtherRegisterWork::Clock::now() + std::chrono::seconds(5));
    require(cpu.retire_ether_arm64_register_loop(work) == long_steps && cpu.regs[Cpu::ECX] == 0 &&
            cpu.regs[Cpu::EAX] == long_rounds * 7 && cpu.regs[Cpu::EDX] == long_rounds * 12 &&
            cpu.regs[Cpu::ESI] == long_rounds && cpu.eip == loop + sizeof(loop_code) &&
            cpu.eflags == native_flags, "the worker finishes a long private loop with exact x86 state");
    require(work.instruction_at(16).ip == loop && work.instruction_at(16).bytes[0] == 0x48,
            "the compiled loop supplies exact hotspot sample addresses");

    prepare(false);
    constexpr unsigned shared_data = 0x9400;
    cpu.regs[Cpu::EAX] = shared_data;
    cpu.write64(shared_data, 42);
    require(cpu.prepare_ether_arm64_register_loop(32, work), "capture the guarded loop before a write");
    cpu.write8(loop + 2, 0x00); // Change the ADD destination from RAX to shared RAM at [RAX].
    const auto retired_before_edit = cpu.instructions;
    work.run();
    require(cpu.retire_ether_arm64_register_loop(work) == 0 &&
            !cpu.prepare_ether_arm64_register_loop(32, work) && lock.owns_lock() &&
            cpu.instructions == retired_before_edit && cpu.read64(shared_data) == 42,
            "a rewritten private loop cannot enter the parallel path");
    require(cpu.step() == cosmic_pc::StepResult::Executed && cpu.read64(shared_data) == 49,
            "the rewritten shared instruction remains executable on the scalar path");
    std::cout << "SMP_ETHER_PRIVATE_LOOP_PASS rounds=256 steps=4096 irq_enabled_steps=16\n";
}

void install_long_mode_smp_guest(Cpu& cpu, const std::uint8_t* guest, std::size_t bytes) {
    // APs enter long mode through the guest GDT and the BSP's page tables.
    constexpr std::uint8_t trampoline[] = {
        0xfa,0x31,0xc0,0x8e,0xd8,0x66,0x0f,0x01,0x16,0x00,0x51, // CLI; DS=0; LGDT [5100h].
        0x0f,0x20,0xc0,0x0c,0x01,0x0f,0x22,0xc0,0x66,0xea,0x1b,0x80,0,0,8,0,
        0x66,0xb8,0x10,0,0x8e,0xd8,0x8e,0xd0,                    // Load DS and SS.
        0xb8,0x20,0,0,0,0x0f,0x22,0xe0,0xb8,0,0x10,0,0,0x0f,0x22,0xd8, // CR4.PAE; CR3=1000h.
        0xb9,0x80,0,0,0xc0,0x0f,0x32,0x0d,0,1,0,0,0x0f,0x30,     // EFER.LME.
        0x0f,0x20,0xc0,0x0d,0,0,1,0x80,0x0f,0x22,0xc0,0xea,0,0x81,0,0,0x18,0,
    };
    cpu.write64(0x5008, 0x00cf9a000000ffffULL);
    cpu.write64(0x5010, 0x00cf92000000ffffULL);
    cpu.write64(0x5018, 0x00af9a000000ffffULL);
    cpu.write16(0x5100, 31);
    cpu.write32(0x5102, 0x5000);
    cpu.memory.write_block(kStartupVector << 12, trampoline, sizeof(trampoline));
    cpu.memory.write_block(kSmpEntry, guest, bytes);
    cpu.write32(kStartFlag, 0);
}

void start_long_mode_smp_guest(Cpu& cpu) {
    cpu.eip = kSmpEntry;
    cpu.eflags = 2;
    cpu.halted = false;
    cpu.write32(kLapic + 0x300, 0x000cc500);
    cpu.write32(kLapic + 0x300, 0x000c8500);
    cpu.write32(kLapic + 0x300, 0x000c0600 | kStartupVector);
}

// Run the public board scheduler with one application processor. A fixed IPI
// lets both versions see exactly one request independent of the host's speed.
void measure_secondary_irq_work(bool copy, bool pending) {
    constexpr unsigned vector = 0xd1, handler = 0x18000, idt = 0x15000;
    constexpr unsigned source = 0x100000, destination = 0x150000, bytes = 0x20000;
    constexpr unsigned iterations = 65536;
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault, "long-mode SMP IRQ fixture");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1}, true, {},
                           PcMotherboardC06::CpuClock::Experimental4_5THz);
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    board.enable_active_clock();
    board.enable_threaded_smp(2);
    board.enable_execution_target();
    cpu.write64(0x2000 + 3 * 8, 0x6003);
    cpu.write64(0x6000 + ((kLapic >> 21) & 511) * 8, kLapic | 0x83);
    cpu.write16(0x5180, 4095);
    cpu.write64(0x5182, idt);
    cpu.write64(idt + 16 * vector, (handler & 0xffffU) | (0x18ULL << 16) |
                                    (0x8eULL << 40) | (std::uint64_t(handler >> 16) << 48));
    cpu.write64(idt + 16 * vector + 8, 0);
    // The handler records the stage it interrupted, then issues EOI and IRETQ.
    constexpr std::uint8_t irq[] = {
        0x50, 0x8b,0x04,0x25,0,0x40,1,0, 0x89,0x04,0x25,4,0x40,1,0,
        0xc7,0x83,0xb0,0,0,0,0,0,0,0, 0x58,0x48,0xcf
    };
    cpu.memory.write_block(handler, irq, sizeof(irq));
    std::vector<std::uint8_t> program{
        0xbc,0,0,0x1f,0,                         // MOV ESP,1F0000h.
        0x0f,0x01,0x1c,0x25,0x80,0x51,0,0,       // LIDT [5180h].
        0xbb,0,0,0xe0,0xfe,                      // EBX = LAPIC MMIO base.
        0xc7,0x83,0xf0,0,0,0,0xff,1,0,0,        // Enable the LAPIC.
        0xc7,0x83,0x80,0,0,0,0xf0,0,0,0         // TPR = F0h masks D1h.
    };
    const auto emit = [&](std::initializer_list<std::uint8_t> code) {
        program.insert(program.end(), code);
    };
    if (pending) emit({0xc7,0x83,0,3,0,0,0xd1,0,4,0});  // Send D1h to self.
    emit({0xfb,0x90,0x31,0xd2});                           // STI; NOP; XOR EDX,EDX.
    std::uint32_t expected = 0;
    std::vector<std::uint8_t> pattern;
    if (copy) {
        emit({0xbe,0,0,0x10,0, 0xbf,0,0,0x15,0, 0xb9,0,0,2,0, 0xf3,0xa4});
        pattern.resize(bytes);
        for (unsigned i = 0; i < bytes; ++i) pattern[i] = std::uint8_t(i ^ (i >> 8));
        cpu.memory.write_block(source, pattern.data(), bytes);
        cpu.write8(destination - 1, 0xa6);
        cpu.write8(destination + bytes, 0x5b);
    } else {
        emit({0xb9,0,0,1,0,                               // ECX = 65536.
              0x01,0xca, 0x81,0xf2,0xdf,0x9b,0x57,0x13,  // ADD EDX,ECX; XOR EDX,13579BDFh.
              0xc1,0xc2,5, 0xff,0xc9, 0x75,0xf1});       // ROL EDX,5; DEC ECX; JNZ.
        for (unsigned n = iterations; n; --n) {
            expected = (expected + n) ^ 0x13579bdfU;
            expected = (expected << 5) | (expected >> 27);
        }
    }
    emit({0x9c,0x58, 0x89,0x04,0x25,8,0x40,1,0,       // Save flags, result and final ECX.
          0x89,0x14,0x25,12,0x40,1,0, 0x89,0x0c,0x25,16,0x40,1,0,
          0xc7,0x04,0x25,0,0x40,1,0,1,0,0,0,         // Stage = 1.
          0xc7,0x83,0x80,0,0,0,0,0,0,0,              // Unmask D1h.
          0xc7,0x04,0x25,0,0x40,1,0,2,0,0,0, 0xfa,0xf4}); // Stage = 2; CLI; HLT.
    install_long_mode_smp_guest(cpu, program.data(), program.size());
    start_long_mode_smp_guest(cpu);
    cpu.eip = 0x7000;
    cpu.write8(0x7000, 0xf4); // Halt just the BSP.
    const auto begin = PcMachine::RunClock::now();
    const auto deadline = begin + std::chrono::seconds(15);
    while (board.secondary_can_run() && PcMachine::RunClock::now() < deadline &&
           !board.faulted_cpu() && !cpu.fault) {
        board.run(8192, std::min(deadline, PcMachine::RunClock::now() + std::chrono::milliseconds(8)));
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        PcMachine::RunClock::now() - begin).count();
    if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
    const auto flags = cpu.read32(0x14008);
    require(!cpu.fault && board.processors_started() == 2 && !board.secondary_can_run() &&
            board.threaded_smp_stats().dispatches && cpu.read32(0x14000) == 2 &&
            cpu.read32(0x14004) == unsigned(pending) && board.evidence().apic_irqs == unsigned(pending) &&
            cpu.read32(0x1400c) == expected && cpu.read32(0x14010) == 0 &&
            (flags & (Cpu::IF | Cpu::ZF)) == (Cpu::IF | Cpu::ZF),
            "AP arithmetic, retirement boundary or deferred IPI changed");
    auto checksum = expected;
    if (copy) {
        std::vector<std::uint8_t> result(bytes);
        cpu.memory.read_block(destination, result.data(), bytes);
        require(result == pattern && cpu.read8(destination - 1) == 0xa6 &&
                cpu.read8(destination + bytes) == 0x5b, "REP changed guest bytes");
        checksum = 2166136261U;
        for (const auto byte : result) checksum = (checksum ^ byte) * 16777619U;
    }
    const auto native = board.processor_ether_arm64_steps(1);
    std::uint64_t rep = 0;
#ifndef PRIME_R101_BASELINE
    rep = board.secondary_rep_elements();
    require(board.lapic_vector_stats()[vector].eois == unsigned(pending) &&
            board.rtc_evidence().irq8_delivered == 0, "self IPI attribution");
    if (copy) require(rep > bytes / 2 && rep == board.secondary_rep_bytes(), "inactive AP REP route");
    if (!copy && Cpu::ether_arm64_host())
        require(native > iterations * 4, "inactive AP ARM64 route with IF set");
#endif
    std::cout << "SMP_IRQ_SAMPLE work=" << (copy ? "rep" : "registers") << " pending=" << pending
              << " wall_ns=" << ns << " steps=" << board.processor_steps(1) << " native=" << native
              << " rep=" << rep << " checksum=" << checksum << " flags=" << flags
              << " deliveries=" << board.evidence().apic_irqs << '\n';
}

void test_secondary_streaming_stores(bool threaded) {
    constexpr unsigned source = 0xa000, destination = 0xb000, rounds = 16;
    // Enable OSFXSR, load two distinct XMM values, then write eight consecutive
    // 16-byte values per iteration. Only the secondary CPU runs this code.
    constexpr std::uint8_t guest[] = {
        0x0f,0x20,0xe0, 0x0d,0x00,0x02,0,0, 0x0f,0x22,0xe0,
        0xbe,0,0xa0,0,0, 0xbf,0,0xb0,0,0, 0xb9,rounds,0,0,0,
        0xf3,0x0f,0x6f,0x06, 0xf3,0x0f,0x6f,0x4e,0x10,
        0x66,0x0f,0xe7,0x07,      0x66,0x0f,0xe7,0x4f,0x10,
        0x66,0x0f,0xe7,0x47,0x20, 0x66,0x0f,0xe7,0x4f,0x30,
        0x66,0x0f,0xe7,0x47,0x40, 0x66,0x0f,0xe7,0x4f,0x50,
        0x66,0x0f,0xe7,0x47,0x60, 0x66,0x0f,0xe7,0x4f,0x70,
        0x81,0xc7,0x80,0,0,0, 0xff,0xc9, 0x75,0xcf, 0xf4,
    };
    std::uint64_t scalar_steps = 0;
    for (bool accelerated : {false, true}) {
        PcMachine pc(4U << 20);
        pc.boot_long_mode_demo();
        pc.run(4000);
        auto& cpu = pc.cpu;
        PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2,1});
        if (threaded) board.enable_threaded_smp(2);
        if (accelerated) pc.enable_boot_acceleration();
        install_long_mode_smp_guest(cpu, guest, sizeof(guest));
        for (unsigned i = 0; i < 32; ++i) cpu.write8(source + i, std::uint8_t(0xa0 + i));
        cpu.write8(destination - 1, 0x71);
        cpu.write8(destination + 128 * rounds, 0x72);
        start_long_mode_smp_guest(cpu);
        cpu.write16(0x7000, 0xfeeb); // Park the BSP in a JMP to itself.
        cpu.eip = 0x7000;
        for (unsigned turn = 0; turn < 64 && board.secondary_can_run(); ++turn) {
            const unsigned budget = turn % 3 ? 128 : 7;
            const auto previous = board.total_processor_steps();
            board.run(budget);
            require(board.total_processor_steps() - previous <= budget, "AP streaming stores exceeded the grant");
        }
        require(board.processors_started() == 2 && !board.secondary_can_run() &&
                !board.faulted_cpu() && !cpu.fault, "secondary XMM stream must halt successfully");
        for (unsigned i = 0; i < 128 * rounds; ++i)
            require(cpu.read8(destination + i) == 0xa0 + i % 32, "AP XMM bytes diverged");
        require(cpu.read8(destination - 1) == 0x71 && cpu.read8(destination + 128 * rounds) == 0x72,
                "secondary XMM stream overwrote its boundaries");
        const auto batches = board.secondary_store_batches(), steps = board.secondary_store_steps();
        if (accelerated) {
            require(steps > 0 && steps >= 2 * batches && steps <= 8 * batches &&
                    board.processor_steps(1) == scalar_steps, "secondary stores must coalesce with exact retirement");
            std::cout << "SMP_MOVNTDQ_PASS threaded=" << threaded << " stores=" << steps
                      << " batches=" << batches << " guest_steps=" << scalar_steps << '\n';
        } else {
            require(!batches && !steps, "scalar secondary must skip XMM batches");
            scalar_steps = board.processor_steps(1);
        }
    }
}

void test_signed_projection_with_smp_quantum(bool threaded) {
    if (!Cpu::ether_arm64_host()) return;
    constexpr unsigned loop = 0x7000;
    constexpr unsigned quantum = 32;
    constexpr std::uint8_t spin[] = {0xeb,0xfe}; // AP stays runnable.
    // The four instructions observed together in the Windows ISO trace.
    constexpr std::uint8_t code[] = {
        0x48,0x2b,0xc2, 0x48,0xff,0xc2, 0x48,0x3b,0xc2, 0x7f,0xf5
    };
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault, "signed SMP loop fixture must boot in long mode");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1});
    if (threaded) board.enable_threaded_smp();
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    install_long_mode_smp_guest(cpu, spin, sizeof(spin));
    start_long_mode_smp_guest(cpu);
    board.run(256);
    require(board.processors_started() == 2 && !board.faulted_cpu(), "signed SMP loop fixture must start its AP");
    cpu.memory.write_block(loop, code, sizeof(code));
    const auto reset_loop = [&] {
        cpu.eip = loop;
        cpu.eflags = 2 | Cpu::IF;
        cpu.regs[Cpu::EAX] = 100000;
        cpu.regs[Cpu::EDX] = 7;
    };
    for (unsigned attempt = 0; attempt < 4 && !cpu.ether_arm64_register_loop_length(); ++attempt) {
        reset_loop();
        cpu.try_ether_arm64_block(4);
    }
    require(cpu.ether_arm64_register_loop_length() == 4, "signed SMP loop must be ready in the ARM64 cache");
    reset_loop();
    const auto bsp_start = board.processor_steps(0);
    const auto ap_start = board.processor_steps(1);
    const auto projected_start = board.processor_state_vector_steps(0);
    const auto off_bus_start = cpu.ether_arm64_stats().off_bus_register_steps;
    board.run(2 * quantum);
    const auto projected = board.processor_state_vector_steps(0) - projected_start;
    require(!cpu.fault && !board.faulted_cpu() && cpu.flag(Cpu::IF) && cpu.eip == loop &&
                cpu.regs[Cpu::EAX] == 99916 && cpu.regs[Cpu::EDX] == 15,
            "projecting the short SMP loop changed the guest state");
    require(board.processor_steps(0) - bsp_start == quantum &&
                board.processor_steps(1) - ap_start == quantum &&
                board.cpu_execution_totals().projected_steps == projected,
            "both CPUs must receive exactly one quantum");
    if (threaded) {
        const auto off_bus = cpu.ether_arm64_stats().off_bus_register_steps - off_bus_start;
        require(board.worker_count() == 2 && board.threaded_smp_stats().peak_workers == 2 &&
                    off_bus == quantum && !projected,
                "the BSP must run native register blocks outside the bus while the scalar AP has a worker");
        std::cout << "SMP_THREADED_ARM64_PASS workers=2 off_bus_steps=" << off_bus << '\n';
    } else {
        require(board.worker_count() == 1 && projected == 28,
                "the serial 32-step SMP grant must project the signed loop");
        std::cout << "SMP_SIGNED_PROJECTION_PASS quantum=" << quantum << " projected=" << projected << '\n';
    }
}

void test_secondary_interpreter_projection() {
    if (Cpu::ether_arm64_host()) return;
    constexpr std::uint8_t guest[] = {
        0xb8,0x3f,0x15,0,0, 0xba,7,0,0,0,                  // MOV EAX,5439; MOV EDX,7.
        0x48,0x2b,0xc2, 0x48,0xff,0xc2, 0x48,0x3b,0xc2, 0x7f,0xf5, // SUB; INC; CMP; JG.
        0x89,0x04,0x25,0x00,0x44,0,0, 0xf4                  // Store the result at kStartFlag; HLT.
    };
    const auto run = [&](bool accelerate) {
        PcMachine pc(4U << 20);
        pc.boot_long_mode_demo();
        pc.run(4000);
        auto& cpu = pc.cpu;
        require(cpu.is_long_mode() && cpu.halted && !cpu.fault, "AP must start with the BSP's long-mode tables");
        PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1});
        pc.enable_boot_acceleration(accelerate);
        install_long_mode_smp_guest(cpu, guest, sizeof(guest));
        start_long_mode_smp_guest(cpu);
        cpu.eip = 0x7000;
        cpu.write16(0x7000, 0xfeeb); // Keep the BSP runnable without modifying the AP result.
        board.run(2048);
        const auto projected = board.processor_state_vector_steps(1);
        const auto vector_steps = board.processor_state_vector_interpreter_steps(1);
        require(board.processors_started() == 2 && !board.faulted_cpu() &&
                    cpu.read32(kStartFlag) == 104 &&
                    (projected != 0) == accelerate && (vector_steps != 0) == accelerate &&
                    board.cpu_execution_totals().state_vector_interpreter_steps == vector_steps &&
                    !board.processor_ether_arm64_steps(1),
                "the secondary vector interpreter changed the result or attribution");
        return board.processor_steps(1);
    };
    require(run(true) == run(false), "projected AP should retire the same guest instructions");
    std::cout << "SMP_STATE_VECTOR_INTERPRETER_PASS projected_ap=1 exact_steps=1\n";
}

void test_mixed_parallel_and_shared_writes(unsigned writer, bool read_ram, bool opt_in) {
    if (!Cpu::ether_arm64_host()) return;
    const std::uint8_t readers[] = {
        0xb8,1,0,0,0,0x0f,0xa2,0xc1,0xeb,0x18,                 // CPUID.1 gives the APIC ID.
        0x83,0xfb,static_cast<std::uint8_t>(writer),0x74,0x21,   // One CPU uses the shared counter.
        0xb9,0,0xca,0x9a,0x3b,0x31,0xc0,0xbe,0,0,0x10,0,       // ECX=1e9, EAX=0, ESI=1 MiB.
        0x48,0x8b,0x16,0x48,0x01,0xd0,0x83,0xc6,8,             // Load and sum, then advance ESI.
        0x81,0xe6,0xff,0xff,0x13,0,0x83,0xe9,1,0x75,0xec,0xf4, // Wrap the pointer and repeat.
        0xf0,0xff,0x04,0x25,0,0x32,0,0,0xeb,0xf6,             // LOCK INC [3200h] in a loop.
    };
    const std::uint8_t registers[] = {
        0xb8,1,0,0,0,0x0f,0xa2,0xc1,0xeb,0x18,                 // CPUID.1; read APIC ID.
        0x83,0xfb,static_cast<std::uint8_t>(writer),0x74,0x11,   // One CPU uses the shared counter.
        0xb9,0,0xca,0x9a,0x3b,0x31,0xc0,                       // ECX=1e9; EAX=0.
        0x48,0x83,0xc0,7,0x83,0xe9,1,0x75,0xf7,0xf4,          // ADD; SUB; JNZ; HLT.
        0xf0,0xff,0x04,0x25,0,0x32,0,0,0xeb,0xf6,             // LOCK INC [3200h]; JMP.
    };
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault && cpu.cr3 == 0x1000,
            "mixed SMP fixture must start in long mode");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {3, 1}, opt_in);
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    if (read_ram) {
        install_long_mode_smp_guest(cpu, readers, sizeof(readers));
        std::vector<std::uint64_t> values(32768);
        for (unsigned i = 0; i < values.size(); ++i) values[i] = i + 1;
        cpu.memory.write_block(1U << 20, reinterpret_cast<const std::uint8_t*>(values.data()),
                               values.size() * sizeof(values[0]));
    } else {
        install_long_mode_smp_guest(cpu, registers, sizeof(registers));
    }
    start_long_mode_smp_guest(cpu);
    board.run(2048);
    require(board.processors_started() == 3 && !board.faulted_cpu() && !cpu.fault,
            "both worker candidates and the shared writer must start");

    constexpr unsigned budget = 3 * 1024;
    const auto retired_before = board.total_processor_steps();
    const auto boot_before = board.processor_steps(0);
    const auto ticks_before = board.evidence().ticks;
    const auto count_before = cpu.read32(kSharedCounter);
    const auto batches_before = board.parallel_mixed_batches();
    board.run(budget, PcMachine::RunClock::now() - std::chrono::milliseconds(1));
    require(board.total_processor_steps() == retired_before && board.evidence().ticks == ticks_before,
            "an expired mixed dispatch may not advance the guest");
    pc.waiting_key = true;
    board.run(budget);
    require(board.total_processor_steps() == retired_before && board.evidence().ticks == ticks_before,
            "a BIOS wait must pause all mixed dispatches");
    pc.waiting_key = false;
    for (unsigned turn = 0; turn < 8; ++turn) {
        const auto count = cpu.read32(kSharedCounter);
        board.run(budget);
        require(cpu.read32(kSharedCounter) > count, "each turn must serve the excluded writer");
    }
    const auto batches = board.parallel_mixed_batches() - batches_before;
    require(!board.faulted_cpu() && !cpu.fault, "the mixed workload must complete without faulting");
    if (read_ram && opt_in) {
        require(board.worker_count() == 2 && batches > 0 && board.parallel_ram_batches() > 0,
                "the shared writer must not block two read-only RAM workers");
    } else {
        require(board.worker_count() == 1 && batches == 0,
                "workers may only wake for eligible, explicitly enabled mixed batches");
    }
    require(board.total_processor_steps() - retired_before <= 8 * budget &&
                board.evidence().ticks - ticks_before == board.processor_steps(0) - boot_before,
            "mixed dispatches must stay within the global step budget and preserve the device clock");
    std::cout << "SMP_MIXED_DISPATCH_PASS writer=" << writer << " read_ram=" << read_ram << " opt_in=" << opt_in
              << " workers=" << board.worker_count() << " batches=" << batches
              << " shared_writes=" << cpu.read32(kSharedCounter) - count_before << '\n';
}

void test_parallel_register_continuations() {
    if (!Cpu::ether_arm64_host()) return;
    // Warm both backedges and the two-instruction bridge before the go
    // signal. Each vCPU publishes its private results after both loops.
    constexpr std::uint8_t guest[] = {
        0xb8,1,0,0,0,0x0f,0xa2,0xc1,0xeb,0x18,0x89,0xdf,       // CPUID; get vCPU ID.
        0xc1,0xe7,5,0x81,0xc7,0,0x45,0,0,                      // EDI = jobs + id * 32.
        0x41,0xb8,4,0,0,0,                                     // Four warm-up passes.
        0xb9,64,0,0,0,0xbe,64,0,0,0,                            // 64 rounds for each warm-up loop.
        0x31,0xc0,0x31,0xd2,                                   // Reset results before either pass.
        0x48,0x83,0xc0,7,0x83,0xe9,1,0x75,0xf7,                // ADD RAX,7; SUB ECX,1; JNZ.
        0x48,0x83,0xc0,5,0xeb,0,                               // ADD RAX,5; JMP to next loop.
        0x48,0x83,0xc2,11,0x83,0xee,1,0x75,0xf7,               // ADD RDX,11; SUB ESI,1; JNZ.
        0x83,0x3c,0x25,0,0x44,0,0,0,0x75,0x1d,                // Publish only after the measured pass.
        0x41,0xff,0xc8,0x75,0xcb,                              // Repeat the remaining warm-up passes.
        0xc7,0x47,0x18,1,0,0,0,                                // Mark all three blocks warm.
        0x83,0x3c,0x25,0,0x44,0,0,0,0x74,0xf6,                // Wait for the start flag.
        0x8b,0x0f,0x8b,0x77,4,0xeb,0xbd,                       // Load measured counts and go to the loops.
        0x48,0x89,0x47,8,0x48,0x89,0x57,16,                     // Publish both sums.
        0xc7,0x47,0x18,2,0,0,0,0xf4,                            // Mark completion, then halt.
    };
    constexpr unsigned boot_rounds = 65536, secondary_rounds = 8192, second_rounds = 4096;
    const auto first_rounds = [=](unsigned id) { return id ? secondary_rounds + 512 * id : boot_rounds; };
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault && cpu.cr3 == 0x1000,
            "BSP must provide the APs' long-mode page tables");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
    pc.enable_boot_acceleration();
    pc.enable_boot_hotspots(true, false);
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    install_long_mode_smp_guest(cpu, guest, sizeof(guest));
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto job = kJobs + id * kJobBytes;
        cpu.write32(job, first_rounds(id));
        cpu.write32(job + 4, second_rounds);
        cpu.write32(job + 24, 0);
    }
    start_long_mode_smp_guest(cpu);
    const auto run_until = [&](unsigned state, unsigned budget) {
        const auto reached = [&] {
            for (unsigned id = 0; id < kProcessors; ++id)
                if (cpu.read32(kJobs + id * kJobBytes + 24) != state) return false;
            return true;
        };
        const auto deadline = PcMachine::RunClock::now() + std::chrono::seconds(10);
        for (unsigned attempt = 0; attempt < 64 && !reached() && PcMachine::RunClock::now() < deadline; ++attempt) {
            board.run(budget, deadline);
            if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
            if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
            require(pc.host_error.empty(), pc.host_error);
        }
        require(reached(), "the register-loop guest must reach phase " + std::to_string(state));
    };
    run_until(1, 2048);
    require(board.processors_started() == kProcessors, "all vCPUs must warm both register loops");
    const auto before = board.parallel_register_continuations();
    std::array<std::uint64_t, kProcessors> before_steps{};
    for (unsigned id = 0; id < kProcessors; ++id) before_steps[id] = board.processor_steps(id);
    cpu.write32(kStartFlag, 1);
    run_until(2, 3 * kProcessors * (boot_rounds + second_rounds) + 8192);
    const auto continuations = board.parallel_register_continuations() - before;
    require(board.worker_count() == kProcessors && continuations == 2 * kProcessors,
            "every SMP worker must advance through the register bridge and second loop inside one dispatch");
    const auto totals = board.cpu_execution_totals();
    require(totals.guest_steps == board.total_processor_steps() &&
                totals.ether_steps > board.processor_ether_arm64_steps(0) &&
                totals.projected_steps > board.processor_state_vector_steps(0) &&
                totals.projected_steps <= totals.ether_steps && totals.ether_steps <= totals.guest_steps &&
                totals.processors_with_projected_steps == kProcessors,
            "phase telemetry must count native and projected work on all vCPUs");
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto job = kJobs + id * kJobBytes;
        require(cpu.read64(job + 8) == 7ULL * first_rounds(id) + 5 &&
                    cpu.read64(job + 16) == 11ULL * second_rounds,
                "continuing independently changed the register result for vCPU " + std::to_string(id));
        const auto compute_steps = 3 * (first_rounds(id) + second_rounds) + 2;
        const auto retired = board.processor_steps(id) - before_steps[id];
        require(retired >= compute_steps && retired <= compute_steps + 16,
                "continuing independently changed the retired steps for vCPU " + std::to_string(id));
    }
    std::cout << "SMP_REGISTER_CONTINUATIONS_PASS workers=" << board.worker_count()
              << " continuations=" << continuations << '\n';
}

void test_parallel_ram_vectors(bool shared, unsigned worker_limit = kProcessors) {
    if (!Cpu::ether_arm64_host()) return;
    constexpr unsigned words = 8192, logical_bytes = 40U << 20;
    constexpr auto vector_bytes = cosmic_pc::SparseRam::kPagesPerTable * cosmic_pc::SparseRam::kPageSize;
    constexpr std::uint8_t guest[] = {
        0xb8,1,0,0,0,0x0f,0xa2,0xc1,0xeb,0x18,0x89,0xdf,         // CPUID; get the vCPU ID.
        0xc1,0xe7,5,0x81,0xc7,0,0x45,0,0,                        // EDI = jobs + id * 32.
        0x48,0x8b,0x37,0x8b,0x4f,8,0xc7,0x47,0x18,1,0,0,0,     // Load job and mark ready.
        0x83,0x3c,0x25,0,0x44,0,0,0,0x74,0xf6,0x31,0xc0,        // Wait for start_flag; zero EAX.
        0x48,0x8b,0x16,0x48,0x01,0xd0,0x48,0x83,0xc6,8,          // Read and sum one word.
        0x83,0xe9,1,0x75,0xf1,                                   // SUB ECX,1; JNZ.
        0x48,0x89,0x47,0x10,0xc7,0x47,0x18,2,0,0,0,0xf4,        // Publish sum and halt.
    };
    PcMachine pc(logical_bytes);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault && cpu.cr3 == 0x1000,
            "BSP must provide the APs' long-mode page tables");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2}, worker_limit < kProcessors);
    if (worker_limit < kProcessors) board.enable_threaded_smp(worker_limit);
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    for (unsigned i = 1; i < logical_bytes / vector_bytes; ++i)
        cpu.write64(0x3000 + i * 8, (i * vector_bytes) | 0x83U);
    install_long_mode_smp_guest(cpu, guest, sizeof(guest));
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto range = shared ? 0 : id;
        const auto address = (range + 1) * vector_bytes;
        if (!shared || !id) {
            std::vector<std::uint64_t> values(words);
            for (unsigned i = 0; i < words; ++i) values[i] = i + range;
            cpu.memory.write_block(address, reinterpret_cast<const std::uint8_t*>(values.data()),
                                   values.size() * sizeof(values[0]));
        }
        const auto job = kJobs + id * kJobBytes;
        cpu.write64(job, address);
        cpu.write32(job + 8, words);
        cpu.write32(job + 24, 0);
    }
    start_long_mode_smp_guest(cpu);
    const auto reached = [&](unsigned marker) {
        for (unsigned id = 0; id < kProcessors; ++id)
            if (cpu.read32(kJobs + id * kJobBytes + 24) != marker) return false;
        return true;
    };
    const auto check_guest = [&] {
        if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
        if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
        require(pc.host_error.empty(), pc.host_error);
    };
    for (unsigned turn = 0; turn < 64 && !reached(1); ++turn) {
        board.run(1024);
        check_guest();
    }
    require(reached(1) && board.processors_started() == kProcessors, "All readers must be ready");
    if (shared) {
        // IRQ8 latched in the legacy PIC while LAPIC LINT0 has ExtINT masked.
        // The guest can read IRR, even though this route cannot interrupt the BSP.
        cpu.write32(kLapic + 0xf0, 0x1ff);
        cpu.out_hook(0x21, 1, 0xfb);
        cpu.out_hook(0xa1, 1, 0xfe);
        cpu.out_hook(0x70, 1, 0x0b);
        cpu.out_hook(0x71, 1, 0x42);
        for (unsigned i = 0; i < 4096 && !board.rtc_evidence().irq8_assertions; ++i) board.advance_one();
        cpu.out_hook(0x71, 1, 0x02);
        cpu.set_flag(Cpu::IF, true);
        require(cpu.in_hook(0xa0, 1).value_or(0) & 1U, "IRQ8 must remain latched in PIC IRR");
    }
    const auto before = board.parallel_ram_steps();
    cpu.write32(kStartFlag, 1);
    const auto deadline = PcMachine::RunClock::now() + std::chrono::seconds(10);
    for (unsigned turn = 0; turn < 64 && !reached(2) && PcMachine::RunClock::now() < deadline; ++turn) {
        board.run(kProcessors * words * 5 + 8192, deadline);
        check_guest();
    }
    require(reached(2) && board.worker_count() == worker_limit, "Each vCPU must finish on the bounded SMP path");
    if (worker_limit < kProcessors)
        require(board.threaded_smp_stats().peak_workers == worker_limit,
                "Parallel dispatch telemetry must count lanes instead of vCPUs");
    const auto parallel = board.parallel_ram_steps() - before;
    require(parallel >= std::uint64_t(kProcessors) * words * 4 &&
                cpu.memory.metrics().page_state_vectors >= (shared ? 2U : kProcessors + 1),
            "C06 must dispatch the RAM loop through the shared vectors");
    if (shared) require(!board.evidence().irq_delivered && (cpu.in_hook(0xa0, 1).value_or(0) & 1U),
                        "inactive PIC route must preserve IRR throughout parallel execution");
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto expected = std::uint64_t(words) * (words - 1) / 2 + (shared ? 0 : id * words);
        require(cpu.read64(kJobs + id * kJobBytes + 16) == expected,
                "Parallel RAM loop changed the guest sum");
    }
    std::cout << "SMP_RAM_PASS workers=" << board.worker_count()
              << " vcpus=" << kProcessors << " shared=" << shared << " parallel_steps=" << parallel << '\n';
}
void check_cpu_clock(PcMotherboardC06::CpuClock profile, unsigned ratio) {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2}, false, {}, profile);
    auto& cpu = pc.cpu;
    cpu.reset(0, 0x7200);
    const std::uint8_t loop[]{0x43,0xeb,0xfd}; // INC BX; JMP back to INC
    cpu.memory.write_block(0x7200, loop, sizeof(loop));
    const unsigned work = 512 * ratio;
    board.run(1);
    board.run(work - 1);
    require(board.evidence().ticks == 512 && board.total_processor_steps() == work &&
            cpu.regs[Cpu::EBX] == work / 2, "CPU work per peripheral tick");
    const auto read_tsc = [&] {
        cpu.eip = 0x7300;
        cpu.write16(0x7300, 0x310f);
        require(cpu.step() == cosmic_pc::StepResult::Executed, "Guest RDTSC");
        return (cpu.regs[Cpu::EDX] << 32) | cpu.regs[Cpu::EAX];
    };
    const auto firmware_rate = std::uint64_t(PcMachine::kVirtualCyclesPerSecond) * ratio;
    const auto hz = profile == PcMotherboardC06::CpuClock::Experimental4_5THz
        ? 4'500'000'000'000ULL : firmware_rate * Cpu::kTscTicksPerCycle;
    const auto firmware_tsc = [&](std::uint64_t steps) {
        return (steps * (hz / Cpu::kTscTicksPerCycle) / firmware_rate) * Cpu::kTscTicksPerCycle;
    };
    require(read_tsc() == firmware_tsc(work), "Firmware timestamp");
    // Mix the batch and scalar paths across a device edge, then switch clock
    // sources with a partial device tick still pending.
    const auto ticks = board.evidence().ticks;
    pc.device_batch_advance_hook(3 * ratio - 1);
    require(board.evidence().ticks == ticks + 2, "A partial device tick must not fire early");
    board.advance_one();
    require(board.evidence().ticks == ticks + 3, "The scalar step must consume the batch remainder");
    if (ratio > 1) board.advance_one();
    const auto before = read_tsc();
    const auto cycles = std::uint64_t(work) + 3 * ratio + (ratio > 1 ? 1 : 0);
    require(before == firmware_tsc(cycles), "Batched firmware timestamp");
    const auto ram_before = board.ram_clock_cycles();
    require(ram_before == cycles * 4'500'000'000'000ULL / firmware_rate,
            "Firmware RAM clock must retain a partial peripheral tick across batches");
    const auto origin = PcMachine::RunClock::time_point{};
    unsigned samples = 0;
    board.enable_active_clock([&] {
        return origin + std::chrono::milliseconds(samples++ ? 1000 : 0);
    });
    cpu.eip = 0x7200;
    board.run(64);
    require(board.active_clock_ticks() == PcMachine::kVirtualCyclesPerSecond &&
            read_tsc() - before == hz && board.guest_tsc_hz() == hz,
            "A guest second must preserve the ACPI oscillator and the requested TSC rate");
    require(board.ram_clock_cycles() - ram_before == 4'500'000'000'000ULL,
            "One active second must give 4.5 trillion RAM cycles in every CPU profile");
    std::cout << "PRIME_CPU_CLOCK_PASS guest_tsc_hz=" << hz << " peripheral_ticks=512 retired=" << work << '\n';
}

void check_all_smp_timestamps() {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8,2}, true, {},
                           PcMotherboardC06::CpuClock::Twentyfold);
    board.enable_threaded_smp(8);
    auto& cpu = pc.cpu;
    cpu.reset(0,0x7200);
    cpu.a20_enabled = true;
    const std::uint8_t guest[]{
        0x66,0xb8,0x0b,0,0,0, 0x66,0x31,0xc9, 0x0f,0xa2, // CPUID: EDX = APIC ID
        0x89,0xd7,0xc1,0xe7,3,                         // DI = APIC ID * 8
        0x80,0x3e,0,0x44,0, 0x74,0xf9,                  // wait for the common sample gate
        0x0f,0x31,                                     // RDTSC
        0x66,0x89,0x85,0,0x36, 0x66,0x89,0x95,4,0x36, // store both halves
        0xeb,0xfe};
    cpu.memory.write_block(0x7200, guest, sizeof(guest));
    cpu.memory.write_block(kStartupVector << 12, guest, sizeof(guest));
    bool sampling = false;
    unsigned samples = 0;
    board.enable_active_clock([&] {
        const bool advanced = sampling && ++samples >= 2;
        return PcMachine::RunClock::time_point{} + std::chrono::seconds(advanced ? 1 : 0);
    });
    cpu.write32(kLapic + 0x300, 0x000cc500);
    cpu.write32(kLapic + 0x300, 0x000c8500);
    cpu.write32(kLapic + 0x300, 0x000c0600 | kStartupVector);
    board.run(8192);
    require(board.processors_started() == kProcessors, "All vCPUs must start");
    cpu.write8(kStartFlag, 1);
    sampling = true;
    board.run(8192);
    for (unsigned id = 0; id < kProcessors; ++id)
        require(cpu.read64(0x3600 + id * 8) == 4'581'817'600ULL,
                "64-bit timestamp on vCPU " + std::to_string(id));
    require(board.active_clock_ticks() == PcMachine::kVirtualCyclesPerSecond, "Peripheral oscillator rate");
    std::cout << "PRIME_SMP_TSC_PASS vcpus=" << kProcessors << " guest_hz=" << board.guest_tsc_hz() << '\n';
}

void check_protected32_timestamps(PcMotherboardC06::CpuClock profile, std::uint64_t expected_hz) {
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8,2}, true, {}, profile);
    board.enable_threaded_smp(8);
    auto& cpu = pc.cpu;
    cpu.reset(0,0x7200);
    cpu.a20_enabled = true;
    constexpr std::uint8_t wait_for_aps[]{0x80,0x3e,0x01,0x44,0, 0x74,0xf9};
    cpu.memory.write_block(0x7200,wait_for_aps,sizeof(wait_for_aps));
    install_protected32_entry(cpu, 0x7200 + sizeof(wait_for_aps));
    install_protected32_entry(cpu, kStartupVector << 12);
    constexpr std::uint8_t guest[]{
        0x66,0xb8,0x10,0, 0x8e,0xd8,                      // Load flat DS.
        0xb8,0x0b,0,0,0, 0x31,0xc9, 0x0f,0xa2,            // CPUID.0B.0: EDX = APIC ID.
        0x89,0xd7, 0xc1,0xe7,4,                          // EDI = APIC ID * 16.
        0x0f,0x31, 0x89,0x87,0,0x36,0,0, 0x89,0x97,4,0x36,0,0,
        0x80,0x3d,0,0x44,0,0,0, 0x74,0xf7,               // Wait for the second sample.
        0x0f,0x31, 0x89,0x87,8,0x36,0,0, 0x89,0x97,12,0x36,0,0,
        0xeb,0xfe};
    cpu.memory.write_block(0x9020,guest,sizeof(guest));

    PcMachine::RunClock::time_point now{};
    bool measure = false;
    unsigned samples = 0;
    board.enable_active_clock([&] {
        if (measure && ++samples == 2) now += std::chrono::seconds(1);
        return now;
    }, PcMotherboardC06::ClockStart::Guest32Or64);
    cpu.write32(kLapic + 0x300,0x000cc500);
    cpu.write32(kLapic + 0x300,0x000c8500);
    cpu.write32(kLapic + 0x300,0x000c0600 | kStartupVector);
    board.run(8192);
    require(board.processors_started() == kProcessors && cpu.is_real_mode() && board.active_clock_enabled(),
            "A 32-bit AP must start the clock while the BSP is still in real mode");
    cpu.write8(kStartFlag + 1,1);
    board.run(8192);
    require(std::string(cpu.mode_name()) == "protected32", "BSP must join the 32-bit guest");

    now += std::chrono::seconds(10);  // Time between run calls is excluded.
    cpu.write8(kStartFlag,1);
    measure = true;
    board.run(8192);
    const auto initial = cpu.read64(0x3600);
    for (unsigned id = 0; id < kProcessors; ++id) {
        const auto record = 0x3600 + id * 16;
        require(cpu.read64(record) == initial && cpu.read64(record + 8) - initial == expected_hz,
                "Protected32 RDTSC delta on vCPU " + std::to_string(id));
    }
    require(board.active_clock_elapsed_ns() == 1'000'000'000 && board.guest_tsc_hz() == expected_hz &&
            board.active_clock_ticks() == PcMachine::kVirtualCyclesPerSecond, "Shared active second");
    std::cout << "PRIME_PROTECTED32_TSC_PASS vcpus=" << kProcessors << " active_ns=" << board.active_clock_elapsed_ns()
              << " rdtsc_delta=" << cpu.read64(0x3608) - initial << '\n';
}

void check_live_x64_clock_switch() {
    using namespace std::chrono;
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8,2}, true, {},
                           PcMotherboardC06::CpuClock::Experimental4_5THz);
    PcMachine::RunClock::time_point now{};
    nanoseconds next_interval{};
    board.enable_active_clock([&] {
        const auto sample = now;
        now += next_interval;
        next_interval = nanoseconds::zero();
        return sample;
    }, PcMotherboardC06::ClockStart::Guest32Or64);
    board.run(8192);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && !cpu.fault && board.active_clock_enabled(), "Long64 clock guest boot");
    const std::uint8_t probe[]{0x0f,0x31,0xeb,0xfc};  // RDTSC; repeat.
    cpu.memory.write_block(0x7200, probe, sizeof(probe));
    cpu.halted = false;
    cpu.eip = 0x7200;
    const auto read_after = [&](nanoseconds active) {
        next_interval = active;
        board.run(64);
        require(!cpu.fault && cpu.is_long_mode(), "Long64 timestamp loop");
        return (cpu.regs[Cpu::EDX] << 32) | cpu.regs[Cpu::EAX];
    };
    const auto first = read_after(nanoseconds::zero());
    const auto ram_first = board.ram_clock_cycles();
    auto latest = first;
    for (unsigned i = 0; i < 16; ++i) latest = read_after(nanoseconds(1));
    require(latest - first == 72'000, "Accumulate fractional cycles before a peripheral tick");
    require(board.ram_clock_cycles() - ram_first == 72'000, "RAM clock advances below one peripheral tick");
    board.set_experimental_tsc(false);
    now += hours(1);  // Elapsed outside the VM.
    require(read_after(nanoseconds::zero()) == latest, "Switch down without a timestamp jump");
    const auto ram_resumed = board.ram_clock_cycles();
    require(ram_resumed == ram_first + 72'000, "A paused hour must not advance RAM time");
    const auto reference = read_after(seconds(1));
    require(reference - latest == 4'581'817'600ULL, "Reference rate in the same long64 session");
    require(board.ram_clock_cycles() - ram_resumed == 4'500'000'000'000ULL,
            "Changing the live TSC rate must leave the RAM oscillator independent");
    board.set_experimental_tsc(true);
    require(read_after(nanoseconds::zero()) == reference, "Switch up without a timestamp jump");
    const auto fast = read_after(seconds(1));
    require(fast - reference == 4'500'000'000'000ULL, "Long64 RDTSC at 4.5 THz");
    require(read_after(seconds(30)) - fast == 30 * 4'500'000'000'000ULL,
            "Long active sample must not overflow intermediate arithmetic");
    require(board.active_clock_ticks() == std::uint64_t(PcMachine::kVirtualCyclesPerSecond) * 32,
            "Only the TSC changes frequency");
    std::cout << "PRIME_LIVE_X64_TSC_PASS reference_hz=" << reference - latest
              << " experimental_hz=" << fast - reference << " clock_continuous=1\n";
}

// Complete identical arithmetic on all 16 vCPUs. Count only the loop body,
// excluding setup, BSP polling and virtual timer ticks from the reported rate.
void measure_smp_clock(PcMotherboardC06::CpuClock profile) {
    constexpr unsigned iterations = 4096, result_address = 0x3600;
    constexpr std::uint16_t checksum = (std::uint64_t(iterations) * (iterations + 1) / 2) & 0xffff;
    PcMachine pc(4U << 20);
    pc.boot_demo();
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2}, true, {}, profile);
    board.enable_threaded_smp(8);
    auto& cpu = pc.cpu;
    cpu.reset(0,0x7200);
    cpu.a20_enabled = true;
    const std::uint8_t code[]{
        0x66,0xb8,0x0b,0,0,0, 0x66,0x31,0xc9, 0x0f,0xa2, // CPUID gives this vCPU's APIC ID
        0x89,0xd6,0xd1,0xe6, 0xb9,0,0x10, 0x31,0xdb,   // SI = ID*2; CX = 4096; BX = 0
        0x01,0xcb,0xe2,0xfc,                             // ADD BX,CX; LOOP
        0x89,0x9c,0,0x36, 0xfa,0xf4                     // publish checksum; CLI; HLT
    };
    cpu.memory.write_block(0x8000,code,sizeof(code));
    cpu.memory.write_block(0x7200,code,sizeof(code));
    cpu.write16(0x7200 + sizeof(code) - 2,0xfeeb); // BSP continues until all APs publish.
    cpu.write32(kLapic+0x300,0x000cc500);
    cpu.write32(kLapic+0x300,0x000c8500);
    cpu.write32(kLapic+0x300,0x000c0600 | kStartupVector);
    const auto start = PcMachine::RunClock::now();
    for (unsigned turn = 0; turn < 96; ++turn) {
        board.run(8192);
        bool complete = true;
        for (unsigned id = 0; id < kProcessors; ++id)
            complete &= cpu.read16(result_address+id*2) == checksum;
        if (complete) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(PcMachine::RunClock::now()-start).count();
            const auto work = std::uint64_t(kProcessors)*iterations*2;
            require(!board.faulted_cpu() && board.processors_started() == kProcessors, "All CPU checksums");
            std::cout << "PRIME_SMP_CLOCK_BENCH guest_tsc_hz=" << board.guest_tsc_hz()
                      << " vcpus=" << kProcessors << " useful_instructions=" << work
                      << " elapsed_ns=" << ns << " instructions_per_second=" << work*1000000000ULL/ns << '\n';
            return;
        }
    }
    throw std::runtime_error("The SMP CPU clock benchmark did not complete on every vCPU");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--measure-irq-work") {
            measure_secondary_irq_work(false, false);
            measure_secondary_irq_work(false, true);
            measure_secondary_irq_work(true, true);
            return 0;
        }
        using Profile = PcMotherboardC06::CpuClock;
        if (argc == 2 && std::string(argv[1]) == "--measure-idle-aps") {
            test_bsp_irq_during_ap_rep();
            measure_sleeping_siblings(false);
            measure_sleeping_siblings(true);
            return 0;
        }
        test_bsp_irq_during_ap_rep();
        measure_sleeping_siblings(false);
        measure_sleeping_siblings(true);
        check_cpu_clock(Profile::Standard, 1);
        check_cpu_clock(Profile::Double, 2);
        check_cpu_clock(Profile::Twentyfold, 20);
        check_cpu_clock(Profile::Experimental4_5THz, 20);
        check_all_smp_timestamps();
        check_protected32_timestamps(Profile::Double, 458'181'760ULL);
        check_protected32_timestamps(Profile::Twentyfold, 4'581'817'600ULL);
        check_protected32_timestamps(Profile::Experimental4_5THz, 4'500'000'000'000ULL);
        check_live_x64_clock_switch();
        if (argc == 2 && std::string(argv[1]) == "--measure-cpu-clock") {
            for (unsigned trial = 0; trial < 4; ++trial) {
                measure_smp_clock(trial % 2 ? Profile::Twentyfold : Profile::Double);
                measure_smp_clock(trial % 2 ? Profile::Double : Profile::Twentyfold);
            }
            return 0;
        }
        test_eight_cores_sixteen_threads(false);
        test_eight_cores_sixteen_threads(true);
        test_eight_cores_sixteen_threads(true, 8);
        test_secondary_interrupts(false);
        test_secondary_interrupts(true);
        test_ether_worker_block();
        test_secondary_streaming_stores(false);
        test_secondary_streaming_stores(true);
        test_signed_projection_with_smp_quantum(false);
        test_signed_projection_with_smp_quantum(true);
        test_secondary_interpreter_projection();
        test_mixed_parallel_and_shared_writes(2, true, true);
        test_mixed_parallel_and_shared_writes(0, true, true);
        test_mixed_parallel_and_shared_writes(2, false, true);
        test_mixed_parallel_and_shared_writes(0, true, false);
        test_parallel_register_continuations();
        test_parallel_ram_vectors(false);
        test_parallel_ram_vectors(true);
        test_parallel_ram_vectors(true, 8);
        std::cout << "SMP_PASS cores=8 logical_processors=16 aps_executed=15 atomic_shared_ram=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
