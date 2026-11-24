#include "qssd_disk.hpp"
#include "bios_hard_disk.hpp"
#include "pc_machine.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
void need(bool ok,const char* label) {
  if(!ok)throw std::runtime_error(label);
}
template<class Action> void rejects(Action action,const char* label) {
  bool failed=false;
  try{action();}catch(const std::exception&){failed=true;}
  need(failed,label);
}

void verify_parallel_io(const std::filesystem::path& path) {
  using Disk=reverse_windows::QssdDisk;
  constexpr auto first=1'000'000'000'000ULL+17;
  constexpr auto bytes=520*Disk::kBlockSize+13;
  constexpr auto retry_at=first+1024*Disk::kBlockSize;
  Disk::create_bytes(path,Disk::kMaxLogicalBytes);
  const auto workers=std::make_shared<reverse_windows::SmpWorkers>(8);
  std::vector<std::uint8_t> payload(bytes),actual(bytes);
  std::uint32_t rng=0x3608ac9d;
  for(auto& byte:payload){
    rng^=rng<<13;
    rng^=rng>>17;
    rng^=rng<<5;
    byte=static_cast<std::uint8_t>(rng);
  }
  {
    Disk volume(path,Disk::Access::ReadWrite,workers);
    need(volume.write_bytes(first,bytes,payload.data()),"Parallel QSSD write failed");
    need(volume.read_bytes(first,actual.data(),bytes)&&actual==payload,
         "Parallel unaligned QSSD read changed the payload");
    const auto parallel=volume.parallel_io_stats();
    need(parallel.peak_workers==8&&parallel.write_transactions==1&&
         parallel.write_bytes==bytes&&parallel.read_runs==1&&parallel.read_bytes>2*1024*1024,
         "Large QSSD I/O did not use all eight workers");
    bool stored=false;
    const auto outer=workers->try_run(2,[&](unsigned lane,unsigned){
      if(!lane)stored=volume.write_bytes(retry_at,bytes,payload.data());
    });
    need(outer==2&&stored&&volume.parallel_io_stats().write_transactions==1,
         "QSSD inside an active SMP dispatch must complete using its caller");
  }
  {
    Disk volume(path,Disk::Access::ReadOnly,workers);
    need(volume.stats().commits==2&&volume.read_bytes(first,actual.data(),bytes)&&actual==payload&&
         volume.read_bytes(retry_at,actual.data(),bytes)&&actual==payload,
         "Replaying the journal changed an exact parallel or nested write");
    constexpr auto offset = (first / Disk::kBlockSize + 16) * Disk::kBlockSize;
    std::vector<std::uint8_t> reread(64 * Disk::kBlockSize);
    const auto before = volume.io_stats().read_calls;
    need(volume.read_bytes(offset, reread.data(), reread.size()) &&
         std::equal(reread.begin(), reread.end(), payload.begin() + (offset - first)),
         "Reopened contiguous RAW read changed bytes");
    const auto cold = volume.io_stats().read_calls - before;
    const auto hits = volume.cache_stats().hits;
    std::fill(reread.begin(), reread.end(), 0);
    need(volume.read_bytes(offset, reread.data(), reread.size()) &&
         std::equal(reread.begin(), reread.end(), payload.begin() + (offset - first)),
         "Cached contiguous RAW read changed bytes");
    const auto warm = volume.io_stats().read_calls - before - cold;
    need(cold == 1 && warm == 1 && volume.cache_stats().hits - hits == 64 &&
         volume.cache_stats().resident_bytes <= 512 * 1024, "RAW reread did not verify the current record");
    std::cout << "PRIME_QSSD_RAW_REREAD_PASS bytes=" << reread.size()
              << " cold_reads=" << cold << " warm_reads=" << warm << '\n';
  }
  {
    Disk volume(path, Disk::Access::ReadOnly);
    constexpr auto offset = (first / Disk::kBlockSize + 16) * Disk::kBlockSize;
    std::vector<std::uint8_t> run(256 * Disk::kBlockSize);
    const auto before = volume.io_stats().read_calls;
    need(volume.read_bytes(offset, run.data(), run.size()) &&
         std::equal(run.begin(), run.end(), payload.begin() + (offset - first)) &&
         volume.io_stats().read_calls - before == 1 && volume.peak_raw_read_blocks() == 256,
         "A serial 256-block RAW run must read exactly one journal chunk");
  }
  std::filesystem::remove(path);
  std::cout<<"PRIME_QSSD_PARALLEL_PASS offset="<<first<<" bytes="<<bytes
           <<" workers=8 nested_serial=1 exact=1\n";
}
}

