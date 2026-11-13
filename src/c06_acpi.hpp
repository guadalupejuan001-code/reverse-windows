#pragma once
// ACPI discovery and fixed resources for the C06 virtual motherboard.
#include "pc_machine.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace reverse_windows {
class AcpiC06 {
 public:
  static constexpr std::uint32_t rsdp_address=0xE0000;
  static constexpr std::uint32_t rsdt_address=0xE0100;
  static constexpr std::uint32_t xsdt_address=0xE0180;
  static constexpr std::uint32_t fadt_address=0xE0200;
  static constexpr std::uint32_t madt_address=0xE0400;
  static constexpr std::uint32_t dsdt_address=0xE0600;
  static constexpr std::uint32_t facs_address=0xE0800;
  static constexpr std::uint32_t pci_config_ports=0xCF8;
  static constexpr std::uint32_t primary_ide_ports=0x1F0;
  static constexpr std::uint32_t primary_ide_control=0x3F6;
  static constexpr std::uint32_t secondary_ide_ports=0x170;
  static constexpr std::uint32_t secondary_ide_control=0x376;
  static constexpr std::uint32_t pm_event_port=0x600;
  static constexpr std::uint32_t pm_control_port=0x604;
  static constexpr std::uint32_t pm_timer_port=0x608;

  static void u16(std::vector<std::uint8_t>& b,std::size_t at,std::uint16_t v){
    if(at+2>b.size())throw std::out_of_range("ACPI u16");
    for(unsigned i=0;i<2;++i)b[at+i]=std::uint8_t(v>>(i*8));
  }
  static void u32(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v){
    if(at+4>b.size())throw std::out_of_range("ACPI u32");
    for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(v>>(i*8));
  }
  static void u64(std::vector<std::uint8_t>& b,std::size_t at,std::uint64_t v){
    if(at+8>b.size())throw std::out_of_range("ACPI u64");
    for(unsigned i=0;i<8;++i)b[at+i]=std::uint8_t(v>>(i*8));
  }
  static void checksum(std::vector<std::uint8_t>& b,std::size_t slot,std::size_t n){
    if(n>b.size()||slot>=n)throw std::out_of_range("ACPI checksum field");
    b[slot]=0;
    unsigned sum=0;for(std::size_t i=0;i<n;++i)sum+=b[i];
    b[slot]=std::uint8_t(0U-sum);
  }
  static std::vector<std::uint8_t> table(const char* signature,std::size_t size,std::uint8_t revision=1){
    if(size<36||size>4096)throw std::invalid_argument("ACPI table bounds");
    std::vector<std::uint8_t> b(size,0);
    std::memcpy(b.data(),signature,4);u32(b,4,std::uint32_t(size));
    b[8]=revision;
    std::memcpy(b.data()+10,"REVERS",6);
    std::memcpy(b.data()+16,"RVPC2026",8);
    u32(b,24,1);
    std::memcpy(b.data()+28,"RWPC",4);u32(b,32,1);
    return b;
  }
  static void publish(cosmic_pc::Cpu& cpu,std::uint32_t address,std::vector<std::uint8_t> b){
    if(address<0xe0000||address>0xeffff||b.size()>0x100000ULL-address)
      throw std::runtime_error("ACPI must stay in reserved BIOS segment");
    checksum(b,9,b.size());
    cpu.memory.write_block(address,b.data(),b.size());
  }
 private:
  static std::vector<std::uint8_t> dsdt_aml() {
    // Name(\_S5, Package(){5, 5}).
    std::vector<std::uint8_t> aml{0x08,0x5c,'_','S','5','_',0x12,0x06,0x02,0x0a,0x05,0x0a,0x05};
    const auto finish_large_package=[&](std::size_t length_offset){
      const auto size=aml.size()-length_offset;
      if(size>0xfffU)throw std::length_error("AML package exceeds two-byte length");
      aml[length_offset]=std::uint8_t(0x40U|(size&0x0fU));
      aml[length_offset+1]=std::uint8_t(size>>4U);
    };
    const auto scope_length=aml.size()+1;
    aml.insert(aml.end(),{0x10,0,0,0x5c,'_','S','B','_'});
    const auto root_length=aml.size()+2;
    aml.insert(aml.end(),{
      0x5b,0x82,0,0,'P','C','I','0',
      0x08,'_','H','I','D',0x0c,0x41,0xd0,0x0a,0x03, // EISAID("PNP0A03")
      0x08,'_','S','E','G',0x00, // segment 0
      0x08,'_','B','B','N',0x00  // base bus 0
    });
    const auto crs_length=aml.size()+6;
    aml.insert(aml.end(),{0x08,'_','C','R','S',0x11,0,0,0x0a,0});
    const auto resource_start=aml.size();
    const auto word_range=[&](std::uint8_t type,std::uint8_t flags,
                              std::uint16_t first,std::uint16_t count){
      const auto at=aml.size();
      aml.insert(aml.end(),{0x88,0x0d,0x00,type,0x0c,flags});
      aml.resize(at+16);
      u16(aml,at+8,first);
      u16(aml,at+10,std::uint16_t(first+count-1));
      u16(aml,at+14,count);
    };
    word_range(2,0,0,1); // Bus 0 only.
    word_range(1,3,secondary_ide_ports,8);
    word_range(1,3,primary_ide_ports,8);
    word_range(1,3,secondary_ide_control,1);
    word_range(1,3,primary_ide_control,1);
    aml.insert(aml.end(),{0x79,0x00}); // EndTag. IDE uses fixed IRQ14/15.
    aml[resource_start-1]=std::uint8_t(aml.size()-resource_start);
    finish_large_package(crs_length);
    // No device routes interrupts through PCI INTx in this profile.
    aml.insert(aml.end(),{0x08,'_','P','R','T',0x12,0x02,0x00});
    // The configuration ports belong to the root bridge, not its children.
    aml.insert(aml.end(),{
      0x5b,0x82,0x1e,'R','E','S','0',
      0x08,'_','H','I','D',0x0c,0x41,0xd0,0x0c,0x02, // EISAID("PNP0C02")
      0x08,'_','C','R','S',0x11,0x09,0x0a,6,
      0x4b,std::uint8_t(pci_config_ports),std::uint8_t(pci_config_ports>>8),8,
      0x79,0x00
    });
    finish_large_package(root_length);
    finish_large_package(scope_length);
    return aml;
  }

