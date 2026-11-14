#include "bios_hard_disk.hpp"
#include "c06_motherboard.hpp"
#include "qssd_disk.hpp"
#include "storage_profile.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::BiosHardDisk;
using reverse_windows::PcMotherboardC06;
using reverse_windows::QssdDisk;
using Clock=std::chrono::steady_clock;

constexpr unsigned sector_size=512, command_sectors=125, units=16;
constexpr unsigned unit_bytes=command_sectors*sector_size;
constexpr std::uint64_t below_lba28=0x1000, above_lba28=0x10001000;
constexpr std::uint32_t source=0x20000,target=0x40000;

void require(bool condition,const std::string& reason) {
  if(!condition)throw std::runtime_error(reason);
}
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    auto name=(std::filesystem::temp_directory_path()/"prime-qssd-XXXXXX").string();
    if(!::mkdtemp(name.data()))throw std::runtime_error("Cannot create fixture directory");
    path=name;
  }
  ~TempDir(){std::error_code error;std::filesystem::remove_all(path,error);}
};

std::vector<std::uint8_t> data_for(unsigned unit) {
  const auto block_size=QssdDisk::kBlockSize;
  std::array<std::uint8_t,QssdDisk::kBlockSize> reused{};
  std::array<std::uint8_t,13> pattern{};
  std::uint64_t rng=0x8fcb49a03d1423f1ULL ^ unit;
  const auto next=[&]{
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
    return std::uint8_t(rng>>31);
  };
  for(auto& byte:reused)byte=next();
  for(auto& byte:pattern)byte=next();
  std::vector<std::uint8_t> result(unit_bytes,0);
  for(std::size_t i=0;i<result.size();++i){
    switch(i/block_size%5){
      case 0: case 1: result[i]=reused[i%block_size];break;
      case 2: result[i]=pattern[(i%block_size)%pattern.size()];break;
      case 3: break;
      case 4: result[i]=next();break;
    }
  }
  return result;
}

void put(Cpu& cpu,unsigned address,const std::vector<std::uint8_t>& code) {
  cpu.memory.write_block(address,code.data(),code.size());
}
void run(PcMachine& pc,unsigned entry,unsigned steps=48000,bool irq=false) {
  pc.cpu.eip=entry;pc.cpu.halted=false;
  pc.cpu.eflags=irq?2|Cpu::IF:2;
  pc.run(steps);
  if(pc.cpu.fault)throw std::runtime_error(pc.cpu.fault->describe());
  require(pc.cpu.halted,"Storage guest failed to reach HLT");
}
void edd(PcMachine& pc,bool write,std::uint64_t lba,std::uint32_t buffer,unsigned count) {
  auto& cpu=pc.cpu;
  constexpr auto code=0xa000,dap=0x8100;
  cpu.write16(dap,0x10);
  cpu.write16(dap+2,std::uint16_t(count));
  cpu.write16(dap+4,0);cpu.write16(dap+6,std::uint16_t(buffer>>4));
  cpu.write64(dap+8,lba);
  put(cpu,code,{0xbe,0,0x81,0xb2,0x80,0xb4,std::uint8_t(write?0x43:0x42),0xcd,0x13,0xf4});
  run(pc,code,5);
  require(!cpu.flag(Cpu::CF)&&cpu.reg8(4)==0&&cpu.read16(dap+2)==count,
          "INT 13h EDD transfer failed");
}
void boot_protected(PcMachine& pc) {
  pc.boot_long_mode_demo();
  for(unsigned i=0;i<3000;++i){
    const auto r=pc.cpu.step();
    if(r==cosmic_pc::StepResult::Faulted)throw std::runtime_error(pc.cpu.fault->describe());
    if(std::string_view(pc.cpu.mode_name())=="protected32"&&pc.cpu.segments[Cpu::SS]==0x10)return;
  }
  throw std::runtime_error("Cannot enter protected-mode ATA fixture");
}