int main() {
  const auto root=std::filesystem::temp_directory_path()/
      ("reverse-qssd-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path=root.string()+".rqvd";
  const auto corrupt=root.string()+"-corrupt.rqvd";
  const auto enormous=root.string()+"-8pib.rqvd";
  try {
    reverse_windows::QssdDisk::create(path,64);
    need(std::filesystem::file_size(path)==64,"sparse disk allocated physical capacity at creation");
    rejects([&]{reverse_windows::QssdDisk::create(path,64);},"QSSD overwrote an existing disk");
    std::array<std::uint8_t,4096> unique{},roundtrip{},zero{};
    std::uint32_t rng=0x8128acd1;
    for(auto& byte:unique){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;byte=static_cast<std::uint8_t>(rng);}
    {
      reverse_windows::QssdDisk disk(path);
      need(disk.bytes()==64*reverse_windows::QssdDisk::kGiB,"logical capacity mismatch");
      need(disk.read(128,8,roundtrip.data()),"empty-sector read failed");
      need(roundtrip==zero,"unwritten bytes not zero");
      need(disk.write(128,8,unique.data()),"first RAW chunk write failed");
      need(disk.write(256,8,unique.data()),"deduplicated chunk write failed");
      need(disk.read(128,8,roundtrip.data())&&roundtrip==unique,"RAW data mismatch");
      roundtrip.fill(0);
      need(disk.read(256,8,roundtrip.data())&&roundtrip==unique,"referenced bytes mismatch");
      const auto last=disk.sectors()-8;
      need(disk.write(last,8,unique.data())&&disk.read(last,8,roundtrip.data())&&roundtrip==unique,
           "sparse high-address data mismatch");
      need(!disk.write(disk.sectors(),1,unique.data()),"out-of-range sector write accepted");
      need(disk.write(128,8,zero.data()),"clear to sparse ZERO failed");
      roundtrip.fill(0xff);
      need(disk.read(128,8,roundtrip.data())&&roundtrip==zero,"zero overwrite mismatch");
      std::array<std::uint8_t,4096> pattern{};
      for(std::size_t i=0;i<pattern.size();++i)pattern[i]=static_cast<std::uint8_t>(i%3+1);
      need(disk.write(512,8,pattern.data()),"pattern compression write failed");
      roundtrip.fill(0);
      need(disk.read(512,8,roundtrip.data())&&roundtrip==pattern,"pattern decode mismatch");
      const auto stats=disk.stats();
      need(stats.deduplicated_blocks>=1&&stats.zero_blocks>=1&&stats.pattern_blocks>=1,
           "QSSD exact dedup/zero/pattern not activated");
      need(stats.commits==5&&stats.qssd_serial>=6,"QSSD StateBus not recording guest transactions");
      need(stats.physical_bytes<8192,"QSSD storage failed to avoid repeated physical blocks");
      const auto syncs=disk.io_stats().sync_calls;
      need(syncs==stats.commits&&disk.flush()&&disk.flush()&&disk.io_stats().sync_calls==syncs,
           "Clean flush resynced an already durable QSSD journal");
      std::cout<<"REVERSE_QSSD_STORAGE_PASS logical="<<stats.logical_bytes
               <<" physical="<<stats.physical_bytes
               <<" commits="<<stats.commits
               <<" referenced="<<stats.deduplicated_blocks<<"\n";
    }
    // A torn trailing record is ignored: only a validated COMMIT reaches StateBus.
    {
      int fd=::open(path.c_str(),O_WRONLY|O_APPEND);
      need(fd>=0,"cannot simulate interrupted transaction");
      const std::array<char,9> partial{'Q','B','L','K','b','a','d','!','!'};
      need(::write(fd,partial.data(),partial.size())==static_cast<ssize_t>(partial.size()),
           "cannot write torn-tail fixture");
      ::close(fd);
    }
    {
      reverse_windows::QssdDisk reopen(path);
      need(reopen.stats().commits==5,"uncommitted bytes became visible");
      need(reopen.read(256,8,roundtrip.data())&&roundtrip==unique,"QSSD reboot lost data");
      need(reopen.read(128,8,roundtrip.data())&&roundtrip==zero,"QSSD reboot restored obsolete data");
      need(reopen.stats().physical_bytes==std::filesystem::file_size(path),
           "QSSD did not prune incomplete tail");
      // Exercise BIOS guest machine instructions through new QSSD sector backend.
      cosmic_pc::PcMachine pc(2048ULL*1024*1024,256ULL*1024*1024);
      pc.boot_demo();
      reverse_windows::BiosHardDisk bios(pc,reopen);
      const std::array<std::uint8_t,16> dap{
          0x10,0x00,0x01,0x00,0x00,0x90,0x00,0x00,
          0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
      pc.cpu.memory.write_block(0x8100,dap.data(),dap.size());
      const std::array<std::uint8_t,31> guest{
          0xb8,0xef,0xbe, // MOV AX, BEEF
          0xa3,0x00,0x90, // MOV [9000], AX
          0xbe,0x00,0x81, // MOV SI,8100
          0xb2,0x80,      // MOV DL,80
          0xb4,0x43,      // MOV AH,43
          0xcd,0x13,      // INT 13 EDD write
          0xb8,0x00,0x00, // clear AX
          0xa3,0x00,0x90, // zero RAM
          0xbe,0x00,0x81, // MOV SI,8100
          0xb2,0x80,      // MOV DL,80
          0xb4,0x42,      // MOV AH,42
          0xcd,0x13,      // INT 13 EDD read
          0xf4           // HLT
      };
      pc.cpu.memory.write_block(0x7c00,guest.data(),guest.size());
      pc.cpu.reset(0,0x7c00);
      pc.cpu.set_reg16(cosmic_pc::Cpu::ESP,0x7b00);
      pc.run(200);
      need(pc.cpu.halted&&!pc.cpu.fault&&!pc.cpu.flag(cosmic_pc::Cpu::CF),
           "x86 guest BIOS QSSD INT13 read-write failed");
      need(pc.cpu.memory[0x9000]==0xef&&pc.cpu.memory[0x9001]==0xbe,
           "x86 BIOS QSSD roundtrip did not restore RAM");
    }
    {
      reverse_windows::QssdDisk reboot(path);
      std::array<std::uint8_t,512> sector{};
      need(reboot.read(7,1,sector.data())&&sector[0]==0xef&&sector[1]==0xbe,
           "real x86 guest write did not persist");
      need(reboot.io_stats().sync_calls==0&&reboot.flush()&&reboot.flush()&&
           reboot.io_stats().sync_calls==1,
           "An existing QSSD image must keep the first flush barrier");
      std::array<std::uint8_t,512> mbr{};
      mbr[0]=0xb8;mbr[1]=0x34;mbr[2]=0x12; // MOV AX,1234
      mbr[3]=0xa3;mbr[4]=0x00;mbr[5]=0x91; // MOV [9100],AX
      mbr[6]=0xf4;                       // HLT
      mbr[510]=0x55;mbr[511]=0xaa;
      need(reboot.write(0,1,mbr.data()),"cannot save QSSD MBR");
      cosmic_pc::PcMachine pc(2048ULL*1024*1024,256ULL*1024*1024);
      pc.mount([&reboot](std::uint64_t offset,std::uint8_t* dest,std::size_t size){
        return reboot.read_bytes(offset,dest,size);
      },reboot.bytes(),"raw");
      reverse_windows::BiosHardDisk bios(pc,reboot);
      pc.run(100);
      need(!pc.cpu.fault&&pc.cpu.halted&&
           pc.cpu.memory[0x9100]==0x34&&pc.cpu.memory[0x9101]==0x12,
           "real x86 QSSD persistent MBR boot failed");
    }
    // Simultaneous readers must observe an entire committed two-block revision,
    // never a mixture of the two sides of an in-flight guest transaction.
    {
      reverse_windows::QssdDisk volume(path);
      std::array<std::uint8_t,8192> a{},b{},seen{};
      a.fill(0x15);b.fill(0xe7);
      need(volume.write(2048,16,a.data()),"cannot seed atomic multi-block transaction");
      std::atomic<bool> failed{false};
      std::thread reader([&]{
        for(unsigned i=0;i<500;++i){
          if(!volume.read(2048,16,seen.data())||(seen!=a&&seen!=b)){
            failed=true;break;
          }
        }
      });
      bool written=true;
      for(unsigned i=0;i<32;++i)
        if(!volume.write(2048,16,(i%2)?a.data():b.data())){written=false;break;}
      reader.join();
      need(written&&!failed,"QSSD concurrent reader saw a torn transaction");
      std::cout<<"REVERSE_QSSD_CONCURRENT_SNAPSHOT_PASS commits="<<volume.stats().commits<<"\n";
    }
    using reverse_windows::QssdDisk;
    constexpr auto max_bytes=QssdDisk::kMaxLogicalBytes;
    constexpr auto last_sector=(max_bytes-QssdDisk::kBlockSize)/QssdDisk::kSectorSize;
    // The two block indices differ only in bits above the old 32-bit index.
    constexpr auto lower_sector=((1ULL<<32)-1)*QssdDisk::kBlockSize/QssdDisk::kSectorSize;
    std::array<std::uint8_t,QssdDisk::kBlockSize> upper{},lower{},actual{};
    upper.fill(0x5a);
    lower.fill(0xa5);
    QssdDisk::create_bytes(enormous,max_bytes);
    {
      QssdDisk huge(enormous);
      need(huge.bytes()==max_bytes,"8 PiB logical capacity differs from the header");
      need(huge.write(lower_sector,8,lower.data())&&huge.write(last_sector,8,upper.data()),
           "8 PiB disk write failed");
      need(huge.read(lower_sector,8,actual.data())&&actual==lower,
           "8 PiB tail write aliased the low 32-bit block");
      need(huge.read(last_sector,8,actual.data())&&actual==upper,
           "8 PiB tail data incorrect");
      need(huge.stats().physical_bytes<4096&&huge.checkpoint(),
           "Sparse disk used excessive storage or failed to checkpoint");
    }
    need(QssdDisk::compact(enormous).checkpoint_saved,"8 PiB compaction lost its checkpoint");
    {
      QssdDisk huge(enormous,QssdDisk::Access::ReadOnly);
      need(huge.recovery_stats().used_checkpoint&&huge.bytes()==max_bytes,
           "8 PiB index was not recovered from the compacted checkpoint");
      need(huge.read(lower_sector,8,actual.data())&&actual==lower&&
           huge.read(last_sector,8,actual.data())&&actual==upper,
           "Compaction or checkpoint recovery changed a high-address block");
    }
    std::filesystem::remove(enormous);
    std::filesystem::remove(enormous+".rqix");
    rejects([&]{QssdDisk::create_bytes(enormous,max_bytes+QssdDisk::kBlockSize);},
            "QSSD accepted capacity above its supported range");
    verify_parallel_io(enormous);
    // Full journal corruption MUST be rejected, not silently reinterpreted.
    reverse_windows::QssdDisk::create(corrupt,1);
    {
      reverse_windows::QssdDisk volume(corrupt);
      need(volume.write(16,8,unique.data()),"corruption fixture write failed");
    }
    {
      int fd=::open(corrupt.c_str(),O_RDWR);
      need(fd>=0,"cannot modify corruption fixture");
      std::uint8_t byte{};
      need(::pread(fd,&byte,1,64+40+8)==1,"cannot read corruption fixture byte");
      byte^=1;
      need(::pwrite(fd,&byte,1,64+40+8)==1,"cannot corrupt fixture");
      ::close(fd);
    }
    rejects([&]{reverse_windows::QssdDisk invalid(corrupt);},
            "tampered complete QSSD record was accepted");
    std::filesystem::remove(path);
    std::filesystem::remove(corrupt);
    std::filesystem::remove(enormous);
    std::cout<<"REVERSE_QSSD_X86_DISK_REBOOT_PASS Windows10_installed=false\n";
    return 0;
  }catch(const std::exception& error){
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    std::filesystem::remove(corrupt,ignored);
    std::filesystem::remove(enormous,ignored);
    std::filesystem::remove(enormous+".rqix",ignored);
    std::cerr<<"REVERSE_QSSD_TEST_FAIL: "<<error.what()<<"\n";
    return 1;
  }
}
