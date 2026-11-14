#include "c06_motherboard.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using cosmic_pc::PcMachine;
using reverse_windows::GpuVram;
using reverse_windows::GuestGpuC08;
using reverse_windows::PcMotherboardC06;
using Clock = std::chrono::steady_clock;

constexpr unsigned kCode = 0xa000;
constexpr std::uint32_t kFrom = 0x0010'0000, kInto = 0x0090'0000;
constexpr std::uint64_t kVram = 108ULL * 1024 * 1024 * 1024;
constexpr std::size_t kChunk = 512U * 1024, kDataBytes = 8U * 1024 * 1024;
constexpr unsigned kChunks = kDataBytes / kChunk;
constexpr auto kBar = RWGPU_BAR_DEFAULT;
static_assert(kVram + kDataBytes < GpuVram::kBytes && kVram % kDataBytes == 0);

class TestDirectory {
 public:
  TestDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "rw-vram-transfer-XXXXXX").string();
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
    for (unsigned index=1; index<16; ++index)
      pc_.cpu.write64(0x3000+index*8, (index<<21) | 0x83U);
    std::vector<std::uint8_t> init{
      0xfa, 0x66,0xba,0xf8,0x0c, 0xb8,0x04,0x18,0x00,0x80,0xef,
      0x66,0xba,0xfc,0x0c, 0xb8,0x06,0,0,0,0xef,0xf4
    };
    load(init);
    run();
    require(pc_.cpu.read32(kBar + RWGPU_REG_ABI) == RWGPU_ABI &&
            (pc_.cpu.read32(kBar + RWGPU_REG_CAPS) & RWGPU_CAP_DIRECT_VRAM) &&
            pc_.cpu.read32(kBar + RWGPU_REG_DIRECT_MAX) == kChunk,
            "Direct VRAM transfers are not available on the C08 PCI adapter");
  }

  void prepare(unsigned operation, unsigned batches) {
    if (batches > kChunks) throw std::invalid_argument("Too many test batches");
    std::vector<std::uint8_t> guest{0xfa};
    for (unsigned batch=0; batch<batches; ++batch) {
      const auto offset = std::uint64_t(batch) * kChunk;
      const bool upload = operation == RWGPU_UPLOAD;
      const auto vram = kVram + offset;
      const auto from = upload ? kFrom + offset : vram;
      const auto into = upload ? vram : kInto + offset;
      mmio(guest, RWGPU_REG_OFFSET_LO, std::uint32_t(from));
      mmio(guest, RWGPU_REG_OFFSET_HI, std::uint32_t(from >> 32));
      mmio(guest, RWGPU_REG_AUX_LO, std::uint32_t(into));
      mmio(guest, RWGPU_REG_AUX_HI, std::uint32_t(into >> 32));
      mmio(guest, RWGPU_REG_LENGTH, kChunk);
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_COMPLETED);
      guest.insert(guest.end(), {0x8b,0x03,0xff,0xc0}); // MOV EAX,[RBX]; INC EAX
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_TICKET);
      guest.insert(guest.end(), {0x89,0x03}); // MOV [RBX],EAX
      mmio(guest, RWGPU_REG_DOORBELL, operation);
      guest.push_back(0xbb); imm(guest, kBar + RWGPU_REG_STATUS);
      guest.insert(guest.end(),{0x8b,0x03,0x83,0xf8,0x01,0x74,0x01,0xf4});
    }
    stop_ = kCode + guest.size() + 1;
    guest.push_back(0xf4);
    load(guest);
  }

  void transfer() {
    run();
    require(pc_.cpu.eip==stop_, "Guest received an incomplete transfer");
  }

 private:
  PcMachine& pc_;
  std::uint64_t stop_=0;

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
    pc_.run(1024);
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