struct Pio {
  std::vector<std::uint8_t> code;
  void out(std::uint16_t port,std::uint8_t value) {
    code.insert(code.end(),{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xb0,value,0xee});
  }
  void in8(std::uint16_t port,unsigned dest) {
    code.insert(code.end(),{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xec,0xa2});
    dword(dest);
  }
  void dword(std::uint32_t value) {
    for(unsigned i=0;i<4;++i)code.push_back(std::uint8_t(value>>(i*8)));
  }
  void task(std::uint64_t lba,unsigned sectors,bool ext) {
    out(0x1f6,0xe0|(ext?0:std::uint8_t(lba>>24)&15));
    if(ext){
      out(0x1f2,std::uint8_t(sectors>>8));
      for(unsigned i=0;i<3;++i)out(std::uint16_t(0x1f3+i),std::uint8_t(lba>>(24+8*i)));
    }
    out(0x1f2,std::uint8_t(sectors));
    for(unsigned i=0;i<3;++i)out(std::uint16_t(0x1f3+i),std::uint8_t(lba>>(8*i)));
  }
  void data(bool read,unsigned address,unsigned bytes) {
    code.insert(code.end(),{0xfc,0x66,0xba,0xf0,0x01,0xb9});dword(bytes/4);
    code.push_back(read?0xbf:0xbe);dword(address);
    code.insert(code.end(),{0xf3,std::uint8_t(read?0x6d:0x6f)}); // REP INSD / OUTSD.
  }
  void execute(PcMachine& pc,unsigned steps=48000,bool irq=false) {
    code.push_back(0xf4);put(pc.cpu,0xa000,code);run(pc,0xa000,steps,irq);
  }
};

