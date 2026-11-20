#include "android_boot_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::AndroidBootSession;
using reverse_windows::PcMotherboardC06;

namespace {
constexpr std::uint64_t entry = 0x9000;
constexpr unsigned iterations = 250'000;

std::vector<std::uint8_t> countdown(bool wide, bool mix) {
    std::vector<std::uint8_t> bytes;
    const auto emit = [&](std::initializer_list<std::uint8_t> instruction) {
        if (wide) bytes.push_back(0x48);
        bytes.insert(bytes.end(), instruction);
    };
    emit({0x01,0xc8});                       // ADD accumulator,counter.
    if (mix) {
        emit({0x35,0xdf,0x9b,0x57,0x13});   // XOR accumulator,13579BDFh.
        emit({0xc1,0xc0,5});                // ROL accumulator,5.
    }
    if (wide) emit({0xff,0xc9});            // DEC RCX.
    else bytes.push_back(0x49);             // DEC ECX.
    const auto back = std::uint8_t(0U - unsigned(bytes.size()) - 2);
    bytes.insert(bytes.end(), {0x75,back,0xf4});  // JNZ to ADD; HLT.
    return bytes;
}

template<class Word>
Word reference(unsigned count, bool mix) {
    Word sum = 0;
    while (count) {
        sum += count--;
        if (mix) {
            sum ^= 0x13579bdfU;
            sum = (sum << 5) | (sum >> (sizeof(Word) * 8 - 5));
        }
    }
    return sum;
}

void measure(std::string_view version, bool wide, bool mix) {
    PcMachine pc(4U << 20);
    pc.boot_long_mode_demo();
    auto& cpu = pc.cpu;
    cpu.use_windows_x64_profile(true);
    PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, AndroidBootSession::kPrimeCpuTopology,
                           true, {}, AndroidBootSession::kPrimeCpuClock);
    pc.enable_boot_acceleration();
    cpu.enable_page_translation_cache();
    cpu.enable_ether_arm64_blocks();
    board.enable_active_clock({}, PcMotherboardC06::ClockStart::Guest32Or64);
    const std::string_view mode = wide ? "long64" : "protected32";
    for (unsigned i = 0; i < 8192 && cpu.mode_name() != mode && !cpu.fault; ++i) board.run(1);
    if (cpu.mode_name() != mode || cpu.fault) throw std::runtime_error("Guest mode setup failed");
    const auto program = countdown(wide, mix);
    cpu.memory.write_block(entry, program.data(), program.size());
    for (unsigned count : {1024U, iterations}) {
        cpu.halted = false;
        cpu.eip = entry;
        cpu.eflags = 2 | Cpu::CF;
        cpu.regs[Cpu::EAX] = 0;
        cpu.regs[Cpu::ECX] = count;
        const auto expected = wide ? reference<std::uint64_t>(count, mix) : reference<std::uint32_t>(count, mix);
        const auto steps = std::uint64_t(count) * (mix ? 5U : 3U) + 1;
        const auto finish = cpu.instructions + steps;
        const auto old_native = cpu.ether_arm64_stats();
        const auto old_projected = cpu.state_vector_interpreter_stats().projected_steps;
        const auto begin = PcMachine::RunClock::now();
        while (!cpu.halted && !cpu.fault && cpu.instructions < finish) {
            const auto budget = unsigned(std::min<std::uint64_t>(4096, finish - cpu.instructions));
            board.run(budget, PcMachine::RunClock::now() + std::chrono::milliseconds(8));
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(PcMachine::RunClock::now() - begin).count();
        if (cpu.fault || !cpu.halted || cpu.eip != entry + program.size() || cpu.instructions != finish ||
            cpu.regs[Cpu::EAX] != expected || cpu.regs[Cpu::ECX] != 0 || !cpu.flag(Cpu::ZF))
            throw std::runtime_error("Guest result, flags or retired instructions differ from reference");
        if (count == iterations) {
            const auto& native = cpu.ether_arm64_stats();
            const auto projected = native.state_vector_steps - old_native.state_vector_steps +
                                   cpu.state_vector_interpreter_stats().projected_steps - old_projected;
            std::cout << "ENGINE_SAMPLE " << version << ' ' << mode << ' ' << (mix ? "mix" : "sum")
                      << ' ' << ns << ' ' << steps << ' ' << cpu.regs[Cpu::EAX] << ' ' << cpu.eflags
                      << ' ' << native.repeated_loop_steps - old_native.repeated_loop_steps
                      << ' ' << projected << '\n';
        }
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    for (bool wide : {false, true})
        for (bool mix : {true, false}) measure(argv[1], wide, mix);
}
