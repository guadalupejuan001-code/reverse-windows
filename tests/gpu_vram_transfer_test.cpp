#include "c06_motherboard.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using cosmic_pc::PcMachine;
using reverse_windows::GpuVram;
using reverse_windows::GuestGpuC08;
using reverse_windows::PcMotherboardC06;
using Clock = std::chrono::steady_clock;

constexpr unsigned kCode = 0x20000;
constexpr std::uint32_t kFrom = 0x0020'0000;
constexpr std::uint64_t kVram = 108ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMiB = 1024U * 1024U;
constexpr std::size_t kChunk = 512U * 1024;
constexpr std::size_t kResidentPayload = 8U * kMiB, kBulkPayload = 256U * kMiB;
constexpr auto kBar = RWGPU_BAR_DEFAULT;
static_assert(kVram + kBulkPayload < GpuVram::kBytes && kVram % kResidentPayload == 0);

class TestDirectory {
 public:
  TestDirectory() {
    auto pattern = (std::filesystem::current_path() / "rw-vram-transfer-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("No temporary VRAM directory");
    path = pattern;
  }
  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// All register writes use MOV instructions executed by the actual guest.
class TransferGuest {
 public:
  explicit TransferGuest(PcMachine& pc) : pc_(pc) {
    constexpr auto page = kBar & ~((1U << 21) - 1);
    pc_.cpu.write64(0x2000+3*8, 0x4003);
    pc_.cpu.write64(0x4000+((page>>21)&0x1ff)*8, page | 0x83U);
    // Map the full guest payload and destination in long mode.
    for (std::size_t index = 1; index < (pc_.cpu.memory.size() >> 21); ++index)
      pc_.cpu.write64(0x3000+index*8, (index<<21) | 0x83U);
    std::vector<std::uint8_t> init{
      0xfa, 0x66,0xba,0xf8,0x0c, 0xb8,0x04,0x18,0x00,0x80,0xef,
      0x66,0xba,0xfc,0x0c, 0xb8,0x06,0,0,0,0xef,0xf4
    };
    load(init);
    run();
    require(pc_.cpu.read32(kBar + RWGPU_REG_ABI) == RWGPU_ABI &&
            (pc_.cpu.read32(kBar + RWGPU_REG_CAPS) & RWGPU_CAP_DIRECT_VRAM) &&
            pc_.cpu.read32(kBar + RWGPU_REG_DIRECT_MAX) == GpuVram::kTransferBytes,
            "Direct VRAM transfers are not available on the C08 PCI adapter");
  }

  void prepare(unsigned operation, std::size_t bytes, std::size_t chunk,
               std::uint64_t vram_offset = kVram, unsigned status = RWGPU_OK) {
    if (!chunk || bytes % chunk) throw std::invalid_argument("Test transfer is not chunk-aligned");
    std::vector<std::uint8_t> guest{0xfa};
    mmio(guest, RWGPU_REG_LENGTH_HI, 0);
    for (std::size_t offset = 0; offset < bytes; offset += chunk) {
      const bool upload = operation == RWGPU_UPLOAD;
      const auto vram = vram_offset + offset;
      const auto from = upload ? kFrom + offset : vram;
      const auto into = upload ? vram : kFrom + offset;
      mmio(guest, RWGPU_REG_OFFSET_LO, std::uint32_t(from));
      mmio(guest, RWGPU_REG_OFFSET_HI, std::uint32_t(from >> 32));
      mmio(guest, RWGPU_REG_AUX_LO, std::uint32_t(into));
      mmio(guest, RWGPU_REG_AUX_HI, std::uint32_t(into >> 32));
      mmio(guest, RWGPU_REG_LENGTH, std::uint32_t(chunk));
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_COMPLETED);
      guest.insert(guest.end(), {0x8b,0x03,0xff,0xc0}); // MOV EAX,[RBX]; INC EAX
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_TICKET);
      guest.insert(guest.end(), {0x89,0x03}); // MOV [RBX],EAX
      mmio(guest, RWGPU_REG_DOORBELL, operation);
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_STATUS);
      guest.insert(guest.end(), {0x8b,0x03,0x83,0xf8,std::uint8_t(status),0x74,0x01,0xf4});
    }
    stop_ = kCode + guest.size() + 1;
    steps_ = unsigned(bytes / chunk) * 25 + 64;
    guest.push_back(0xf4);
    load(guest);
  }

  void transfer() {
    run();
    if (pc_.cpu.eip != stop_)
      throw std::runtime_error("Guest transfer failed at ticket " +
          std::to_string(pc_.cpu.read32(kBar + RWGPU_REG_COMPLETED)) + " with status " +
          std::to_string(pc_.cpu.read32(kBar + RWGPU_REG_STATUS)));
  }

 private:
  PcMachine& pc_;
  std::uint64_t stop_=0;
  unsigned steps_=64;

  static void imm(std::vector<std::uint8_t>& guest, std::uint32_t value) {
    for (unsigned i=0;i<4;++i) guest.push_back(std::uint8_t(value>>(8*i)));
  }
  static void mmio(std::vector<std::uint8_t>& guest, unsigned offset, std::uint32_t value) {
    guest.push_back(0xbb); imm(guest,kBar+offset);
    guest.insert(guest.end(),{0xc7,0x03}); imm(guest,value);
  }
  void load(const std::vector<std::uint8_t>& code) {
    pc_.cpu.memory.write_block(kCode,code.data(),code.size());
  }
  void run() {
    pc_.cpu.eip=kCode;
    pc_.cpu.halted=false;
    pc_.run(steps_);
    if (pc_.cpu.fault) throw std::runtime_error(pc_.cpu.fault->describe());
    require(pc_.cpu.halted,"PCI transfer fixture did not finish");
  }
};