void ata(PcMachine& pc,bool write,std::uint64_t lba,unsigned buffer,unsigned count,
         bool multiple,bool extended=true,unsigned transferred=0,bool irq=false) {
  Pio p;
  p.task(lba,count,extended);
  p.out(0x1f7,multiple?(write?(extended?0x39:0xc5):(extended?0x29:0xc4)):
                      (write?(extended?0x34:0x30):(extended?0x24:0x20)));
  p.data(!write,buffer,transferred?transferred:count*sector_size);
  p.in8(0x1f7,0x9400);
  p.execute(pc,transferred?std::min(48000U,transferred+80):count*128+80,irq);
  require(pc.cpu.read8(0x9400)==(transferred?0x58:0x50),"Unexpected ATA status after PIO");
}
void set_multiple(PcMachine& pc,unsigned count,std::uint8_t status=0x50) {
  Pio p;p.out(0x1f2,std::uint8_t(count));p.out(0x1f7,0xc6);p.in8(0x1f7,0x9400);
  p.execute(pc,20);
  require(pc.cpu.read8(0x9400)==status,"SET MULTIPLE MODE accepted an invalid count");
}
void guest_equal(const PcMachine& pc,unsigned address,const std::vector<std::uint8_t>& expected) {
  std::vector<std::uint8_t> observed(expected.size());
  pc.cpu.memory.read_block(address,observed.data(),observed.size());
  require(observed==expected,"Guest disk payload differs from fixture");
}
std::uint64_t ns(Clock::duration elapsed) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}
void xor_large_revision(const std::filesystem::path& path) {
  constexpr auto bytes=QssdDisk::kBlockSize;
  constexpr auto extent=16384*QssdDisk::kGiB; // Full supported 16 TiB logical address range.
  constexpr unsigned blocks=512;
  QssdDisk::create(path,16384);
  using Block=std::array<std::uint8_t,bytes>;
  std::vector<Block> expected(blocks);
  std::array<std::uint64_t,blocks> lbas{};
  std::uint64_t rng=0xa1b2c3d4e5f60719ULL;
  const auto random=[&]{
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
    return std::uint8_t(rng>>39);
  };
  for(unsigned i=0;i<blocks;++i){
    lbas[i]=(extent/bytes-1)*i/(blocks-1)*(bytes/512);
    auto& data=expected[i];
    if(i%2==0){
      const unsigned period=i/2%32+1;
      for(unsigned j=0;j<period;++j)data[j]=random();
      for(unsigned j=period;j<data.size();++j)data[j]=data[j%period];
    }else{
      for(auto& byte:data)byte=random();
    }
  }
  const auto reduce=[](std::uint64_t word,const Block& block){
    for(unsigned j=0;j<block.size();++j)word^=std::uint64_t(block[j])<<((j%8)*8);
    return word;
  };
  std::uint64_t expected_xor=0;
  {
    QssdDisk disk(path);
    for(unsigned i=0;i<blocks;++i){
      require(disk.write(lbas[i],8,expected[i].data()),"Sparse large-volume write failed");
      expected_xor=reduce(expected_xor,expected[i]);
      require(disk.xor_summary().word==expected_xor,"Published XOR does not match committed block");
    }
    for(unsigned i=0;i<blocks;i+=8){
      expected_xor=reduce(expected_xor,expected[i]);
      expected[i].fill(0);
      require(disk.write(lbas[i],8,expected[i].data()),"Sparse large-volume erase failed");
      require(disk.xor_summary().word==expected_xor,"Zero overwrite did not remove prior XOR");
    }
    require(disk.checkpoint(),"Large-volume checkpoint failed");
  }
  QssdDisk disk(path,QssdDisk::Access::ReadOnly);
  require(disk.recovery_stats().used_checkpoint&&disk.bytes()==extent,
          "Large-volume query did not recover the exact checkpoint");
  const auto stats=disk.io_stats();
  const auto result=disk.xor_summary();
  require(result.revision==disk.stats().qssd_serial&&result.nonzero_blocks==448,
          "Large-volume summary lost its StateBus revision or live blocks");
  const auto direct=[&](){
    std::uint64_t word=0;
    Block block;
    for(unsigned i=0;i<blocks;++i){
      require(disk.read(lbas[i],8,block.data())&&block==expected[i],"Large-volume byte oracle failed");
      word=reduce(word,block);
    }
    return word;
  };
  require(result.word!=0&&result.word==expected_xor&&result.word==direct(),
          "QSSD root XOR differs from direct projection");
  unsigned parity=0;
  for(unsigned bit=0;bit<64;++bit)parity^=unsigned((result.word>>bit)&1U);
  require(result.parity==parity,"QSSD root parity differs from byte oracle");
  constexpr unsigned repeats=10000;
  auto started=Clock::now();
  std::uint64_t accumulator=0;
  const auto physical=disk.io_stats();
  for(unsigned i=0;i<repeats;++i)accumulator+=disk.xor_summary().word;
  const auto root_ns=ns(Clock::now()-started)/repeats;
  require(accumulator==result.word*repeats&&disk.io_stats().read_calls==physical.read_calls&&
          disk.io_stats().write_calls==stats.write_calls,"Summary unexpectedly did journal I/O");
  started=Clock::now();
  require(direct()==result.word,"Timed direct path differs");
  const auto direct_ns=ns(Clock::now()-started);
  std::cout<<"PRIME_C06_QSSD_XOR_PASS logical_bytes="<<extent<<" indexed_blocks="<<result.nonzero_blocks
           <<" root_ns="<<root_ns<<" sparse_scan_ns="<<direct_ns<<" root_pread=0 xor_oracle=exact\n";
}
struct Result {
  std::uint64_t write_ns=0,read_ns=0,reopen_ns=0;
  std::uint64_t pio_write_bytes=0,pio_read_bytes=0;
  QssdDisk::Stats written{};
  QssdDisk::IoStats write_io{},read_io{};
};
enum class Route { Bios, Sectors, Multiple };

