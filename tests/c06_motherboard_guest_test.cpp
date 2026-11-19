#include "c06_motherboard.hpp"
#include "pc_machine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <sstream>
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
void paged_boot_batches_with_timer(){
  enum class Fixture { Rep, Scan, Arm64, Counter };
  const auto check=[](bool accelerate,Fixture fixture,unsigned deadline){
    PcMachine pc(16*1024*1024);
    pc.boot_long_mode_demo();run(pc,12000);
    auto& cpu=pc.cpu;
    need(cpu.halted&&cpu.is_long_mode(),"paged timer fixture did not boot");
    PcMotherboardC06 board(pc);
    pc.enable_boot_acceleration(accelerate);
    cpu.enable_page_translation_cache(accelerate);
    cpu.enable_ether_arm64_blocks(accelerate &&
        (fixture == Fixture::Arm64 || fixture == Fixture::Counter));
    pc.enable_boot_hotspots();
    constexpr auto apic=Cpu::kLocalApicAddress;
    // Map the APIC for the guest handler. The address remains MMIO.
    cpu.write64(0x2000+3*8,0x4003);
    cpu.write64(0x4000+503*8,apic|0x83);
    constexpr unsigned entry=0xa000, into=0x24000, total=8192, idt=0x18000, handler=0x19000;
    constexpr unsigned log=0xb000;
    cpu.idtr={0x7ff,idt};
    cpu.write64(idt+0x42*16,std::uint64_t(handler&0xffff)|0x8e0000180000ULL|
                (std::uint64_t(handler>>16)<<48));
    cpu.write64(idt+0x42*16+8,0);
    code(cpu,handler,{
        0x50,0x89,0x0c,0x25,0x00,0xb0,0,0,
        0x48,0x8b,0x44,0x24,0x08,
        0x48,0x89,0x04,0x25,0x08,0xb0,0,0,
        0x48,0xb8,0xb0,0,0xe0,0xfe,0,0,0,0,
        0xc7,0x00,0,0,0,0,0x58,0x48,0xcf});
    cpu.write32(apic+0xf0,0x1ff);
    cpu.write32(apic+0x320,0x42);
    cpu.write32(apic+0x380,deadline);
    cpu.eip=entry;cpu.halted=false;
    cpu.regs[Cpu::ESP]=0x7000;
    cpu.eflags=2|Cpu::IF;
    if (fixture == Fixture::Arm64) {
      code(cpu,entry,{0x48,0x8b,0x36,0x49,0x3b,0xf6,0x75,0xf8,0xfa,0xf4});
      constexpr unsigned chain = 1024;
      for (unsigned n = 0; n < chain; ++n)
        cpu.write64(into + n * 8, into + (n + 1) * 8);
      cpu.regs[Cpu::ESI] = into;
      cpu.regs[Cpu::R14] = into + chain * 8;
    } else if(fixture == Fixture::Scan) {
      code(cpu,entry,{0x44,0x8d,0x40,0x01,0x48,0xff,0xc2,0x41,0x8b,0xc0,
                      0x41,0x89,0x81,0xd8,0x2e,0,0,0x80,0x3a,0xe8,0x75,0xea,0xfa,0xf4});
      cpu.memory.fill(into,0x8c,total);
      cpu.write8(into+total-1,0xe8);
      cpu.regs[Cpu::R9]=0xc000-0x2ed8;
      cpu.regs[Cpu::EDX]=into-1;
      cpu.regs[Cpu::EAX]=0;
    } else if (fixture == Fixture::Counter) {
      code(cpu,entry,{0x48,0xff,0xc9,0x75,0xfb,0xfa,0xf4}); // DEC RCX; JNZ; CLI; HLT.
      cpu.regs[Cpu::ECX]=total;
      cpu.eflags|=Cpu::CF;
    }else{
      code(cpu,entry,{0xf3,0xaa,0xfa,0xf4});
      cpu.regs[Cpu::EDI]=into;
      cpu.regs[Cpu::EAX]=0x5a;
      cpu.regs[Cpu::ECX]=total;
    }
    const auto initial_steps = cpu.instructions;
    for(unsigned slice=0;slice<40&&!cpu.halted;++slice)run(pc,4091);
    const auto interrupted=cpu.read64(log+8);
    const auto last_ip = fixture == Fixture::Scan ? 20 : fixture == Fixture::Arm64 ? 6 :
        fixture == Fixture::Counter ? 3 : 0;
    need(cpu.halted&&cpu.regs[Cpu::ESP]==0x7000&&board.evidence().irq_delivered==1&&
        interrupted>=entry&&interrupted<=entry+last_ip,"paged guest lost the live timer boundary");
    if (fixture == Fixture::Arm64) {
      need(cpu.regs[Cpu::ESI] == cpu.regs[Cpu::R14] &&
           (!accelerate || !Cpu::ether_arm64_host() ||
            pc.boot_acceleration().arm64_block_steps > 2900),
           "ARM64 blocks did not finish the same pointer chain with the live LAPIC");
    } else if(fixture == Fixture::Scan) {
      need(cpu.regs[Cpu::EDX]==into+total-1&&cpu.read32(0xc000)==total&&
          (!accelerate||pc.boot_acceleration().scan_loop_steps>47000),"x64 scan did not finish or batch");
    } else if (fixture == Fixture::Counter) {
      need(cpu.regs[Cpu::ECX]==0 && cpu.eflags==(2|Cpu::CF|Cpu::PF|Cpu::ZF) &&
           cpu.read32(log)>0 && cpu.read32(log)<total &&
           (interrupted==entry || interrupted==entry+3) &&
           (!accelerate || pc.boot_acceleration().counter_loop_steps>16000),
           "counter loop must retire exactly and allow the LAPIC to see the original boundary");
    }else{
      need(cpu.regs[Cpu::ECX]==0&&cpu.read8(into+total-1)==0x5a&&
          (!accelerate||pc.boot_acceleration().paged_rep_elements>8100),"paged REP did not finish or batch");
    }
    const auto batched = pc.boot_acceleration().rep_elements + pc.boot_acceleration().scan_loop_steps +
        pc.boot_acceleration().arm64_block_steps + pc.boot_acceleration().counter_loop_steps;
    const auto count_steps = [&](const cosmic_pc::BootHotspots& profile) {
      std::ostringstream result;
      profile.write_json(result);
      return result.str();
    };
    const auto total_profile = count_steps(*pc.boot_hotspots());
    const auto scalar_profile = count_steps(*pc.boot_scalar_hotspots());
    const auto observed = [](std::uint64_t steps) {
      return "\"observed_steps\":" + std::to_string(steps) + ",";
    };
    // The setup and software timer probes completed before sampling began.
    need(total_profile.find(observed(cpu.instructions - initial_steps)) != std::string::npos &&
             scalar_profile.find(observed(cpu.instructions - initial_steps - batched)) != std::string::npos,
         "Scalar sampling must exclude every completed batch and keep the logical total");
    need(scalar_profile.find("\"sampled_step_ns\":") != std::string::npos &&
             (accelerate || scalar_profile.find("\"estimated_sample_ns\":") != std::string::npos),
         "Scalar hotspots must report sampled execution duration");
    return std::array<std::uint64_t,6>{interrupted,cpu.read32(log),cpu.instructions,
        cpu.eflags,board.evidence().ticks,cpu.clock_cycles()};
  };
  for(const auto fixture:{Fixture::Rep,Fixture::Scan,Fixture::Arm64,Fixture::Counter}){
    for (const auto deadline : {307U, 308U}) {
      const auto scalar=check(false,fixture,deadline);
      const auto accelerated=check(true,fixture,deadline);
      need(scalar==accelerated,"paged acceleration changed the timer boundary or guest result");
    }
  }
  std::cout<<"ETHER_PAGED_BATCH_IRQ_PASS rep=1 byte_scan=1 arm64="<<Cpu::ether_arm64_host()<<" counter=1\n";
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
void zero_boot_with_lapic_guest(){
  const auto check=[](bool accelerate, unsigned deadline){
    PcMachine pc(16*1024*1024);boot32(pc);
    PcMotherboardC06 board(pc);
    pc.enable_boot_acceleration(accelerate);
    pc.enable_boot_hotspots();
    auto& cpu=pc.cpu;
    cpu.idtr={0x1000,0x15000};
    cpu.regs[Cpu::ESP]=0x190000;
    gate(cpu,0x42,0x13000);
    code(cpu,0x13000,{
        0x89,0x0d,0x00,0x91,0,0, // Save ECX.
        0x89,0x15,0x04,0x91,0,0, // Save EDX.
        0x50,0x8b,0x44,0x24,0x04,0xa3,0x08,0x91,0,0, // Save interrupted EIP.
        0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0,0x58,0xcf});
    cpu.write32(0xfee000f0,0x1ff);
    cpu.write32(0xfee00320,0x42);
    cpu.write32(0xfee00380,deadline);

    constexpr unsigned entry=0xa000, dst=0x200ffd, count=8194;
    cpu.memory.fill(dst-1,0x5a,count+2);
    cpu.regs[Cpu::EDX]=dst;
    cpu.regs[Cpu::ECX]=count;
    cpu.eflags=2|Cpu::IF|Cpu::DF|Cpu::CF|Cpu::OF;
    code(cpu,entry,{0xc6,0x02,0,0x42,0x83,0xe9,1,0x75,0xf7,0xfa,0xf4});
    cpu.eip=entry;
    for(unsigned slice=0;slice<12&&!cpu.halted;++slice)run(pc,4091);
    const auto ip=cpu.read32(0x9108), ecx=cpu.read32(0x9100), edx=cpu.read32(0x9104);
    std::vector<std::uint8_t> data(count);
    cpu.memory.read_block(dst,data.data(),data.size());
    need(cpu.halted&&cpu.regs[Cpu::ECX]==0&&cpu.regs[Cpu::EDX]==dst+count&&
         cpu.regs[Cpu::ESP]==0x190000&&cpu.eflags==(2|Cpu::DF|Cpu::PF|Cpu::ZF),
         "zero loop must preserve final flags, registers and IRQ stack");
    need(std::all_of(data.begin(),data.end(),[](auto byte){return byte==0;})&&
         cpu.read8(dst-1)==0x5a&&cpu.read8(dst+count)==0x5a,
         "zero loop changed bytes outside its exact multi-page range");
    need(board.evidence().apic_irqs==1&&ecx>0&&ecx<count&&
         (ip==entry||ip==entry+3||ip==entry+4||ip==entry+7),
         "LAPIC must interrupt between the original guest opcodes");
    need(!accelerate||pc.boot_acceleration().zero_loop_steps>32000,
         "the protected32 zero loop did not use bounded batches");
    return std::array<std::uint64_t,6>{ip,ecx,edx,cpu.instructions,cpu.eflags,board.evidence().ticks};
  };
  for(const auto deadline:{300U,301U,302U,303U})
    need(check(false,deadline)==check(true,deadline),"zero batches changed the LAPIC interrupt boundary");

  PcMachine pc(16*1024*1024);boot32(pc);
  auto& cpu=pc.cpu;
  code(cpu,0xa000,{0xc6,0x02,0,0x42,0x83,0xe9,1,0x75,0xf7,0xf4});
  cpu.eip=0xa000;
  cpu.regs[Cpu::ECX]=80;
  cpu.regs[Cpu::EDX]=0x200000;
  run(pc,4);
  need(cpu.zero_loop_batch_candidate(),"scalar JNZ did not seed the zero loop");
  cpu.memory[0xa002]=0x5a;
  need(cpu.try_zero_loop_batch(128)==0,"changed loop bytes must reject batching");
  cpu.memory[0xa002]=0;
  cpu.regs[Cpu::EDX]=0xa000;
  need(cpu.try_zero_loop_batch(128)==0,"writes that overlap the loop must remain scalar");
  cpu.regs[Cpu::EDX]=0x200000;
  cpu.memory.attach_window(0x200010,1,[](std::uint64_t){return std::uint8_t{0};},
                            [](std::uint64_t,std::uint8_t){});
  need(cpu.try_zero_loop_batch(128)==0,"Ether memory must retain byte callbacks");
  cpu.mmio_claim_hook=[](std::uint64_t address){return address==0x200210;};
  cpu.mmio_range_claim_hook=[](std::uint64_t address,std::size_t bytes){
    return address<=0x200210&&0x200210-address<bytes;
  };
  cpu.regs[Cpu::EDX]=0x200200;
  need(cpu.try_zero_loop_batch(128)==0,"MMIO inside the span must retain device writes");
  cpu.regs[Cpu::EDX]=0x200300;
  cpu.step_trace_hook=[](const auto&){};
  need(cpu.try_zero_loop_batch(128)==0,"exact instruction traces must remain scalar");
  cpu.step_trace_hook={};
  need(cpu.try_zero_loop_batch(17)==16&&cpu.eip==0xa000&&cpu.regs[Cpu::ECX]==75,
       "only whole zero-loop iterations may retire within a slice");
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
  const std::array<std::uint8_t,2048> sector{};
  PcMotherboardC06 board(pc,nullptr,[&](auto offset,auto* into,auto bytes){
    if(offset>sector.size()||bytes>sector.size()-offset)return false;
    std::copy_n(sector.data()+offset,bytes,into);
    return true;
  },sector.size());
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
    {AcpiC06::fadt_address,244U},{AcpiC06::madt_address,AcpiC06::madt_bytes},
    {AcpiC06::dsdt_address,dsdt_size}
  }};
  for(const auto& entry:tables)
    need(checksum(pc.cpu,entry.first,entry.second)==0,"ACPI table checksum invalid");
  need(pc.cpu.read32(AcpiC06::rsdt_address+36)==AcpiC06::fadt_address &&
       pc.cpu.read64(AcpiC06::xsdt_address+44)==AcpiC06::madt_address,
       "ACPI table pointers do not describe the actual firmware");
  need(pc.cpu.read32(AcpiC06::madt_address+36)==0xfee00000U,
       "MADT LAPIC base does not match the mapped MMIO device");
  need((pc.cpu.read16(AcpiC06::fadt_address+109)&0x0fU)==0x0fU,
       "FADT must keep MSI disabled when the PCI GPU is absent");
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
          0xb8,0x40,0,0,0x80,       // RAM clock Hz, low dword at 0:0.0 + 40h.
          0x66,0xba,0xf8,0x0c,0xef,0x66,0xba,0xfc,0x0c,0xed,
          0xa3,0x14,0x90,0,0,
          0xb8,0x44,0,0,0x80,       // High dword at 0:0.0 + 44h.
          0x66,0xba,0xf8,0x0c,0xef,0x66,0xba,0xfc,0x0c,0xed,
          0xa3,0x18,0x90,0,0,
          0xb8,0,0xf8,0,0x80,      // absent dev31
          0x66,0xba,0xf8,0x0c,0xef,
          0x66,0xba,0xfc,0x0c,0xed,
          0xa3,0x04,0x90,0,0,
          0xb0,0x0b,0xe6,0x70,0xe4,0x71, // CMOS status B
          0xa2,0x08,0x90,0,0,
          0xa1,0,0,0x0e,0,       // guest reads "RSD " at E0000
          0xa3,0x0c,0x90,0,0,
          0x8b,0x1d,0x10,0,0x0e,0, // RSDP -> RSDT address.
          0x8b,0x5b,0x24,        // RSDT[0] -> FADT address.
          0x8b,0x53,0x40,        // FADT.pm1a_cnt_blk -> DX.
          0x66,0xed,0x66,0xa3,0x10,0x90,0,0, // SCI_EN after firmware init
          0x66,0xb8,0x00,0x34,    // MOV AX, 3400 (S5 + SLP_EN)
          0x66,0xef,             // OUT DX, AX
          0x66,0xed,0x66,0xa3,0x12,0x90,0,0, // Must not execute after S5.
          0xf4});
  code(pc.cpu,0xa000,p);pc.cpu.eip=0xa000;pc.cpu.halted=false;
  pc.cpu.regs[Cpu::ESP]=0x190000;
  run(pc,100);
  need(pc.cpu.read32(0x9000)==0x00011b36,
       "real x86 guest PCI CF8/CFC failed to read host bridge ID");
  const auto guest_pci_id=pc.cpu.read32(0x9000);
  need(pc.cpu.read64(0x9014)==4'500'000'000'000ULL,
       "x86 guest must discover the full 64-bit RAM clock in the host bridge");
  need(pc.cpu.read32(0x9004)==0xffffffffU,
       "nonexistent PCI device must read all ones");
  need(pc.cpu.memory[0x9008]==2,"guest CMOS status B does not indicate BCD 24-hour");
  need(pc.cpu.read32(0x900c)==0x20445352U,
       "guest did not discover ACPI RSDP signature via real MOV");
  need(pc.cpu.read16(0x9010)==1&&pc.cpu.read16(0x9012)==0&&pc.status()=="powered_off"&&
       pc.cpu.in_hook(AcpiC06::pm_control_port,2)==0x1401&&board.evidence().powered_off,
       "S5 must latch SLP_TYP and stop the guest before the next instruction");
  const auto off_ticks=board.evidence().ticks;
  run(pc,100);
  need(board.evidence().ticks==off_ticks,"Powered-off board must be quiescent");
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
  need(board.ram_clock_cycles()==0,"RAM clock must restart with the motherboard");
  need(pc.cpu.read32(facs+16)==0 &&
       pc.cpu.read32(AcpiC06::fadt_address+36)==facs &&
       checksum(pc.cpu,AcpiC06::dsdt_address,dsdt_size)==0,
       "reboot did not restore FACS and PCI root discovery");
  std::cout<<"C06_PCI_ACPI_CMOS_REAL_GUEST_PASS pci=0x"<<std::hex
           <<guest_pci_id<<std::dec<<" tables=5 reboot=PASS\n";
}
void ram_refresh_guest() {
  PcMachine pc(4U << 20);
  pc.boot_demo();
  PcMotherboardC06 board(pc);
  auto& cpu = pc.cpu;
  cpu.reset(0, 0x7200);
  code(cpu, 0x7200, {0xe4, 0x61}); // IN AL,61h reads the DRAM refresh output.
  const auto refresh = [&] {
    cpu.eip = 0x7200;
    need(cpu.step() == cosmic_pc::StepResult::Executed, "Guest RAM refresh read");
    return (cpu.regs[Cpu::EAX] & 0x10U) != 0;
  };
  pc.device_batch_advance_hook(53);
  need(!refresh(), "Refresh must stay low until RAM cycle 2^26");
  board.advance_one();
  need(refresh(), "First RAM refresh edge");
  pc.device_batch_advance_hook(52);
  need(refresh(), "Refresh must stay high until RAM cycle 2^27");
  board.advance_one();
  need(!refresh(), "Second RAM refresh edge");
}

