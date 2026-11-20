#include "android_boot_session.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::AndroidBootSession;
using reverse_windows::EtherX86Sessions;
using reverse_windows::PcMotherboardC06;

constexpr unsigned kVcpus = 16;
constexpr unsigned kSessionsPerVcpu = 2;
constexpr unsigned kDataAddress = 0x9000;
constexpr std::uint64_t kLiveValue = 0xf04c4136cbe590b8ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put_word(std::vector<std::uint8_t>& bytes, unsigned offset, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) bytes[offset + byte] = std::uint8_t(value >> (8 * byte));
}

std::uint64_t word_at(const std::vector<std::uint8_t>& bytes, unsigned offset) {
    require(offset + 8 <= bytes.size(), "result is missing its private RAM words");
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte) value |= std::uint64_t(bytes[offset + byte]) << (8 * byte);
    return value;
}

struct SumCase {
    std::uint64_t initial;
    std::uint64_t first;
    unsigned count;

    std::uint64_t total() const {
        return initial + count * first + std::uint64_t(count) * (count - 1) / 2;
    }

    std::uint64_t initial_rotor() const { return 0xa735f090cd0071cdULL ^ initial; }

    std::uint64_t final_rotor() const {
        const auto angle = unsigned(std::uint64_t(count) * (count + 1) / 2) & 63U;
        const auto rotor = initial_rotor();
        return angle ? (rotor >> angle) | (rotor << (64 - angle)) : rotor;
    }

    EtherX86Sessions::Job job() const {
        EtherX86Sessions::Job job;
        job.code = {
            0x48,0x8b,0x04,0x25,0x00,0x90,0x00,0x00, // MOV RAX,[9000h]
            0x48,0x01,0xf0,                          // ADD RAX,RSI
            0x48,0xd3,0xcb,                          // ROR RBX,CL
            0x48,0xff,0xc6,                          // INC RSI
            0x83,0xe9,0x01,                          // SUB ECX,1
            0x75,0xf2,                               // JNZ back to ADD
            0x48,0x89,0x04,0x25,0x00,0x90,0x00,0x00, // MOV [9000h],RAX
            0xf4                                    // HLT
        };
        job.registers[Cpu::ESI] = first;
        job.registers[Cpu::ECX] = count;
        job.registers[Cpu::EBX] = initial_rotor();
        job.data.resize(16);
        put_word(job.data, 0, initial);
        put_word(job.data, 8, ~initial);
        job.step_limit = 5 * count + 3;
        return job;
    }
};

EtherX86Sessions::Result await(EtherX86Sessions::Ticket& ticket) {
    require(ticket.result.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
            "accepted session did not complete");
    return ticket.result.get();
}

void run_isolated_jobs(PcMotherboardC06& board) {
    struct Session {
        SumCase sum;
        EtherX86Sessions::Ticket ticket;
    };
    std::vector<Session> sessions;
    sessions.reserve(kVcpus * kSessionsPerVcpu);
    const unsigned rounds = Cpu::ether_arm64_host() ? 262144 : 2048;
    for (unsigned owner = 0; owner < kVcpus; ++owner) {
        for (unsigned index = 0; index < kSessionsPerVcpu; ++index) {
            const unsigned seed = kSessionsPerVcpu * owner + index;
            const SumCase sum{0x478fbad010ULL + seed, 97ULL * seed, rounds + 31 * seed};
            auto ticket = board.submit_ether_x86_session(owner, sum.job());
            require(ticket.has_value(), "could not admit one of two independent sessions for a vCPU");
            sessions.push_back({sum, std::move(*ticket)});
        }
    }
    for (auto& session : sessions) {
        const auto result = await(session.ticket);
        const auto expected = session.sum.total();
        require(result.fault.empty() && result.halted && result.steps == 5 * session.sum.count + 3,
                "x86 session did not finish its complete work");
        if (Cpu::ether_arm64_host())
            require(result.projected_steps >= 5 * session.sum.count - 1024,
                    "the private x86 session did not project its rotate loop");
        require(result.registers[Cpu::EAX] == expected &&
                result.registers[Cpu::ESI] == session.sum.first + session.sum.count &&
                result.registers[Cpu::EBX] == session.sum.final_rotor() &&
                result.registers[Cpu::ECX] == 0,
                "session did not retain separate x86 register state");
        require(word_at(result.data, 0) == expected && word_at(result.data, 8) == ~session.sum.initial,
                "sessions did not retain separate data at identical guest addresses");
    }
    for (unsigned owner = 0; owner < kVcpus; ++owner) {
        const auto stats = board.ether_x86_session_stats(owner);
        require(stats.submitted == kSessionsPerVcpu && stats.completed == kSessionsPerVcpu &&
                !stats.in_flight && stats.peak_in_flight >= 1 &&
                stats.peak_in_flight <= kSessionsPerVcpu,
                "host session counters did not follow the owning vCPU");
    }
}