Result measure(const std::filesystem::path& path,Route route) {
  QssdDisk::create(path,256);
  const bool bios_route=route==Route::Bios;
  const bool multiple=route==Route::Multiple;
  const auto start_lba=bios_route?below_lba28:above_lba28;
  Result out;
  for(const bool write:{true,false}){
    const auto opened=Clock::now();
    QssdDisk disk(path);
    if(!write)out.reopen_ns=ns(Clock::now()-opened);
    PcMachine pc(16U<<20);
    if(bios_route)pc.boot_demo();
    else boot_protected(pc);
    BiosHardDisk bios(pc,disk);
    PcMotherboardC06 board(pc,&disk);
    if(multiple){
      set_multiple(pc,16);
      pc.enable_boot_acceleration();
    }
    const auto initial=disk.io_stats();
    for(unsigned unit=0;unit<units;++unit){
      const auto payload=data_for(unit);
      if(write)pc.cpu.memory.write_block(source,payload.data(),payload.size());
      const auto lba=start_lba+unit*128;
      const auto started=Clock::now();
      const auto ram=write?source:target;
      if(bios_route)edd(pc,write,lba,ram,command_sectors);
      else ata(pc,write,lba,ram,command_sectors,multiple);
      if(write)out.write_ns+=ns(Clock::now()-started);
      else out.read_ns+=ns(Clock::now()-started);
      if(!write)guest_equal(pc,target,payload);
    }
    auto io=disk.io_stats();
    io.read_calls-=initial.read_calls;io.read_bytes-=initial.read_bytes;io.read_ns-=initial.read_ns;
    if(write){out.write_io=io;out.written=disk.stats();}
    else out.read_io=io;
    const auto groups=bios_route?units:multiple?units*8:units*command_sectors;
    const auto& ata=board.attached_ata()->backend();
    const auto batched=board.attached_ata()->counters().batched_pio_bytes;
    require(multiple?batched>=units*unit_bytes-4096:batched==0,
            "The PIO fast path did not match the selected control mode");
    if(write)out.pio_write_bytes=batched;
    else out.pio_read_bytes=batched;
    require(bios_route?(bios.transfers().bulk_copies==units):
                       (write?ata.writes:ata.reads)==groups,
            "The guest did not exercise the expected backend transfer sizes");
    require(!write||(out.written.commits==groups&&io.sync_calls==groups&&io.write_calls==groups),
            "Every acknowledged QSSD revision needs one durable append");
  }
  return out;
}