void prime_rtc_cmos_guest() {
  const auto exercise = [](bool apic, bool accelerate) {
    PcMachine pc(16U << 20);
    pc.boot_long_mode_demo();
    run(pc, 4000);
    auto& cpu = pc.cpu;
    need(cpu.halted && cpu.is_long_mode() && cpu.cpl() == 0,
         "RTC fixture did not enter long mode at CPL0");
    PcMotherboardC06 board(pc);
    pc.enable_boot_acceleration(accelerate);
    constexpr std::uint32_t entry=0xa000, handler=0x13000, stack=0x190000;
    const std::uint8_t vector=apic ? 0x58 : 0x70;
    cpu.idtr = {0x1000, 0x15000};
    const auto descriptor = cpu.idtr.base + 16 * vector;
    cpu.write64(descriptor, (handler & 0xffffU) |
        (std::uint64_t(cpu.segments[Cpu::CS]) << 16) | (0x8eULL << 40) |
        (std::uint64_t(handler >> 16) << 48));
    cpu.write64(descriptor + 8, 0);
    if (apic) {
      constexpr auto lapic=PcMotherboardC06::kLocalApic, ioapic=PcMotherboardC06::kIoApic;
      cpu.write64(0x2000 + 3 * 8, 0x4003);
      cpu.write64(0x4000 + ((ioapic >> 21) & 511) * 8, ioapic | 0x83);
      cpu.write64(0x4000 + ((lapic >> 21) & 511) * 8, lapic | 0x83);
      cpu.write32(lapic + 0xf0, 0x1ff);
      cpu.write32(ioapic, 0x21);
      cpu.write32(ioapic + 0x10, 0); // Destination LAPIC ID 0.
      cpu.write32(ioapic, 0x20);
      cpu.write32(ioapic + 0x10, vector); // GSI8, edge-triggered, unmasked.
    }

    const auto out = [](auto& bytes, std::uint8_t port, std::uint8_t value) {
      emit(bytes, {0x66,0xba,port,0,0xb0,value,0xee}); // MOV DX,port; OUT DX,AL
    };
    const auto store = [](auto& bytes, std::uint8_t offset, std::uint8_t opcode) {
      emit(bytes, {opcode,0x04,0x25,offset,0x90,0,0}); // MOV [0x9000+offset],AL/EAX
    };
    const auto read_cmos = [&](auto& bytes, std::uint8_t index, std::uint8_t offset) {
      out(bytes, 0x70, index);
      emit(bytes, {0x66,0xba,0x71,0,0xec}); // IN AL,DX
      store(bytes, offset, 0x88);
    };
    const auto write_cmos = [&](auto& bytes, std::uint8_t index, std::uint8_t value) {
      out(bytes, 0x70, index);
      out(bytes, 0x71, value);
    };

    std::vector<std::uint8_t> irq{0x50,0x52}; // PUSH RAX; PUSH RDX
    read_cmos(irq, 0x0c, 0x07);
    read_cmos(irq, 0x0c, 0x08);
    if (apic) {
      emit(irq, {0xb8,0xb0,0x00,0xe0,0xfe,0xc7,0x00,0,0,0,0}); // LAPIC EOI.
    } else {
      out(irq, 0xa0, 0x20);
      out(irq, 0x20, 0x20);
    }
    emit(irq, {0x5a,0x58,0x48,0xcf}); // POP RDX; POP RAX; IRETQ
    code(cpu, handler, irq);

    std::vector<std::uint8_t> program{0xfa};
    read_cmos(program, 0x0a, 0x00);
    write_cmos(program, 0x8a, 0x25); // Select status A with NMI masked, then exact OUT from the fault.
    read_cmos(program, 0x8a, 0x01);
    write_cmos(program, 0x0c, 0xff);
    read_cmos(program, 0x0c, 0x02);
    write_cmos(program, 0x0d, 0);
    read_cmos(program, 0x0d, 0x03);
    write_cmos(program, 0x01, 0xc0); // Seconds alarm wildcard.
    read_cmos(program, 0x01, 0x04);
    write_cmos(program, 0x38, 0xa5);
    read_cmos(program, 0x38, 0x05);
    if (!apic) {
      out(program, 0x21, 0xfb); // Master PIC cascade.
      out(program, 0xa1, 0xfe); // Slave PIC IRQ8.
    }
    write_cmos(program, 0x0b, 0x42);
    read_cmos(program, 0x0b, 0x06);
    emit(program, {0xfb,0xf4,0x0f,0x31}); // STI; HLT; RDTSC
    store(program, 0x10, 0x89);
    emit(program, {0xf4,0x0f,0x31}); // Wait for the second RTC interrupt.
    store(program, 0x14, 0x89);
    emit(program, {0xfa,0xf4});
    code(cpu, entry, program);
    cpu.eip = entry;
    cpu.halted = false;
    cpu.regs[Cpu::ESP] = stack;
    run(pc, 512);
    need(cpu.halted && (cpu.read8(0x9000) & 0x7fU) == 0x26 &&
         (cpu.read8(0x9001) & 0x7fU) == 0x25 && !(cpu.read8(0x9002) & 0x80U) &&
         cpu.read8(0x9003) == 0x80 && cpu.read8(0x9004) == 0xc0 &&
         cpu.read8(0x9005) == 0xa5 && cpu.read8(0x9006) == 0x42 &&
         board.evidence().irq_delivered == 0,
         "RTC indexed write/read semantics or the first IRQ8 deadline are incorrect");
    run(pc, 1536);
    need(cpu.halted && board.evidence().irq_delivered == 1 && cpu.read32(0x9010) &&
         !cpu.read32(0x9014), "RTC did not wake the guest through the selected interrupt route");
    run(pc, 1600);
    constexpr auto period = PcMachine::kVirtualCyclesPerSecond / 2048U;
    const auto elapsed = cpu.read32(0x9014) - cpu.read32(0x9010);
    need(cpu.halted && !cpu.flag(Cpu::IF) && cpu.regs[Cpu::ESP] == stack &&
         board.evidence().irq_delivered == 2 && board.evidence().apic_irqs == (apic ? 2U : 0U) &&
         (cpu.read8(0x9007) & 0xc0U) == 0xc0U &&
         !(cpu.read8(0x9008) & 0xc0U) && elapsed >= period * Cpu::kTscTicksPerCycle &&
         elapsed <= (period + 1) * Cpu::kTscTicksPerCycle,
         "RTC periodic IRQ8 interval or status C acknowledgment is incorrect");
  };
  for (bool apic : {false, true})
    for (bool accelerate : {false, true}) exercise(apic, accelerate);
  std::cout << "PRIME_RTC_CMOS_X64_PASS rate_hz=2048 pic_irq8=2 ioapic_irq8=2 status_c_read_clear=1\n";
}

