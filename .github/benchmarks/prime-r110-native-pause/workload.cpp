#include "c06_motherboard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::PcMotherboardC06;
using Clock = PcMachine::RunClock;

constexpr unsigned lapic = PcMotherboardC06::kLocalApic;
constexpr unsigned bsp_entry = 0x7000;
constexpr unsigned ap_dispatch = 0x8100;
constexpr unsigned ap_entry = 0x8300;
constexpr unsigned serialize_rdt = 0xb000;
constexpr unsigned bsp_word = 0x18000;
constexpr unsigned ap_word = bsp_word + 128;
constexpr unsigned history = 0x19000;
constexpr unsigned row_spacing = 0x200;
constexpr unsigned release_word = 0x1b000, finish_word = 0x1b004;
constexpr unsigned ready_table = 0x1b080, done_table = 0x1b100;
constexpr unsigned rounds = 16;
constexpr unsigned max_slices = 512;
constexpr auto slice_budget = std::chrono::milliseconds(8);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#ifndef PRIME_BASELINE_PAUSE_SCHEDULER
void check_pause_observer() {
    Cpu cpu(1U << 20);
    constexpr unsigned spin = 0x7000, stamp = 0x7100, handler = 0x7200;
    constexpr std::uint8_t poll[]{0xf3,0x90,0xeb,0xfc};
    constexpr std::uint8_t read_time[]{0x0f,0x31};
    cpu.reset(0, spin);
    cpu.regs[Cpu::ESP] = 0x7ff0;
    cpu.memory.write_block(spin, poll, sizeof(poll));
    cpu.memory.write_block(stamp, read_time, sizeof(read_time));
    cpu.write16(0x100, handler);
    const auto spin_once = [&] {
        require(cpu.step() == cosmic_pc::StepResult::Executed && cpu.step() == cosmic_pc::StepResult::Executed,
                "the PAUSE observer fixture faulted");
    };
    cpu.instructions = UINT64_MAX - 5;
    for (unsigned i = 0; i < 3; ++i) {
        spin_once();
        require(!cpu.pause_wait_hint(), "a brief PAUSE run was treated as a wait");
    }
    spin_once();
    require(cpu.pause_wait_hint(), "a repeated PAUSE run was not recognized across counter wrap");
    cpu.eip = stamp;
    require(cpu.step() == cosmic_pc::StepResult::Executed && !cpu.pause_wait_hint(),
            "a timestamp read did not clear the wait hint");
    cpu.eip = spin;
    for (unsigned i = 0; i < 4; ++i) spin_once();
    cpu.eflags |= Cpu::IF;
    require(cpu.pause_wait_hint() && cpu.raise_external_irq(0x40) && cpu.eip == handler && !cpu.pause_wait_hint(),
            "an accepted IRQ must clear the interrupted pause hint");
}
#endif

void append_word(std::vector<std::uint8_t>& program, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) program.push_back(value >> (8 * i));
}

void append_poll(std::vector<std::uint8_t>& program, unsigned address) {
    const auto start = program.size();
    program.insert(program.end(), {0xf3,0x90,0x8b,0x04,0x25}); // PAUSE; MOV EAX,[address].
    append_word(program, address);
    program.insert(program.end(), {0x85,0xc0,0x74}); // TEST EAX,EAX; JE start.
    program.push_back(static_cast<std::uint8_t>(int(start) - int(program.size() + 1)));
}

