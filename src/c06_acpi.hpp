#pragma once
// C06: static, checksummed ACPI discovery tables placed in reserved BIOS RAM.
// These tables describe ONLY the C06 virtual devices. Full Windows ACPI/AML
// compliance, SCI, PCI routing tables and power management remain separate gates.
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
    u32(fadt,40,dsdt_address); // DSDT 32-bit physical address
    u16(fadt,46,9);           // SCI_INT wired as intended; SCI dispatch pending
    u32(fadt,56,0x600);       // PM1a event block
    u32(fadt,64,0x604);       // PM1a control
    u32(fadt,76,0x608);       // PM timer
    fadt[88]=4;fadt[89]=2;fadt[91]=4;
    fadt[108]=0x32;          // CMOS century register
    u16(fadt,109,2);         // 8042 is present on this PC
    u64(fadt,140,dsdt_address); // X_DSDT
    // Extended GAS for PM1a control (system I/O, 16-bit, port 604).
    fadt[172]=1;fadt[173]=16;fadt[175]=2;u64(fadt,176,0x604);
    // Extended GAS for PM timer (system I/O, 32-bit, port 608).
    fadt[208]=1;fadt[209]=32;fadt[211]=3;u64(fadt,212,0x608);
    publish(cpu,fadt_address,std::move(fadt));

    auto madt=table("APIC",64,3);
    u32(madt,36,0xfee00000U);u32(madt,40,1); // legacy 8259 PIC also present
    madt[44]=0;madt[45]=8;madt[46]=0;madt[47]=0;u32(madt,48,1);
    madt[52]=1;madt[53]=12;madt[54]=1;madt[55]=0; // IOAPIC ID1
    u32(madt,56,0xfec00000U);u32(madt,60,0);
    publish(cpu,madt_address,std::move(madt));

    // Minimal AML namespace for the standard S5 sleep package.
    // Firmware discovery validity is tested separately from AML completeness.
    const std::vector<std::uint8_t> aml{
      0x08,0x5c,'_','S','5','_',0x12,0x06,0x02,0x0a,0x05,0x0a,0x05
    };
    auto dsdt=table("DSDT",36+aml.size(),2);
    std::copy(aml.begin(),aml.end(),dsdt.begin()+36);
    publish(cpu,dsdt_address,std::move(dsdt));
  }
};
} // namespace reverse_windows