void rtc_irq8_during_service() {
  constexpr auto lapic = PcMotherboardC06::kLocalApic, ioapic = PcMotherboardC06::kIoApic;
  constexpr unsigned vector = 0xd1, handler = 0x13000, stack = 0x190000;
  constexpr unsigned period = (PcMachine::kVirtualCyclesPerSecond + 2047) / 2048;
  for (bool level : {false, true}) {
    PcMachine pc(16U << 20);
    boot32(pc);
    PcMotherboardC06 board(pc);
    auto& cpu = pc.cpu;
    cpu.idtr = {0x1000, 0x15000};
    cpu.regs[Cpu::ESP] = stack;
    gate(cpu, vector, handler);
    code(cpu, handler, {
        0xe4,0x71,                              // IN AL,71h: acknowledge RTC register C.
        0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0,    // MOV DWORD [LAPIC EOI],0
        0xcf                                    // IRETD
    });
    const auto step = [&] {
      need(cpu.step() == cosmic_pc::StepResult::Executed, "IRQ8 handler instruction failed");
    };
    const auto next_period = [&] {
      for (unsigned i = 0; i < period; ++i) board.advance_one();
    };
    cpu.write32(lapic + 0xf0, 0x1ff);
    cpu.write32(ioapic, 0x20);                  // GSI8 redirection, physical destination 0.
    const auto route = vector | (level ? 1U << 15 : 0);
    cpu.write32(ioapic + 0x10, route | (1U << 16));
    cpu.out_hook(0x70, 1, 0x0a); cpu.out_hook(0x71, 1, 0x25);
    cpu.out_hook(0x70, 1, 0x0b); cpu.out_hook(0x71, 1, 0x42);
    cpu.out_hook(0x70, 1, 0x0c);
    cpu.eip = 0xa000;
    code(cpu, 0xa000, {0xfb,0x90,0xf4});        // STI; NOP clears the interrupt shadow.
    step();
    step();
    next_period();
    need(board.rtc_evidence().register_c == 0xc0 && board.evidence().apic_irqs == 0,
         "masked RTC input was not held at the device");
    cpu.write32(ioapic + 0x10, route);
    board.advance_one();
    if (!level) {
      need(board.evidence().apic_irqs == 0, "unmasking an edge input invented an RTC edge");
      need(cpu.in_hook(0x71, 1) == 0xc0, "RTC did not acknowledge the masked edge");
      next_period();
    }
    const auto first = board.interrupt_state();
    need(cpu.eip == handler && first.in_service == vector && first.in_service_delivery_id == 1,
         level ? "held level IRQ8 was lost on unmask" : "new RTC edge was lost after unmask");
    step();
    need(board.rtc_evidence().register_c == 0, "guest IN did not clear the first RTC request");
    next_period();                              // Another RTC pulse before the first LAPIC EOI.
    need(board.rtc_evidence().register_c == 0xc0 && board.evidence().apic_irqs == 1,
         "a second request must wait while the first handler is in service");
    step();                                     // EOI for the first interrupt.
    step();                                     // IRETD restores IF.
    board.advance_one();
    const auto second = board.interrupt_state();
    need(cpu.eip == handler && second.in_service == vector && second.in_service_delivery_id == 2 &&
         second.in_service_since_tick > first.in_service_since_tick && board.evidence().apic_irqs == 2,
         "RTC request asserted between register C read and EOI was lost");
    step(); step(); step();
    board.advance_one();
    const auto idle = board.interrupt_state();
    const auto rtc = board.rtc_evidence();
    need(board.evidence().apic_irqs == 2 && idle.pending == 0 && idle.in_service == 0 &&
         idle.in_service_delivery_id == 0 && idle.in_service_since_tick == 0 &&
         cpu.regs[Cpu::ESP] == stack && rtc.irq8_acknowledgements == rtc.irq8_assertions &&
         rtc.irq8_redirection_low == route && cpu.read32(ioapic + 0x10) == route,
         "acknowledged RTC request repeated or remained in service");
  }
  std::cout << "PRIME_RTC_IRQ8_DURING_SERVICE_PASS vector=0xd1 edge=1 level=1\n";
}

