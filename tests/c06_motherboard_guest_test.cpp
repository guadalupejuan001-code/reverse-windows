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
void pic_legacy_x87_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  cpu.idtr={0x1000,0x15000};
  cpu.regs[Cpu::ESP]=0x190000;
  cpu.cr0&=~0x2eU; // Clear NE, TS, EM and MP.
  gate(cpu,0x75,0x13000);
  code(cpu,0x13000,{
      0xb0,0,0xe6,0xf0,   // Acknowledge FERR# and assert IGNNE#.
      0x9b,               // A waiting instruction can now run.
      0xdf,0xe0,          // FNSTSW AX still reports the unmasked error.
      0x66,0xa3,0,0x91,0,0,
      0xdb,0xe2,          // FNCLEX clears pending status, FERR# and IGNNE#.
      0xb0,0x20,0xe6,0xa0,0xe6,0x20,
      0xcf});
  cpu.write16(0x9104,0x037b); // Unmask divide-by-zero.
  const std::vector<std::uint8_t> program={
      0xfa,
      0xb0,0xfb,0xe6,0x21, // Unmask the master cascade.
      0xb0,0xdf,0xe6,0xa1, // Unmask slave IRQ13.
      0xd9,0x2d,4,0x91,0,0, // FLDCW [9104]
      0xd9,0xe8,0xd9,0xee, // FLD1, FLDZ
      0xde,0xf9,           // FDIVP ST(1), ST(0): 1/0
      0xe4,0x20,0xa2,2,0x91,0,0, // Master IRR while IF is clear.
      0xe4,0xa0,0xa2,3,0x91,0,0, // Slave IRR while IF is clear.
      0x9b,0xf4};          // FWAIT, HLT
  code(cpu,0xa000,program);
  cpu.eip=0xa000;
  run(pc,40);
  const auto wait_ip=0xa000+program.size()-2;
  const auto wait_insns=cpu.instructions;
  need((cpu.read16(0x9102)&0x2004U)==0x2004U,
       "Both PICs must expose the pending IRQ13 cascade while CPU IF is clear");
  need(cpu.eip==wait_ip && cpu.x87_ferr_asserted() && !cpu.halted,
       "FWAIT did not hold at the same IP with FERR# pending");
  need(cpu.step()==cosmic_pc::StepResult::WaitingForX87 && cpu.instructions==wait_insns &&
       board.evidence().irq_delivered==0,
       "FWAIT retired or IRQ13 was delivered while IF was clear");
  cpu.set_flag(Cpu::IF,true); // Permit the pending hardware interrupt at FWAIT.
  run(pc,40);
  need(cpu.halted && cpu.eip==wait_ip+2 && cpu.regs[Cpu::ESP]==0x190000 &&
       board.evidence().irq_delivered==1 && !cpu.x87_ferr_asserted() &&
       (cpu.read16(0x9100)&0x8084U)==0x8084U,
       "IRQ13/IGNNE# handler did not reenter the held FWAIT after one interrupt");
  cpu.halted=false;
  cpu.eip=0xa000;
  run(pc,40);
  need(cpu.eip==wait_ip && cpu.x87_ferr_asserted() && !cpu.halted &&
       board.evidence().irq_delivered==1,
       "New x87 error bypassed FWAIT after FNCLEX released IGNNE#");
  cpu.set_flag(Cpu::IF,true);
  run(pc,40);
  need(cpu.halted && board.evidence().irq_delivered==2 && !cpu.x87_ferr_asserted(),
       "A second independent FERR# edge did not deliver IRQ13");
  std::cout<<"C02_X87_LEGACY_FERR_PASS IRQ13=2 FWAIT=hold-and-retry IGNNE=cleared\n";
}
void optical_irq_wakes_sti_hlt(){
  PcMachine pc(16*1024*1024);boot32(pc);
  std::array<std::uint8_t,2048> optical_sector{};
  PcMotherboardC06 board(pc,nullptr,
      [&optical_sector](auto offset,auto* data,auto count){
        if(offset>optical_sector.size()||count>optical_sector.size()-offset)return false;
        std::copy_n(optical_sector.data()+offset,count,data);
        return true;
      },optical_sector.size());
  auto& cpu=pc.cpu;
  cpu.idtr={0x1000,0x15000};
  cpu.regs[Cpu::ESP]=0x190000;
  gate(cpu,0x77,0x13000);
  code(cpu,0x13000,{
      0x50,0x52,                         // PUSH EAX, EDX
      0x66,0xba,0x77,0x01,0xec,           // Sample ATAPI command status.
      0xa2,0x00,0x91,0x00,0x00,           // Save sampled status.
      0x8b,0x44,0x24,0x08,                // Record saved EIP from IRQ frame.
      0xa3,0x04,0x91,0x00,0x00,
      0xb0,0x20,0xe6,0xa0,0xe6,0x20,       // EOI to slave and master.
      0x5a,0x58,0xcf});                    // POP EDX, EAX; IRETD
  const std::vector<std::uint8_t> program={
      0xfa,                               // Queue IRQ15 with CPU IRQs disabled.
      0xb0,0xfb,0xe6,0x21,                 // Unmask PIC cascade.
      0xb0,0x7f,0xe6,0xa1,                 // Unmask IRQ15.
      0x66,0xba,0x77,0x01,0xb0,0xa1,0xee, // IDENTIFY PACKET DEVICE.
      0xfb,0xf4,                          // STI; HLT before pending IRQ delivery.
      0xc6,0x05,0x01,0x91,0x00,0x00,0x01, // Reached code after the wait.
      0xfa,0xf4};
  constexpr auto start=0xa000U;
  constexpr auto after_hlt=start+18;
  code(cpu,start,program);
  cpu.eip=start;
  run(pc,70);
  need(cpu.halted&&cpu.read8(0x9101)==1,
       "ATAPI IRQ15 must wake the first HLT and resume its next instruction");
  need(cpu.read32(0x9104)==after_hlt&&(cpu.read8(0x9100)&0x48U)==0x48U&&
       board.evidence().irq_delivered==1&&cpu.regs[Cpu::ESP]==0x190000,
       "ATAPI IRQ15 must see DRQ, stack the instruction after HLT, and balance IRET");
  std::cout<<"C03_ATAPI_IRQ15_STI_HLT_PASS irq15=1 wake=1 resume_ip="<<after_hlt<<"\n";
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
  const auto guest_pci_id=pc.cpu.read32(0x9000);
  need(pc.cpu.read32(0x9004)==0xffffffffU,
       "nonexistent PCI device must read all ones");
  need(pc.cpu.memory[0x9008]==2,"guest CMOS status B does not indicate BCD 24-hour");
  need(pc.cpu.read32(0x900c)==0x20445352U,
       "guest did not discover ACPI RSDP signature via real MOV");
  need(board.evidence().powered_off,
       "guest ACPI PM1a S5 request not recognized");
  // Reboot the SAME PcMachine while the motherboard remains attached:
  // firmware ACPI must be restored after guest RAM is zeroed.
  pc.boot_long_mode_demo();
  std::array<std::uint8_t,8> rsdp_after_reset{};
  pc.cpu.memory.read_block(AcpiC06::rsdp_address,rsdp_after_reset.data(),8);
  need(std::memcmp("RSD PTR ",rsdp_after_reset.data(),8)==0 &&
       checksum(pc.cpu,AcpiC06::rsdp_address,36)==0,
       "reboot lost ACPI discovery in reserved firmware memory");
  need(!board.evidence().powered_off && board.evidence().ticks==0,
       "motherboard retained stale PM/timer state across reboot");
  std::cout<<"C06_PCI_ACPI_CMOS_REAL_GUEST_PASS pci=0x"<<std::hex
           <<guest_pci_id<<std::dec<<" tables=5 reboot=PASS\n";
}
void ioapic_lapic_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  pc.cpu.idtr={0x1000,0x15000};pc.cpu.regs[Cpu::ESP]=0x190000;
  gate(pc.cpu,0x31,0x13300);
  code(pc.cpu,0x13300,{
      0xc7,0x05,0x00,0x91,0,0,0x56,0x34,0x12,0x90,
      // Mask GSI0 after first IRQ so the test can prove a completed
      // interrupt frame instead of inspecting a periodically re-entered ISR.
      0xc7,0x05,0x00,0,0xc0,0xfe,0x10,0,0,0,
      0xc7,0x05,0x10,0,0xc0,0xfe,0x31,0,0x01,0,
      0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0, // APIC EOI
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
void ps2_keyboard_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  board.enable_ps2_keyboard();
  pc.cpu.idtr={0x1000,0x15000};
  pc.cpu.regs[Cpu::ESP]=0x190000;
  pc.cpu.regs[Cpu::EDI]=0x9300;
  gate(pc.cpu,0x21,0x13400);
  code(pc.cpu,0x13400,{
      0xe4,0x60,             // IN AL, 60h
      0xaa,                  // STOSB, next guest byte
      0xb0,0x20,0xe6,0x20, // PIC EOI
      0xcf                   // IRETD
  });
  std::vector<std::uint8_t> p;
  const auto out=[&](std::uint8_t value,std::uint8_t port){
    emit(p,{0xb0,value,0xe6,port});
  };
  out(0x11,0x20);out(0x20,0x21);out(0x04,0x21);out(0x01,0x21);
  out(0xfd,0x21); // Unmask PS/2 IRQ1 only.
  out(0x60,0x64);out(0x05,0x60); // System + IRQ1, set 2 at the data port.
  out(0xf2,0x60); // Identify keyboard and receive ACK, AB, 83.
  emit(p,{0xfb,0xf4,0xeb,0xfd}); // STI; HLT; JMP to HLT for the next byte.
  code(pc.cpu,0xa000,p);pc.cpu.eip=0xa000;pc.cpu.halted=false;pc.cpu.fault.reset();
  run(pc,160);
  need(pc.cpu.read8(0x9300)==0xfa && pc.cpu.read8(0x9301)==0xab &&
       pc.cpu.read8(0x9302)==0x83 && pc.cpu.regs[Cpu::EDI]==0x9303,
       "guest PS/2 identify responses missing or out of order");
  need(board.inject_ps2_key(0xe04b,true) && board.inject_ps2_key(0xe04b,false),
       "host extended key was not queued");
  run(pc,160);
  const std::array<std::uint8_t,5> sequence{0xe0,0x6b,0xe0,0xf0,0x6b};
  for(unsigned i=0;i<sequence.size();++i)
    need(pc.cpu.read8(0x9303+i)==sequence[i],"guest scan-set 2 make/break bytes differ");
  need(board.evidence().irq_delivered==8 && pc.cpu.regs[Cpu::EDI]==0x9308 &&
       pc.cpu.regs[Cpu::ESP]==0x190000,"PS/2 IRQ1 must resume and return the guest for every byte");
  std::cout<<"C06_PS2_KEYBOARD_IRQ1_PASS bytes=8 protected32=true\n";
}
void queued_lapic_requests_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  board.enable_ps2_keyboard();
  pc.cpu.idtr={0x1000,0x15000};
  pc.cpu.regs[Cpu::ESP]=0x190000;
  pc.cpu.regs[Cpu::EDI]=0x9400;
  gate(pc.cpu,0x31,0x13400);
  gate(pc.cpu,0x42,0x13500);
  code(pc.cpu,0x13400,{
      0xe4,0x60,0xaa, // Read the keyboard byte and append to guest evidence.
      0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0,0xcf // LAPIC EOI and IRETD.
  });
  code(pc.cpu,0x13500,{
      0xb0,0x42,0xaa, // Mark timer completion and append to guest evidence.
      0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0,0xcf
  });
  std::vector<std::uint8_t> setup{0xfa}; // CLI until both devices have raised IRQ.
  const auto write_reg=[&](std::uint32_t address,std::uint32_t value){
    emit(setup,{0xc7,0x05});
    for(unsigned i=0;i<4;++i)setup.push_back(std::uint8_t(address>>(8*i)));
    for(unsigned i=0;i<4;++i)setup.push_back(std::uint8_t(value>>(8*i)));
  };
  write_reg(0xfee000f0,0x1ff); // Enable LAPIC.
  write_reg(0xfec00000,0x12); // IOAPIC GSI1.
  write_reg(0xfec00010,0x31); // PS/2 vector 31h.
  write_reg(0xfee00320,0x42); // One-shot timer vector 42h.
  write_reg(0xfee00380,2);
  emit(setup,{0xb0,0x60,0xe6,0x64,0xb0,0x45,0xe6,0x60,0xeb,0xfe});
  code(pc.cpu,0xa000,setup);pc.cpu.eip=0xa000;pc.cpu.halted=false;pc.cpu.fault.reset();
  run(pc,100);
  need(board.inject_ps2_key(0x1e,true),"test keyboard did not queue a scan code");
  run(pc,25);
  need(board.evidence().irq_delivered==0,"IRQ was delivered while guest IF was clear");
  code(pc.cpu,0xa100,{0xfb,0xf4,0xeb,0xfd});
  pc.cpu.eip=0xa100;
  run(pc,100);
  need(pc.cpu.read8(0x9400)==0x42 && pc.cpu.read8(0x9401)==0x1e &&
       pc.cpu.regs[Cpu::EDI]==0x9402 && board.evidence().apic_irqs==2,
       "LAPIC lost a pending request or delivered vectors out of priority order");
  need(pc.cpu.regs[Cpu::ESP]==0x190000,"queued LAPIC interrupts did not restore the guest stack");
  std::cout<<"C06_LAPIC_QUEUED_IRQ_PASS timer=0x42 keyboard=0x31\n";
}
}
int main(){
  try{
    pic_pit_guest();
    optical_irq_wakes_sti_hlt();
    pic_legacy_x87_guest();
    pci_acpi_cmos_guest();
    ioapic_lapic_guest();
    ps2_keyboard_guest();
    queued_lapic_requests_guest();
    std::cout<<"REVERSE_WINDOWS_C06_MOTHERBOARD_PASS checks="<<checks
             <<" windows_booted=false full_acpi=false full_apic=false\n";
    return 0;
  }catch(const std::exception& error){
    std::cerr<<"REVERSE_WINDOWS_C06_MOTHERBOARD_FAIL "<<error.what()<<"\n";
    return 1;
  }
}
