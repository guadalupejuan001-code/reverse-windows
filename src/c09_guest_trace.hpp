#pragma once
// C09: observation-only JSONL of the real canonical Mundo X x86 guest.
// Does not implement a second interpreter, bootloader, driver, or Windows UI.
#include "pc_machine.hpp"
#include "c06_motherboard.hpp"
#include "qssd_disk.hpp"
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace reverse_windows {
class C09GuestTrace final {
  using Cpu=cosmic_pc::Cpu;
  cosmic_pc::PcMachine& pc_;
  PcMotherboardC06& board_;
  QssdDisk* qssd_;
  std::ofstream output_;
  std::uint64_t cap_=0,emitted_=0,dropped_=0,steps_=0;
  std::uint64_t delivered_exceptions_=0,terminal_faults_=0;
  std::uint64_t io_reads_=0,io_writes_=0,mmio_reads_=0,mmio_writes_=0;
  std::uint64_t bios_calls_=0,hardware_irqs_=0,mode_changes_=0;
  bool failed_=false,finalized_=false;
  std::string last_error_;
  std::function<void(const Cpu::StepTrace&)> previous_step_;
  std::function<void(const Cpu::IrqTrace&)> previous_irq_;
  std::function<std::optional<std::uint32_t>(std::uint16_t,unsigned)> previous_in_;
  std::function<bool(std::uint16_t,unsigned,std::uint32_t)> previous_out_;
  std::function<std::optional<std::uint8_t>(std::uint64_t)> previous_mmio_read_;
  std::function<bool(std::uint64_t,std::uint8_t)> previous_mmio_write_;
  std::function<bool(Cpu&,std::uint8_t)> previous_bios_;

  static std::string quote(const std::string& s){return cosmic_pc::PcMachine::quote(s);}
  static std::string address(std::uint64_t v){
    std::ostringstream s;s<<"0x"<<std::hex<<std::setfill('0')<<std::setw(16)<<v;
    return s.str();
  }
  static std::string bytes(const std::vector<std::uint8_t>& data) {
    std::ostringstream s;s<<std::hex<<std::setfill('0');
    for(const auto b:data)s<<std::setw(2)<<unsigned(b);
    return s.str();
  }
  void record(const std::string& json,bool essential=false) noexcept {
    if(failed_)return;
    if(!essential&&emitted_>=cap_){++dropped_;return;}
    try {
      output_<<json<<'\n';
      if(!output_) {failed_=true;last_error_="Trace file write failed";}
      else ++emitted_;
    }catch(...) {failed_=true;last_error_="Trace output exception";}
  }
  void step(const Cpu::StepTrace& t) {
    ++steps_;
    if(t.delivered_vector>=0)++delivered_exceptions_;
    if(t.terminal_fault)++terminal_faults_;
    if(t.mode_before!=t.mode_after)++mode_changes_;
    std::ostringstream o;
    o<<"{\"type\":\"instruction\",\"step\":"<<t.sequence
     <<",\"mode_before\":"<<quote(t.mode_before)
     <<",\"mode_after\":"<<quote(t.mode_after)
     <<",\"cs_before\":"<<quote(address(t.cs_before))
     <<",\"rip_before\":"<<quote(address(t.rip_before))
     <<",\"cs_after\":"<<quote(address(t.cs_after))
     <<",\"rip_after\":"<<quote(address(t.rip_after))
     <<",\"cpl_before\":"<<(t.mode_before=="real16"?0:(t.cs_before&3))
     <<",\"cpl_after\":"<<(t.mode_after=="real16"?0:(t.cs_after&3))
     <<",\"opcode\":"<<quote(address(t.opcode))
     <<",\"decoded\":"<<quote(bytes(t.decoded_bytes))
     <<",\"flags_before\":"<<quote(address(t.flags_before))
     <<",\"flags_after\":"<<quote(address(t.flags_after))
     <<",\"rsp_before\":"<<quote(address(t.rsp_before))
     <<",\"rsp_after\":"<<quote(address(t.rsp_after))
     <<",\"page_walks_delta\":"<<t.page_walks_delta
     <<",\"cr2\":"<<quote(address(t.cr2))
     <<",\"cr3\":"<<quote(address(t.cr3))
     <<",\"outcome\":"<<quote(t.outcome)
     <<",\"delivered_vector\":"<<t.delivered_vector
     <<",\"delivered_error\":"<<t.delivered_error
     <<",\"delivered_linear\":"<<quote(address(t.delivered_linear));
    if(t.terminal_fault){
      o<<",\"fault_code\":"<<static_cast<unsigned>(t.terminal_fault->code)
       <<",\"fault_error\":"<<t.terminal_fault->error_code
       <<",\"fault_linear\":"<<quote(address(t.terminal_fault->linear_address))
       <<",\"fault_reason\":"<<quote(t.terminal_fault->reason);
    }
    o<<"}";
    record(o.str());
    if(t.mode_before!=t.mode_after) {
      // Durable milestone even if the optional per-step quota is exhausted.
      std::ostringstream v;
      v<<"{\"type\":\"mode-transition\",\"step\":"<<t.sequence
       <<",\"from\":"<<quote(t.mode_before)
       <<",\"to\":"<<quote(t.mode_after)
       <<",\"cs\":"<<quote(address(t.cs_after))
       <<",\"rip\":"<<quote(address(t.rip_after))<<"}";
      record(v.str(),true);
    }
    if(t.terminal_fault){
      // Always preserve the first/last terminal blocker, even after trace cap.
      std::ostringstream v;
      v<<"{\"type\":\"terminal-fault\",\"step\":"<<t.sequence
       <<",\"cs\":"<<quote(address(t.cs_before))
       <<",\"rip\":"<<quote(address(t.rip_before))
       <<",\"decoded\":"<<quote(bytes(t.decoded_bytes))
       <<",\"reason\":"<<quote(t.terminal_fault->reason)<<"}";
      record(v.str(),true);
    }
  }
  void hardware(const Cpu::IrqTrace& t) {
    ++hardware_irqs_;
    std::ostringstream o;
    o<<"{\"type\":\"hardware-irq\",\"vector\":"<<unsigned(t.vector)
     <<",\"step\":"<<t.sequence<<",\"delivered\":"<<(t.delivered?"true":"false")
     <<",\"from_cs\":"<<quote(address(t.cs_before))
     <<",\"from_rip\":"<<quote(address(t.rip_before))
     <<",\"to_cs\":"<<quote(address(t.cs_after))
     <<",\"to_rip\":"<<quote(address(t.rip_after))<<"}";
    record(o.str());
  }
 public:
  C09GuestTrace(cosmic_pc::PcMachine& pc,PcMotherboardC06& board,QssdDisk* disk,
                const std::string& file,std::uint64_t cap)
    :pc_(pc),board_(board),qssd_(disk),output_(file,std::ios::binary|std::ios::trunc),cap_(cap){
    if(!output_)throw std::runtime_error("Cannot create C09 guest trace file");
    if(cap<1||cap>2000000)throw std::invalid_argument("Trace quota 1..2000000");
    // The already mounted El Torito boot image and actual boot CS:IP are
    // authoritative; no synthetic Windows success token is inserted.
    std::ostringstream header;
    header<<"{\"type\":\"start\",\"schema\":\"reverse-windows-c09-trace-v1\""
          <<",\"cpu\":\"original-Mundo-X-PcMachine\""
          <<",\"boot_kind\":"<<quote(pc_.boot_kind)
          <<",\"initial_cs\":"<<quote(address(pc_.cpu.segments[Cpu::CS]))
          <<",\"initial_rip\":"<<quote(address(pc_.cpu.eip))
          <<",\"max_events\":"<<cap_
          <<",\"qssd_attached\":"<<(qssd_?"true":"false")
          <<",\"windows_boot_verified\":false}";
    record(header.str(),true);
    if(failed_)throw std::runtime_error(last_error_);
    previous_step_=pc_.cpu.step_trace_hook;
    previous_irq_=pc_.cpu.irq_trace_hook;
    previous_in_=pc_.cpu.in_hook;
    previous_out_=pc_.cpu.out_hook;
    previous_mmio_read_=pc_.cpu.mmio_read_hook;
    previous_mmio_write_=pc_.cpu.mmio_write_hook;
    previous_bios_=pc_.cpu.interrupt_hook;
    pc_.cpu.step_trace_hook=[this](const Cpu::StepTrace& t){
      if(previous_step_)try{previous_step_(t);}catch(...){}
      step(t);
    };
    pc_.cpu.irq_trace_hook=[this](const Cpu::IrqTrace& t){
      if(previous_irq_)try{previous_irq_(t);}catch(...){}
      hardware(t);
    };
    pc_.cpu.in_hook=[this](std::uint16_t port,unsigned width){
      std::optional<std::uint32_t> result;
      try{if(previous_in_)result=previous_in_(port,width);}
      catch(...){record("{\"type\":\"io-in-error\",\"port\":"+std::to_string(port)+"}");throw;}
      ++io_reads_;
      record("{\"type\":\"io-in\",\"port\":"+std::to_string(port)+
             ",\"width\":"+std::to_string(width)+",\"handled\":"+
             std::string(result?"true":"false")+",\"value\":"+
             std::to_string(result.value_or(0))+"}");
      return result;
    };
    pc_.cpu.out_hook=[this](std::uint16_t port,unsigned width,std::uint32_t value){
      bool accepted=false;
      try{accepted=previous_out_&&previous_out_(port,width,value);}
      catch(...){record("{\"type\":\"io-out-error\",\"port\":"+std::to_string(port)+"}");throw;}
      ++io_writes_;
      record("{\"type\":\"io-out\",\"port\":"+std::to_string(port)+
             ",\"width\":"+std::to_string(width)+",\"value\":"+
             std::to_string(value)+",\"handled\":"+
             std::string(accepted?"true":"false")+"}");
      return accepted;
    };
    pc_.cpu.mmio_read_hook=[this](std::uint64_t address){
      auto value=previous_mmio_read_?previous_mmio_read_(address):
                                   std::optional<std::uint8_t>{};
      ++mmio_reads_;
      record("{\"type\":\"mmio-read\",\"address\":"+quote(C09GuestTrace::address(address))+
             ",\"handled\":"+std::string(value?"true":"false")+
             ",\"value\":"+std::to_string(value.value_or(0))+"}");
      return value;
    };
    pc_.cpu.mmio_write_hook=[this](std::uint64_t address,std::uint8_t value){
      const auto accepted=previous_mmio_write_&&previous_mmio_write_(address,value);
      ++mmio_writes_;
      record("{\"type\":\"mmio-write\",\"address\":"+quote(C09GuestTrace::address(address))+
             ",\"value\":"+std::to_string(value)+
             ",\"handled\":"+std::string(accepted?"true":"false")+"}");
      return accepted;
    };
    pc_.cpu.interrupt_hook=[this](Cpu& cpu,std::uint8_t vector){
      const auto ah=cpu.reg8(4);
      const auto drive=cpu.reg8(6);
      const auto reads_before=pc_.disk_reads;
      bool handled=false;
      try{handled=previous_bios_&&previous_bios_(cpu,vector);}
      catch(...){
        record("{\"type\":\"bios-interrupt-error\",\"vector\":"+std::to_string(vector)+
               ",\"ah\":"+std::to_string(ah)+"}",true);
        throw;
      }
      ++bios_calls_;
      record("{\"type\":\"bios-interrupt\",\"vector\":"+std::to_string(vector)+
             ",\"ah\":"+std::to_string(ah)+",\"drive\":"+std::to_string(drive)+
             ",\"handled\":"+std::string(handled?"true":"false")+
             ",\"carry\":"+std::string(cpu.flag(Cpu::CF)?"true":"false")+
             ",\"media_reads_delta\":"+std::to_string(pc_.disk_reads-reads_before)+"}");
      return handled;
    };
  }
  C09GuestTrace(const C09GuestTrace&)=delete;
  C09GuestTrace& operator=(const C09GuestTrace&)=delete;
  ~C09GuestTrace(){
    pc_.cpu.step_trace_hook=std::move(previous_step_);
    pc_.cpu.irq_trace_hook=std::move(previous_irq_);
    pc_.cpu.in_hook=std::move(previous_in_);
    pc_.cpu.out_hook=std::move(previous_out_);
    pc_.cpu.mmio_read_hook=std::move(previous_mmio_read_);
    pc_.cpu.mmio_write_hook=std::move(previous_mmio_write_);
    pc_.cpu.interrupt_hook=std::move(previous_bios_);
  }
  void note_boot_plan(const cosmic_pc::BootPlan& plan,std::uint64_t medium_bytes) {
    std::ostringstream o;
    o<<"{\"type\":\"el-torito-boot-plan\",\"kind\":"<<quote(plan.kind)
     <<",\"load_segment\":"<<quote(address(plan.load_segment))
     <<",\"initial_cs\":"<<quote(address(plan.initial_cs))
     <<",\"initial_rip\":"<<quote(address(plan.initial_ip))
     <<",\"boot_drive\":"<<unsigned(plan.drive)
     <<",\"logical_sector_bytes\":"<<plan.logical_sector_size
     <<",\"source_offset\":"<<plan.source_offset
     <<",\"image_bytes\":"<<plan.image.size()
     <<",\"medium_bytes\":"<<medium_bytes
     <<",\"windows_boot_verified\":false}";
    record(o.str(),true);
  }
  void finish(){
    if(finalized_)return;
    finalized_=true;
    const auto platform=board_.evidence();
    const auto* ata=board_.attached_ata();
    const auto ata_stats=ata?ata->counters():AtaC07::Counters{};
    std::ostringstream o;
    o<<"{\"type\":\"finish\",\"cpu_status\":"<<quote(pc_.status())
     <<",\"steps_observed\":"<<steps_
     <<",\"events_written\":"<<emitted_+1
     <<",\"events_dropped\":"<<dropped_
     <<",\"instructions\":"<<pc_.cpu.instructions
     <<",\"cs\":"<<quote(address(pc_.cpu.segments[Cpu::CS]))
     <<",\"rip\":"<<quote(address(pc_.cpu.eip))
     <<",\"mode\":"<<quote(pc_.cpu.mode_name())
     <<",\"cr0\":"<<quote(address(pc_.cpu.cr0))
     <<",\"cr2\":"<<quote(address(pc_.cpu.cr2))
     <<",\"cr3\":"<<quote(address(pc_.cpu.cr3))
     <<",\"cr4\":"<<quote(address(pc_.cpu.cr4))
     <<",\"page_walks_total\":"<<pc_.cpu.page_walks
     <<",\"delivered_exceptions\":"<<delivered_exceptions_
     <<",\"terminal_faults\":"<<terminal_faults_
     <<",\"mode_transitions\":"<<mode_changes_
     <<",\"hardware_irqs\":"<<hardware_irqs_
     <<",\"io_reads\":"<<io_reads_
     <<",\"io_writes\":"<<io_writes_
     <<",\"mmio_reads\":"<<mmio_reads_
     <<",\"mmio_writes\":"<<mmio_writes_
     <<",\"bios_calls\":"<<bios_calls_
     <<",\"bios_disk_reads\":"<<pc_.disk_reads
     <<",\"ata_commands\":"<<ata_stats.commands
     <<",\"ata_sectors_read\":"<<ata_stats.read_sectors
     <<",\"ata_sectors_written\":"<<ata_stats.write_sectors
     <<",\"ata_irqs\":"<<platform.ata_irqs
     <<",\"qssd_commits\":"<<(qssd_?qssd_->stats().commits:0)
     <<",\"windows_boot_verified\":false";
    if(pc_.cpu.fault)
      o<<",\"fault_reason\":"<<quote(pc_.cpu.fault->describe());
    o<<"}";
    record(o.str(),true);
    output_.flush();
    if(failed_||!output_)throw std::runtime_error("C09 trace file could not be fully persisted");
  }
};
} // namespace reverse_windows