void prime_pci_inventory() {
  struct Disk final : reverse_windows::BlockDisk {
    std::uint64_t bytes()const noexcept override{return 512;}
    std::uint64_t sectors()const noexcept override{return 1;}
    bool read_bytes(std::uint64_t,std::uint8_t*,std::size_t)override{return false;}
    bool read(std::uint64_t,std::uint16_t,std::uint8_t*)override{return false;}
    bool write(std::uint64_t,std::uint16_t,const std::uint8_t*)override{return false;}
    bool flush()override{return true;}
  } disk;
  const std::array<std::uint8_t,2048> sector{};
  const auto reader=[&](std::uint64_t offset,std::uint8_t* out,std::size_t count){
    if(offset>sector.size()||count>sector.size()-offset)return false;
    std::copy_n(sector.data()+offset,count,out);
    return true;
  };
  for(unsigned media=0;media<4;++media){
    PcMachine pc(16U<<20);boot32(pc);
    const bool fixed=(media&1)!=0, optical=(media&2)!=0;
    PcMotherboardC06 board(pc,fixed?&disk:nullptr,optical?reverse_windows::AtapiCdrom::Reader(reader):
                           reverse_windows::AtapiCdrom::Reader{},optical?sector.size():0);
    auto& cpu=pc.cpu;
    constexpr unsigned entry=0xa000,result=0x9200;
    // Enumerate all 32 PCI slots with real CF8/CFC accesses. Continue the
    // same probe through compatibility mode, decode disable and re-enable.
    std::vector<std::uint8_t> program{
      0xfa,0xfc,0xbf,0,0x92,0,0,          // CLI; CLD; MOV EDI,result
      0xbb,0,0,0,0x80,0xb9,0x20,0,0,0,  // EBX=config; ECX=32
      0x66,0xba,0xf8,0x0c,0x89,0xd8,0xef,
      0x66,0xba,0xfc,0x0c,0xed,0xab,
      0x81,0xc3,0,0x08,0,0,0xe2,0xeb};  // next device and LOOP
    const auto select=[&](std::uint32_t key){
      emit(program,{0x66,0xba,0xf8,0x0c,0xb8});
      for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(key>>(8*i)));
      emit(program,{0xef,0x66,0xba,0xfc,0x0c});
    };
    const auto status=[&](std::uint16_t port){
      emit(program,{0x66,0xba,std::uint8_t(port),std::uint8_t(port>>8),0xec,0xaa});
    };
    select(0x80001008);emit(program,{0xed,0xab}); // Class and programming interface.
    status(0x1f7);status(0x177);
    select(0x80001004);emit(program,{0xb0,0x06,0xee,0xec,0xaa});
    // A command to disabled ports must neither fault nor reach the device.
    emit(program,{0x66,0xba,0xf7,0x01,0xb0,0xec,0xee});
    emit(program,{0x66,0xba,0x77,0x01,0xb0,0xa1,0xee});
    status(0x1f7);status(0x177);
    select(0x80001004);emit(program,{0xb0,0x07,0xee,0xec,0xaa});
    status(0x1f7);status(0x177);
    emit(program,{0xf4});
    code(cpu,entry,program);cpu.eip=entry;
    run(pc,600);
    need(cpu.halted&&cpu.read32(result)==0x00011b36&&cpu.read32(result+4)==0x00021b36&&
         cpu.read32(result+8)==(media?0xc0711b36U:UINT32_MAX),
         "PCI IDs must match the mounted storage profile");
    for(unsigned slot=3;slot<32;++slot)
      need(cpu.read32(result+4*slot)==UINT32_MAX,"PCI enumeration exposed a phantom device");
    const auto control=result+32*4;
    need(cpu.read32(control)==(media?0x01010001U:UINT32_MAX)&&
         cpu.read8(control+4)==(fixed?0x50:0xff)&&cpu.read8(control+5)==(optical?0x40:0xff)&&
         cpu.read8(control+6)==(media?0:0xff)&&cpu.read8(control+7)==0xff&&
         cpu.read8(control+8)==0xff&&cpu.read8(control+9)==(media?1:0xff)&&
         cpu.read8(control+10)==(fixed?0x50:0xff)&&cpu.read8(control+11)==(optical?0x40:0xff),
         "IDE configuration, missing channels and PCI I/O decode disagree");
    need((!fixed||board.attached_ata()->counters().commands==0)&&board.evidence().irq_delivered==0,
         "PCI decode disable leaked a guest command");
    const auto aml=AcpiC06::dsdt_address+36;
    const auto resource_length=aml+13+8+30+9;
    need(cpu.read8(resource_length)==(media?82:18),
         "The firmware PCI root must offer IDE I/O resources only with the controller present");
  }
  std::cout<<"PRIME_C05_PCI_ENUMERATION_PASS profiles=4 slots=32 open_bus=1 decode_gate=1\n";
}

