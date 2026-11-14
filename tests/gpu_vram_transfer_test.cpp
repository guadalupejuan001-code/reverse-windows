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
#include <vector>
#include <unistd.h>

namespace {
using cosmic_pc::PcMachine;
using reverse_windows::GpuPciC01;
using reverse_windows::GpuVram;
using reverse_windows::PcMotherboardC06;
using Clock = std::chrono::steady_clock;

constexpr unsigned kCode = 0x0002'0000;
constexpr std::uint32_t kFrom = 0x0020'0000;
constexpr std::uint64_t kVram = 108ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMiB = 1024U * 1024U;
constexpr std::size_t kChunk = 512U * 1024;
constexpr std::size_t kResidentTestBytes = 8U * kMiB;
constexpr double kGddr7Rtx5090GbPerSecond = 1792.0;
static_assert(kVram + 256U*kMiB < GpuVram::kBytes && kVram % kResidentTestBytes == 0);

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
  TransferGuest(PcMachine& pc, PcMotherboardC06& board) : pc_(pc) {
    constexpr auto page = GpuPciC01::kWindowBase & ~((1U << 21) - 1);
    pc_.cpu.write64(0x2000+3*8, 0x4003);
    pc_.cpu.write64(0x4000+((page>>21)&0x1ff)*8, page | 0x83U);
    // Map the full guest payload and destination in long mode.
    for (std::size_t index=1; index<(pc_.cpu.memory.size()>>21); ++index)
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

  void prepare(unsigned operation, std::size_t size, std::size_t chunk=kChunk,
               std::uint64_t vram_offset=kVram, unsigned status=GpuPciC01::Done) {
    if (size % chunk != 0) throw std::invalid_argument("Test transfer is not chunk-aligned");
    std::vector<std::uint8_t> guest{0xfa};
    for (std::size_t offset=0; offset<size; offset+=chunk) {
      const bool upload = operation == GpuPciC01::Upload;
      const auto vram = vram_offset + offset;
      const auto from = upload ? kFrom + offset : vram;
      const auto into = upload ? vram : kFrom + offset;
      mmio(guest,GpuPciC01::kSource,std::uint32_t(from));
      mmio(guest,GpuPciC01::kSourceHigh,std::uint32_t(from>>32));
      mmio(guest,GpuPciC01::kDestination,std::uint32_t(into));
      mmio(guest,GpuPciC01::kDestinationHigh,std::uint32_t(into>>32));
      mmio(guest,GpuPciC01::kCountLow,std::uint32_t(chunk));
      mmio(guest,GpuPciC01::kDoorbell,operation);
      guest.push_back(0xbb); imm(guest,GpuPciC01::kWindowBase+GpuPciC01::kStatus);
      guest.insert(guest.end(),{0x8b,0x03,0x83,0xf8,std::uint8_t(status),0x74,0x01,0xf4});
    }
    stop_ = kCode + guest.size() + 1;
    steps_ = unsigned(size/chunk)*20+64;
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
  unsigned steps_=64;

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

void run_transfer_test(std::size_t payload_bytes,std::size_t command_bytes,unsigned passes) {
  TestDirectory directory;
  const auto path = directory.path / "windows.rqvd.vram.rqvd";
  PcMachine pc(payload_bytes+16U*kMiB);
  std::vector<std::uint8_t> expected(payload_bytes), actual(payload_bytes);
  double upload=0, reopened=0, open=0, warm=0;
  std::uint64_t journal=0;
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc,board);
    auto elapsed=Clock::duration::zero();
    guest.prepare(GpuPciC01::Upload,payload_bytes,command_bytes);
    for (unsigned pass=0;pass<passes;++pass) {
      // Unique, non-periodic payload every pass. No size ratios from implicit
      // zeros, deduplication or repeatedly reusing identical QSSD records.
      fill(expected,0x74e93aba023df41bULL+pass);
      pc.cpu.memory.write_block(kFrom,expected.data(),expected.size());
      const auto start=Clock::now();
      guest.transfer();
      elapsed+=Clock::now()-start;
    }
    upload=bandwidth(payload_bytes*passes,elapsed);
    journal=board.gpu().journal_bytes();
    require(journal >= payload_bytes*passes && journal < 2*payload_bytes*passes,
            "Journal did not store the dense benchmark payload");
    // A separate logical range exercises the last exact address and the file
    // contains only submitted pages and metadata, even with the large capacity.
    std::vector<std::uint8_t> tail{0xd5,0x29,0x8a,0x28,0x69,0x07,0xb3};
    const auto kLast = GpuVram::kBytes-tail.size();
    pc.cpu.memory.write_block(kFrom,tail.data(),tail.size());
    guest.prepare(GpuPciC01::Upload,GpuVram::kTransferBytes,GpuVram::kTransferBytes,
                  kVram+1,GpuPciC01::InvalidRange);
    guest.transfer();
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
    require(volume.stats().commits==payload_bytes/command_bytes*passes+1,
            "Each PCI upload command must publish one durable transaction");
  }
  {
    boot(pc);
    PcMotherboardC06 board(pc,nullptr,{},0,path);
    TransferGuest guest(pc,board);
    guest.prepare(GpuPciC01::Download,payload_bytes,command_bytes);
    // Device reopen and index replay happen on this first read. The host OS
    // may still cache the journal. Keep that distinction explicit.
    auto start=Clock::now();
    guest.transfer();
    reopened=bandwidth(payload_bytes,Clock::now()-start);
    pc.cpu.memory.read_block(kFrom,actual.data(),actual.size());
    require(actual==expected,"Dirty or missing payload after re-opening the journal");
    constexpr unsigned open_passes=3;
    auto elapsed=Clock::duration::zero();
    for(unsigned pass=0;pass<open_passes;++pass){
      board.release_vram_cache();
      pc.cpu.memory.fill(kFrom,0,payload_bytes);
      start=Clock::now();
      guest.transfer();
      elapsed+=Clock::now()-start;
      pc.cpu.memory.read_block(kFrom,actual.data(),actual.size());
      require(actual==expected,"Open QSSD transfer lost data after VRAM cache eviction");
    }
    open=bandwidth(payload_bytes*open_passes,elapsed);
    guest.prepare(GpuPciC01::Download,kResidentTestBytes,kChunk,
                  kVram+payload_bytes-kResidentTestBytes);
    guest.transfer();
    constexpr unsigned warm_passes=64;
    start=Clock::now();
    for (unsigned pass=0;pass<warm_passes;++pass) guest.transfer();
    warm=bandwidth(kResidentTestBytes*warm_passes,Clock::now()-start);
    pc.cpu.memory.read_block(kFrom,actual.data(),kResidentTestBytes);
    require(std::equal(actual.begin(),actual.begin()+kResidentTestBytes,
                       expected.end()-kResidentTestBytes),
            "Resident PCI transfers did not return the original bytes");
  }
#if !defined(PRIMEVGC_BASELINE)
  {
    // The READ cache is empty after reopen. Check that the first raw run
    // requires one read and that corruption discovered after mount is rejected.
    reverse_windows::QssdDisk volume(path,reverse_windows::QssdDisk::Access::ReadOnly);
    const auto reads=volume.io_stats().read_calls;
    require(volume.read_bytes(kVram,actual.data(),kChunk) &&
            std::equal(actual.begin(),actual.begin()+kChunk,expected.begin()),
            "QSSD coalesced read did not reproduce the guest block");
    require(volume.io_stats().read_calls-reads==1,
            "Contiguous QSSD records must be read with one pread");
    constexpr std::uint64_t record_bytes=4096+40+16;
    const auto last_epoch=64+(passes-1)*(payload_bytes/4096*record_bytes+
                          payload_bytes/command_bytes*56);
    const auto corrupt_byte=last_epoch+3*record_bytes+40+31;
    const int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
    require(fd>=0,"Cannot open private checksum fixture");
    const auto changed=std::uint8_t(expected[3*4096+31]^1U);
    const auto written=::pwrite(fd,&changed,1,static_cast<off_t>(corrupt_byte));
    ::close(fd);
    require(written==1,"Cannot corrupt private checksum fixture");
    std::fill_n(actual.begin(),kChunk,0x5a);
    require(!volume.read_bytes(kVram,actual.data(),kChunk) &&
            std::all_of(actual.begin(),actual.begin()+kChunk,[](auto byte){return byte==0x5a;}),
            "A malformed raw run must fail before publishing data");
  }
  bool rejected=false;
  try {
    reverse_windows::QssdDisk corrupt(path);
  } catch(const std::runtime_error&) {
    rejected=true;
  }
  require(rejected,"Batched replay accepted a corrupt payload");
#endif
  std::cout<<"PRIME_GPU_C02_VRAM_PASS logical_bytes=120000000000 last_byte=119999999999 "
           <<"reopened=1 exact_bytes="<<payload_bytes<<" journal_bytes="<<journal
           <<" command_bytes="<<command_bytes<<'\n'
           <<std::fixed<<std::setprecision(3)
           <<"PRIME_GPU_C02_TRANSFER_GB_PER_S upload_durable="<<upload
           <<" download_reopened="<<reopened<<" download_resident="<<warm
           <<" download_open="<<open
           <<" gddr7_rtx5090_peak="<<kGddr7Rtx5090GbPerSecond<<'\n';
}
}

int main(int argc,char** argv) {
  try {
    const std::string_view mode=argc==2?argv[1]:"";
    if(argc>2||(argc==2&&mode!="--bulk"&&mode!="--bulk-512k"))
      throw std::invalid_argument("Usage: gpu-vram-transfer [--bulk | --bulk-512k]");
    run_transfer_test(mode.empty()?kResidentTestBytes:256U*kMiB,
                      mode=="--bulk"?GpuVram::kTransferBytes:kChunk,mode.empty()?3:1);
  } catch (const std::exception& error) {
    std::cerr<<"PRIME_GPU_C02_VRAM_FAIL "<<error.what()<<'\n';
    return 1;
  }
}
