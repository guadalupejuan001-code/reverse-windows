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
using reverse_windows::GpuPciC01;
using reverse_windows::GpuVram;
using reverse_windows::PcMotherboardC06;
using Clock = std::chrono::steady_clock;

constexpr unsigned kCode = 0xa000;
constexpr std::uint32_t kFrom = 0x0010'0000, kInto = 0x0090'0000;
constexpr std::uint64_t kVram = 108ULL * 1024 * 1024 * 1024;
constexpr std::size_t kChunk = 512U * 1024, kDataBytes = 8U * 1024 * 1024;
constexpr unsigned kChunks = kDataBytes / kChunk;
constexpr double kGddr7Rtx5090GbPerSecond = 1792.0;
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
  TransferGuest(PcMachine& pc, PcMotherboardC06& board) : pc_(pc) {
    constexpr auto page = GpuPciC01::kWindowBase & ~((1U << 21) - 1);
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
    require(pc_.cpu.read32(GpuPciC01::kWindowBase + GpuPciC01::kAbi)==2,
            "VRAM PCI ABI is not available");
    require(board.gpu().durable(), "Test must use the same durable VRAM path as an ISO session");
  }

  void prepare(unsigned operation, unsigned batches) {
    if (batches > kChunks) throw std::invalid_argument("Too many test batches");
    std::vector<std::uint8_t> guest{0xfa};
    for (unsigned batch=0; batch<batches; ++batch) {
      const auto offset = std::uint64_t(batch) * kChunk;
      const bool upload = operation == GpuPciC01::Upload;
      const auto vram = kVram + offset;
      const auto from = upload ? kFrom + offset : vram;
      const auto into = upload ? vram : kInto + offset;
      mmio(guest,GpuPciC01::kSource,std::uint32_t(from));
      mmio(guest,GpuPciC01::kSourceHigh,std::uint32_t(from>>32));
      mmio(guest,GpuPciC01::kDestination,std::uint32_t(into));
      mmio(guest,GpuPciC01::kDestinationHigh,std::uint32_t(into>>32));
      mmio(guest,GpuPciC01::kCountLow,kChunk);
      mmio(guest,GpuPciC01::kDoorbell,operation);
      guest.push_back(0xbb); imm(guest,GpuPciC01::kWindowBase+GpuPciC01::kStatus);
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
    guest.push_back(0xbb); imm(guest,GpuPciC01::kWindowBase+offset);
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
    TransferGuest guest(pc,board);
    auto elapsed=Clock::duration::zero();
    guest.prepare(GpuPciC01::Upload,kChunks);
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
    journal=board.gpu().journal_bytes();
    require(journal >= kDataBytes*passes && journal < 2*kDataBytes*passes,
            "Journal did not store the dense benchmark payload");
    // A separate logical range exercises the last exact address and the file
    // contains only submitted pages and metadata, even with the large capacity.
    std::vector<std::uint8_t> tail{0xd5,0x29,0x8a,0x28,0x69,0x07,0xb3};
    const auto kLast = GpuVram::kBytes-tail.size();
    pc.cpu.memory.write_block(kFrom,tail.data(),tail.size());
    const auto base=GpuPciC01::kWindowBase;
    pc.cpu.write32(base+GpuPciC01::kSource,kFrom);
    pc.cpu.write32(base+GpuPciC01::kDestination,std::uint32_t(kLast));
    pc.cpu.write32(base+GpuPciC01::kDestinationHigh,std::uint32_t(kLast>>32));
    pc.cpu.write32(base+GpuPciC01::kCountLow,tail.size());
    pc.cpu.write32(base+GpuPciC01::kDoorbell,GpuPciC01::Upload);
    require(pc.cpu.read32(base+GpuPciC01::kStatus)==GpuPciC01::Done,
            "Writing the final VRAM bytes failed");
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
    TransferGuest guest(pc,board);
    guest.prepare(GpuPciC01::Download,kChunks);
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
    start=Clock::now();
    for (unsigned pass=0;pass<warm_passes;++pass) guest.transfer();
    warm=bandwidth(kDataBytes*warm_passes,Clock::now()-start);
    pc.cpu.memory.read_block(kInto,actual.data(),actual.size());
    require(actual==expected,"Resident PCI transfers did not return the original bytes");
  }
  std::cout<<"PRIME_GPU_C02_VRAM_PASS logical_bytes=120000000000 last_byte=119999999999 "
           <<"reopened=1 exact_bytes="<<kDataBytes<<" journal_bytes="<<journal<<'\n'
           <<std::fixed<<std::setprecision(3)
           <<"PRIME_GPU_C02_TRANSFER_GB_PER_S upload_durable="<<upload
           <<" download_reopened="<<reopened<<" download_resident="<<warm
           <<" gddr7_rtx5090_peak="<<kGddr7Rtx5090GbPerSecond<<'\n';
}
}

int main() {
  try {
    run_transfer_test();
  } catch (const std::exception& error) {
    std::cerr<<"PRIME_GPU_C02_VRAM_FAIL "<<error.what()<<'\n';
    return 1;
  }
}
