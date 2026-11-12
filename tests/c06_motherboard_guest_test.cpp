#include "c06_motherboard.hpp"
#include "pc_machine.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::PcMotherboardC06;
using reverse_windows::AcpiC06;
std::uint64_t checks=0;
void need(bool ok,const std::string& reason){
  ++checks;if(!ok)throw std::runtime_error(reason);
}
void code(Cpu& cpu,std::uint32_t at,const std::vector<std::uint8_t>& bytes){
  cpu.memory.write_block(at,bytes.data(),bytes.size());
}
void code(Cpu& cpu,std::uint32_t at,std::initializer_list<std::uint8_t> bytes){
  code(cpu,at,std::vector<std::uint8_t>(bytes));
}
void boot32(PcMachine& pc){
  pc.boot_long_mode_demo();
  for(unsigned i=0;i<10000;++i){
    const auto r=pc.cpu.step();
    if(r==cosmic_pc::StepResult::Faulted)throw std::runtime_error(pc.cpu.fault->describe());
    if(std::string(pc.cpu.mode_name())=="protected32"&&pc.cpu.segments[Cpu::SS]==0x10)
      return;
  }
  throw std::runtime_error("C06 could not reach real protected32 guest");
}
void gate(Cpu& cpu,unsigned vector,std::uint32_t target){
  const auto base=cpu.idtr.base+vector*8;
  const std::uint64_t entry=(target&0xffffU)|(std::uint64_t(0x08)<<16)|
      (std::uint64_t(0x8e)<<40)|(std::uint64_t(target>>16)<<48);
  cpu.write64(base,entry);
}
void run(PcMachine& pc,unsigned steps=160){
  pc.run(steps);
  if(pc.cpu.fault)throw std::runtime_error(pc.cpu.fault->describe());
}
void emit(std::vector<std::uint8_t>& c,std::initializer_list<std::uint8_t> bytes){
  c.insert(c.end(),bytes.begin(),bytes.end());
}
void pic_pit_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  pc.cpu.idtr={0x1000,0x15000};
  pc.cpu.regs[Cpu::ESP]=0x190000;
  gate(pc.cpu,0x20,0x13000);
  code(pc.cpu,0x13000,{
      0xc7,0x05,0x00,0x90,0,0,0x78,0x56,0x34,0x12, // MOV [9000],12345678
      0xb0,0x20,0xe6,0x20,                         // EOI command to master PIC
      0xcf});                                     // IRETD (guest)
  std::vector<std::uint8_t> p;
  const auto out=[&](std::uint8_t value,std::uint8_t port){
    emit(p,{0xb0,value,0xe6,port});
  };
  out(0x11,0x20);out(0x20,0x21);out(0x04,0x21);out(0x01,0x21);
  out(0xfe,0x21); // unmask only PIT IRQ0
  out(0x34,0x43);out(0x10,0x40);out(0,0x40);
  emit(p,{0xfb,0xf4,0xeb,0xfe}); // STI; HLT until IRQ; JMP $
  code(pc.cpu,0xa000,p);
  pc.cpu.eip=0xa000;pc.cpu.halted=false;pc.cpu.fault.reset();
  run(pc,200);
  need(pc.cpu.read32(0x9000)==0x12345678U,
       "x86 guest PIC/PIT IRQ0 handler did not run");
  need(board.evidence().pit_irqs>=1&&board.evidence().irq_delivered>=1,
       "PIC did not dispatch guest IRQ from deterministic PIT");
  need(pc.cpu.regs[Cpu::ESP]==0x190000,
       "IRQ0 IRETD did not balance guest stack");
  std::cout<<"C06_PIC_PIT_GUEST_IRQ_PASS ticks="<<board.evidence().ticks
           <<" pit="<<board.evidence().pit_irqs<<"\n";
}
unsigned checksum(Cpu& cpu,std::uint32_t addr,std::uint32_t size){
  unsigned total=0;for(unsigned i=0;i<size;++i)total+=cpu.memory[addr+i];
  return total&255;
}
void pci_acpi_cmos_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  // ACPI RSDP 2.0 + XSDT/RSDT/FADT/MADT/DSDT reside in E820 reserved BIOS ROM.
  std::array<std::uint8_t,8> rsdp_signature{};
  pc.cpu.memory.read_block(0xe0000,rsdp_signature.data(),rsdp_signature.size());
  need(std::memcmp("RSD PTR ",rsdp_signature.data(),8)==0,
       "RSDP BIOS discovery signature missing");
  need(checksum(pc.cpu,AcpiC06::rsdp_address,20)==0 &&
       checksum(pc.cpu,AcpiC06::rsdp_address,36)==0,
       "RSDP 1.0/2.0 checksums do not match");
  const std::array<std::pair<std::uint32_t,unsigned>,5> tables{{
    {AcpiC06::rsdt_address,44U},{AcpiC06::xsdt_address,52U},
    {AcpiC06::fadt_address,244U},{AcpiC06::madt_address,64U},
    {AcpiC06::dsdt_address,49U}
  }};
  for(const auto& entry:tables)
    need(checksum(pc.cpu,entry.first,entry.second)==0,"ACPI table checksum invalid");
  need(pc.cpu.read32(AcpiC06::rsdt_address+36)==AcpiC06::fadt_address &&
       pc.cpu.read64(AcpiC06::xsdt_address+44)==AcpiC06::madt_address,
       "ACPI table pointers do not describe the actual firmware");
  need(pc.cpu.read32(AcpiC06::madt_address+36)==0xfee00000U,
       "MADT LAPIC base does not match the mapped MMIO device");

  std::vector<std::uint8_t> p;
  emit(p,{0xb8,0,0,0,0x80,   // MOV EAX,80000000 (PCI 0:0.0 vendor)
          0x66,0xba,0xf8,0x0c,0xef,  // MOV DX,CF8; OUT DX,EAX
          0x66,0xba,0xfc,0x0c,0xed,  // MOV DX,CFC; IN EAX,DX
          0xa3,0x00,0x90,0,0,       // store to RAM
          0xb8,0,0xf8,0,0x80,      // absent dev31
          0x66,0xba,0xf8,0x0c,0xef,
          0x66,0xba,0xfc,0x0c,0xed,
          0xa3,0x04,0x90,0,0,
          0xb0,0x0b,0xe6,0x70,0xe4,0x71, // CMOS status B
          0xa2,0x08,0x90,0,0,
          0xa1,0,0,0x0e,0,        // guest reads "RSD " at E0000
          0xa3,0x0c,0x90,0,0,
          0x66,0xba,0x04,0x06,    // MOV DX, 604 ACPI PM1a CNT
          0x66,0xb8,0x00,0x34,    // MOV AX, 3400 (S5 + SLP_EN)
          0x66,0xef,             // OUT DX, AX
          0xf4});
  code(pc.cpu,0xa000,p);pc.cpu.eip=0xa000;pc.cpu.halted=false;
  pc.cpu.regs[Cpu::ESP]=0x190000;
  run(pc,100);
  need(pc.cpu.read32(0x9000)==0x00011b36,
       "real x86 guest PCI CF8/CFC failed to read host bridge ID");
  need(pc.cpu.read32(0x9004)==0xffffffffU,
       "nonexistent PCI device must read all ones");
  need(pc.cpu.memory[0x9008]==2,"guest CMOS status B does not indicate BCD 24-hour");
  need(pc.cpu.read32(0x900c)==0x20445352U,
       "guest did not discover ACPI RSDP signature via real MOV");
  need(board.evidence().powered_off,
       "guest ACPI PM1a S5 request not recognized");
  std::cout<<"C06_PCI_ACPI_CMOS_REAL_GUEST_PASS pci=0x"<<std::hex
           <<pc.cpu.read32(0x9000)<<std::dec<<" tables=5\n";
}
void ioapic_lapic_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  pc.cpu.idtr={0x1000,0x15000};pc.cpu.regs[Cpu::ESP]=0x190000;
  gate(pc.cpu,0x31,0x13300);
  code(pc.cpu,0x13300,{
      0xc7,0x05,0x00,0x91,0,0,0x56,0x34,0x12,0x90,
      0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0, // APIC EOI in MMIO
      0xcf
  });
  std::vector<std::uint8_t> p;
  const auto write_reg=[&](std::uint32_t address,std::uint32_t value){
    emit(p,{0xc7,0x05});
    for(unsigned i=0;i<4;++i)p.push_back(std::uint8_t(address>>(8*i)));
    for(unsigned i=0;i<4;++i)p.push_back(std::uint8_t(value>>(8*i)));
  };
  emit(p,{0xa1,0x30,0,0xe0,0xfe, // MOV EAX, [FEE00030] LAPIC version
          0xa3,0x04,0x91,0,0});  // store version
  write_reg(0xfee000f0,0x1ff);   // software enable local APIC
  write_reg(0xfec00000,0x10);   // IOAPIC select redirection0 low
  write_reg(0xfec00010,0x31);   // unmask GSI0 vector31
  emit(p,{0xb0,0x34,0xe6,0x43,
          0xb0,0x10,0xe6,0x40,
          0xb0,0x00,0xe6,0x40,
          0xfb,0xf4,0xeb,0xfe});
  code(pc.cpu,0xa000,p);pc.cpu.eip=0xa000;pc.cpu.halted=false;
  run(pc,220);
  need(pc.cpu.read32(0x9104)==0x00030014,
       "guest did not read actual LAPIC MMIO version");
  need(pc.cpu.read32(0x9100)==0x90123456,
       "guest IOAPIC -> LAPIC vector31 handler did not execute");
  need(board.evidence().apic_irqs>=1,
       "guest IRQ did not route through IOAPIC/LAPIC");
  need(pc.cpu.regs[Cpu::ESP]==0x190000,
       "LAPIC guest IRET did not restore interrupted stack");
  bool unsupported=false;
  try{(void)pc.cpu.read32(0xfee00120);}catch(const std::exception&){unsupported=true;}
  need(unsupported,"unsupported local APIC register must not fake value");
  std::cout<<"C06_IOAPIC_LAPIC_MMIO_IRQ_PASS delivered="<<board.evidence().apic_irqs<<"\n";
}
}
int main(){
  try{
    pic_pit_guest();
    pci_acpi_cmos_guest();
    ioapic_lapic_guest();
    std::cout<<"REVERSE_WINDOWS_C06_MOTHERBOARD_PASS checks="<<checks
             <<" windows_booted=false full_acpi=false full_apic=false\n";
    return 0;
  }catch(const std::exception& error){
    std::cerr<<"REVERSE_WINDOWS_C06_MOTHERBOARD_FAIL "<<error.what()<<"\n";
    return 1;
  }
}