void run_transfer_test() {
  TestDirectory directory;
  const auto path = directory.path / "windows.rqvd.vram.rqvd";
  PcMachine pc(32U * 1024 * 1024);
  std::vector<std::uint8_t> expected(kDataBytes), actual(kDataBytes);
  double upload=0, reopened=0, warm=0;
  std::uint64_t journal=0;
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc);
    auto elapsed=Clock::duration::zero();
    guest.prepare(RWGPU_UPLOAD, kChunks);
    constexpr unsigned passes=3;
    for (unsigned pass=0;pass<passes;++pass) {
      // Unique, non-periodic payload every pass. No size ratios from implicit
      // zeros, deduplication or repeatedly reusing identical QSSD records.
      fill(expected,0x74e93aba023df41bULL+pass);
      pc.cpu.memory.write_block(kFrom,expected.data(),expected.size());
      const auto start=Clock::now();
      guest.transfer();
      elapsed+=Clock::now()-start;
    }
    upload=bandwidth(kDataBytes*passes,elapsed);
    require(board.attached_gpu(), "C08 PCI adapter missing");
    journal=board.attached_gpu()->vram_journal_bytes();
    require(journal >= kDataBytes*passes && journal < 2*kDataBytes*passes,
            "Journal did not store the dense benchmark payload");
    // A separate logical range exercises the last exact address and the file
    // contains only submitted pages and metadata, even with the large capacity.
    std::vector<std::uint8_t> tail{0xd5,0x29,0x8a,0x28,0x69,0x07,0xb3};
    const auto kLast = GpuVram::kBytes-tail.size();
    pc.cpu.memory.write_block(kFrom,tail.data(),tail.size());
    require(submit(pc, RWGPU_UPLOAD, kFrom, kLast, tail.size()) == RWGPU_OK,
            "Writing the final VRAM bytes failed");
    require(submit(pc, RWGPU_UPLOAD, kFrom, kLast, tail.size() + 1) == RWGPU_INVALID &&
            submit(pc, RWGPU_UPLOAD, 0x9ffff, kLast, 2) == RWGPU_INVALID &&
            submit(pc, RWGPU_COPY, kVram, kVram + 1, kChunk) == RWGPU_INVALID,
            "Invalid DMA spans must fail before modifying VRAM");
    const auto separate = kVram - kChunk;
    require(submit(pc, RWGPU_UPLOAD, kFrom, separate, tail.size()) == RWGPU_OK &&
            submit(pc, RWGPU_COPY, separate, separate + 2, tail.size()) == RWGPU_OK &&
            submit(pc, RWGPU_DOWNLOAD, separate + 2, kInto, tail.size()) == RWGPU_OK,
            "Overlapping VRAM copies must preserve source bytes");
    std::vector<std::uint8_t> copied(tail.size());
    pc.cpu.memory.read_block(kInto, copied.data(), copied.size());
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
  }
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc);
    guest.prepare(RWGPU_DOWNLOAD, kChunks);
    // Device reopen and index replay happen on this first read. The host OS
    // may still cache the journal. Keep that distinction explicit.
    auto start=Clock::now();
    guest.transfer();
    reopened=bandwidth(kDataBytes,Clock::now()-start);
    pc.cpu.memory.read_block(kInto,actual.data(),actual.size());
    require(actual==expected,"Dirty or missing payload after re-opening the journal");
    std::fill(actual.begin(),actual.end(),0);
    pc.cpu.memory.write_block(kInto,actual.data(),actual.size());
    constexpr unsigned warm_passes=64;
    const auto* gpu = board.attached_gpu();
    require(gpu, "C08 PCI adapter missing after reopen");
    const auto before = gpu->resident_download_bytes();
    start=Clock::now();
    for (unsigned pass=0;pass<warm_passes;++pass) guest.transfer();
    warm=bandwidth(kDataBytes*warm_passes,Clock::now()-start);
    pc.cpu.memory.read_block(kInto,actual.data(),actual.size());
    require(actual==expected && gpu->resident_download_bytes() - before == kDataBytes * warm_passes,
            "Resident PCI transfers lost bytes or bypassed the direct path");
    const auto warm_bytes = gpu->resident_download_bytes();
    board.set_gpu_cache_enabled(false);
    require(submit(pc, RWGPU_DOWNLOAD, kVram, kInto, kChunk) == RWGPU_OK &&
            gpu->resident_download_bytes() == warm_bytes, "Pressure did not disable the resident cache");
    board.set_gpu_cache_enabled(true);
    require(submit(pc, RWGPU_DOWNLOAD, kVram, kInto, kChunk) == RWGPU_OK &&
            submit(pc, RWGPU_DOWNLOAD, kVram, kInto, kChunk) == RWGPU_OK &&
            gpu->resident_download_bytes() == warm_bytes + kChunk, "Cache did not recover after pressure");
  }
  std::cout<<"PRIME_C08_PRIMEVGC_VRAM_PASS logical_bytes=120000000000 last_byte=119999999999 "
           <<"reopened=1 exact_bytes="<<kDataBytes<<" journal_bytes="<<journal<<'\n'
           <<std::fixed<<std::setprecision(3)
           <<"PRIME_C08_PRIMEVGC_TRANSFER_GB_PER_S upload_durable="<<upload
           <<" download_reopened="<<reopened<<" download_resident="<<warm<<'\n';
}
}

int main() {
  try {
    run_transfer_test();
  } catch (const std::exception& error) {
    std::cerr<<"PRIME_C08_PRIMEVGC_VRAM_FAIL "<<error.what()<<'\n';
    return 1;
  }
}