void install_ap_trampoline(Cpu& cpu) {
    constexpr std::uint8_t trampoline[] = {
        0xfa,0x31,0xc0,0x8e,0xd8,0x66,0x0f,0x01,0x16,0x00,0x51,
        0x0f,0x20,0xc0,0x0c,0x01,0x0f,0x22,0xc0,0x66,0xea,0x1b,0x80,0,0,8,0,
        0x66,0xb8,0x10,0,0x8e,0xd8,0x8e,0xd0,
        0xb8,0x20,0,0,0,0x0f,0x22,0xe0,0xb8,0,0x10,0,0,0x0f,0x22,0xd8,
        0xb9,0x80,0,0,0xc0,0x0f,0x32,0x0d,0,1,0,0,0x0f,0x30,
        0x0f,0x20,0xc0,0x0d,0,0,1,0x80,0x0f,0x22,0xc0,0xea,0,0x81,0,0,0x18,0,
    };
    cpu.write64(0x5008, 0x00cf9a000000ffffULL);
    cpu.write64(0x5010, 0x00cf92000000ffffULL);
    cpu.write64(0x5018, 0x00af9a000000ffffULL);
    cpu.write16(0x5100, 31);
    cpu.write32(0x5102, 0x5000);
    cpu.memory.write_block(0x8000, trampoline, sizeof(trampoline));
    // Same serialization as the call at ntoskrnl RVA 0x39b40e.
    constexpr std::uint8_t barrier[]{0x0f,0x20,0xd0, 0x0f,0x22,0xd0, 0xc3};
    cpu.memory.write_block(serialize_rdt, barrier, sizeof(barrier));
}

void install_ap_dispatch(Cpu& cpu) {
    // CPUID.0B returns APIC ID in EDX. AP 1 exchanges timestamps with the
    // BSP while every other participant polls the independent finish word.
    std::vector<std::uint8_t> guest{0xb8,0x0b,0,0,0,0x31,0xc9,0x0f,0xa2,0xc7,0x04,0x95};
    append_word(guest, ready_table);
    append_word(guest, 1);
    guest.insert(guest.end(), {0x83,0xfa,0x01,0x75,0x05,0xe9}); // CMP EDX,1; JNE poller; JMP peer.
    append_word(guest, ap_entry - ap_dispatch - guest.size() - 4);
    append_poll(guest, finish_word);
    guest.insert(guest.end(), {0xc7,0x04,0x95}); // MOV dword [done_table+EDX*4],1.
    append_word(guest, done_table);
    append_word(guest, 1);
    guest.insert(guest.end(), {0xfa,0xf4});
    cpu.memory.write_block(ap_dispatch, guest.data(), guest.size());
}

void install_exchange(Cpu& cpu, bool ap) {
    const auto address = ap ? ap_entry : bsp_entry;
    const auto own_time = history + (ap ? 2 * row_spacing : 0);
    std::vector<std::uint8_t> guest;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        guest.insert(guest.end(), bytes.begin(), bytes.end());
    };
    const auto literal = [&](std::uint32_t word) {
        for (unsigned i = 0; i < 4; ++i) guest.push_back(word >> (i * 8));
    };
    emit({0xbc}); literal(ap ? 0x28000 : 0x26000); // Private stacks for the serializing CALL.
    emit({0x41,0xbc}); literal(ap ? ap_word : bsp_word); // R12 = own timestamp slot.
    emit({0x41,0xbf}); literal(ap ? bsp_word : ap_word); // R15 = peer timestamp slot.
    emit({0x41,0xbd}); literal(own_time); // R13 = local timestamps for the host oracle.
    emit({0x41,0xbe}); literal(own_time + row_spacing); // R14 = observed peer timestamps.
    emit({0xbe}); literal(rounds);
    emit({0x33,0xff}); // EDI = most recently consumed peer value, initially zero.
    if (!ap) {
        emit({0xc7,0x04,0x25}); literal(ready_table); literal(1);
    }
    append_poll(guest, release_word);
    const auto loop = guest.size();
    // Exact hot loop at ntoskrnl RVAs 0x39b3ff..0x39b40e.
    emit({0xff,0xce,0xf3,0x90,0x49,0x8b,0x1f,0x48,0x3b,0xdf,0x74,0xf6,0x48,0x8b,0xfb,0xe8});
    literal(serialize_rdt - address - guest.size() - 4);
    // RDTSC; SHL RDX,32; OR RAX,RDX; MOV [R12],RAX, identical to the kernel publication.
    emit({0x0f,0x31,0x48,0xc1,0xe2,0x20,0x48,0x0b,0xc2,0x49,0x89,0x04,0x24});
    emit({0x49,0x89,0x45,0x00,0x49,0x89,0x1e,0x49,0x83,0xc5,0x08,0x49,0x83,0xc6,0x08});
    emit({0x85,0xf6,0x75});
    guest.push_back(static_cast<std::uint8_t>(int(loop) - int(guest.size() + 1)));
    if (!ap) {
        emit({0xc7,0x04,0x25}); literal(finish_word); literal(1);
    }
    emit({0xfa,0xf4});
    cpu.memory.write_block(address, guest.data(), guest.size());
}