void boot(PcMachine& pc) {
  pc.boot_long_mode_demo();
  pc.run(12000);
  require(pc.cpu.is_long_mode() && pc.cpu.halted && !pc.cpu.fault,
          "Could not boot x64 transfer guest");
}

void fill(std::vector<std::uint8_t>& bytes, std::uint64_t seed) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    bytes[i] = std::uint8_t(seed >> 9);
  }
}

double bandwidth(std::uint64_t bytes, Clock::duration elapsed) {
  const double ns = std::chrono::duration<double,std::nano>(elapsed).count();
  require(ns > 0, "Invalid benchmark duration");
  return double(bytes) / ns;
}

std::uint32_t submit(PcMachine& pc, unsigned op, std::uint64_t source, std::uint64_t destination,
                     std::uint64_t count) {
  pc.cpu.write32(kBar + RWGPU_REG_OFFSET_LO, std::uint32_t(source));
  pc.cpu.write32(kBar + RWGPU_REG_OFFSET_HI, std::uint32_t(source >> 32));
  pc.cpu.write32(kBar + RWGPU_REG_AUX_LO, std::uint32_t(destination));
  pc.cpu.write32(kBar + RWGPU_REG_AUX_HI, std::uint32_t(destination >> 32));
  pc.cpu.write32(kBar + RWGPU_REG_LENGTH, std::uint32_t(count));
  pc.cpu.write32(kBar + RWGPU_REG_LENGTH_HI, std::uint32_t(count >> 32));
  pc.cpu.write32(kBar + RWGPU_REG_TICKET, pc.cpu.read32(kBar + RWGPU_REG_COMPLETED) + 1);
  pc.cpu.write32(kBar + RWGPU_REG_DOORBELL, op);
  return pc.cpu.read32(kBar + RWGPU_REG_STATUS);
}

