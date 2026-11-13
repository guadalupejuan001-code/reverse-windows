#include "c09_guest_trace.hpp"
#include "qssd_disk.hpp"
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

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::PcMotherboardC06;
using reverse_windows::C09GuestTrace;
using reverse_windows::QssdDisk;
unsigned checks=0;
void need(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void boot32(PcMachine& pc){
  pc.boot_long_mode_demo();
  for(unsigned i=0;i<12000;++i){
    const auto status=pc.cpu.step();
    if(status==cosmic_pc::StepResult::Faulted)
      throw std::runtime_error(pc.cpu.fault->describe());
    if(std::string(pc.cpu.mode_name())=="protected32"&&
       pc.cpu.segments[Cpu::SS]==0x10)return;
  }
  throw std::runtime_error("C09 hardware guest didn't enter x86 protected32");
}
void append(std::vector<std::uint8_t>& p,std::initializer_list<std::uint8_t> b){
  p.insert(p.end(),b.begin(),b.end());
}
void out8(std::vector<std::uint8_t>& p,std::uint16_t port,std::uint8_t b) {
  append(p,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),
            0xb0,b,0xee});
}
void gate(Cpu& cpu,std::uint8_t vector,std::uint32_t destination){
  cpu.idtr={0x1000,0x15000};
  const auto descriptor=(std::uint64_t(destination&0xffffU))|
      (std::uint64_t(8)<<16)|(std::uint64_t(0x8e)<<40)|
      (std::uint64_t(destination>>16)<<48);
  cpu.write64(cpu.idtr.base+8U*vector,descriptor);
}
void run(const std::string& path,const std::string& journal){
  QssdDisk::create(path,1);
  std::array<std::uint8_t,512> payload{},recovered{};
  for(unsigned i=0;i<payload.size();++i)payload[i]=std::uint8_t((i*17+9)&255);
  {
    QssdDisk disk(path);
    PcMachine pc(16*1024*1024);boot32(pc);
    PcMotherboardC06 board(pc,&disk);
    pc.cpu.memory.write_block(0xb000,payload.data(),payload.size());
    gate(pc.cpu,0x2e,0x13000);
    // IRQ14 guest ISR preserves EAX/EDX, acknowledges slave + master,
    // writes a guest-memory marker and returns by IRETD.
    const std::array<std::uint8_t,30> handler{{
      0x52,0x50,                        // PUSH EDX; PUSH EAX
      0xc7,0x05,0x00,0x91,0,0,0x78,0x56,0x34,0x12,
      0xb0,0x20,0x66,0xba,0xa0,0,0xee, // EOI slave
      0x66,0xba,0x20,0,0xee,           // EOI master
      0x58,0x5a,0xcf,0x90,0x90        // POP; POP; IRETD
    }};
    pc.cpu.memory.write_block(0x13000,handler.data(),handler.size());
    std::vector<std::uint8_t> p;
    // These instructions result in real physical APIC MMIO events.
    append(p,{0xa1,0x30,0x00,0xe0,0xfe,
              0xa3,0x04,0x91,0,0,
              0xc7,0x05,0xf0,0x00,0xe0,0xfe,0xff,0x01,0,0});
    // Explicit PIC remap: master 0x20, slave 0x28, cascade IRQ2,
    // ATA IRQ14 = slave line6 -> vector 0x2e.
    out8(p,0x20,0x11);out8(p,0x21,0x20);
    out8(p,0x21,0x04);out8(p,0x21,0x01);out8(p,0x21,0xfb);
    out8(p,0xa0,0x11);out8(p,0xa1,0x28);
    out8(p,0xa1,0x02);out8(p,0xa1,0x01);out8(p,0xa1,0xbf);
    out8(p,0x1f6,0xe0);out8(p,0x1f2,1);
    out8(p,0x1f3,7);out8(p,0x1f4,0);out8(p,0x1f5,0);
    out8(p,0x1f7,0x30);
    append(p,{0xbe,0x00,0xb0,0,0,  // MOV ESI,0xb000
              0xb9,0x00,0x01,0,0,  // MOV ECX,256
              0x66,0xba,0xf0,0x01, // MOV DX,1F0
              0xfc,0xf3,0x66,0x6f, // CLD; REP OUTSW (256 steps)
              0xfb,0xf4});         // STI; HLT after queued IRQ
    pc.cpu.memory.write_block(0xa000,p.data(),p.size());
    pc.cpu.eip=0xa000;pc.cpu.halted=false;pc.cpu.fault.reset();
    pc.cpu.regs[Cpu::ESP]=0x190000;
    pc.cpu.eflags=2;
    {
      C09GuestTrace trace(pc,board,&disk,journal,25000);
      pc.run(1200);
      if(pc.cpu.fault)throw std::runtime_error(pc.cpu.fault->describe());
      trace.finish();
    }
    need(pc.cpu.read32(0x9100)==0x12345678,
         "actual IRQ14 guest ISR did not run");
    need(pc.cpu.read32(0x9104)==0x00030014,
         "guest physical LAPIC version read failed");
    need(board.evidence().ata_irqs>=1,"PCI ATA IRQ14 did not reach PIC/IDT");
    need(board.attached_ata()->counters().write_sectors==1,
         "ATA did not commit one sector into original QSSD");
    need(disk.stats().commits==1,
         "C09 hardware path didn't retain original QSSD StateBus commit");
    need(disk.read(7,1,recovered.data())&&recovered==payload,
         "real guest ATA PIO wrote different QSSD bytes");
  }
  {
    QssdDisk disk(path);
    need(disk.stats().commits==1&&
         disk.read(7,1,recovered.data())&&recovered==payload,
         "C09 device-trace test lost sector after real QSSD reopen");
  }
  std::ifstream file(journal);
  need(bool(file),"C09 device trace file missing");
  std::string line;
  bool irq=false,mmio_read=false,mmio_write=false,ata_port=false,finish=false;
  for(;std::getline(file,line);){
    if(line.find("\"type\":\"hardware-irq\",\"vector\":46")!=std::string::npos)irq=true;
    if(line.find("\"type\":\"mmio-read\",\"address\":\"0x00000000fee00030\"")!=std::string::npos)
      mmio_read=true;
    if(line.find("\"type\":\"mmio-write\",\"address\":\"0x00000000fee000f0\"")!=std::string::npos)
      mmio_write=true;
    if(line.find("\"type\":\"io-out\",\"port\":496")!=std::string::npos)ata_port=true;
    if(line.find("\"type\":\"finish\"")!=std::string::npos &&
       line.find("\"qssd_commits\":1")!=std::string::npos &&
       line.find("\"hardware_irqs\":1")!=std::string::npos)finish=true;
  }
  need(irq&&mmio_read&&mmio_write&&ata_port&&finish,
       "C09 trace omitted authenticated hardware IRQ/MMIO/ATA/QSSD evidence");
  std::cout<<"C09_HARDWARE_IRQ14_MMIO_QSSD_TRACE_PASS vector=46 "
              "mmio_read_write=true ata_pio=true qssd_commits=1\n";
}
}
int main(){
  const auto base=(std::filesystem::temp_directory_path()/
    ("c09-hardware-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
  const auto path=base+".rqvd",trace=base+".jsonl";
  try{
    run(path,trace);
    std::filesystem::remove(path);
    std::filesystem::remove(trace);
    std::cout<<"REVERSE_WINDOWS_C09_HARDWARE_TRACE_PASS checks="<<checks
             <<" windows_booted=false\n";
    return 0;
  }catch(const std::exception& e){
    std::cerr<<"REVERSE_WINDOWS_C09_HARDWARE_TRACE_FAIL "<<e.what()<<"\n";
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    std::filesystem::remove(trace,ignored);
    return 1;
  }
}