void run_exchange(unsigned processors) {
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    pc.run(4000);
    auto& cpu = pc.cpu;
    require(cpu.is_long_mode() && cpu.halted && !cpu.fault, "long64 setup failed");
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {processors / 2, 2}, true, {},
                           PcMotherboardC06::CpuClock::Experimental4_5THz);
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    board.enable_active_clock();
    board.enable_threaded_smp(4);
    board.enable_execution_target();
    cpu.write64(0x2000 + 3 * 8, 0x6003);
    cpu.write64(0x6000 + ((lapic >> 21) & 511) * 8, lapic | 0x83);
    install_ap_trampoline(cpu);
    install_ap_dispatch(cpu);
    install_exchange(cpu, false);
    install_exchange(cpu, true);
    cpu.write64(bsp_word, UINT64_MAX);
    cpu.write64(ap_word, 0);
    cpu.eip = bsp_entry;
    cpu.eflags = 2;
    cpu.halted = false;
    cpu.write32(lapic + 0x300, 0x000cc500);
    cpu.write32(lapic + 0x300, 0x000c8500);
    cpu.write32(lapic + 0x300, 0x000c0608);
    const auto warm_deadline = Clock::now() + std::chrono::seconds(4);
    bool ready = false;
    while (!ready && Clock::now() < warm_deadline && !board.faulted_cpu()) {
        board.run(processors * 32 - 1, std::min(warm_deadline, Clock::now() + slice_budget));
        ready = true;
        for (unsigned id = 0; id < processors; ++id) ready &= cpu.read32(ready_table + id * 4) == 1;
    }
    require(ready && !board.faulted_cpu(), "the full exchange group never became ready");
    for (unsigned turn = 0; turn < 3; ++turn)
        board.run(processors * 32 - 1, Clock::now() + slice_budget);
    cpu.write32(release_word, 1);
    const auto bsp_before = cpu.instructions;
    const auto ap_before = board.processor_steps(1);
    const auto total_before = board.total_processor_steps();
#ifndef PRIME_BASELINE_PAUSE_SCHEDULER
    const auto previous_stats = board.threaded_smp_stats();