void verify_coalesced_reads(const std::filesystem::path& path, std::vector<std::uint8_t>& actual,
                            const std::vector<std::uint8_t>& expected, std::uint64_t last_epoch) {
  using reverse_windows::QssdDisk;
  constexpr auto block_bytes = QssdDisk::kBlockSize;
  constexpr auto record_bytes = block_bytes + 40 + 16;
  {
    QssdDisk volume(path, QssdDisk::Access::ReadOnly);
    const auto reads = volume.io_stats().read_calls;
    require(volume.read_bytes(kVram, actual.data(), kChunk) &&
            std::equal(actual.begin(), actual.begin() + kChunk, expected.begin()) &&
            volume.io_stats().read_calls - reads == 1,
            "QSSD must fetch contiguous records with one read and preserve exact bytes");
    const auto fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    require(fd >= 0, "Cannot open private checksum fixture");
    const auto changed = std::uint8_t(expected[3 * block_bytes + 31] ^ 1U);
    const auto offset = last_epoch + 3 * record_bytes + 40 + 31;
    const auto written = ::pwrite(fd, &changed, 1, static_cast<off_t>(offset));
    ::close(fd);
    require(written == 1, "Cannot damage private checksum fixture");
    std::fill_n(actual.begin(), kChunk, 0x5a);
    require(!volume.read_bytes(kVram, actual.data(), kChunk) &&
            std::all_of(actual.begin(), actual.begin() + kChunk, [](auto byte) { return byte == 0x5a; }),
            "Corrupted raw records must fail before sending their run");
  }
  bool rejected = false;
  try {
    QssdDisk volume(path, QssdDisk::Access::ReadOnly);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  require(rejected, "Batched replay accepted a corrupted payload");
}

void run_transfer_test(std::size_t payload, std::size_t chunk, unsigned passes) {
  TestDirectory directory;
  const auto path = directory.path / "windows.rqvd.vram.rqvd";
  PcMachine pc(payload + 16U * kMiB);
  std::vector<std::uint8_t> expected(payload), actual(payload);
  double upload = 0, reopened = 0, open = 0, warm = 0;
  std::uint64_t journal = 0, last_epoch = 64;
  const auto upload_commits = payload / chunk * passes;
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc);
    auto elapsed=Clock::duration::zero();
    guest.prepare(RWGPU_UPLOAD, payload, chunk);
    const auto* gpu = board.attached_gpu();
    require(gpu, "C08 PCI adapter missing");
    for (unsigned pass=0;pass<passes;++pass) {
      // Unique, non-periodic payload every pass. No size ratios from implicit
      // zeros, deduplication or repeatedly reusing identical QSSD records.
      fill(expected,0x74e93aba023df41bULL+pass);
      pc.cpu.memory.write_block(kFrom,expected.data(),expected.size());
      if (pass) last_epoch = gpu->vram_journal_bytes();
      const auto start=Clock::now();
      guest.transfer();
      elapsed+=Clock::now()-start;
    }
    upload = bandwidth(payload * passes, elapsed);
    journal = gpu->vram_journal_bytes();
    require(journal >= payload * passes && journal < 2 * payload * passes,
            "Journal did not store the dense benchmark payload");
    // A separate logical range exercises the last exact address and the file
    // contains only submitted pages and metadata, even with the large capacity.
    std::vector<std::uint8_t> tail{0xd5,0x29,0x8a,0x28,0x69,0x07,0xb3};
    const auto kLast = GpuVram::kBytes-tail.size();
    pc.cpu.memory.write_block(kFrom,tail.data(),tail.size());
    guest.prepare(RWGPU_UPLOAD, GpuVram::kTransferBytes, GpuVram::kTransferBytes,
                  kVram + 1, RWGPU_INVALID);
    guest.transfer();
    require(submit(pc, RWGPU_UPLOAD, kFrom, kLast, tail.size()) == RWGPU_OK,
            "Writing the final VRAM bytes failed");
    require(submit(pc, RWGPU_UPLOAD, kFrom, kLast, tail.size() + 1) == RWGPU_INVALID &&
            submit(pc, RWGPU_UPLOAD, 0x9ffff, kLast, 2) == RWGPU_INVALID &&
            submit(pc, RWGPU_COPY, kVram, kVram + 1, GpuVram::kTransferBytes) == RWGPU_INVALID,
            "Invalid DMA spans must fail before modifying VRAM");
    const auto separate = kVram - kChunk;
    require(submit(pc, RWGPU_UPLOAD, kFrom, separate, tail.size()) == RWGPU_OK &&
            submit(pc, RWGPU_COPY, separate, separate + 2, tail.size()) == RWGPU_OK &&
            submit(pc, RWGPU_DOWNLOAD, separate + 2, kFrom, tail.size()) == RWGPU_OK,
            "Overlapping VRAM copies must preserve source bytes");
    std::vector<std::uint8_t> copied(tail.size());
    pc.cpu.memory.read_block(kFrom, copied.data(), copied.size());
    require(copied == tail, "VRAM copy changed overlapping bytes");
    constexpr auto domain = std::uint64_t{1} << 32;
    constexpr auto sum = (domain / 2) * (domain - 1);
    require(submit(pc, RWGPU_SUM_INDICES, 0, 0, domain) == RWGPU_OK &&
            pc.cpu.read64(kBar + RWGPU_REG_RESULT_LO) == sum &&
            submit(pc, RWGPU_SUM_INDICES, 0, 0, domain + 1) == RWGPU_INVALID &&
            pc.cpu.read64(kBar + RWGPU_REG_RESULT_LO) == 0,
            "GPU Virtual did not return the exact sum over the uint32 domain");
  }
  {
    reverse_windows::QssdDisk volume(path,reverse_windows::QssdDisk::Access::ReadOnly);
    std::uint8_t last=0;
    require(volume.bytes()==120'000'000'000ULL &&
            volume.read_bytes(volume.bytes()-1,&last,1) && last==0xb3 &&
            !volume.read_bytes(volume.bytes(),&last,1),
            "Cold reopen lost the exact mathematical VRAM limit or final byte");
    require(volume.stats().commits == upload_commits + 3,
            "Expected one commit per upload plus the three boundary and overlap writes");
  }
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc);
    guest.prepare(RWGPU_DOWNLOAD, payload, chunk);
    // Device reopen and index replay happen on this first read. The host OS
    // may still cache the journal. Keep that distinction explicit.
    auto start=Clock::now();
    guest.transfer();
    reopened = bandwidth(payload, Clock::now() - start);
    pc.cpu.memory.read_block(kFrom, actual.data(), actual.size());
    require(actual==expected,"Dirty or missing payload after re-opening the journal");
    const auto* gpu = board.attached_gpu();
    require(gpu, "C08 PCI adapter missing after reopen");

    const auto prior = gpu->resident_download_bytes();
    board.set_gpu_cache_enabled(false);
    constexpr unsigned open_passes = 3;
    auto elapsed = Clock::duration::zero();
    for (unsigned pass = 0; pass < open_passes; ++pass) {
      pc.cpu.memory.fill(kFrom, 0, payload);
      start = Clock::now();
      guest.transfer();
      elapsed += Clock::now() - start;
      pc.cpu.memory.read_block(kFrom, actual.data(), actual.size());
      require(actual == expected, "Open QSSD read changed the payload");
    }
    require(gpu->resident_download_bytes() == prior, "Pressure did not disable the resident cache");
    open = bandwidth(payload * open_passes, elapsed);

    board.set_gpu_cache_enabled(true);
    const auto warm_offset = kVram + payload - kResidentPayload;
    guest.prepare(RWGPU_DOWNLOAD, kResidentPayload, kChunk, warm_offset);
    guest.transfer(); // Recover the resident cache through the production read.
    pc.cpu.memory.fill(kFrom, 0, kResidentPayload);
    constexpr unsigned warm_passes = 64;
    const auto before = gpu->resident_download_bytes();
    start = Clock::now();
    for (unsigned pass = 0; pass < warm_passes; ++pass) guest.transfer();
    warm = bandwidth(kResidentPayload * warm_passes, Clock::now() - start);
    pc.cpu.memory.read_block(kFrom, actual.data(), kResidentPayload);
    require(std::equal(actual.begin(), actual.begin() + kResidentPayload, expected.end() - kResidentPayload) &&
            gpu->resident_download_bytes() - before == kResidentPayload * warm_passes,
            "Resident PCI transfers lost bytes or bypassed the direct path");
  }
  verify_coalesced_reads(path, actual, expected, last_epoch);
  std::cout<<"PRIME_C08_PRIMEVGC_VRAM_PASS logical_bytes=120000000000 last_byte=119999999999 "
           <<"reopened=1 exact_bytes="<<payload<<" journal_bytes="<<journal
           <<" command_bytes="<<chunk<<" upload_commits="<<upload_commits<<'\n'
           <<std::fixed<<std::setprecision(3)
           <<"PRIME_C08_PRIMEVGC_TRANSFER_GB_PER_S upload_durable="<<upload
           <<" download_reopened="<<reopened<<" download_open="<<open<<" download_resident="<<warm<<'\n';
}
}

int main(int argc, char** argv) {
  try {
    const std::string_view mode = argc == 2 ? argv[1] : "";
    if (argc > 2 || (argc == 2 && mode != "--bulk" && mode != "--bulk-512k"))
      throw std::invalid_argument("Usage: gpu-vram-transfer [--bulk | --bulk-512k]");
    run_transfer_test(mode.empty() ? kResidentPayload : kBulkPayload,
                      mode == "--bulk" ? GpuVram::kTransferBytes : kChunk, mode.empty() ? 3 : 1);
  } catch (const std::exception& error) {
    std::cerr<<"PRIME_C08_PRIMEVGC_VRAM_FAIL "<<error.what()<<'\n';
    return 1;
  }
}
