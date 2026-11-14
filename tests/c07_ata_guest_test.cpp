#include "c06_motherboard.hpp"
#include "c07_ata.hpp"
#include "qssd_disk.hpp"
#include "bios_hard_disk.hpp"
#include "pc_machine.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::QssdDisk;
using reverse_windows::PcMotherboardC06;
std::uint64_t checks=0;
void need(bool v,const char* m){++checks;if(!v)throw std::runtime_error(m);}
void boot32(PcMachine& pc){
  pc.boot_long_mode_demo();
  for(unsigned i=0;i<11000;++i) {
    const auto s=pc.cpu.step();
    if(s==cosmic_pc::StepResult::Faulted)
      throw std::runtime_error(pc.cpu.fault->describe());
    if(std::string(pc.cpu.mode_name())=="protected32" &&
       pc.cpu.segments[Cpu::SS]==0x10)return;
  }
  throw std::runtime_error("C07 guest did not reach real protected32");
}
void gate(Cpu& cpu,std::uint8_t vector,std::uint32_t at){
  cpu.idtr={0x1000,0x15000};
  const auto raw=(std::uint64_t(at&0xffffU))|
      (std::uint64_t(8)<<16)|(std::uint64_t(0x8e)<<40)|
      (std::uint64_t(at>>16)<<48);
  cpu.write64(cpu.idtr.base+8U*vector,raw);
}
void ins(std::vector<std::uint8_t>& p,std::initializer_list<std::uint8_t> bytes){
  p.insert(p.end(),bytes.begin(),bytes.end());
}
void imm32(std::vector<std::uint8_t>& p,std::uint32_t x){
  for(unsigned i=0;i<4;++i)p.push_back(std::uint8_t(x>>(8*i)));
}
void out8(std::vector<std::uint8_t>& p,std::uint16_t port,std::uint8_t x){
  ins(p,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xb0,x,0xee});
}
void out32(std::vector<std::uint8_t>& p,std::uint16_t port,std::uint32_t x){
  p.push_back(0xb8);imm32(p,x);
  ins(p,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xef});
}
void in32_store(std::vector<std::uint8_t>& p,std::uint16_t port,std::uint32_t memory){
  ins(p,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xed,0xa3});
  imm32(p,memory);
}
void in8_store(std::vector<std::uint8_t>& p,std::uint16_t port,std::uint32_t memory){
  ins(p,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xec,0xa2});
  imm32(p,memory);
}
void rep_words(std::vector<std::uint8_t>& p,bool disk_to_ram,
               std::uint32_t guest_memory,unsigned count=256){
  // MOV EDI/ESI,guest_memory ; MOV ECX,count ; MOV DX,1F0 ;
  // CLD ; REP INSW / OUTSW. Each is an actual x86 instruction.
  p.push_back(disk_to_ram?0xbf:0xbe);imm32(p,guest_memory);
  p.push_back(0xb9);imm32(p,count);
  ins(p,{0x66,0xba,0xf0,0x01,0xfc,0xf3,0x66,
         std::uint8_t(disk_to_ram?0x6d:0x6f)});
}
void lba28(std::vector<std::uint8_t>& p,std::uint32_t sector){
  out8(p,0x1f6,std::uint8_t(0xe0|(sector>>24)));
  out8(p,0x1f2,1);
  out8(p,0x1f3,std::uint8_t(sector));
  out8(p,0x1f4,std::uint8_t(sector>>8));
  out8(p,0x1f5,std::uint8_t(sector>>16));
}
void lba48(std::vector<std::uint8_t>& p,std::uint64_t sector){
  out8(p,0x1f6,0xe0);
  out8(p,0x1f2,0);
  out8(p,0x1f3,std::uint8_t(sector>>24));
  out8(p,0x1f4,std::uint8_t(sector>>32));
  out8(p,0x1f5,std::uint8_t(sector>>40));
  out8(p,0x1f2,1);
  out8(p,0x1f3,std::uint8_t(sector));
  out8(p,0x1f4,std::uint8_t(sector>>8));
  out8(p,0x1f5,std::uint8_t(sector>>16));
}
void setup_pic(std::vector<std::uint8_t>& p){
  out8(p,0x20,0x11);out8(p,0x21,0x20);
  out8(p,0x21,0x04);out8(p,0x21,0x01);out8(p,0x21,0xfb);
  out8(p,0xa0,0x11);out8(p,0xa1,0x28);
  out8(p,0xa1,0x02);out8(p,0xa1,0x01);out8(p,0xa1,0xbf);
}
void program(PcMachine& pc,const std::vector<std::uint8_t>& p,
             unsigned budget=3500) {
  pc.cpu.memory.write_block(0xd000,p.data(),p.size());
  pc.cpu.eip=0xd000;pc.cpu.halted=false;pc.cpu.fault.reset();
  pc.cpu.regs[Cpu::ESP]=0x190000;pc.cpu.eflags=2|Cpu::IF;
  pc.run(budget);
  if(pc.cpu.fault)throw std::runtime_error(pc.cpu.fault->describe());
  need(pc.cpu.halted,"guest program did not finish with a real HLT");
}
std::vector<std::uint8_t> bios_data(const std::array<std::uint8_t,512>& reference) {
  std::vector<std::uint8_t> v(reference.begin(),reference.end());return v;
}
void run_device(const std::string& path) {
  constexpr std::uint32_t low=7;
  constexpr std::uint64_t high=0x10000020ULL; // truly above LBA28 limit
  std::array<std::uint8_t,512> payload{},persist{},zero{};
  for(std::size_t i=0;i<payload.size();++i)
    payload[i]=std::uint8_t((i*73+i/7+3)&255);
  {
    QssdDisk disk(path);
    need(disk.sectors()>high+1,"sparse LBA48 disk fixture too small");
    PcMachine pc(16*1024*1024);boot32(pc);
    reverse_windows::BiosHardDisk bios(pc,disk);
    PcMotherboardC06 board(pc,&disk);
    need(board.attached_ata()!=nullptr,"board failed to own ATA controller");
    gate(pc.cpu,0x2e,0x13000);
    const std::array<std::uint8_t,23> irq_code{
      0x52,0x50,                  // PUSH EDX / PUSH EAX (preserve PIO DX)
      0xff,0x05,0x00,0x97,0,0,   // INC DWORD PTR [9700]
      0xb0,0x20,
      0x66,0xba,0xa0,0x00,0xee, // OUT A0,AL -> EOI slave
      0x66,0xba,0x20,0x00,0xee, // OUT 20,AL -> EOI master
      0x58,0x5a,0xcf             // POP EAX / POP EDX / IRETD
    };
    pc.cpu.memory.write_block(0x13000,irq_code.data(),irq_code.size());
    pc.cpu.memory.write_block(0xb000,payload.data(),512);
    pc.cpu.memory.fill(0xb400,0,512);

    std::vector<std::uint8_t> p;
    setup_pic(p);
    out32(p,0xcf8,0x80001000U);in32_store(p,0xcfc,0x9500);
    out32(p,0xcf8,0x80001008U);in32_store(p,0xcfc,0x9504);
    out32(p,0xcf8,0x80001010U);out32(p,0xcfc,0xffffffffU);in32_store(p,0xcfc,0x9520);
    out32(p,0xcf8,0x8000103cU);in32_store(p,0xcfc,0x9524);
    out8(p,0x3f6,6);in8_store(p,0x3f6,0x9510); // SRST with nIEN set.
    out8(p,0x3f6,2);
    for(unsigned i=0;i<5;++i)in8_store(p,0x1f1+i,0x9511+i);
    out8(p,0x1f6,0xe0);out8(p,0x1f7,0xec);rep_words(p,true,0xa000,16);
    ins(p,{0xf4});
    program(pc,p);
    need(pc.cpu.read32(0x9520)==0 && pc.cpu.read16(0x9524)==0x00ff,
         "legacy IDE must leave its BAR and PCI INTx pin unimplemented");
    need(pc.cpu.memory[0x9510]==0x80&&pc.cpu.read32(0x9511)==0x00010101U&&
         pc.cpu.memory[0x9515]==0,"ATA reset signature on the primary master was wrong");
    need(board.attached_ata()->counters().identifies==1&&
         board.attached_ata()->counters().identify_data_transfers==0&&
         board.evidence().ata_irqs==0,"partial IDENTIFY was counted or signaled while nIEN set");

    p.clear();
    out8(p,0x1f6,0xf0);
    out8(p,0x1f2,0x55);out8(p,0x1f3,0xaa);out8(p,0x1f7,0xec);
    in8_store(p,0x1f2,0x9516);in8_store(p,0x1f3,0x9517);in8_store(p,0x1f7,0x9518);
    out8(p,0x1f6,0xe0);
    in8_store(p,0x3f6,0x9508); // Poll alternate status without acknowledging IRQ.
    out8(p,0x3f6,0); // Re-enable and deliver the pending master interrupt.
    rep_words(p,true,0xa020,240);
    in8_store(p,0x1f7,0x9519);
    ins(p,{0xf4});
    program(pc,p);
    need(pc.cpu.read16(0x9516)==0&&pc.cpu.memory[0x9518]==0,
         "ATA task-file probes detected a disk on an empty slave");
    need(pc.cpu.memory[0x9519]==0x50,"completed IDENTIFY did not clear DRQ");
    need(board.attached_ata()->counters().identifies==1&&
         board.attached_ata()->counters().identify_data_transfers==1&&
         board.evidence().ata_irqs==1&&pc.cpu.read32(0x9700)==1,
         "IDENTIFY from primary master lost data or pending IRQ after absent slave probe");

    p.clear();
    out8(p,0x3f6,2);out8(p,0x1f7,0xec);
    in8_store(p,0x1f7,0x951a); // Main status acknowledges even while nIEN is set.
    out8(p,0x3f6,0);
    out8(p,0x3f6,4);out8(p,0x3f6,0);
    ins(p,{0xf4});
    program(pc,p);
    need(pc.cpu.memory[0x951a]==0x58&&board.attached_ata()->counters().identifies==2&&
         board.attached_ata()->counters().identify_data_transfers==1&&board.evidence().ata_irqs==1,
         "acknowledged or interrupted IDENTIFY was signaled or counted as complete");

    p.clear();
    out8(p,0x1f1,3);out8(p,0x1f2,0x08);out8(p,0x1f7,0xef); // SET FEATURES PIO 0.
    in8_store(p,0x1f7,0x951b);
    out8(p,0x1f1,3);out8(p,0x1f2,0x42);out8(p,0x1f7,0xef); // UDMA unsupported.
    in8_store(p,0x1f7,0x951c);in8_store(p,0x1f1,0x951d);
    ins(p,{0xf4});
    program(pc,p);
    need(pc.cpu.memory[0x951b]==0x50&&pc.cpu.memory[0x951c]==0x51&&
         pc.cpu.memory[0x951d]==0x04,"SET FEATURES must accept PIO and reject UDMA");

    p.clear();
    lba28(p,low);out8(p,0x1f7,0x30);rep_words(p,false,0xb000);
    in8_store(p,0x1f7,0x9509);
    out8(p,0x1f7,0xe7);in8_store(p,0x1f7,0x950a);
    lba28(p,low);out8(p,0x1f7,0x20);
    rep_words(p,true,0xb400);in8_store(p,0x1f7,0x950b);
    ins(p,{0xf4});
    program(pc,p);
    need(pc.cpu.read32(0x9500)==0xc0711b36U,
         "real guest PCI IDE vendor/device enumeration failed");
    need(pc.cpu.read32(0x9504)==0x01010001U,
         "PCI device is not class 0x0101 legacy IDE");
    need(pc.cpu.memory[0x9508]==0x58,"IDENTIFY did not assert DRQ");
    need(pc.cpu.read16(0xa000)==0x40,"IDENTIFY did not report ATA fixed disk");
    need(pc.cpu.read16(0xa000+83*2)==0x7400&&pc.cpu.read16(0xa000+86*2)==0x3400&&
         pc.cpu.read16(0xa000+87*2)==0x4000,"IDENTIFY LBA48 and FLUSH capabilities are invalid");
    need(pc.cpu.read16(0xa000+49*2)==0x0200&&pc.cpu.read16(0xa000+53*2)==0&&
         pc.cpu.read16(0xa000+47*2)==0x8010&&pc.cpu.read16(0xa000+59*2)==0x0100&&
         pc.cpu.read16(0xa000+106*2)==0x4000,
         "IDENTIFY should describe 512-byte sectors and claim only supported PIO operations");
    need(pc.cpu.read32(0xa000+60*2)==0x0fffffffU,
         "IDENTIFY LBA28 capacity limit incorrect");
    need(pc.cpu.read64(0xa000+100*2)==disk.sectors(),"IDENTIFY sector count differs from QSSD backend");
    std::string model;
    for(unsigned i=27;i<47;++i){
      const auto w=pc.cpu.read16(0xa000+i*2);
      model.push_back(char(w>>8));model.push_back(char(w));
    }
    std::string expected_model="REVERSE QSSD VIRTUAL ATA DISK";
    expected_model.resize(40,' ');
    need(model==expected_model,"IDENTIFY model encoding or padding differs");
    unsigned sum=0;
    for(unsigned i=0;i<512;++i)sum+=pc.cpu.memory[0xa000+i];
    need(pc.cpu.memory[0xa000+510]==0xa5&&(sum&0xffU)==0,"IDENTIFY integrity word is invalid");
    std::cout<<"C03_QSSD_IDENTIFY_DEVICE_PASS sectors="<<disk.sectors()
             <<" sector_bytes=512 lba48=1 checksum=1 irq14_unmasked=1 absent_slave=1\n";
    need(pc.cpu.memory[0x9509]==0x50&&pc.cpu.memory[0x950a]==0x50&&
         pc.cpu.memory[0x950b]==0x50,
         "ATA PIO complete or FLUSH status incorrect");
    pc.cpu.memory.read_block(0xb400,persist.data(),512);
    need(persist==payload,"guest REP OUTSW/INSW failed sector byte equality");
    need(pc.cpu.read32(0x9700)>=1 && board.evidence().ata_irqs>=1,
         "ATA IRQ14 not delivered via cascaded master+slave PIC");
    need(board.attached_ata()->counters().write_sectors==1 &&
         board.attached_ata()->counters().read_sectors==1 &&
         board.attached_ata()->counters().flushes==1,
         "ATA controller did not operate on the chosen BlockDisk");
    std::cout<<"C07_REAL_GUEST_PCI_ATA_PIO_IRQ_PASS sectors=1 irq14="
             <<board.evidence().ata_irqs<<" writes="
             <<board.attached_ata()->counters().write_sectors<<"\n";

    // LBA48 must reach an address not representable by ATA LBA28.
    std::vector<std::uint8_t> ext;
    lba48(ext,high);out8(ext,0x1f7,0x34);rep_words(ext,false,0xb000);
    out8(ext,0x1f7,0xea);in8_store(ext,0x1f7,0x9600);
    ins(ext,{0xf4});
    program(pc,ext);
    need(pc.cpu.memory[0x9600]==0x50,"LBA48 WRITE EXT / FLUSH EXT failed");
    need(disk.read(high,1,persist.data()) && persist==payload,
         "ATA LBA48 did not write actual high logical sector");
    need(board.attached_ata()->counters().write_sectors==2,
         "ATA LBA48 write was not a real backend commit");
    std::vector<std::uint8_t> high_read;
    lba48(high_read,high);out8(high_read,0x1f7,0x24);
    rep_words(high_read,true,0xbc00);ins(high_read,{0xf4});
    program(pc,high_read);
    pc.cpu.memory.read_block(0xbc00,persist.data(),512);
    need(persist==payload,"ATA LBA48 READ EXT via real REP INSW lost guest bytes");
    std::cout<<"C07_LBA48_HIGH_SPARSE_GUEST_PASS lba="<<high<<"\n";

    // Two contiguous sectors in one ATA command. This exercises the PIO
    // data phase and IRQ transition between sectors, not just 1-sector I/O.
    std::array<std::uint8_t,1024> multi_data{},multi_back{};
    for(std::size_t i=0;i<multi_data.size();++i)
      multi_data[i]=std::uint8_t((i*23+i/17+11)&255);
    pc.cpu.memory.write_block(0xc000,multi_data.data(),multi_data.size());
    std::vector<std::uint8_t> multi;
    out8(multi,0x1f6,0xe0);out8(multi,0x1f2,2);
    out8(multi,0x1f3,20);out8(multi,0x1f4,0);out8(multi,0x1f5,0);
    out8(multi,0x1f7,0x30);rep_words(multi,false,0xc000,512);
    out8(multi,0x1f6,0xe0);out8(multi,0x1f2,2);
    out8(multi,0x1f3,20);out8(multi,0x1f4,0);out8(multi,0x1f5,0);
    out8(multi,0x1f7,0x20);rep_words(multi,true,0xc800,512);
    ins(multi,{0xf4});program(pc,multi);
    pc.cpu.memory.read_block(0xc800,multi_back.data(),multi_back.size());
    need(multi_back==multi_data,"ATA 2-sector PIO register/DRQ transition failed");
    need(board.attached_ata()->counters().write_sectors==4,
         "ATA multi-sector guest did not commit both sectors");
    std::cout<<"C07_ATA_PIO_MULTI_SECTOR_PASS sectors=2\n";

    // Bad task-file address and invalid command must return ATA error,
    // not a host CPU fault, and never write beyond QSSD.
    std::vector<std::uint8_t> bad;
    // This 256-GiB QSSD legitimately contains the entire LBA28 range.
    // To test IDNF, issue LBA48 at exactly one sector beyond its last LBA.
    lba48(bad,disk.sectors());out8(bad,0x1f7,0x24);
    in8_store(bad,0x1f7,0x9601);in8_store(bad,0x1f1,0x9602);
    out8(bad,0x1f7,0x99);
    in8_store(bad,0x1f7,0x9603);in8_store(bad,0x1f1,0x9604);
    ins(bad,{0xf4});
    program(pc,bad);
    need(pc.cpu.memory[0x9601]==0x51&&pc.cpu.memory[0x9602]==0x10,
         "LBA out of range must set ATA IDNF and ERR");
    need(pc.cpu.memory[0x9603]==0x51&&pc.cpu.memory[0x9604]==0x04,
         "unsupported ATA command must set ABRT, not fake success");
    need(board.attached_ata()->counters().aborts>=2,
         "ATA controller did not record rejected commands");
    std::cout<<"C07_ATA_COMMAND_ERRORS_PASS idnf=16 abort=4\n";

    // Reset while only ten words of a WRITE have arrived: the disk must
    // never contain an uncommitted sector from the incomplete PIO FIFO.
    std::vector<std::uint8_t> torn;
    lba28(torn,8);out8(torn,0x1f7,0x30);
    rep_words(torn,false,0xb000,10);
    out8(torn,0x3f6,4);out8(torn,0x3f6,0);
    ins(torn,{0xf4});
    program(pc,torn);
    need(disk.read(8,1,persist.data())&&persist==zero,
         "ATA reset wrote incomplete PIO data into QSSD");
    std::cout<<"C07_ATA_PARTIAL_RESET_PASS committed_partial=0\n";

    // Boot one more time using the SAME PcMachine+QSSD+motherboard: no
    // duplicated storage namespace or hidden backup disk is allowed.
    boot32(pc);
    std::vector<std::uint8_t> after;
    lba28(after,low);out8(after,0x1f7,0x20);
    rep_words(after,true,0xb800);ins(after,{0xf4});
    program(pc,after);
    pc.cpu.memory.read_block(0xb800,persist.data(),512);
    need(persist==payload,"machine reboot lost ATA/QSSD persisted sector");
    std::cout<<"C07_SAME_MACHINE_REBOOT_PASS qssd=1\n";
  }
  {
    QssdDisk disk(path);
    need(disk.read(low,1,persist.data())&&persist==payload,
         "QSSD reopen lost original PIO sector");
    need(disk.read(high,1,persist.data())&&persist==payload,
         "QSSD reopen lost LBA48 PIO sector");
    need(disk.read(8,1,persist.data())&&persist==zero,
         "QSSD reopen recovered an aborted PIO write");
    std::array<std::uint8_t,1024> persisted_multi{};
    std::array<std::uint8_t,512> expected_sector{};
    need(disk.read(20,2,persisted_multi.data()),
         "QSSD reopen cannot read 2-sector ATA transfer");
    for(unsigned i=0;i<1024;++i) {
      const auto expected=std::uint8_t((i*23+i/17+11)&255);
      need(persisted_multi[i]==expected,"QSSD multi-sector replay byte mismatch");
    }
    need(disk.stats().commits==4,"ATA guest writes are not exactly four QSSD commits");
    std::cout<<"C07_QSSD_DURABILITY_REOPEN_PASS commits="
             <<disk.stats().commits<<" physical="<<disk.stats().physical_bytes<<"\n";

    PcMachine pc(16*1024*1024);pc.boot_demo();
    reverse_windows::BiosHardDisk bios(pc,disk);
    constexpr std::array<std::uint8_t,16> dap{
      0x10,0,1,0,0x00,0xb8,0,0,0x07,0,0,0,0,0,0,0};
    pc.cpu.memory.write_block(0x8100,dap.data(),dap.size());
    constexpr std::array<std::uint8_t,10> guest{
      0xbe,0,0x81,0xb2,0x80,0xb4,0x42,0xcd,0x13,0xf4};
    pc.cpu.memory.write_block(0x7c00,guest.data(),guest.size());
    pc.run(60);
    need(!pc.cpu.fault&&pc.cpu.halted&&!pc.cpu.flag(Cpu::CF),
         "BIOS INT13 guest cannot read ATA-written QSSD sector");
    pc.cpu.memory.read_block(0xb800,persist.data(),512);
    need(persist==payload,
         "BIOS and native ATA controller do not share the exact same QSSD bytes");
    std::cout<<"C07_BIOS_ATA_SINGLE_QSSD_PASS disk_dl80=1\n";
  }
}
void test_corrupt_and_torn(const std::string& path){
  const auto damage=path+".bad.rqvd";
  std::filesystem::copy_file(path,damage);
  {
    std::fstream f(damage,std::ios::in|std::ios::out|std::ios::binary);
    need(bool(f),"cannot open copied QSSD corrupt fixture");
    f.seekg(95);char raw=0;f.get(raw);
    f.seekp(95);f.put(char(std::uint8_t(raw)^0x40));f.flush();
    need(bool(f),"cannot corrupt committed QSSD record");
  }
  bool rejected=false;
  try{QssdDisk corrupted(damage);}catch(const std::exception&){rejected=true;}
  need(rejected,"QSSD corruption was silently accepted after ATA guest writes");
  std::filesystem::remove(damage);
  {
    int fd=::open(path.c_str(),O_WRONLY|O_APPEND);
    need(fd>=0,"cannot open QSSD torn-tail fixture");
    const char tail[]="QBLKbadpartial";
    need(::write(fd,tail,sizeof(tail)-1)==ssize_t(sizeof(tail)-1),
         "failed to append interrupted QSSD transaction");
    ::close(fd);
  }
  {
    QssdDisk recovered(path);
    need(recovered.stats().commits==4,
         "QSSD torn final journal record recovered as a committed ATA write");
    std::array<std::uint8_t,512> data{};
    need(recovered.read(7,1,data.data())&&data[0]==3,
         "QSSD tore off previously committed ATA data");
  }
  std::cout<<"C07_QSSD_CORRUPT_REJECT_TORN_RECOVER_PASS\n";
}
}
int main(){
  const auto root=std::filesystem::temp_directory_path()/
     ("reverse-ata-c07-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path=root.string()+".rqvd";
  try{
    QssdDisk::create(path,256);
    run_device(path);
    test_corrupt_and_torn(path);
    std::filesystem::remove(path);
    std::cout<<"REVERSE_WINDOWS_C07_REAL_ATA_PASS checks="<<checks
             <<" pci_class=0101 pio=true irq14=true qssd=true windows_booted=false\n";
    return 0;
  }catch(const std::exception& e){
    std::cerr<<"REVERSE_WINDOWS_C07_REAL_ATA_FAIL "<<e.what()<<"\n";
    std::error_code ignored;std::filesystem::remove(path,ignored);
    return 1;
  }
}