#endif
    const auto start = Clock::now();
    struct WindowEvidence {
        std::int64_t ns = 0;
        std::uint64_t bsp_steps = 0, ap_steps = 0, all_steps = 0;
    };
    std::array<WindowEvidence, max_slices> evidence{};
    unsigned slices = 0;
    std::int64_t maximum_ns = 0;
    while ((!cpu.halted || board.secondary_can_run()) && slices < max_slices && !board.faulted_cpu()) {
        const auto window = Clock::now();
        board.run(0x7fffffff, window + slice_budget);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - window).count();
        maximum_ns = std::max(maximum_ns, ns);
        evidence[slices] = {ns, cpu.instructions - bsp_before, board.processor_steps(1) - ap_before,
                            board.total_processor_steps() - total_before};
        ++slices;
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    const auto completed = cpu.halted && !board.secondary_can_run() &&
            board.processors_started() == processors && board.startup_ipis().sipi_targets_applied == processors - 1 &&
            board.threaded_smp_stats().dispatches != 0;
    if (!completed) {
        std::cerr << "SMP_PAUSE_WAIT_INCOMPLETE vcpus=" << processors << " windows=" << slices
                  << " elapsed_ns=" << elapsed_ns << " bsp_word=" << cpu.read64(bsp_word)
                  << " ap_word=" << cpu.read64(ap_word) << '\n';
        for (unsigned i = 0; i < slices; ++i) {
            const auto& point = evidence[i];
            std::cerr << "SMP_PAUSE_WAIT_WINDOW slice=" << i + 1 << " ns=" << point.ns
                      << " bsp_steps=" << point.bsp_steps << " ap_steps=" << point.ap_steps
                      << " total_steps=" << point.all_steps << '\n';
        }
    }
    if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
    require(completed, "the reciprocal exchange failed to complete");
    for (unsigned id = 2; id < processors; ++id) {
        require(cpu.read32(done_table + id * 4) == 1 && std::string_view(board.processor_state(id)) == "halted",
                "a passive vCPU did not see the published finish word");
    }

    // The CPU that starts with a zero local word is the first to consume the seed.
    const auto first = history + 2 * row_spacing;
    const auto second = history;
    std::uint64_t earlier = 0, minimum_delta = UINT64_MAX;
    for (unsigned i = 0; i < rounds; ++i) {
        const auto first_time = cpu.read64(first + i * 8);
        const auto second_time = cpu.read64(second + i * 8);
        require(cpu.read64(first + row_spacing + i * 8) == (i ? earlier : UINT64_MAX) &&
                cpu.read64(second + row_spacing + i * 8) == first_time &&
                first_time > earlier && second_time > first_time,
                "a timestamp was lost, stale, torn, or published out of reciprocal order");
        std::cerr << "SMP_PAUSE_WAIT_TIMESTAMP vcpus=" << processors << " round=" << i + 1
                  << " ap1=" << first_time << " bsp=" << second_time << '\n';
        minimum_delta = std::min(minimum_delta, std::min(first_time - earlier, second_time - first_time));
        earlier = second_time;
    }
    const auto bsp_final = cpu.read64(bsp_word), ap_final = cpu.read64(ap_word);
    require(bsp_final == cpu.read64(history + (rounds - 1) * 8) &&
            ap_final == cpu.read64(history + 2 * row_spacing + (rounds - 1) * 8),
            "the final shared timestamp slots differ from the serialized histories");
    std::cout << "SMP_PAUSE_WAIT_PASS vcpus=" << processors << " exchanged_per_cpu=" << rounds
              << " passive_acknowledgements=" << processors - 2
              << " elapsed_ns=" << elapsed_ns << " windows=" << slices << " max_window_ns=" << maximum_ns
              << " total_steps=" << board.total_processor_steps() - total_before
              << " bsp_steps=" << cpu.instructions - bsp_before << " ap_steps=" << board.processor_steps(1) - ap_before
              << " bsp_native=" << board.processor_ether_arm64_steps(0) << " ap_native=" << board.processor_ether_arm64_steps(1)
              << " bsp_final=" << bsp_final << " ap_final=" << ap_final << " min_retired_tsc_delta=" << minimum_delta;
#ifndef PRIME_BASELINE_PAUSE_SCHEDULER
    const auto& stats = board.threaded_smp_stats();
    const auto yields = stats.pause_yields - previous_stats.pause_yields;
    const auto steps = stats.pause_steps - previous_stats.pause_steps;
    const auto admissions = stats.pause_admissions - previous_stats.pause_admissions;
    require(processors == 2 ? (!yields && !admissions) : (yields && admissions && steps <= yields * 64),
            "unexpected activation of adaptive PAUSE scheduling");
    std::cout << " pause_yields=" << yields << " pause_steps=" << steps << " pause_admissions=" << admissions;
#endif
    std::cout << '\n';
}

int main() {
    try {
#ifndef PRIME_BASELINE_PAUSE_SCHEDULER
        check_pause_observer();
#endif
        run_exchange(2);
        run_exchange(16);
    } catch (const std::exception& error) {
        std::cerr << "SMP_PAUSE_WAIT_FAILURE " << error.what() << '\n';
        return 1;
    }
    return 0;
}