 public:
  static void install(cosmic_pc::Cpu& cpu) {
    if(cpu.memory.size()<0x100000)throw std::runtime_error("ACPI BIOS RAM unavailable");
    // RSDP 2.0: ACPI 1.0 prefix plus 64-bit XSDT pointer and full checksum.
    std::vector<std::uint8_t> rsdp(36,0);
    std::memcpy(rsdp.data(),"RSD PTR ",8);
    std::memcpy(rsdp.data()+9,"REVERS",6);
    rsdp[15]=2;u32(rsdp,16,rsdt_address);
    u32(rsdp,20,36);u64(rsdp,24,xsdt_address);
    checksum(rsdp,8,20);checksum(rsdp,32,36);
    cpu.memory.write_block(rsdp_address,rsdp.data(),rsdp.size());

    auto rsdt=table("RSDT",44);
    u32(rsdt,36,fadt_address);u32(rsdt,40,madt_address);
    publish(cpu,rsdt_address,std::move(rsdt));

    auto xsdt=table("XSDT",52);
    u64(xsdt,36,fadt_address);u64(xsdt,44,madt_address);
    publish(cpu,xsdt_address,std::move(xsdt));

    // Legacy FADT registers are backed by the C06 ACPI PM I/O subset.
    auto fadt=table("FACP",244,4);
    u32(fadt,36,facs_address);
    u32(fadt,40,dsdt_address); // DSDT 32-bit physical address
    u16(fadt,46,9);           // SCI interrupt number
    u32(fadt,56,pm_event_port);
    u32(fadt,64,pm_control_port);
    u32(fadt,76,pm_timer_port);
    fadt[88]=4;fadt[89]=2;fadt[91]=4;
    fadt[108]=0x32;          // CMOS century register
    u16(fadt,109,0x0a);     // 8042 present, MSI unsupported
    u64(fadt,132,facs_address);
    u64(fadt,140,dsdt_address); // X_DSDT
    // Extended GAS for PM1a control (system I/O, 16-bit, port 604).
    fadt[172]=1;fadt[173]=16;fadt[175]=2;u64(fadt,176,pm_control_port);
    // Extended GAS for PM timer (system I/O, 32-bit, port 608).
    fadt[208]=1;fadt[209]=32;fadt[211]=3;u64(fadt,212,pm_timer_port);
    publish(cpu,fadt_address,std::move(fadt));

    // FACS is writable firmware memory and has no SDT header or checksum.
    std::vector<std::uint8_t> facs(64,0);
    std::memcpy(facs.data(),"FACS",4);
    u32(facs,4,std::uint32_t(facs.size()));
    facs[32]=1;
    cpu.memory.write_block(facs_address,facs.data(),facs.size());

    auto madt=table("APIC",64,3);
    u32(madt,36,0xfee00000U);u32(madt,40,1); // legacy 8259 PIC also present
    madt[44]=0;madt[45]=8;madt[46]=0;madt[47]=0;u32(madt,48,1);
    madt[52]=1;madt[53]=12;madt[54]=1;madt[55]=0; // IOAPIC ID1
    u32(madt,56,0xfec00000U);u32(madt,60,0);
    publish(cpu,madt_address,std::move(madt));

    const auto aml=dsdt_aml();
    auto dsdt=table("DSDT",36+aml.size(),2);
    std::copy(aml.begin(),aml.end(),dsdt.begin()+36);
    publish(cpu,dsdt_address,std::move(dsdt));
  }
};
} // namespace reverse_windows