void recovery_and_compatibility(const std::filesystem::path& path) {
  QssdDisk::create(path,256);
  const auto payload=data_for(3);
  std::uint64_t committed_end=0;
  {
    QssdDisk disk(path);
    PcMachine pc(16U<<20);boot_protected(pc);
    PcMotherboardC06 board(pc,&disk);
    Pio irq;
    irq.out(0x20,0x11);irq.out(0x21,0x20);irq.out(0x21,4);irq.out(0x21,1);irq.out(0x21,0xfb);
    irq.out(0xa0,0x11);irq.out(0xa1,0x28);irq.out(0xa1,2);irq.out(0xa1,1);irq.out(0xa1,0xbf);
    auto& cpu=pc.cpu;
    cpu.regs[Cpu::ESP]=0x190000;cpu.idtr={0x7ff,0x15000};
    cpu.write64(0x15000+0x2e*8,0x00018e0000083000ULL);
    put(cpu,0x13000,{0x50,0x52,0xff,0x05,4,0x94,0,0,0x66,0xba,0xf7,1,0xec,
                     0xb0,0x20,0xe6,0xa0,0xe6,0x20,0x5a,0x58,0xcf});
    irq.execute(pc,80);
    set_multiple(pc,16);set_multiple(pc,3,0x51);
    Pio id;
    id.out(0x1f7,0xec);id.data(true,target,512);id.execute(pc,400);
    require(cpu.read16(target+2*47)==0x8010&&cpu.read16(target+2*59)==0x0110,
            "IDENTIFY did not publish the negotiated multiple count");
    Pio drain;drain.execute(pc,60,true);
    cpu.write32(0x9404,0);
    pc.enable_boot_acceleration();
    cpu.memory.write_block(source,payload.data(),payload.size());
    ata(pc,true,below_lba28,source,19,true,false,0,true);
    require(cpu.read32(0x9404)==2&&disk.stats().commits==2,
            "ATA LBA28 IRQ group mismatch: ISR="+std::to_string(cpu.read32(0x9404))+
            " board="+std::to_string(board.evidence().ata_irqs)+
            " commits="+std::to_string(disk.stats().commits));
    cpu.write32(0x9404,0);
    ata(pc,false,below_lba28,target,19,true,false,0,true);
    require(cpu.read32(0x9404)==2,"ATA read groups changed the IRQ boundary");
    guest_equal(pc,target,{payload.begin(),payload.begin()+19*512});
    require(board.attached_ata()->counters().batched_pio_bytes>0,
            "IRQ boundary fixture did not use the PIO fast path");
    ata(pc,true,above_lba28,source,16,true,true,3*512);
    Pio reset;reset.out(0x3f6,4);reset.out(0x3f6,0);
    reset.task(above_lba28,16,true);reset.out(0x1f7,0x29);
    reset.in8(0x1f7,0x9400);reset.in8(0x1f1,0x9401);reset.execute(pc,120);
    require(cpu.read16(0x9400)==0x0451&&disk.stats().commits==2,
            "ATA reset published an incomplete group or retained the prior negotiation");
    committed_end=disk.stats().physical_bytes;
    require(disk.checkpoint(),"QSSD checkpoint before recovery failed");
    require(disk.write(above_lba28,16,payload.data()),"Cannot stage recovery fixture");
  }
  // Keep valid data records from the final transaction but tear its commit.
  const auto tail=std::filesystem::file_size(path);
  require(tail>committed_end+56,"Recovery fixture did not have a complete append");
  std::filesystem::resize_file(path,tail-4);
  {
    QssdDisk disk(path);
    require(disk.recovery_stats().used_checkpoint&&disk.stats().commits==2&&
            disk.stats().physical_bytes==committed_end&&std::filesystem::file_size(path)==committed_end,
            "The incomplete transaction became visible or changed the committed cutoff");
    std::vector<std::uint8_t> zeros(16*512);
    std::vector<std::uint8_t> stored(zeros.size(),0xff);
    require(disk.read(above_lba28,16,stored.data())&&stored==zeros,"Uncommitted data survived reopen");
    require(disk.read(below_lba28,16,stored.data())&&std::equal(stored.begin(),stored.end(),payload.begin()),
            "Previously acknowledged ATA group did not survive the restart");
    require(disk.write(above_lba28,16,payload.data()),"Cannot reuse the truncated journal offset");
  }
  const auto compacted=QssdDisk::compact(path);
  QssdDisk final_disk(path);
  std::array<std::uint8_t,16*512> replayed{};
  require(compacted.replaced&&final_disk.read(above_lba28,16,replayed.data())&&
          std::equal(replayed.begin(),replayed.end(),payload.begin()),
          "QSSD offline compaction lost data from an ATA group");
  std::cout<<"PRIME_C06_QSSD_RECOVERY_PASS partial_pio=1 torn_commit=1 irq_groups=2 checkpoint=1 compact=1\n";
}