void check_fault_and_budget(PcMotherboardC06& board) {
    EtherX86Sessions::Job invalid_opcode;
    invalid_opcode.code = {0x0f,0x0b}; // UD2
    invalid_opcode.step_limit = 64;
    auto failed = board.submit_ether_x86_session(1, std::move(invalid_opcode));
    require(failed.has_value(), "could not submit isolated opcode failure");
    EtherX86Sessions::Job endless;
    endless.code = {0xeb,0xfe}; // JMP $
    endless.step_limit = 37;
    auto bounded = board.submit_ether_x86_session(0, std::move(endless));
    require(bounded.has_value(), "could not submit bounded isolated job");

    const auto fault = await(*failed);
    require(!fault.fault.empty() && !fault.halted, "invalid opcode was not a private session fault");
    const auto budget = await(*bounded);
    require(!budget.halted && budget.steps == 37 && budget.budget_exhausted,
            "a session did not honor its own instruction budget");
    const SumCase followup{0x1829, 31, 71};
    auto next = board.submit_ether_x86_session(1, followup.job());
    require(next.has_value(), "private fault prevented next session on the same logical processor");
    const auto result = await(*next);
    require(result.halted && result.fault.empty() && word_at(result.data, 0) == followup.total(),
            "faulting session affected follow-up work on its vCPU");
}

void check_android_session() {
    AndroidBootSession boot({}, 0, AndroidBootSession::BootSource::SelfTest, -1, 0,
                            {4U << 20, 4U << 20}, {}, {}, {},
                            AndroidBootSession::kPrimeCpuTopology);
    boot.configure_ether_x86_sessions(2, 2);
    const SumCase sum{0xea432, 7, 46};
    auto ticket = boot.submit_ether_x86_session(15, sum.job());
    require(ticket.has_value(), "Android session did not admit private work for vCPU 15");
    const auto result = await(*ticket);
    require(result.fault.empty() && result.halted && word_at(result.data, 0) == sum.total(),
            "Android session did not return the private calculation");
    const auto json = boot.diagnostic();
    const auto jobs = json.find("\"ether_x86_sessions\":{\"host_workers\":2,\"by_vcpu\":[");
    require(jobs != std::string::npos &&
            json.find("{\"id\":15,\"submitted\":1,\"completed\":1,\"in_flight\":0,"
                      "\"peak_in_flight\":1}", jobs) != std::string::npos &&
            json.find("{\"id\":0,\"submitted\":0,\"completed\":0,\"in_flight\":0,"
                      "\"peak_in_flight\":0}", jobs) != std::string::npos,
            "Android diagnostic did not report private sessions by owning vCPU");
    require(json.find("\"started\":1") != std::string::npos &&
            json.find("\"phase_x86_steps\":0") != std::string::npos &&
            json.find("\"sipi_icr_writes\":0") != std::string::npos,
            "private Android session work depended on starting the live guest");
    const auto preflight = AndroidBootSession::check_hardware({}, -1, 0, {}, {4U << 20, 4U << 20},
                                                             {}, AndroidBootSession::kPrimeCpuTopology);
    const auto check = preflight.find("\"ether_x86_sessions\":{\"passed\":true,\"owners\":16,\"host_workers\":32,");
    if (Cpu::ether_arm64_host())
        require(check != std::string::npos &&
                preflight.find("\"requested_jobs\":32,\"completed_jobs\":32,\"guest_steps\":251708496", check) !=
                    std::string::npos &&
                preflight.find("\"projected_steps\":0", check) == std::string::npos,
                "Android hardware check did not report the 32 verified x86 jobs and their projections");
    else
        require(preflight.find("\"ether_x86_sessions\":") == std::string::npos,
                "non-ARM host should skip the ARM64 hardware check");
}
}

int main() {
    try {
        PcMachine pc(4U << 20);
        pc.boot_long_mode_demo();
        pc.run(4000);
        require(pc.cpu.is_long_mode() && pc.cpu.halted && !pc.cpu.fault,
                "the live machine failed to initialize");
        PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
        pc.cpu.write64(kDataAddress, kLiveValue);
        const auto live_registers = pc.cpu.regs;
        const auto live_ip = pc.cpu.eip;
        const auto live_steps = pc.cpu.instructions;
        const auto live_ticks = board.evidence().ticks;
        const SumCase test{1, 2, 3};
        require(!board.submit_ether_x86_session(0, test.job()), "unconfigured board admitted a private job");
        board.configure_ether_x86_sessions(kVcpus * kSessionsPerVcpu, kSessionsPerVcpu);
        require(!board.submit_ether_x86_session(kVcpus, test.job()), "board admitted an invalid owner");
        run_isolated_jobs(board);
        check_fault_and_budget(board);

        const auto ipis = board.startup_ipis();
        require(board.processors_started() == 1 && ipis.init_icr_writes == 0 && ipis.sipi_icr_writes == 0,
                "private execution changed which live guest processors were started");
        for (unsigned owner = 1; owner < kVcpus; ++owner)
            require(board.processor_steps(owner) == 0, "private work counted as live guest AP steps");
        require(pc.cpu.read64(kDataAddress) == kLiveValue && pc.cpu.regs == live_registers &&
                pc.cpu.eip == live_ip && pc.cpu.instructions == live_steps &&
                board.evidence().ticks == live_ticks && !pc.cpu.fault,
                "private execution modified the live guest");
        check_android_session();
        std::cout << "PRIME_ETHER_X86_SESSIONS_PASS owners=16 complete_private_jobs=32"
                     " live_guest_unchanged=1 private_fault=1 bounded_job=1 android_diagnostic=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PRIME_ETHER_X86_SESSIONS_FAIL " << error.what() << '\n';
        return 1;
    }
}