void ioapic_lapic_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  unsigned word_reads=0,word_writes=0,byte_reads=0,byte_writes=0;
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  cpu.mmio_read32_hook=[read=cpu.mmio_read32_hook,&word_reads](std::uint64_t address){
    ++word_reads;
    return read(address);
  };
  cpu.mmio_write32_hook=[write=cpu.mmio_write32_hook,&word_writes](std::uint64_t address,std::uint32_t value){
    ++word_writes;
    return write(address,value);
  };
  cpu.mmio_read_hook=[read=cpu.mmio_read_hook,&byte_reads](std::uint64_t address){
    ++byte_reads;
    return read(address);
  };
  cpu.mmio_write_hook=[write=cpu.mmio_write_hook,&byte_writes](std::uint64_t address,std::uint8_t value){
    ++byte_writes;
    return write(address,value);
  };
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
  emit(p,{0xb8,1,0,0,0,0x0f,0xa2,0x89,0x15,0x08,0x91,0,0, // CPUID.01H
          0x89,0x1d,0x10,0x91,0,0, // CPUID EBX topology.
          0xb9,0x1b,0,0,0,0x0f,0x32,0xa3,0x0c,0x91,0,0}); // IA32_APIC_BASE
  write_reg(0xfee000f0,0x1ff);   // software enable local APIC
  write_reg(0xfec00000,0x10);   // IOAPIC select redirection0 low
  write_reg(0xfec00010,0x31);   // unmask GSI0 vector31
  emit(p,{0xb0,0x34,0xe6,0x43,
          0xb0,0x10,0xe6,0x40,
          0xb0,0x00,0xe6,0x40,
          0xfb,0xf4,0xeb,0xfe});
  code(pc.cpu,0xa000,p);pc.cpu.eip=0xa000;pc.cpu.halted=false;
  run(pc,220);
  need(word_reads==1&&word_writes==6&&byte_reads==0&&byte_writes==0,
       "each aligned APIC access must use a single dword transaction");
  need(cpu.read16(0xfee00031)==0x0500&&word_reads==1&&byte_reads==2,
       "a partial APIC register read must retain byte semantics");
  need(pc.cpu.read32(0x9104)==0x00050014,
       "guest did not read actual LAPIC MMIO version");
  need((pc.cpu.read32(0x9108)&((1U<<28)|(1U<<9)))==(1U<<9) &&
       (pc.cpu.read32(0x9110)>>16)==1&&pc.cpu.read8(AcpiC06::madt_address+45)==8&&
       pc.cpu.read8(AcpiC06::madt_address+47)==0&&pc.cpu.read32(0xfee00020)==0&&
       (pc.cpu.read32(0x910c)&0xfffff000U)==pc.cpu.read32(AcpiC06::madt_address+36) &&
       (pc.cpu.read32(0x910c)&0xfffU)==0x900,
       "guest CPUID, IA32_APIC_BASE and MADT must describe the same bootstrap LAPIC");
  need(pc.cpu.read32(0x9100)==0x90123456,
       "guest IOAPIC -> LAPIC vector31 handler did not execute");
  need(board.evidence().apic_irqs>=1,
       "guest IRQ did not route through IOAPIC/LAPIC");
  need(pc.cpu.regs[Cpu::ESP]==0x190000,
       "LAPIC guest IRET did not restore interrupted stack");
  bool unsupported=false;
  try{(void)pc.cpu.read32(0xfee00290);}catch(const std::exception&){unsupported=true;}
  need(unsupported,"unsupported local APIC register must not fake value");
  unsupported=false;
  try{cpu.write32(0xfee00290,0);}catch(const std::exception&){unsupported=true;}
  need(unsupported&&word_writes==7&&byte_writes==0,
       "a rejected dword write must fault without retrying bytes");
  std::cout<<"C06_IOAPIC_LAPIC_MMIO_IRQ_PASS delivered="<<board.evidence().apic_irqs<<"\n";
}
void nested_apic_guest(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  cpu.idtr={0x1000,0x15000};
  cpu.regs[Cpu::ESP]=0x190000;
  gate(cpu,0x42,0x13000);
  gate(cpu,0x61,0x13200);
  const auto mov_mmio=[](auto& program,std::uint32_t address,std::uint32_t value){
    emit(program,{0xc7,0x05});
    for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(address>>(8*i)));
    for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(value>>(8*i)));
  };
  const auto snapshot=[](auto& program,std::uint32_t address,std::uint32_t storage){
    emit(program,{0xa1});
    for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(address>>(8*i)));
    emit(program,{0xa3});
    for(unsigned i=0;i<4;++i)program.push_back(std::uint8_t(storage>>(8*i)));
  };
  constexpr auto apic=std::uint32_t(PcMachine::kLocalApicAddress);
  std::vector<std::uint8_t> outer{0x50}; // Preserve EAX across the interrupt.
  snapshot(outer,apic+0x120,0x9004); // ISR vector 42h.
  snapshot(outer,apic+0x0a0,0x9008); // PPR from ISR.
  mov_mmio(outer,apic+0x300,0x40061); // Self IPI with higher priority.
  snapshot(outer,apic+0x230,0x900c); // Pending vector 61h with IF clear.
  emit(outer,{0xfb,0xf4,0xfa}); // Wake from HLT via the self IPI.
  snapshot(outer,apic+0x0a0,0x9010); // PPR must return to the outer ISR.
  mov_mmio(outer,apic+0x0b0,0);
  emit(outer,{0x58,0xcf});
  code(cpu,0x13000,outer);
  std::vector<std::uint8_t> inner{0x50};
  snapshot(inner,apic+0x130,0x9014);
  snapshot(inner,apic+0x0a0,0x9018);
  mov_mmio(inner,apic+0x0b0,0);
  emit(inner,{0x58,0xcf});
  code(cpu,0x13200,inner);
  std::vector<std::uint8_t> guest{0xfa};
  mov_mmio(guest,apic+0x0f0,0x1ff);
  mov_mmio(guest,apic+0x3e0,0x0b); // Divide by 1.
  mov_mmio(guest,apic+0x320,0x42);
  mov_mmio(guest,apic+0x380,12);
  emit(guest,{0xfb,0xf4,0xfa,0xf4}); // HLT wakes on the timer, then stops.
  code(cpu,0xa000,guest);cpu.eip=0xa000;
  run(pc,100);
  need(cpu.halted&&!cpu.flag(Cpu::IF)&&cpu.regs[Cpu::ESP]==0x190000&&
       board.evidence().apic_irqs==2,"APIC must wake HLT and unwind nested IRQs");
  need(cpu.read32(0x9004)==4&&cpu.read32(0x9008)==0x40&&cpu.read32(0x900c)==2&&
       cpu.read32(0x9010)==0x40&&cpu.read32(0x9014)==2&&cpu.read32(0x9018)==0x60,
       "APIC IRR, ISR and PPR did not reflect the actual interrupt nesting");
  need(cpu.read32(apic+0x120)==0&&cpu.read32(apic+0x130)==0&&
       board.interrupt_state().in_service==0&&board.interrupt_state().priority==0,
       "EOI must release exactly the highest in-service interrupt");
  std::cout<<"C06_APIC_NESTED_HLT_PASS timer=0x42 self_ipi=0x61 eoi=2\n";
}
void apic_timer_divisor_and_global_enable(){
  PcMachine pc(16*1024*1024);boot32(pc);
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  constexpr auto apic=std::uint32_t(PcMachine::kLocalApicAddress);
  constexpr std::array<unsigned,8> divisors{{2,4,8,16,32,64,128,1}};
  cpu.write32(apic+0xf0,0x1ff);
  cpu.write32(apic+0x320,0x42);
  for(unsigned i=0;i<divisors.size();++i){
    cpu.write32(apic+0x3e0,(i&3U)|((i&4U)<<1));
    cpu.write32(apic+0x380,2);
    for(unsigned n=0;n<divisors[i]-1;++n)board.advance_one();
    need(cpu.read32(apic+0x390)==2,"LAPIC timer ticked before its divide interval");
    board.advance_one();
    need(cpu.read32(apic+0x390)==1,"LAPIC timer did not honor its programmed divisor");
  }
  code(cpu,0xa000,{0x0f,0x30}); // WRMSR, with global enable clear.
  cpu.eip=0xa000;
  cpu.regs[Cpu::ECX]=0x1b;cpu.regs[Cpu::EAX]=apic|0x100U;cpu.regs[Cpu::EDX]=0;
  need(cpu.step()==cosmic_pc::StepResult::Executed&&!board.interrupt_state().global_enabled,
       "clearing IA32_APIC_BASE.EN must disable the board's LAPIC");
  for(unsigned i=0;i<8;++i)board.advance_one();
  need(board.interrupt_state().timer_count==1&&board.interrupt_state().pending==0,
       "global disable must stop the countdown and interrupt delivery");
  cpu.eip=0xa000;cpu.regs[Cpu::EAX]=apic|0x900U;
  need(cpu.step()==cosmic_pc::StepResult::Executed&&board.interrupt_state().software_enabled,
       "restoring IA32_APIC_BASE.EN must re-enable the programmed LAPIC");
  board.advance_one();
  need(board.interrupt_state().timer_count==0&&board.interrupt_state().pending==0x42,
       "timer must resume and queue its programmed vector");
  pc.boot_long_mode_demo();
  need(board.interrupt_state().global_enabled&&!board.interrupt_state().software_enabled&&
       !board.interrupt_state().pending&&!board.interrupt_state().timer_count,
       "warm boot must reinitialize the LAPIC");
  std::cout<<"C06_APIC_DIVISOR_MSR_PASS divisors=8 global_enable=1 reset=1\n";
}
void tsc_across_halt(){
  const auto elapsed=[](unsigned interval){
    PcMachine pc(16U<<20);boot32(pc);
    PcMotherboardC06 board(pc);
    auto& cpu=pc.cpu;
    cpu.idtr={0x1000,0x15000};
    cpu.regs[Cpu::ESP]=0x190000;
    gate(cpu,0x42,0x13000);
    code(cpu,0x13000,{0xc7,0x05,0xb0,0,0xe0,0xfe,0,0,0,0,0xcf});
    code(cpu,0xa000,{
        0x0f,0x31,0xa3,0,0x90,0,0,  // RDTSC and store its low word.
        0xfb,0xf4,                  // Wait in HLT for the LAPIC timer.
        0x0f,0x31,0xa3,4,0x90,0,0,
        0xfa,0xf4});
    cpu.eip=0xa000;
    const auto apic=PcMachine::kLocalApicAddress;
    cpu.write32(apic+0xf0,0x1ff);
    cpu.write32(apic+0x320,0x42);
    cpu.write32(apic+0x3e0,0x0b);
    cpu.write32(apic+0x380,interval);
    run(pc,interval+16);
    need(cpu.halted&&!cpu.flag(Cpu::IF)&&board.evidence().apic_irqs==1,
         "TSC fixture did not wake through the LAPIC timer");
    return std::array<std::uint64_t,2>{
        cpu.read32(0x9004)-cpu.read32(0x9000),cpu.instructions};
  };
  const auto short_wait=elapsed(32),long_wait=elapsed(320);
  need(short_wait[1]==long_wait[1]&&long_wait[0]==short_wait[0]+(320-32)*64,
       "RDTSC must include halted virtual cycles without counting them as instructions");
  std::cout<<"C06_APIC_TSC_HLT_PASS short="<<short_wait[0]<<" long="<<long_wait[0]<<"\n";
}
void prime_acpi_sci_timer() {
  struct Result {
    std::array<std::uint64_t,6> values;
    long long host_us;
  };
  const auto exercise=[](bool apic,bool accelerate){
    PcMachine pc(16U<<20);boot32(pc);
    const auto start_cycles=pc.cpu.clock_cycles();
    PcMotherboardC06 board(pc);
    pc.enable_boot_acceleration(accelerate);
    auto& cpu=pc.cpu;
    constexpr unsigned interval=0x800000,entry=0xa000,handler=0x13000,base=0x9200;
    constexpr auto lapic=std::uint32_t(PcMotherboardC06::kLocalApic);
    constexpr auto ioapic=std::uint32_t(PcMotherboardC06::kIoApic);
    const auto fadt=AcpiC06::fadt_address,madt=AcpiC06::madt_address;
    need(cpu.read16(fadt+46)==9&&cpu.read32(fadt+56)==0x600&&cpu.read32(fadt+76)==0x608&&
         (cpu.read32(fadt+112)&0x100U)==0&&cpu.read64(fadt+152)==0x600&&
         cpu.read64(fadt+212)==0x608&&cpu.read8(madt+64)==2&&cpu.read8(madt+65)==10&&
         cpu.read8(madt+67)==9&&cpu.read32(madt+68)==9&&cpu.read16(madt+72)==0x0f,
         "FADT and MADT must point to the backed timer, event range and SCI route");
    const auto store=[](auto& into,unsigned address){
      emit(into,{0xa3});
      for(unsigned i=0;i<4;++i)into.push_back(std::uint8_t(address>>(8*i)));
    };
    const auto write_mmio=[](auto& into,unsigned address,std::uint32_t value){
      emit(into,{0xc7,0x05});
      for(unsigned i=0;i<4;++i)into.push_back(std::uint8_t(address>>(8*i)));
      for(unsigned i=0;i<4;++i)into.push_back(std::uint8_t(value>>(8*i)));
    };
    const auto read_pm_timer=[&](auto& into,unsigned address){
      emit(into,{0x66,0xba,0x08,0x06,0xed});
      store(into,address);
    };
    cpu.idtr={0x1000,0x15000};cpu.regs[Cpu::ESP]=0x190000;
    gate(cpu,apic?0x51:0x71,handler);
    std::vector<std::uint8_t> isr{0x50,0x52,0x66,0xba,0,6,0xed};
    store(isr,base); // Status + enable, both set when the SCI arrives.
    read_pm_timer(isr,base+4);
    emit(isr,{0x66,0xba,0,6,0x66,0xb8,1,0,0x66,0xef,0xed}); // W1C.
    store(isr,base+8);
    if(apic)write_mmio(isr,lapic+0xb0,0);
    else emit(isr,{0xb0,0x20,0xe6,0xa0,0xe6,0x20});
    emit(isr,{0x5a,0x58,0xcf});
    code(cpu,handler,isr);

    std::vector<std::uint8_t> guest{0xfa};
    if(apic){
      write_mmio(guest,lapic+0xf0,0x1ff);
      write_mmio(guest,ioapic,0x23);
      write_mmio(guest,ioapic+0x10,0);
      write_mmio(guest,ioapic,0x22);
      write_mmio(guest,ioapic+0x10,0xa051);
    }else{
      emit(guest,{0xb0,0xfb,0xe6,0x21,0xb0,0xfd,0xe6,0xa1});
    }
    emit(guest,{0x66,0xba,2,6,0x66,0xb8,1,0,0x66,0xef});
    read_pm_timer(guest,base+12);
    emit(guest,{0x0f,0x31});
    store(guest,base+16);
    emit(guest,{0xfb,0xf4,0x0f,0x31});
    store(guest,base+20);
    read_pm_timer(guest,base+24);
    emit(guest,{0xfa,0xf4});
    code(cpu,entry,guest);cpu.eip=entry;
    const auto started=std::chrono::steady_clock::now();
    for(unsigned remaining=interval+128;remaining;){
      const auto slice=std::min(9317U,remaining); // Odd slices also exercise clock phase.
      run(pc,slice);
      remaining-=slice;
    }
    const auto runtime=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-started).count();
    need(cpu.halted&&!cpu.flag(Cpu::IF)&&cpu.regs[Cpu::ESP]==0x190000&&
         board.evidence().sci_irqs==1&&board.evidence().irq_delivered==1&&
         board.evidence().apic_irqs==unsigned(apic)&&cpu.read32(base)==0x00010001&&
         cpu.read32(base+8)==0x00010000,
         "The guest SCI handler must receive exactly one event and clear it before EOI");
    const auto first=cpu.read32(base+12),last=cpu.read32(base+24);
    const auto irq_timer=cpu.read32(base+4);
    const auto tsc=cpu.read32(base+20)-cpu.read32(base+16);
    need(first<64&&irq_timer>=interval&&irq_timer<interval+16&&
         last>=interval&&last<interval+64&&cpu.clock_cycles()-board.evidence().ticks==start_cycles&&
         // RDTSC follows the first IN by two cycles. The second IN
         // follows its RDTSC by three cycles.
         tsc==std::uint64_t(last-first-5)*Cpu::kTscTicksPerCycle,
         "The PM clock and RDTSC must agree across STI/HLT (start=" + std::to_string(first) +
         ", end=" + std::to_string(last) + ", irq=" + std::to_string(irq_timer) +
         ", tsc=" + std::to_string(tsc) + ", CPU=" + std::to_string(cpu.clock_cycles()) +
         ", board=" + std::to_string(board.evidence().ticks) + ", initial offset=" +
         std::to_string(start_cycles) + ")");
    const auto restored_ticks=board.evidence().ticks;
    if(accelerate){
      std::vector<std::uint8_t> again{0xfb,0xf4};
      read_pm_timer(again,base+28);
      emit(again,{0xfa,0xf4});
      constexpr auto wake=entry+0x100;
      code(cpu,wake,again);cpu.eip=wake;cpu.halted=false;
      for(unsigned remaining=interval;remaining;){
        const auto slice=std::min(32000U,remaining);
        run(pc,slice);
        remaining-=slice;
      }
      need(cpu.halted&&cpu.regs[Cpu::ESP]==0x190000&&board.evidence().sci_irqs==2&&
           cpu.read32(base+4)<16&&cpu.read32(base+28)<64&&cpu.read32(base+8)==0x00010000,
           "The 24-bit wrap must raise a second SCI and re-arm after W1C and EOI");
    }
    pc.boot_long_mode_demo();
    need(restored_ticks==interval+128&&board.evidence().sci_irqs==0&&
         pc.cpu.in_hook(0x600,4)==0&&pc.cpu.in_hook(0x608,4)==0,
         "Reset must clear the SCI counters, registers and clock");
    return Result{{{first,last,tsc,irq_timer,restored_ticks,interval}},runtime};
  };
  for(const bool apic:{false,true}){
    const auto scalar=exercise(apic,false),batched=exercise(apic,true);
    need(scalar.values==batched.values,"Idle batching changed guest SCI or timer evidence");
    std::cout<<"PRIME_C05_ACPI_SCI_PASS route="<<(apic?"ioapic":"pic")
             <<" logical_cycles="<<batched.values[4]<<" scalar_us="<<scalar.host_us
             <<" idle_batched_us="<<batched.host_us<<" w1c=1 wake=1 wrap=1 reset=1\n";
  }
}
void prime_pit_channel2_calibration() {
  PcMachine pc(16U<<20);boot32(pc);
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  constexpr unsigned entry=0xa000,base=0x9200;
  const std::vector<std::uint8_t> program{
    0xfa,
    0x31,0xc0,0xe6,0x61,                   // Disable channel 2 gate.
    0xb0,0xb0,0xe6,0x43,                   // Channel 2, low/high, mode 0.
    0xb0,0x00,0xe6,0x42,0xb0,0x10,0xe6,0x42, // A count of 4096 PIT clocks.
    0xe4,0x61,0xa2,0,0x92,0,0,             // OUT2 is low before the gate rises.
    0x66,0xba,0x08,0x06,0xed,0xa3,0x04,0x92,0,0, // Read PM clock.
    0xb0,1,0xe6,0x61,                      // Start decrementing at one third of PM clock.
    0xe4,0x61,0xa8,0x20,0x74,0xfa,         // Poll actual OUT2 on port 61h.
    0xa2,1,0x92,0,0,0xed,0xa3,0x08,0x92,0,0,
    0xf4};
  code(cpu,entry,program);cpu.eip=entry;
  run(pc,15000);
  const auto elapsed=cpu.read32(base+8)-cpu.read32(base+4);
  need(cpu.halted&&!(cpu.read8(base)&0x21U)&&(cpu.read8(base+1)&0x21U)==0x21U&&
       elapsed>=4096*3&&elapsed<=4096*3+16&&board.evidence().irq_delivered==0,
       "PIT channel 2 calibration must measure the board oscillator without issuing an IRQ");
  std::cout<<"PRIME_C05_PIT_CALIBRATION_PASS pit_counts=4096 pm_delta="<<elapsed
           <<" irq=0 gate=1\n";
}
void silent_timer_batching() {
  constexpr unsigned elapsed=47002;
  const auto exercise=[](bool accelerate,bool periodic){
    PcMachine pc(16U<<20);boot32(pc);
    PcMotherboardC06 board(pc);
    if(accelerate)pc.enable_boot_acceleration();
    auto& cpu=pc.cpu;
    constexpr auto apic=PcMotherboardC06::kLocalApic;
    cpu.write32(apic+0xf0,0x1ff);
    cpu.write32(apic+0x320,periodic?0x30042:0x10042);
    cpu.write32(apic+0x3e0,0x0b);
    cpu.write32(apic+0x380,7);
    need(cpu.out_hook(0x43,1,periodic?0xb4:0xb0)&&cpu.out_hook(0x42,1,5)&&
         cpu.out_hook(0x42,1,0)&&cpu.out_hook(0x61,1,1),"could not configure PIT channel 2");
    need(pc.device_batch_limit_hook(elapsed)==elapsed,"an IRQ-free timer clipped the batch");

    code(cpu,0xa000,{0xfb,0xf4}); // STI; HLT, retiring the interrupt shadow.
    cpu.eip=0xa000;
    cpu.step();
    cpu.step();
    need(cpu.halted&&cpu.flag(Cpu::IF),"guest could not enter interruptible HLT");
    const auto retired=cpu.instructions;
    run(pc,elapsed);
    const auto count=cpu.in_hook(0x42,1).value_or(256);
    const auto speaker=cpu.in_hook(0x61,1).value_or(0)&0x20U;
    const auto lapic_count=board.interrupt_state().timer_count;
    need(cpu.halted&&cpu.instructions==retired&&board.evidence().ticks==elapsed&&
         board.evidence().irq_delivered==0&&board.interrupt_state().pending==0,
         "silent timers changed retirement, time, or IRQ state");
    need(count==(periodic?3U:0U)&&lapic_count==(periodic?3U:0U)&&speaker==0x20,
         "silent timers lost the exact countdown or speaker output");

    cpu.idtr={0x1000,0x15000};
    cpu.regs[Cpu::ESP]=0x190000;
    gate(cpu,0x42,0x13000);
    cpu.write32(apic+0x320,periodic?0x20042:0x42);
    if(!periodic)cpu.write32(apic+0x380,3);
    need(pc.device_batch_limit_hook(elapsed)==3,"an IRQ-capable timer must limit the batch");
    board.advance_one();
    board.advance_one();
    need(board.evidence().irq_delivered==0&&cpu.halted,"timer woke the guest early after unmasking");
    board.advance_one();
    need(board.evidence().irq_delivered==1&&!cpu.halted&&cpu.eip==0x13000,
         "timer did not wake the guest at the exact expiry after unmasking");
  };
  for(const bool periodic:{false,true}){
    exercise(false,periodic);
    exercise(true,periodic);
  }
  std::cout<<"PRIME_SILENT_TIMERS_BATCH_PASS ticks="<<elapsed<<" pit2=1 masked_lapic=1 wake_tick=3\n";
}
void active_clock_devices_and_pause(){
  PcMachine pc(16U<<20);boot32(pc);
  PcMotherboardC06 board(pc);
  pc.enable_boot_acceleration();
  auto& cpu=pc.cpu;
  using namespace std::chrono_literals;
  auto now=PcMotherboardC06::Clock::time_point{};
  PcMotherboardC06::Clock::duration advance{};
  board.enable_active_clock([&] {
    const auto observed=now;
    now+=std::exchange(advance,{});
    return observed;
  });
  const auto out=[&](unsigned port,unsigned value){
    need(cpu.out_hook(port,1,value),"clock fixture could not write timer port");
  };
  out(0x43,0x34);out(0x40,5);out(0x40,0);
  out(0x70,0x0a);out(0x71,0x25);
  out(0x70,0x0b);out(0x71,0x42);
  constexpr auto apic=PcMotherboardC06::kLocalApic;
  cpu.write32(apic+0xf0,0x1ff);
  cpu.write32(apic+0x320,0x20042);
  cpu.write32(apic+0x3e0,0x0b);
  cpu.write32(apic+0x380,7);
  code(cpu,0xa000,{0xfa,0xf4});
  cpu.eip=0xa000;
  advance=1ms;
  board.run(16);
  need(board.active_clock_ticks()==3579&&board.active_clock_elapsed_ns()==1000000,
       "one host millisecond must advance the shared clock exactly");
  now+=2h;
  board.run(16);
  need(board.active_clock_ticks()==3579,"host time between native grants leaked into guest time");
  advance=1ms;
  board.run(16);
  const auto rtc=board.rtc_evidence();
  need(board.active_clock_ticks()==7159&&board.evidence().ticks==7159&&
       board.active_clock_elapsed_ns()==2000000&&cpu.in_hook(AcpiC06::pm_timer_port,4)==7159&&
       board.evidence().pit_irqs==477&&board.interrupt_state().timer_count==2&&
       rtc.irq8_assertions==1&&rtc.register_c==0xc0&&board.interrupt_state().pending==0x42&&
       board.evidence().irq_delivered==0,
       "active elapsed time must preserve fractional ticks and queue overdue interrupts exactly once");

  cpu.idtr={0x1000,0x15000};cpu.regs[Cpu::ESP]=0x190000;
  gate(cpu,0x42,0x13000);
  code(cpu,0x13000,{0xfa,0xf4});
  code(cpu,0xa000,{0xfb,0xf4});
  cpu.eip=0xa000;cpu.halted=false;
  board.run(16);
  need(cpu.halted&&board.evidence().irq_delivered==1&&cpu.eip==0x13002,
       "active timing dropped a pending IRQ or duplicated elapsed timer edges");
  advance=2h;
  board.run(16);
  need(board.active_clock_elapsed_ns()==7200002000000ULL&&
       board.active_clock_ticks()==7200ULL*PcMachine::kVirtualCyclesPerSecond+7159,
       "a long active interval overflowed the oscillator conversion");
  pc.reset();
  need(!board.active_clock_enabled()&&board.active_clock_ticks()==0,
       "warm reboot must restart in deterministic firmware time");
  std::cout<<"PRIME_ACTIVE_CLOCK_DEVICES_PASS host_ms=2 ticks=7159 pit_edges=477 rtc_edges=1 pause=1 long=1\n";
}
void active_clock_on_application_processor(){
  PcMachine pc(4U<<20);pc.boot_demo();
  PcMotherboardC06 board(pc,nullptr,{},0,{},nullptr,{2,1});
  auto& cpu=pc.cpu;
  cpu.reset(0,0x7200);cpu.a20_enabled=true;
  cpu.write16(0x7200,0xfeeb);
  code(cpu,0x9000,{
      0x0f,0x31,0x66,0xa3,0x00,0x93,0x90,
      0x0f,0x31,0x66,0xa3,0x04,0x93,
      0xba,0x08,0x06,0x66,0xed,0x66,0xa3,0x08,0x93,0x90,
      0x66,0xed,0x66,0xa3,0x0c,0x93,0xfa,0xf4});
  constexpr auto apic=PcMotherboardC06::kLocalApic;
  cpu.write32(apic+0x310,1U<<24);
  cpu.write32(apic+0x300,0x609U);
  auto now=PcMotherboardC06::Clock::time_point{};
  board.enable_active_clock([&]{return now+=std::chrono::microseconds(1);});
  board.run(64);
  const auto tsc1=cpu.read32(0x9300),tsc2=cpu.read32(0x9304);
  const auto pm1=cpu.read32(0x9308),pm2=cpu.read32(0x930c);
  need(!board.faulted_cpu()&&board.processor_steps(1)==13&&tsc2>tsc1&&pm2>pm1&&
       pm1>=tsc2/cosmic_pc::Cpu::kTscTicksPerCycle&&pm2<=board.evidence().ticks,
       "RDTSC and PM timer must progress on the AP during a single grant");
  std::cout<<"PRIME_AP_CLOCK_PASS tsc_delta="<<tsc2-tsc1<<" pm_delta="<<pm2-pm1<<'\n';
}
void prime_bios_clock() {
  PcMachine pc(16U<<20);
  pc.boot_demo();
  PcMotherboardC06 board(pc);
  auto& cpu=pc.cpu;
  constexpr unsigned entry=0x1000,bios_period=65536*3,bios_day=0x1800b0;
  // Capture INT 1Ah AH=00 before and after the wrap, then read it once more.
  code(cpu,entry,{0x31,0xc0,0xcd,0x1a,0xf4});
  const auto sample=[&](){
    cpu.eip=entry;cpu.halted=false;
    run(pc,3);
    need(cpu.halted&&!cpu.flag(Cpu::CF),"INT 1Ah must return the current BIOS clock");
    return (std::uint32_t(cpu.reg16(Cpu::ECX))<<16)|cpu.reg16(Cpu::EDX);
  };
  cpu.advance_idle_clock(bios_period);
  need(sample()==1&&cpu.reg8(Cpu::EAX)==0,"BIOS clock must advance from virtual cycles only");
  // Seed the elapsed-cycle counter immediately ahead of midnight. No host
  // delay or billions of guest steps are needed to exercise BIOS rollover.
  const auto elapsed=std::uint64_t(bios_day)*bios_period-cpu.clock_cycles();
  cpu.advance_idle_clock(static_cast<unsigned>(elapsed%UINT32_MAX));
  for(auto cycles=elapsed/UINT32_MAX;cycles;--cycles)cpu.advance_idle_clock(UINT32_MAX);
  need(sample()==0&&cpu.reg8(Cpu::EAX)==1&&sample()==0&&cpu.reg8(Cpu::EAX)==0,
       "BIOS midnight flag must clear after the first guest read");
  std::cout<<"PRIME_C05_BIOS_CLOCK_PASS int1a=1 midnight=1\n";
}
void prime_vga_graphics_ports() {
  PcMachine pc(16U << 20);
  pc.cpu.use_windows_x64_profile();
  pc.boot_long_mode_demo();
  PcMotherboardC06 board(pc);
  board.run(4000);
  auto& cpu = pc.cpu;
  need(cpu.halted && cpu.is_long_mode() && !cpu.fault, "VGA probe fixture must reach long64");
  constexpr unsigned entry = 0xa000;
  const auto check = [&](std::uint16_t port, std::uint16_t value) {
    code(cpu, entry, {0xec,                    // IN AL,DX
                      0x66,0xb8,std::uint8_t(value),std::uint8_t(value >> 8),
                      0x66,0xef,               // OUT DX,AX
                      0x66,0x31,0xc0,          // XOR AX,AX
                      0x66,0xed,               // IN AX,DX
                      0xf4});
    cpu.eip = entry;
    cpu.halted = false;
    cpu.eflags = 2;
    cpu.regs[Cpu::EDX] = port;
    cpu.regs[Cpu::EAX] = 0xfffff8071ff65001ULL;
    board.run(1);
    if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
    need(cpu.eip == entry + 1 && cpu.regs[Cpu::EAX] == 0xfffff8071ff65000ULL && cpu.eflags == 2,
         "Prime must retire the VGA index read through C06 with correct RAX and flags");
    board.run(5);
    need(cpu.halted && !cpu.fault && cpu.reg16(Cpu::EAX) == value,
         "The C06 bus must forward the VGA index and data pair");
  };
  check(0x3ce, 0xa508);
  std::cout << "PRIME_VGA_GRAPHICS_PORTS_PASS long64=1 index=03ce data=03cf\n";
  check(0x3c4, 0x0a04);
  std::cout << "PRIME_VGA_SEQUENCER_PORTS_PASS long64=1 index=03c4 data=03c5\n";
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
void ps2_pointer_guest() {
  PcMachine pc(16*1024*1024); boot32(pc);
  PcMotherboardC06 board(pc); board.enable_ps2_keyboard();
  pc.cpu.idtr={0x1000,0x15000};
  pc.cpu.regs[Cpu::ESP]=0x190000; pc.cpu.regs[Cpu::EDI]=0x9500;
  gate(pc.cpu,0x2c,0x13400);
  code(pc.cpu,0x13400,{0xe4,0x64,0xaa,0xe4,0x60,0xaa, // Status + byte observed by the guest.
                       0xb0,0x20,0xe6,0xa0,0xe6,0x20,0xcf});
  std::vector<std::uint8_t> p;
  const auto out=[&](std::uint8_t port,std::uint8_t value){emit(p,{0xb0,value,0xe6,port});};
  out(0x20,0x11);out(0x21,0x20);out(0x21,4);out(0x21,1);
  out(0xa0,0x11);out(0xa1,0x28);out(0xa1,2);out(0xa1,1);
  out(0x21,0xfb);out(0xa1,0xef);
  out(0x64,0xa7);out(0x64,0xa8);out(0x64,0xa9);emit(p,{0xe4,0x60,0xaa});
  out(0x64,0x60);out(0x60,6); // Auxiliary IRQ enabled.
  out(0x64,0xd4);out(0x60,0xf4); // Start mouse data reporting.
  emit(p,{0xfb,0xf4,0xeb,0xfd});
  code(pc.cpu,0xa000,p); pc.cpu.eip=0xa000;pc.cpu.halted=false;pc.cpu.fault.reset();
  run(pc,180);
  board.pointer(12,-7,1); board.pointer(0,0,2); board.pointer(0,0,0);
  run(pc,400);
  const std::array<std::uint8_t,10> bytes{{0xfa,0x29,12,0xf9,0x0a,0,0,8,0,0}};
  need(pc.cpu.read8(0x9500)==0 && pc.cpu.regs[Cpu::EDI]==0x9501+2*bytes.size(),
       "The guest must receive the second-port test and all ten IRQ12 events");
  for (unsigned i=0;i<bytes.size();++i) {
    need((pc.cpu.read8(0x9501+2*i)&0x21)==0x21 && pc.cpu.read8(0x9502+2*i)==bytes[i],
         "AUX status, movement or button transition differed at the guest port");
  }
  std::cout<<"C06_PS2_POINTER_PASS irq12=10 x=12 y=-7 left=1 right=1 released=1\n";
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
  need(board.interrupt_state().pending==0x42,
       "a later lower-priority IRQ must leave the timer vector highest in IRR");
  code(pc.cpu,0xa100,{0xfb,0xf4,0xeb,0xfd});
  pc.cpu.eip=0xa100;
  run(pc,100);
  need(pc.cpu.read8(0x9400)==0x42 && pc.cpu.read8(0x9401)==0x1e &&
       pc.cpu.regs[Cpu::EDI]==0x9402 && board.evidence().apic_irqs==2,
       "LAPIC lost a pending request or delivered vectors out of priority order");
  need(pc.cpu.regs[Cpu::ESP]==0x190000,"queued LAPIC interrupts did not restore the guest stack");
  need(board.interrupt_state().pending==0&&board.interrupt_state().in_service==0,
       "delivery and EOI must clear the pending and in-service priorities");
  pc.cpu.write32(PcMotherboardC06::kLocalApic+0x300,0x400f1); // Self IPI before reset.
  need(board.interrupt_state().pending==0xf1,"self IPI must queue before reset");
  pc.boot_long_mode_demo();
  need(board.interrupt_state().pending==0&&board.interrupt_state().in_service==0,
       "reset must clear the LAPIC priority state even with a queued vector");
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
void timed_rotate_xor_guest() {
  PcMachine pc(4U << 20);
  pc.boot_long_mode_demo();
  PcMotherboardC06 board(pc);
  board.run(4000);
  auto& cpu = pc.cpu;
  need(cpu.is_long_mode() && !cpu.fault, "timed rotate/XOR fixture did not enter x64");
  cpu.enable_page_translation_cache();
  cpu.enable_ether_arm64_blocks();
  pc.enable_boot_acceleration();
  constexpr std::uint32_t entry = 0xa000;
  constexpr unsigned steps = 32'000'028, rounds = steps / 4 + 13;
  constexpr std::uint64_t initial = 0x92ba8401381c0fffULL;
  code(cpu,entry,{0x49,0xc1,0xc0,17,0x49,0x83,0xf0,0xf3,0x49,0xff,0xc9,0x75,0xf3});
  auto expected = initial;
  for (unsigned i = 0; i < (steps / 4 + 1) % 128; ++i)
    expected = ((expected << 17) | (expected >> 47)) ^ 0xfffffffffffffff3ULL;

  std::array<std::int64_t, 31> elapsed{};
  for (auto& ns : elapsed) {
    cpu.eip = entry;
    cpu.halted = false;
    cpu.eflags = 2 | Cpu::DF;
    cpu.regs[Cpu::R8] = initial;
    cpu.regs[Cpu::R9] = rounds;
    board.run(4);
    const auto retired = cpu.instructions, ticks = board.evidence().ticks;
    const auto completed = pc.boot_acceleration().rotate_xor_loop_steps;
    const auto start = PcMachine::RunClock::now();
    board.run(steps,start+std::chrono::seconds(1));
    ns = std::chrono::duration_cast<std::chrono::nanoseconds>(PcMachine::RunClock::now()-start).count();
    need(!cpu.fault && cpu.instructions - retired == steps && board.evidence().ticks - ticks == steps &&
             cpu.eip == entry && cpu.regs[Cpu::R9] == 12 && cpu.regs[Cpu::R8] == expected &&
             pc.boot_acceleration().rotate_xor_loop_steps > completed,
         "timed rotate/XOR changed guest state or skipped the motherboard clock");
  }
  std::sort(elapsed.begin(),elapsed.end());
  const auto ns=elapsed[elapsed.size()/2];
  std::cout<<"C06_TIMED_ROTATE_XOR_PASS steps="<<steps<<" median_ns="<<ns
           <<" equivalent_steps_per_s="<<double(steps)*1e9/double(ns)<<'\n';
}

void ioapic_id_x64_guest() {
  PcMachine pc(16U << 20);
  pc.boot_long_mode_demo();
  PcMotherboardC06 board(pc, nullptr, {}, 0, {}, nullptr, {8, 2});
  board.run(4000);
  auto& cpu = pc.cpu;
  need(cpu.halted && cpu.is_long_mode() && !cpu.fault, "I/O APIC fixture failed to enter long mode");
  constexpr std::uint64_t alias = 0xfffff7f58000a000ULL, pdpt = 0x4000, pd = 0x5000, pt = 0x6000;
  constexpr std::uint32_t entry = 0xa000;
  constexpr std::uint32_t default_id = 16U << 24, moved_id = 0x23U << 24;
  constexpr auto ioapic = PcMotherboardC06::kIoApic;
  cpu.write64(cpu.cr3 + ((alias >> 39) & 511) * 8, pdpt | 3);
  cpu.write64(pdpt + ((alias >> 30) & 511) * 8, pd | 3);
  cpu.write64(pd + ((alias >> 21) & 511) * 8, pt | 3);
  cpu.write64(pt + ((alias >> 12) & 511) * 8, ioapic | 3);
  need(cpu.read32(ioapic + 0x10) == default_id, "I/O APIC reset ID differs from the MADT");

  // Use the high virtual alias and exact dword IOWIN store from the boot fault.
  code(cpu, entry, {0x89,0x13, 0x89,0x4b,0x10, 0x8b,0x43,0x10, 0xf4});
  const auto prepare = [&](unsigned select, std::uint32_t value) {
    cpu.eip = entry;
    cpu.halted = false;
    cpu.eflags = 2 | Cpu::DF;
    cpu.regs[Cpu::EBX] = alias;
    cpu.regs[Cpu::ECX] = value;
    cpu.regs[Cpu::EDX] = select;
  };
  const auto check = [&](unsigned select, std::uint32_t value, std::uint32_t expected) {
    prepare(select, value);
    board.run(8);
    if (cpu.fault) throw std::runtime_error(cpu.fault->describe());
    need(cpu.halted && cpu.regs[Cpu::EAX] == expected && cpu.eflags == (2 | Cpu::DF),
         "paged I/O APIC register write changed flags or returned the wrong value");
  };
  check(0, default_id, default_id);
  check(0, moved_id | 0xffffffU, moved_id);
  check(1, UINT32_MAX, 0x00170011);
  check(2, UINT32_MAX, moved_id);
  prepare(0xf, 0);
  board.run(8);
  need(cpu.fault && cpu.eip == entry + 2 &&
       cpu.fault->reason.find("physical=0xfec00010, bytes=4") != std::string::npos,
       "unsupported I/O APIC register must expose its physical address and width");
  pc.reset();
  need(cpu.read32(ioapic + 0x10) == default_id, "I/O APIC ID did not reset with the VM");
  std::cout << "PRIME_IOAPIC_ID_X64_PASS paged=1 version=1 arbitration=1 reset=1\n";
}
}
int main(){
  try{
    pic_pit_guest();
    rep_boot_with_pit_guest(false);
    rep_boot_with_pit_guest(true);
    paged_boot_batches_with_timer();
    timed_rotate_xor_guest();
    shift_boot_with_pit_guest();
    std::cout<<"ETHER_BOOT_IRQ_TIMING_PASS pit=1 lapic=1 shift_loop=1\n";
    zero_boot_with_lapic_guest();
    std::cout<<"ETHER_BOOT_ZERO_LOOP_PASS protected32=1 lapic=1 bounds=1\n";
    optical_irq_wakes_sti_hlt();
    pic_legacy_x87_guest();
    pci_acpi_cmos_guest();
    ram_refresh_guest();
    prime_rtc_cmos_guest();
    rtc_irq8_during_service();
    prime_pci_inventory();
    ioapic_lapic_guest();
    ioapic_id_x64_guest();
    nested_apic_guest();
    apic_timer_divisor_and_global_enable();
    tsc_across_halt();
    prime_acpi_sci_timer();
    prime_pit_channel2_calibration();
    silent_timer_batching();
    active_clock_devices_and_pause();
    active_clock_on_application_processor();
    prime_bios_clock();
    prime_vga_graphics_ports();
    ps2_keyboard_guest();
    ps2_pointer_guest();
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