void bios_ranges_and_cross_route(const std::filesystem::path& path) {
  QssdDisk::create(path,256);
  QssdDisk disk(path);
  const auto payload=data_for(3);
  PcMachine pc(16U<<20);pc.boot_demo();
  BiosHardDisk bios(pc,disk);
  PcMotherboardC06 board(pc,&disk);
  pc.cpu.memory.write_block(0xfff00,payload.data(),256);
  pc.cpu.memory.write_block(0,payload.data()+256,256);
  pc.cpu.a20_enabled=false;
  edd(pc,true,below_lba28,0xfff00,1);
  require(!bios.transfers().bulk_copies,"A20 wrap took the contiguous RAM path");
  boot_protected(pc);
  ata(pc,false,below_lba28,target,1,false,false);
  guest_equal(pc,target,{payload.begin(),payload.begin()+512});
  set_multiple(pc,16);
  pc.cpu.memory.write_block(source,payload.data(),unit_bytes);
  ata(pc,true,above_lba28,source,16,true);
  pc.boot_demo();edd(pc,false,above_lba28,target,16);
  guest_equal(pc,target,{payload.begin(),payload.begin()+16*512});

  // An exception after a bulk copy must undo it and surrounding byte writes.
  auto& cpu=pc.cpu;
  const auto prior=cpu.interrupt_hook;
  cpu.interrupt_hook=[&](Cpu& guest,std::uint8_t number){
    if(number!=0x66)return prior(guest,number);
    guest.write8(target,0x51);
    require(guest.try_write_ram_block(target+1,payload.data()+50,4097),"Guest RAM range rejected");
    guest.write8(target+1,0xa5);
    require(guest.try_write_ram_block(target,payload.data()+100,4096),"Second guest RAM range rejected");
    guest.fail("rollback fixture");
  };
  put(cpu,0xa000,{0xcd,0x66});
  cpu.eip=0xa000;cpu.halted=false;
  require(cpu.step()==cosmic_pc::StepResult::Faulted,"Guest rollback fixture did not fault");
  guest_equal(pc,target,{payload.begin(),payload.begin()+16*512});
  std::cout<<"PRIME_C06_BIOS_ATA_SHARED_PASS a20_wrap=1 journal_rollback=1 opposite_routes=1\n";
}

void print(const char* name,const Result& r) {
  std::cout<<"PRIME_C06_QSSD_ROUTE mode="<<name<<" guest_bytes="<<units*unit_bytes
           <<" write_ns="<<r.write_ns<<" read_ns="<<r.read_ns<<" reopen_ns="<<r.reopen_ns
           <<" commits="<<r.written.commits<<" journal_bytes="<<r.written.physical_bytes
           <<" raw="<<r.written.raw_blocks<<" references="<<r.written.deduplicated_blocks
           <<" patterns="<<r.written.pattern_blocks<<" zeros="<<r.written.zero_blocks
           <<" sync_calls="<<r.write_io.sync_calls<<" pwrite_calls="<<r.write_io.write_calls
           <<" pio_write_bytes="<<r.pio_write_bytes<<" pio_read_bytes="<<r.pio_read_bytes
           <<" write_syscall_ns="<<r.write_io.write_ns<<" sync_ns="<<r.write_io.sync_ns
           <<" pread_calls="<<r.read_io.read_calls<<" pread_bytes="<<r.read_io.read_bytes
           <<" read_syscall_ns="<<r.read_io.read_ns<<'\n';
}
}  // namespace

int main() {
  try{
    TempDir dir;
    const auto bios=measure(dir.path/"bios.rqvd",Route::Bios);
    const auto sectors=measure(dir.path/"sectors.rqvd",Route::Sectors);
    const auto multiple=measure(dir.path/"multiple.rqvd",Route::Multiple);
    require(multiple.written.physical_bytes<sectors.written.physical_bytes&&
            multiple.written.deduplicated_blocks>0&&bios.written.deduplicated_blocks>0,
            "Grouping and exact QSSD content references did not reduce journal work");
    print("bios",bios);print("ata_sector",sectors);print("ata_multiple",multiple);
    bios_ranges_and_cross_route(dir.path/"cross-route.rqvd");
    recovery_and_compatibility(dir.path/"recovery.rqvd");
    xor_large_revision(dir.path/"xor.rqvd");
    return 0;
  }catch(const std::exception& error){
    std::cerr<<"PRIME_C06_QSSD_FAIL "<<error.what()<<'\n';
    return 1;
  }
}
