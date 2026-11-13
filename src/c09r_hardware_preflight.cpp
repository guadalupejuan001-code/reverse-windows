#include "pc_machine.hpp"
#include "c06_motherboard.hpp"
#include "qssd_disk.hpp"
#include "c09r_guest_vga_fabric.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using cosmic_pc::StepResult;
using reverse_windows::QssdDisk;
using reverse_windows::PcMotherboardC06;
using reverse_windows::GuestVgaFabricC09R;

static void require(bool ok,const char* reason){
  if(!ok)throw std::runtime_error(reason);
}
static std::array<std::uint32_t,4> cpuid(Cpu& cpu,std::uint32_t leaf){
  cpu.reset(0,0x1000);
  cpu.a20_enabled=true;
  cpu.memory[0x1000]=0x0f;cpu.memory[0x1001]=0xa2;
  cpu.regs[Cpu::EAX]=leaf;
  require(cpu.step()==StepResult::Executed && !cpu.fault,
          "Mundo X CPU cannot execute real CPUID guest opcode");
  return {std::uint32_t(cpu.regs[Cpu::EAX]),
          std::uint32_t(cpu.regs[Cpu::EBX]),
          std::uint32_t(cpu.regs[Cpu::ECX]),
          std::uint32_t(cpu.regs[Cpu::EDX])};
}
int main(int argc,char** argv){
  if(argc>2||(argc==2&&std::string(argv[1])!="--self-test")){
    std::cerr<<"No ISO or boot arguments accepted. Only --self-test.\n";
    return 2;
  }
  const auto name=(std::filesystem::temp_directory_path()/
    ("c09r-selftest-"+std::to_string(
     std::chrono::steady_clock::now().time_since_epoch().count()))).string()+".rqvd";
  try{
    // A single canonical PcMachine. No ISO is mounted or executed.
    PcMachine pc(2048ULL*1024*1024,128ULL*1024*1024);
    const auto basic=cpuid(pc.cpu,1);
    const auto extended=cpuid(pc.cpu,0x80000001U);
    const bool pae=basic[3]&(1U<<6),nx=extended[3]&(1U<<20);
    const bool cx16=basic[2]&(1U<<13),prefetchw=extended[2]&(1U<<8);
    const bool lahf=extended[2]&(1U<<0),long64=extended[3]&(1U<<29);
    const bool mmx=basic[3]&(1U<<23),fxsr=basic[3]&(1U<<24);
    const bool sse=basic[3]&(1U<<25),sse2=basic[3]&(1U<<26),x87=basic[3]&1U;
    require(pae&&nx&&cx16&&prefetchw&&lahf&&long64,
            "One or more tested Windows 10 CPU features were lost");
    require(mmx&&fxsr&&sse&&sse2&&!x87,
            "Canonical C02 CPUID profile differs from the verified release profile");
    QssdDisk::create(name,64);
    bool pci=false,ether=false,graphics=false;
    std::uint64_t frame_bytes=0,graphic_serial=0,qssd_physical=0;
    std::array<std::uint8_t,512> data{},replay{};
    for(unsigned i=0;i<data.size();++i)data[i]=std::uint8_t(i*17+3);
    {
      QssdDisk disk(name);
      require(disk.write(128,1,data.data()) && disk.flush(),
              "QSSD original backend cannot persist a guest sector");
      qssd_physical=disk.stats().physical_bytes;
      PcMotherboardC06 board(pc,&disk);
      require(board.attached_ata()!=nullptr,
              "ATA does not refer to the original QSSD BlockDisk");
      auto& cpu=pc.cpu;
      cpu.reset(0,0x1000);cpu.a20_enabled=true;
      const std::array<std::uint8_t,17> pci_code{{
        0x66,0xb8,0x00,0x10,0x00,0x80,
        0xba,0xf8,0x0c,0x66,0xef,
        0xba,0xfc,0x0c,0x66,0xed,0xf4
      }};
      cpu.memory.write_block(0x1000,pci_code.data(),pci_code.size());
      for(unsigned i=0;i<8;++i){
        auto result=cpu.step();
        require(result!=StepResult::Faulted,"PCI guest opcode faulted");
        if(result==StepResult::Halted)break;
      }
      pci=cpu.halted&&cpu.regs[Cpu::EAX]==0xc0711b36U;
      require(pci,"real guest did not enumerate existing PCI ATA");
      const auto addr=PcMachine::kEtherMemoryPhysicalBase;
      cpu.write8(addr,0x5a);
      ether=cpu.read8(addr)==0x5a;
      require(ether,"Ether/XI memory aperture not connected to this PC");
      GuestVgaFabricC09R gpu(pc);
      const auto first=gpu.capture();
      const auto first_state=gpu.state();
      const auto same=gpu.capture();
      require(first.exact_mirror&&same.exact_mirror&&
              same.transferred_bytes==0&&same.revision==first.revision&&
              same.qssd_serial==first.qssd_serial&&gpu.state()==first_state,
              "unchanged guest VGA frame must preserve its exact qSSD revision");
      cpu.reset(0,0x7c00);cpu.a20_enabled=true;
      const std::array<std::uint8_t,13> vga_code{{
        0xb8,0x00,0xb8,0x8e,0xc0,
        0xb8,0x58,0x1f,0x26,0xa3,0x00,0x00,0xf4
      }};
      cpu.memory.write_block(0x7c00,vga_code.data(),vga_code.size());
      for(unsigned i=0;i<10;++i){
        auto result=cpu.step();
        require(result!=StepResult::Faulted,"VGA guest opcode faulted");
        if(result==StepResult::Halted)break;
      }
      const auto updated=gpu.capture();
      graphics=updated.exact_mirror&&
               updated.changed_cells==1&&updated.gpu_planned_cells==1&&
               updated.transferred_bytes==4&&updated.revision==first.revision+1&&
               updated.qssd_serial==first.qssd_serial+1;
      frame_bytes=updated.transferred_bytes;
      graphic_serial=updated.qssd_serial;
      require(graphics,"GPU-VIRTUAL did not select exact changed VGA bytes");
      const auto state=gpu.state();
      require(state->value->completed_revision==state->value->plan.revision,
              "qSSD graphics StateBus failed to commit actual GPU plan");
      require(cpu.memory.allocated_bytes()<128ULL*1024*1024,
              "sparse Mundo X RAM unexpectedly materialized 2 GiB host pages");
    }
    {
      QssdDisk opened(name);
      require(opened.read(128,1,replay.data())&&replay==data,
              "QSSD reopen lost the committed sector");
    }
    const auto yes=[](bool x){return x?"true":"false";};
    std::cout<<"{\"schema\":\"reverse-windows-c09r-readiness-v1\","
      <<"\"target\":\"Windows 10 x64 espanol (user ISO only)\","
      <<"\"iso_loaded\":false,\"windows_executed\":false,"
      <<"\"guest_ram_logical_bytes\":2147483648,"
      <<"\"qssd_logical_bytes\":68719476736,"
      <<"\"qssd_physical_bytes\":"<<qssd_physical
      <<",\"pc_cpu_pae\":"<<yes(pae)
      <<",\"pc_cpu_nx\":"<<yes(nx)
      <<",\"pc_cpu_cx16\":"<<yes(cx16)
      <<",\"pc_cpu_prefetchw\":"<<yes(prefetchw)
      <<",\"pc_cpu_lahf_sahf\":"<<yes(lahf)
      <<",\"pc_cpu_long_mode\":"<<yes(long64)
      <<",\"pc_cpu_mmx\":"<<yes(mmx)
      <<",\"pc_cpu_fxsr\":"<<yes(fxsr)
      <<",\"pc_cpu_sse\":"<<yes(sse)
      <<",\"pc_cpu_sse2\":"<<yes(sse2)
      <<",\"pc_cpu_x87\":"<<yes(x87)
      <<",\"pci_ata_guest_detected\":"<<yes(pci)
      <<",\"ether_xi_memory_aperture\":"<<yes(ether)
      <<",\"gpu_virtual_vga_delta\":"<<yes(graphics)
      <<",\"gpu_vga_delta_upload_bytes\":"<<frame_bytes
      <<",\"gpu_full_frame_bytes\":4000,"
      <<"\"qssd_graphics_serial\":"<<graphic_serial
      <<",\"ready_for_windows10_install\":false}\n";
    std::filesystem::remove(name);
    std::filesystem::remove(name+".rqix");
    std::cout<<"REVERSE_WINDOWS_C09R_READINESS_PASS windows_executed=false ready=false\n";
    return 0;
  }catch(const std::exception& e){
    std::error_code ignored;
    std::filesystem::remove(name,ignored);
    std::filesystem::remove(name+".rqix",ignored);
    std::cerr<<"REVERSE_WINDOWS_C09R_READINESS_FAIL "<<e.what()<<"\n";
    return 1;
  }
}
