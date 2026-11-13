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
void rep_boot_with_pit_guest(bool local_apic){
  const auto check=[&](bool accelerate){
    PcMachine pc(16*1024*1024);boot32(pc);
    PcMotherboardC06 board(pc);
    auto& cpu=pc.cpu;
    pc.enable_boot_acceleration(accelerate);
    pc.enable_boot_hotspots();
    cpu.idtr={0x1000,0x15000};
    cpu.regs[Cpu::ESP]=0x190000;
    gate(cpu,0x42,0x13000);
    std::vector<std::uint8_t> handler{
        0x89,0x0d,0x04,0x91,0,0,             // Record interrupted ECX.
        0x50,0x8b,0x44,0x24,0x04,
        0xa3,0x08,0x91,0,0};                 // Record interrupted EIP.
    if(local_apic)emit(handler,{0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0});
    else emit(handler,{0xb0,0xff,0xe6,0x21,0xb0,0x20,0xe6,0x20});
    emit(handler,{0x58,0xcf});
    code(cpu,0x13000,handler);
    std::vector<std::uint8_t> program{0xfa,0xfc};
    const auto out=[&](std::uint8_t value,std::uint8_t port){emit(program,{0xb0,value,0xe6,port});};
    if(local_apic){
      const auto write_reg=[&](std::uint32_t address,std::uint32_t value){
        emit(program,{0xc7,0x05});
        for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(address>>(8*i)));
        for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(value>>(8*i)));
      };
      write_reg(0xfee000f0,0x1ff);write_reg(0xfee00320,0x42);write_reg(0xfee00380,300);
    }else{
      gate(cpu,0x40,0x13000);
      out(0x11,0x20);out(0x40,0x21);out(0x04,0x21);out(0x01,0x21);out(0xfe,0x21);
      out(0x34,0x43);out(0x2c,0x40);out(0x01,0x40); // PIT period of 300 ticks.
    }
    emit(program,{0xb9,0x00,0x80,0,0,0xbf,0,0,0x20,0,0xb0,0x5a,0xfb});
    const auto rep_ip=0xa000+program.size();
    emit(program,{0xf3,0xaa,0xfa,0xf4});
    code(cpu,0xa000,program);cpu.eip=0xa000;
    run(pc,34000);
    const auto ecx=cpu.read32(0x9104);
    need(cpu.halted&&cpu.read8(0x200000)==0x5a&&cpu.read8(0x207fff)==0x5a&&
         cpu.read8(0x208000)==0&&cpu.regs[Cpu::ECX]==0&&cpu.regs[Cpu::ESP]==0x190000,
         "batched guest must retain REP result, register and interrupt stack semantics");
    need(board.evidence().irq_delivered==1&&ecx>0&&ecx<32768&&cpu.read32(0x9108)==rep_ip,
         "timer IRQ must interrupt REP at a restartable guest element boundary");
    need(!accelerate||pc.boot_acceleration().rep_elements>32000,
         "board never granted large REP batches");
    return std::array<std::uint64_t,4>{ecx,cpu.instructions,board.evidence().ticks,board.evidence().pit_irqs};
  };
  need(check(false)==check(true),"batched timer delivery crossed a guest interrupt deadline");
}
void shift_boot_with_pit_guest(){
  const auto check=[](bool accelerate,std::uint8_t period_low){
    PcMachine pc(16*1024*1024);
    pc.boot_demo();
    PcMotherboardC06 board(pc);
    pc.enable_boot_acceleration(accelerate);
    pc.enable_boot_hotspots();
    auto& cpu=pc.cpu;
    cpu.set_reg16(Cpu::ESP,0x8800);
    cpu.write16(0x40*4,0x9000);
    cpu.write16(0x40*4+2,0);
    code(cpu,0x9000,{
        0x89,0x0e,0x04,0x91,           // Record interrupted CX.
        0x50,0x55,0x89,0xe5,           // Save AX and BP, then set BP to SP.
        0x8b,0x46,0x04,0xa3,0x08,0x91, // Record interrupted IP.
        0xb0,0xff,0xe6,0x21,           // Mask PIT so it fires once.
        0xb0,0x20,0xe6,0x20,           // Acknowledge the PIC.
        0x5d,0x58,0xcf});              // Restore BP and AX, then IRET.
    std::vector<std::uint8_t> program{0xfa};
    const auto out=[&](std::uint8_t value,std::uint8_t port){emit(program,{0xb0,value,0xe6,port});};
    out(0x11,0x20);out(0x40,0x21);out(0x04,0x21);out(0x01,0x21);out(0xfe,0x21);
    out(0x34,0x43);out(period_low,0x40);out(0x01,0x40);
    emit(program,{0xb9,0x00,0x20,0xb8,0x5a,0xa5,0xba,0x7d,0xa2,0xfb});
    constexpr auto entry=0xa000;
    const auto loop_ip=entry+program.size();
    emit(program,{0xd1,0xea,0xd1,0xd8,0xe2,0xfa,0xfa,0xf4});
    code(cpu,entry,program);
    cpu.eip=entry;
    for(unsigned slice=0;slice<8&&!cpu.halted;++slice)run(pc,4091);
    const auto ip=cpu.read16(0x9108);
    const auto cx=cpu.read16(0x9104);
    need(cpu.halted&&cpu.reg16(Cpu::ECX)==0&&cpu.reg16(Cpu::EAX)==0&&
         cpu.reg16(Cpu::EDX)==0&&cpu.reg16(Cpu::ESP)==0x8800,
         "shift loop and IRQ must preserve registers and balance the interrupt stack");
    need(board.evidence().irq_delivered==1&&cx>0&&cx<8192&&
         ip>=loop_ip&&ip<loop_ip+6&&((ip-loop_ip)%2)==0,
         "PIT must interrupt at a shift, rotate or LOOP boundary");
    need(!accelerate||pc.boot_acceleration().shift_loop_steps>23000,
         "board never granted large shift-loop batches");
    return std::array<std::uint64_t,6>{ip,cx,cpu.instructions,cpu.eflags,
                                     board.evidence().ticks,board.evidence().pit_irqs};
  };
  for(const auto period:{0x2c,0x2d,0x2e})
    need(check(false,period)==check(true,period),"shift batches changed the PIT interrupt boundary");
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
  const auto dsdt_size=pc.cpu.read32(AcpiC06::dsdt_address+4);
  const std::array<std::pair<std::uint32_t,unsigned>,5> tables{{
    {AcpiC06::rsdt_address,44U},{AcpiC06::xsdt_address,52U},
    {AcpiC06::fadt_address,244U},{AcpiC06::madt_address,64U},
    {AcpiC06::dsdt_address,dsdt_size}
  }};
  for(const auto& entry:tables)
    need(checksum(pc.cpu,entry.first,entry.second)==0,"ACPI table checksum invalid");
  need(pc.cpu.read32(AcpiC06::rsdt_address+36)==AcpiC06::fadt_address &&
       pc.cpu.read64(AcpiC06::xsdt_address+44)==AcpiC06::madt_address,
       "ACPI table pointers do not describe the actual firmware");
  need(pc.cpu.read32(AcpiC06::madt_address+36)==0xfee00000U,
       "MADT LAPIC base does not match the mapped MMIO device");
  const auto facs=pc.cpu.read32(AcpiC06::fadt_address+36);
  need(facs==AcpiC06::facs_address && (facs&63U)==0 &&
       pc.cpu.read64(AcpiC06::fadt_address+132)==facs &&
       pc.cpu.read32(facs)==0x53434146U && pc.cpu.read32(facs+4)==64 &&
       pc.cpu.read32(facs+16)==0,
       "FADT did not expose aligned writable FACS with an unlocked Global Lock");

  const auto aml=AcpiC06::dsdt_address+36;
  const auto scope=aml+13;
  const auto root=scope+8;
  const auto package_end=[&](std::uint32_t length){
    const auto lead=pc.cpu.read8(length);
    const auto count=lead>>6;
    unsigned size=lead&(count?15U:63U);
    for(unsigned i=0;i<unsigned(count);++i)
      size|=unsigned(pc.cpu.read8(length+1+i))<<(4+8*i);
    return length+size;
  };
  need(pc.cpu.read8(scope)==0x10 &&
       pc.cpu.read32(scope+4)==0x5f42535fU && pc.cpu.read16(root)==0x825b &&
       pc.cpu.read32(root+4)==0x30494350U && pc.cpu.read32(root+14)==0x030ad041U &&
       package_end(scope+1)==AcpiC06::dsdt_address+dsdt_size &&
       package_end(root+2)==package_end(scope+1),
       "DSDT does not describe the PCI0 root in the system bus scope");
  const auto crs=root+30;
  const auto resources=crs+10;
  const std::array<std::pair<std::uint16_t,std::uint16_t>,5> ranges{{
    {0,1},{0x170,8},{0x1f0,8},{0x376,1},{0x3f6,1}
  }};
  need(pc.cpu.read32(crs+1)==0x5352435fU && pc.cpu.read8(crs+5)==0x11 &&
       pc.cpu.read8(crs+9)==82 && package_end(crs+6)==resources+82 &&
       pc.cpu.read16(resources+80)==0x0079,
       "PCI0 resource template has invalid AML boundaries");
  for(unsigned i=0;i<ranges.size();++i){
    const auto entry=resources+16*i;
    const auto [first,count]=ranges[i];
    need(pc.cpu.read16(entry)==0x0d88 && pc.cpu.read8(entry+3)==(i==0?2:1) &&
         pc.cpu.read16(entry+8)==first && pc.cpu.read16(entry+10)==first+count-1 &&
         pc.cpu.read16(entry+14)==count,
         "PCI0 resource template does not match the fixed bus and IDE ports");
  }
  const auto prt=resources+82;
  const auto reserved=prt+8;
  need(pc.cpu.read32(prt+1)==0x5452505fU && pc.cpu.read8(prt+7)==0 &&
       pc.cpu.read16(reserved)==0x825b && pc.cpu.read32(reserved+3)==0x30534552U &&
       package_end(reserved+2)==package_end(root+2) &&
       pc.cpu.read32(reserved+13)==0x020cd041U &&
       pc.cpu.read32(reserved+26)==0x0cf80147U &&
       pc.cpu.read32(reserved+30)==0x08010cf8U &&
       pc.cpu.read16(reserved+34)==0x0079,
       "DSDT incorrectly routes PCI INTx or does not reserve PCI config ports");

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
          0x66,0xed,0x66,0xa3,0x10,0x90,0,0, // SCI_EN after firmware init
          0x66,0xb8,0x00,0x34,    // MOV AX, 3400 (S5 + SLP_EN)
          0x66,0xef,             // OUT DX, AX
          0x66,0xed,0x66,0xa3,0x12,0x90,0,0, // SCI_EN stays set, SLP_EN is write-only
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
  need(pc.cpu.read16(0x9010)==1 && pc.cpu.read16(0x9012)==0x1401,
       "guest ACPI PM1a control did not preserve SCI_EN and SLP_TYP");
  need(board.evidence().powered_off,
       "guest ACPI PM1a S5 request not recognized");
  pc.cpu.write32(facs+16,2);
  need(pc.cpu.read32(facs+16)==2,"FACS Global Lock is not writable");
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
  need(pc.cpu.read32(facs+16)==0 &&
       pc.cpu.read32(AcpiC06::fadt_address+36)==facs &&
       checksum(pc.cpu,AcpiC06::dsdt_address,dsdt_size)==0,
       "reboot did not restore FACS and PCI root discovery");
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
void cr8_lapic_priority_guest(){
  PcMachine pc(16*1024*1024);
  pc.boot_long_mode_demo();
  auto& cpu=pc.cpu;
  for(unsigned i=0;i<5000&&!cpu.is_long_mode();++i){
    if(cpu.step()==cosmic_pc::StepResult::Faulted)
      throw std::runtime_error(cpu.fault->describe());
  }
  need(cpu.is_long_mode(),"CR8 fixture did not execute an actual transition to long mode");
  PcMotherboardC06 board(pc);
  constexpr std::uint32_t apic=0xfee00000, entry=0xa000, handler=0x13000, stack=0x190000;
  constexpr std::uint8_t vector=0x42;
  cpu.write64(0x2000+(apic>>30)*8,0x4003);
  cpu.write64(0x4000+((apic>>21)&511)*8,std::uint64_t(apic)|0x83);
  cpu.idtr={0x1000,0x15000};
  cpu.write64(cpu.idtr.base+16*vector, (handler&0xffffU)|
      (std::uint64_t(cpu.segments[Cpu::CS])<<16)|(0x8eULL<<40)|
      (std::uint64_t(handler>>16)<<48));
  cpu.write64(cpu.idtr.base+16*vector+8,0);
  code(cpu,handler,{
      0xc6,0x04,0x25,0x00,0x90,0x00,0x00,vector, // MOV byte [0x9000], vector
      0x50,0xb8,0xb0,0x00,0xe0,0xfe,             // PUSH RAX; MOV EAX,LAPIC EOI
      0xc7,0x00,0x00,0x00,0x00,0x00,0x58,0x48,0xcf // EOI; POP RAX; IRETQ
  });
  std::vector<std::uint8_t> guest{0xfa};
  const auto write_reg=[&](std::uint32_t address,std::uint32_t value){
    emit(guest,{0xbf});
    for(unsigned i=0;i<4;++i)guest.push_back(std::uint8_t(address>>(8*i)));
    emit(guest,{0xc7,0x07});
    for(unsigned i=0;i<4;++i)guest.push_back(std::uint8_t(value>>(8*i)));
  };
  write_reg(apic+0x80,0xab);
  emit(guest,{
      0x45,0x0f,0x20,0x00,      // MOV R8,CR8; ModR/M MOD bits are ignored.
      0xb8,0x05,0x00,0x00,0x00, // MOV EAX,5
      0x44,0x0f,0x22,0xc0,      // MOV CR8,RAX
      0x8b,0x17                 // MOV EDX,[RDI], the same LAPIC TPR.
  });
  write_reg(apic+0xf0,0x1ff);
  write_reg(apic+0x320,vector);
  write_reg(apic+0x380,3);
  emit(guest,{0xfb,0x90}); // STI; clear the one-instruction interrupt shadow.
  const auto wait_ip=entry+guest.size();
  emit(guest,{0xeb,0xfe});
  code(cpu,entry,guest);
  cpu.eip=entry;
  cpu.regs[Cpu::ESP]=stack;
  run(pc,100);
  need(cpu.eip==wait_ip && cpu.regs[Cpu::R8]==0xa && cpu.regs[Cpu::EDX]==0x50,
       "CR8 and LAPIC MMIO must share priority, and a CR8 write must clear low TPR bits");
  need(board.evidence().apic_irqs==0&&cpu.read8(0x9000)==0,
       "CR8 must hold a pending lower-priority LAPIC timer interrupt");
  code(cpu,static_cast<std::uint32_t>(wait_ip),{0x31,0xc0,0x44,0x0f,0x22,0xc0,0xf4});
  run(pc,64);
  need(cpu.halted&&board.evidence().apic_irqs==1&&cpu.read8(0x9000)==vector&&
       cpu.regs[Cpu::ESP]==stack,
       "lowering CR8 must release the pending IRQ and IRETQ must restore the stack");

  cpu.idtr={0,0};
  cpu.set_apic_task_priority(0x70);
  const auto expect_fault=[&](std::initializer_list<std::uint8_t> bytes,
                              cosmic_pc::FaultCode expected){
    code(cpu,entry,bytes);cpu.eip=entry;cpu.halted=false;cpu.fault.reset();
    const auto before=cpu.regs;
    need(cpu.step()==cosmic_pc::StepResult::Faulted&&cpu.fault->code==expected&&
         cpu.fault->error_code==0&&cpu.eip==entry&&cpu.regs==before&&
         cpu.apic_task_priority()==0x70,
         "invalid MOV control-register operation changed guest state or exception class");
  };
  cpu.regs[Cpu::EAX]=1ULL<<40;
  expect_fault({0x44,0x0f,0x22,0xc0},cosmic_pc::FaultCode::ProtectionFault);
  expect_fault({0x44,0x0f,0x20,0xc8},cosmic_pc::FaultCode::InvalidInstruction);
  expect_fault({0xf0,0x44,0x0f,0x22,0xc0},cosmic_pc::FaultCode::InvalidInstruction);

  // Enter actual ring 3 with a user-accessible identity mapping via IRETQ.
  cpu.write64(0x1000,0x2007);cpu.write64(0x2000,0x3007);cpu.write64(0x3000,0x87);
  cpu.gdtr={0x2f,0x14000};
  cpu.write64(0x14018,0x00af9b000000ffffULL);
  cpu.write64(0x14020,0x00affb000000ffffULL);
  cpu.write64(0x14028,0x00cff3000000ffffULL);
  code(cpu,entry,{0x6a,0x2b,0x68,0x00,0x00,0x1a,0x00,0x6a,0x02,
                  0x6a,0x23,0x68,0x00,0xa1,0x00,0x00,0x48,0xcf});
  cpu.eip=entry;cpu.fault.reset();
  for(unsigned i=0;i<6;++i)need(cpu.step()==cosmic_pc::StepResult::Executed,"ring 3 entry failed");
  need(cpu.cpl()==3&&cpu.eip==0xa100,"CPL3 test did not enter guest ring 3");
  code(cpu,0xa100,{0x44,0x0f,0x20,0xc0});
  need(cpu.step()==cosmic_pc::StepResult::Faulted&&
       cpu.fault->code==cosmic_pc::FaultCode::ProtectionFault&&cpu.fault->error_code==0&&
       cpu.apic_task_priority()==0x70,"CPL3 must not access CR8");
  pc.boot_long_mode_demo();
  need(cpu.apic_task_priority()==0&&cpu.read32(apic+0x80)==0,"reset retained a previous CR8/TPR value");
  std::cout<<"C06_CR8_LAPIC_TPR_PASS mmio=1 pending_irq=1 precise_faults=1 reset=1\n";
}
}
int main(){
  try{
    pic_pit_guest();
    rep_boot_with_pit_guest(false);
    rep_boot_with_pit_guest(true);
    shift_boot_with_pit_guest();
    std::cout<<"ETHER_BOOT_IRQ_TIMING_PASS pit=1 lapic=1 shift_loop=1\n";
    optical_irq_wakes_sti_hlt();
    pic_legacy_x87_guest();
    pci_acpi_cmos_guest();
    ioapic_lapic_guest();
    ps2_keyboard_guest();
    queued_lapic_requests_guest();
    cr8_lapic_priority_guest();
    std::cout<<"REVERSE_WINDOWS_C06_MOTHERBOARD_PASS checks="<<checks
             <<" windows_booted=false full_acpi=false full_apic=false\n";
    return 0;
  }catch(const std::exception& error){
    std::cerr<<"REVERSE_WINDOWS_C06_MOTHERBOARD_FAIL "<<error.what()<<"\n";
    return 1;
  }
}
