#define main reverse_windows_smp_fixture_main
#include "smp_guest_test.cpp"
#undef main

std::vector<std::uint8_t> mixed_worker(unsigned output, unsigned rounds, bool scalar_alu) {
    std::vector<std::uint8_t> bytes;
    const auto load = [&](std::uint8_t op, unsigned value) {
        bytes.push_back(op);
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(value >> (8 * i));
    };
    load(0xbe, output); // MOV ESI, output
    load(0xb9, rounds); // MOV ECX, rounds
    const auto loop = bytes.size();
    const std::uint8_t body[] = {
        0x89,0xc8, 0x83,0xf0,0x69, // MOV EAX, ECX; XOR EAX, 69h
        std::uint8_t(scalar_alu ? 0x11 : 0x01),0xf0, // ADC or ADD EAX, ESI. XOR cleared CF.
        0xeb,0x00, // JMP next, separating the register and RAM blocks.
        0x33,0x06, 0x89,0x06, // XOR EAX, [RSI]; MOV [RSI], EAX
        0x83,0xe9,0x01, 0x75 // SUB ECX, 1; JNZ loop
    };
    bytes.insert(bytes.end(), std::begin(body), std::end(body));
    bytes.push_back(static_cast<std::uint8_t>(int(loop) - int(bytes.size() + 1)));
    bytes.push_back(0xfa); // CLI
    bytes.push_back(0xf4); // HLT
    return bytes;
}

std::uint32_t mixed_result(unsigned output, unsigned rounds) {
    std::uint32_t result = 0;
    for (; rounds; --rounds) result ^= (rounds ^ 0x69U) + output;
    return result;
}

int main(int argc, char** argv) {
    try {
        constexpr unsigned bsp_output = 0x14000, ap_output = 0x16000;
        require(argc == 3 && (std::string(argv[1]) == "native-both" || std::string(argv[1]) == "mixed-ap"),
                "choose native-both or mixed-ap and a round count");
        const auto rounds = std::stoul(argv[2]);
        require(rounds && rounds <= 2'000'000, "invalid workload size");
        const bool scalar_ap = std::string(argv[1]) == "mixed-ap";
        PcMachine pc(4U << 20);
        pc.boot_long_mode_demo();
        pc.run(4000);
        auto& cpu = pc.cpu;
        require(Cpu::ether_arm64_host() && cpu.is_long_mode() && cpu.halted && !cpu.fault,
                "ARM64 SMP fixture setup failed");
        PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {2, 1}, true, {},
                               PcMotherboardC06::CpuClock::Experimental4_5THz);
        pc.enable_boot_acceleration();
        cpu.enable_page_translation_cache();
        cpu.enable_ether_arm64_blocks();
        using Clock = PcMachine::RunClock;
        board.enable_active_clock([] { return Clock::time_point{}; });
        board.enable_threaded_smp(2);
        board.enable_execution_target();
        cpu.write64(0x2000 + 3 * 8, 0x6003);
        cpu.write64(0x6000 + ((kLapic >> 21) & 511) * 8, kLapic | 0x83);
        const auto bsp = mixed_worker(bsp_output, rounds, false);
        const auto ap = mixed_worker(ap_output, rounds, scalar_ap);
        install_long_mode_smp_guest(cpu, ap.data(), ap.size());
        start_long_mode_smp_guest(cpu);
        cpu.memory.write_block(0x7000, bsp.data(), bsp.size());
        cpu.eip = 0x7000;
        const auto before = board.processor_steps(0);
        const auto start = Clock::now();
        const auto end = start + std::chrono::seconds(60);
        while ((!cpu.halted || board.secondary_can_run()) && !board.faulted_cpu() && !cpu.fault && Clock::now() < end)
            board.run(0x7fffffff, std::min(end, Clock::now() + std::chrono::milliseconds(8)));
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (const auto* fault = board.faulted_cpu()) throw std::runtime_error(fault->fault->describe());
        require(cpu.halted && !cpu.fault && !board.secondary_can_run() && board.processors_started() == 2 &&
                board.startup_ipis().sipi_targets_applied == 1 && board.threaded_smp_stats().peak_workers == 2 &&
                board.processor_steps(0) - before == 8 * rounds + 4 &&
                board.processor_steps(1) == 8 * rounds + 27 &&
                cpu.read32(bsp_output) == mixed_result(bsp_output, rounds) &&
                cpu.read32(ap_output) == mixed_result(ap_output, rounds) &&
                board.evidence().irq_delivered == 0,
                "different guest work in the paired SMP run");
        const auto& translated = cpu.ether_arm64_stats();
        std::cout << "SHORT_REGISTER_BUS_SAMPLE scalar_ap=" << scalar_ap << " elapsed_ns=" << duration
                  << " steps=" << board.total_processor_steps() - before
                  << " bsp_steps=" << board.processor_steps(0) - before << " ap_steps=" << board.processor_steps(1)
                  << " bsp_checksum=" << cpu.read32(bsp_output) << " ap_checksum=" << cpu.read32(ap_output)
                  << " bsp_native=" << translated.steps << " ap_native=" << board.processor_ether_arm64_steps(1)
                  << " bsp_off_bus_steps=" << translated.off_bus_register_steps
                  << " dispatches=" << board.threaded_smp_stats().dispatches;
#ifdef REGISTER_BUS_STATS
        std::cout << " bsp_on_bus_steps=" << translated.on_bus_register_steps
                  << " internal_reacquisitions=" << translated.register_bus_reacquisitions;
#endif
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "SHORT_REGISTER_BUS_FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
