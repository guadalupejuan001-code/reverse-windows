#pragma once
// REVERSE Windows C06: one attached motherboard for the canonical PcMachine.
// PCI configuration / 8259 PIC / PIT channel0 / RTC CMOS / ACPI PM register
// model / a bounded LAPIC and IOAPIC register subset. Not a chipset clone.
#include "c06_acpi.hpp"
#include "c07_ata.hpp"
#include "atapi_cdrom.hpp"
#include "pc_machine.hpp"
#include "ps2_keyboard.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace reverse_windows {
class PcMotherboardC06 {
 public:
  static constexpr std::uint64_t kLocalApic=cosmic_pc::PcMachine::kLocalApicAddress,
      kIoApic=cosmic_pc::PcMachine::kIoApicAddress;
  struct Evidence {
    std::uint64_t ticks,irq_delivered,pit_irqs,apic_irqs;
    bool pci_present,acpi_installed,powered_off;
    std::uint64_t ata_irqs=0;
  };

 private:
  struct Pic {
    std::uint8_t vector=0x08, mask=0xff, irr=0, isr=0, stage=0;
    bool expect_icw4=false,read_isr=false;
    void command(std::uint8_t v){
      if(v&0x10){
        stage=2;expect_icw4=bool(v&1);irr=isr=0;mask=0;
      }else if((v&0x18)==0x08){
        if((v&3)==2)read_isr=false;
        else if((v&3)==3)read_isr=true;
      }else if(v&0x20) {
        if((v&0x60)==0x60)isr&=std::uint8_t(~(1u<<(v&7)));
        else if(isr) {
          for(unsigned n=0;n<8;++n)if(isr&(1u<<n)){
            isr&=std::uint8_t(~(1u<<n));break;
          }
        }
      }
    }
    void data(std::uint8_t v){
      if(stage==2) {vector=v&0xf8U;stage=3;}
      else if(stage==3)stage=expect_icw4?4:0;
      else if(stage==4)stage=0;
      else mask=v;
    }
    std::uint8_t status() const {return read_isr?isr:irr;}
    std::uint8_t pending() const {
      const auto candidates=unsigned(irr&~mask);
      for(unsigned n=0;n<8;++n)if(candidates&(1u<<n)){
        // A currently serviced IRQ blocks requests at equal/lower priority.
        if(isr) {
          unsigned first=0;
          while(first<8&&!(isr&(1u<<first)))++first;
          if(n>=first)continue;
        }
        return std::uint8_t(n+1);
      }
      return 0;
    }
    void accepted(unsigned line) {
      irr&=std::uint8_t(~(1u<<line));isr|=std::uint8_t(1u<<line);
    }
  };
  struct Pit {
    std::uint32_t reload=0,counter=0;
    std::uint16_t latch=0,staging=0;
    unsigned access=3,mode=2,phase=0,read_phase=0;
    bool enabled=false,latch_valid=false;
    bool command(std::uint8_t v){
      if((v>>6)!=0)return false; // channel0 only
      if(((v>>4)&3)==0){latch=std::uint16_t(counter);latch_valid=true;read_phase=0;return true;}
      access=(v>>4)&3;mode=(v>>1)&7;
      if(mode==6)mode=2;if(mode==7)mode=3;
      if(mode!=2&&mode!=3)return false;
      phase=0;read_phase=0;latch_valid=false;return true;
    }
    void commit(std::uint16_t raw){reload=raw?raw:65536U;counter=reload;enabled=true;}
    void data(std::uint8_t value){
      if(access==1)commit(value);
      else if(access==2)commit(std::uint16_t(value)<<8);
      else if(phase++%2==0)staging=value;
      else commit(std::uint16_t(staging|(std::uint16_t(value)<<8)));
    }
    std::uint8_t read(){
      const auto value=std::uint16_t(latch_valid?latch:counter);
      if(access==2){latch_valid=false;return std::uint8_t(value>>8);}
      if(access==1){latch_valid=false;return std::uint8_t(value);}
      const auto result=read_phase++%2?std::uint8_t(value>>8):std::uint8_t(value);
      if(read_phase%2==0)latch_valid=false;
      return result;
    }
    bool tick(){
      if(!enabled)return false;
      if(counter) --counter;
      if(counter)return false;
      counter=reload;
      return true;
    }
  };

  cosmic_pc::PcMachine& pc_;
  std::optional<Ps2Keyboard> keyboard_;
  std::function<std::optional<std::uint32_t>(std::uint16_t,unsigned)> prior_in_;
  std::function<bool(std::uint16_t,unsigned,std::uint32_t)> prior_out_;
  std::function<bool(std::uint64_t)> prior_mmio_claim_;
  std::function<bool(std::uint64_t,std::size_t)> prior_mmio_range_claim_;
  std::function<std::optional<std::uint8_t>(std::uint64_t)> prior_mmio_read_;
  std::function<bool(std::uint64_t,std::uint8_t)> prior_mmio_write_;
  std::function<void()> prior_tick_;
  std::function<unsigned(unsigned)> prior_batch_limit_;
  std::function<void(unsigned)> prior_batch_advance_;
  std::function<void()> prior_reset_;
  Pic master_{},slave_{};
  std::unique_ptr<AtaC07> ata_;
  std::unique_ptr<AtapiCdrom> optical_;
  std::array<std::uint8_t,256> ata_cfg_{};
  std::uint64_t ata_irq_delivered_=0;
  Pit pit_{};
  std::uint32_t pci_selector_=0,clock_steps_=0;
  std::uint8_t cmos_index_=0,rtc_b_=2;
  std::array<std::uint8_t,256> host_cfg_{},isa_cfg_{};
  std::uint16_t pm_control_=0x0001; // SCI_EN: firmware starts in ACPI mode.
  bool powered_off_=false;
  bool ferr_was_asserted_=false;
  std::uint64_t ticks_=0,irq_delivered_=0,pit_irqs_=0,apic_irqs_=0;
  std::uint32_t apic_svr_=0xff,apic_lvt_timer_=0x10000;
  std::uint32_t apic_timer_reload_=0,apic_timer_counter_=0,apic_divider_=0x3;
  std::bitset<256> lapic_pending_;
  std::uint32_t lapic_in_service_=0,ioapic_select_=0;
  std::array<std::uint32_t,24> io_redir_low_{},io_redir_high_{};
  // Register dword writes are assembled by the byte MMIO bridge, not applied
  // once per byte. Incomplete writes don't expose partial device state.
  struct PendingWrite {std::uint64_t aligned=0;std::uint32_t value=0;std::uint8_t bytes=0;};
  PendingWrite mmio_pending_{};
  static std::uint32_t part_mask(unsigned width){
    return width==4?UINT32_MAX:(std::uint32_t(1)<<(width*8))-1;
  }
  static std::uint8_t bcd(unsigned n){return std::uint8_t((n/10)*16+(n%10));}
  static void config32(std::array<std::uint8_t,256>& bytes,unsigned at,std::uint32_t v){
    for(unsigned i=0;i<4;++i)bytes[at+i]=std::uint8_t(v>>(8*i));
  }
  static void build_config(std::array<std::uint8_t,256>& b,std::uint16_t device,
                           std::uint8_t type){
    b.fill(0);
    config32(b,0,(std::uint32_t(device)<<16)|0x1b36U); // synthetic vendor
    b[8]=1;b[10]=type;b[11]=0x06; // host/ISA bridge class
    b[0x0e]=0; // multifunction=0; no storage devices declared before C07
  }
  void reset_devices(){
    if(keyboard_) keyboard_->reset();
    master_=Pic{};slave_=Pic{};slave_.vector=0x70;
    pit_=Pit{};pci_selector_=0;clock_steps_=0;
    cmos_index_=0;rtc_b_=2;pm_control_=0x0001;powered_off_=false;
    ferr_was_asserted_=false;pc_.cpu.set_x87_ignne(false);
    ticks_=irq_delivered_=pit_irqs_=apic_irqs_=ata_irq_delivered_=0;
    if(ata_)ata_->reset();
    if(optical_)optical_->reset();
    pc_.cpu.set_apic_task_priority(0);apic_svr_=0xff;apic_lvt_timer_=0x10000;
    apic_timer_reload_=apic_timer_counter_=0;apic_divider_=3;
    lapic_pending_.reset();lapic_in_service_=0;ioapic_select_=0;
    io_redir_low_.fill(0x10000U);io_redir_high_.fill(0);
    mmio_pending_={};
    build_config(host_cfg_,0x0001,0x00);
    build_config(isa_cfg_,0x0002,0x01);
    if(ata_ || optical_){
      ata_cfg_.fill(0);
      config32(ata_cfg_,0,(0xc071U<<16)|0x1b36U); // synthetic C07 PCI IDE
      ata_cfg_[4]=1; // PCI I/O space decode enabled
      ata_cfg_[8]=1; // revision
      ata_cfg_[9]=0; // both channels legacy compatibility, no bus mastering
      ata_cfg_[10]=0x01;ata_cfg_[11]=0x01; // PCI 0x0101 IDE controller
      // Legacy IDE decodes fixed ports and IRQ14/15, without PCI BARs or INTx.
      ata_cfg_[0x3c]=0xff;
    }
    // After a real PcMachine::reset, firmware RAM is new; republish the
    // same discovered tables and keep their E820-reserved addresses.
    AcpiC06::install(pc_.cpu);
  }
  bool owned_port(std::uint16_t p) const {
    if(ata_ && ata_->handles(p) && (ata_cfg_[4]&1U))return true;
    if(optical_ && optical_->handles(p) && (ata_cfg_[4]&1U))return true;
    return p==0x20||p==0x21||p==0xa0||p==0xa1||
           (p>=0x40&&p<=0x43)||(p>=0x70&&p<=0x71)||
           (p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)||
           (p>=AcpiC06::pm_event_port&&p<AcpiC06::pm_timer_port+4);
  }
  std::array<std::uint8_t,256>* selected_pci(){
    if(!(pci_selector_ & 0x80000000U) ||
       ((pci_selector_>>16)&255U)!=0 || ((pci_selector_>>8)&7U)!=0)
      return nullptr;
    const unsigned dev=(pci_selector_>>11)&31;
    if(dev==0)return &host_cfg_;
    if(dev==1)return &isa_cfg_;
    if(dev==2&&(ata_||optical_))return &ata_cfg_;
    return nullptr;
  }
  std::optional<std::uint32_t> pci_read(std::uint16_t p,unsigned width){
    if(p==0xcf8&&width==4)return pci_selector_;
    if(p>=0xcfc&&p+width<=0xd00&&(width==1||width==2||width==4)){
      const auto config=selected_pci();
      const unsigned offset=(pci_selector_&0xfcU)+(p-0xcfc);
      if(offset+width>256)return part_mask(width);
      std::uint32_t value=0;
      for(unsigned i=0;i<width;++i)
        value|=std::uint32_t(config?(*config)[offset+i]:0xff)<<(i*8);
      return value;
    }
    return std::nullopt;
  }
  bool pci_write(std::uint16_t p,unsigned width,std::uint32_t value){
    if(p==0xcf8&&width==4){pci_selector_=value;return true;}
    if(p>=0xcfc&&p+width<=0xd00&&(width==1||width==2||width==4)){
      const auto config=selected_pci();
      if(!config)return true; // non-existent devices ignore writes
      const unsigned offset=(pci_selector_&0xfcU)+(p-0xcfc);
      if(offset+width>256)return true;
      // PCI vendor/device/class/revision read-only. Only standard command
      // low two bits are writable for these transparent host/ISA bridges.
      for(unsigned i=0;i<width;++i){
        const unsigned at=offset+i;
        if(at==4)(*config)[4]=std::uint8_t(value>>(8*i))&3U;
      }
      return true;
    }
    return false;
  }
  std::uint8_t cmos() const {
    std::time_t now=std::time(nullptr);
    std::tm current{};
#if defined(_WIN32)
    gmtime_s(&current,&now);
#else
    gmtime_r(&now,&current);
#endif
    unsigned raw=0;
    switch(cmos_index_&0x7fU){
      case 0x00:raw=current.tm_sec;break;
      case 0x02:raw=current.tm_min;break;
      case 0x04:raw=current.tm_hour;break;
      case 0x06:raw=current.tm_wday+1;break;
      case 0x07:raw=current.tm_mday;break;
      case 0x08:raw=current.tm_mon+1;break;
      case 0x09:raw=(current.tm_year+1900)%100;break;
      case 0x32:raw=(current.tm_year+1900)/100;break;
      case 0x0a:return 0x26; // rate selector; UIP remains zero
      case 0x0b:return rtc_b_; // 24h, BCD by default
      case 0x0c:return 0; // no RTC IRQ pending in C06
      case 0x0d:return 0x80; // valid CMOS battery status
      default:return 0; // unmodeled CMOS storage not advertised as device
    }
    return (rtc_b_&4)?std::uint8_t(raw):bcd(raw);
  }
  std::optional<std::uint32_t> io_read(std::uint16_t p,unsigned width){
    if(keyboard_&&width==1&&(p==0x60||p==0x64)){
      const auto status=prior_in_(0x64,1).value_or(0);
      if(p==0x64)return status|(keyboard_->has_data()?1U:0U);
      if(status&1U)return prior_in_(p,width);
      return keyboard_->read_data();
    }
    if(ata_&&ata_->handles(p)&&(ata_cfg_[4]&1U))return ata_->input(p,width);
    if(optical_&&optical_->handles(p)&&(ata_cfg_[4]&1U))return optical_->input(p,width);
    if(p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)return pci_read(p,width);
    if(p==AcpiC06::pm_timer_port&&width==4)return std::uint32_t(ticks_&0x00ffffffU);
    if(p==AcpiC06::pm_control_port&&width==2)return pm_control_;
    if((p==AcpiC06::pm_event_port||p==AcpiC06::pm_event_port+2)&&width==2)return 0;
    if(width!=1)return std::nullopt;
    switch(p){
      case 0x20:return master_.status();
      case 0x21:return master_.mask;
      case 0xa0:return slave_.status();
      case 0xa1:return slave_.mask;
      case 0x40:return pit_.read();
      case 0x70:return cmos_index_;
      case 0x71:return cmos();
      default:return std::nullopt;
    }
  }
  bool io_write(std::uint16_t p,unsigned width,std::uint32_t value){
    if(keyboard_&&width==1&&(p==0x60||p==0x64)){
      const auto data=std::uint8_t(value);
      if(p==0x60&&!keyboard_->controller_expects_data())return keyboard_->command(data);
      if(!prior_out_(p,width,value))return false;
      if(p==0x64)keyboard_->controller_command(data);
      else keyboard_->controller_data(data);
      return true;
    }
    if(ata_&&ata_->handles(p)&&(ata_cfg_[4]&1U))return ata_->output(p,width,value);
    if(optical_&&optical_->handles(p)&&(ata_cfg_[4]&1U))return optical_->output(p,width,value);
    if(p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)return pci_write(p,width,value);
    if(p==AcpiC06::pm_control_port&&width==2){
      pm_control_=std::uint16_t((value&0x1c00U)|0x0001U); // SLP_TYP and read-only SCI_EN.
      if((value&0x2000)&&((value>>10)&7)==5)powered_off_=true;
      return true;
    }
    if((p==AcpiC06::pm_event_port||p==AcpiC06::pm_event_port+2)&&width==2)
      return true; // no SCI generated, event mask values are not retained
    if(width!=1)return false;
    const auto b=std::uint8_t(value);
    switch(p){
      case 0xf0:
        // PC/AT coprocessor acknowledgement. A PIC edge already recorded in
        // IRR still follows normal acceptance/EOI. IGNNE# lasts until FERR# falls.
        pc_.cpu.set_x87_ignne(pc_.cpu.x87_ferr_asserted());
        return true;
      case 0x20:master_.command(b);return true;
      case 0x21:master_.data(b);return true;
      case 0xa0:slave_.command(b);return true;
      case 0xa1:slave_.data(b);return true;
      case 0x40:pit_.data(b);return true;
      case 0x43:return pit_.command(b);
      case 0x70:cmos_index_=b;return true;
      case 0x71:
        if((cmos_index_&0x7fU)==0x0b && (b&~std::uint8_t{0x06})==0) {
          rtc_b_=b;return true;
        }
        return false; // no fake CMOS/RTC IRQ support
      default:return false;
    }
  }
  bool mmio_claim(std::uint64_t a) const{
    return (a>=kLocalApic&&a<kLocalApic+4096)||
           (a>=kIoApic&&a<kIoApic+4096);
  }
  bool mmio_range_claim(std::uint64_t start,std::size_t length) const {
    const auto overlaps=[&](std::uint64_t base){
      return start<base+4096U && (start>=base || length>base-start);
    };
    return length && (overlaps(kLocalApic) || overlaps(kIoApic) ||
        (prior_mmio_claim_ && (!prior_mmio_range_claim_ || prior_mmio_range_claim_(start,length))));
  }
  std::optional<std::uint32_t> mmio_register(std::uint64_t base) const {
    if(base>=kLocalApic&&base<kLocalApic+4096) {
      const auto r=unsigned(base-kLocalApic);
      switch(r) {
        case 0x20:return 0; // LAPIC ID 0
        case 0x30:return 0x00030014U; // local APIC version; bounded LVT subset
        case 0x80:return pc_.cpu.apic_task_priority();
        case 0xb0:return 0; // EOI write-only
        case 0xf0:return apic_svr_;
        case 0x320:return apic_lvt_timer_;
        case 0x380:return apic_timer_reload_;
        case 0x390:return apic_timer_counter_;
        case 0x3e0:return apic_divider_;
        default:return std::nullopt;
      }
    }
    if(base>=kIoApic&&base<kIoApic+4096) {
      const auto r=unsigned(base-kIoApic);
      if(r==0)return ioapic_select_;
      if(r!=0x10)return std::nullopt;
      if(ioapic_select_==0)return 0x01000000U; // IOAPIC ID1
      if(ioapic_select_==1)return 0x00170011U; // version and max RTE 23
      if(ioapic_select_==2)return 0;
      if(ioapic_select_>=0x10&&ioapic_select_<0x40) {
        const unsigned pin=(ioapic_select_-0x10)/2;
        return (ioapic_select_&1)?io_redir_high_[pin]:io_redir_low_[pin];
      }
    }
    return std::nullopt;
  }
  std::optional<std::uint8_t> mmio_byte(std::uint64_t a) const{
    const auto reg=mmio_register(a&~std::uint64_t{3});
    if(!reg)return std::nullopt;
    return std::uint8_t((*reg>>((a&3)*8))&255U);
  }
  bool mmio_commit(std::uint64_t base,std::uint32_t v) {
    if(base>=kLocalApic&&base<kLocalApic+4096){
      const auto r=unsigned(base-kLocalApic);
      switch(r){
        case 0x80:pc_.cpu.set_apic_task_priority(std::uint8_t(v));return true;
        case 0xb0:lapic_in_service_=0;return true;
        case 0xf0:apic_svr_=v&0x1ffU;return true;
        case 0x320:
          if(v&~0x300ffU)return false; // only fixed, masked and periodic LVT
          apic_lvt_timer_=v;return true;
        case 0x380:apic_timer_reload_=v;apic_timer_counter_=v;return true;
        case 0x3e0:apic_divider_=v&0x0bU;return true;
        default:return false;
      }
    }
    if(base>=kIoApic&&base<kIoApic+4096){
      const auto r=unsigned(base-kIoApic);
      if(r==0){ioapic_select_=v&0xffU;return true;}
      if(r!=0x10)return false;
      if(ioapic_select_>=0x10&&ioapic_select_<0x40){
        const unsigned pin=(ioapic_select_-0x10)/2;
        if(ioapic_select_&1)io_redir_high_[pin]=v&0xff000000U;
        else io_redir_low_[pin]=v;return true;
      }
      return false;
    }
    return false;
  }
  bool mmio_write(std::uint64_t a,std::uint8_t byte) {
    const auto base=a&~std::uint64_t{3};
    if(a%4==0)mmio_pending_={base,0,0};
    if(mmio_pending_.aligned!=base||mmio_pending_.bytes!=(a&3))
      return false; // no partial/out-of-order MMIO register commits
    mmio_pending_.value|=std::uint32_t(byte)<<((a&3)*8);
    ++mmio_pending_.bytes;
    if((a&3)==3){
      const auto value=mmio_pending_.value;mmio_pending_={};
      return mmio_commit(base,value);
    }
    return true;
  }
  bool lapic_on()const{return bool(apic_svr_&0x100);}
  std::optional<std::uint8_t> highest_lapic_request() const {
    if(!lapic_pending_.any())return std::nullopt;
    for(unsigned vector=255;vector>=0x20;--vector)
      if(lapic_pending_[vector])return std::uint8_t(vector);
    return std::nullopt;
  }
  void signal_slave_irq(unsigned line){
    const auto redirect=io_redir_low_[line];
    if(lapic_on() && !(redirect&0x10000U) && (redirect&0xffU)>=0x20U){
      lapic_pending_.set(redirect&0xffU);
    }else{
      slave_.irr|=std::uint8_t(1U<<(line-8));
    }
  }
  void signal_irq0(){
    ++pit_irqs_;
    const auto redirect=io_redir_low_[0];
    if(lapic_on()&&!(redirect&0x10000U) && (redirect&0xffU)>=0x20U){
      lapic_pending_.set(redirect&0xffU);
    }else{
      master_.irr|=1U;
    }
  }
  void poll(){
    ++ticks_;
    auto& cpu=pc_.cpu;
    const bool ferr=cpu.x87_ferr_asserted();
    if(ferr&&!ferr_was_asserted_)signal_slave_irq(13);
    if(!ferr&&ferr_was_asserted_)cpu.set_x87_ignne(false);
    ferr_was_asserted_=ferr;
    if(pit_.tick())signal_irq0();
    if(keyboard_&&keyboard_->needs_irq()&&!(prior_in_(0x64,1).value_or(0)&1U)){
      const auto redirect=io_redir_low_[1];
      if(lapic_on()&&!(redirect&0x10000U)&&(redirect&0xffU)>=0x20U)
        lapic_pending_.set(redirect&0xffU);
      else
        master_.irr|=1U<<1;
      keyboard_->mark_irq();
    }
    // Synthetic tick-based LAPIC timer: not a calibrated physical TSC or
    // 1.193-MHz PIT clock. Windows timing compliance remains unverified.
    if(lapic_on()&&apic_timer_counter_){
      if(--apic_timer_counter_==0){
        if(!(apic_lvt_timer_&0x10000U)&&std::uint8_t(apic_lvt_timer_)>=0x20)
          lapic_pending_.set(apic_lvt_timer_&0xffU);
        if(apic_lvt_timer_&0x20000U)apic_timer_counter_=apic_timer_reload_;
      }
    }
    // The slave's INT output reaches the master independently of CPU IF or
    // LAPIC acceptance. The guest can read both IRRs with interrupts disabled.
    const auto slave_irq=slave_.pending();
    if(slave_irq)master_.irr|=std::uint8_t(1U<<2);
    else if(!(master_.isr&(1U<<2)))master_.irr&=std::uint8_t(~(1U<<2));
    if(cpu.fault||!cpu.flag(cosmic_pc::Cpu::IF))return;
    if(lapic_on()&&!lapic_in_service_){
      const auto request=highest_lapic_request();
      if(request&&(*request&0xf0U)>(cpu.apic_task_priority()&0xf0U)){
        if(cpu.raise_external_irq(*request)){
          lapic_in_service_=*request;lapic_pending_.reset(*request);
          ++apic_irqs_;++irq_delivered_;
        }
        return;
      }
    }
    // 8259 cascade: a pending slave IRQ presents IRQ2 at the master PIC.
    // Deliver the SLAVE vector, not master IRQ2's vector. Both PICs must EOI.
    const auto master_irq=master_.pending();
    if(master_irq){
      const unsigned n=master_irq-1;
      if(n==2&&slave_irq){
        const unsigned sn=slave_irq-1;
        if(cpu.raise_external_irq(std::uint8_t(slave_.vector+sn))){
          slave_.accepted(sn);master_.accepted(2);
          ++irq_delivered_;if(sn==6)++ata_irq_delivered_;
        }
      }else if(cpu.raise_external_irq(std::uint8_t(master_.vector+n))){
        master_.accepted(n);++irq_delivered_;
      }
    }
  }

  // The current step has already polled the board. Stop the batch before
  // another tick could change the interrupt or timer state visible to the CPU.
  unsigned batch_limit(unsigned steps) const {
    if(prior_tick_ || powered_off_ || pc_.cpu.x87_ferr_asserted()!=ferr_was_asserted_ ||
       (keyboard_&&keyboard_->needs_irq()) ||
       (pc_.cpu.flag(cosmic_pc::Cpu::IF) && (master_.pending() ||
          (lapic_on() && !lapic_in_service_ && highest_lapic_request())))) return 1;
    if(pit_.enabled) steps=std::min(steps,std::max(1U,pit_.counter));
    if(lapic_on()&&apic_timer_counter_) steps=std::min(steps,apic_timer_counter_);
    return steps;
  }
  void advance_batch(unsigned extra_steps) {
    ticks_+=extra_steps;
    if(pit_.enabled) pit_.counter-=extra_steps;
    if(lapic_on()&&apic_timer_counter_) apic_timer_counter_-=extra_steps;
  }

 public:
  explicit PcMotherboardC06(cosmic_pc::PcMachine& machine, BlockDisk* disk=nullptr,
                            AtapiCdrom::Reader optical={}, std::uint64_t optical_bytes=0):pc_(machine){
    if(disk)ata_=std::make_unique<AtaC07>(*disk,[this](){signal_slave_irq(14);});
    if(optical)optical_=std::make_unique<AtapiCdrom>(
        std::move(optical),optical_bytes,[this](){signal_slave_irq(15);});
    // The machine must be booted/mounted first. ACPI goes into reserved BIOS
    // memory and is never presented by E820 as allocatable guest RAM.
    prior_in_=pc_.cpu.in_hook;prior_out_=pc_.cpu.out_hook;
    prior_mmio_claim_=pc_.cpu.mmio_claim_hook;
    prior_mmio_range_claim_=pc_.cpu.mmio_range_claim_hook;
    prior_mmio_read_=pc_.cpu.mmio_read_hook;
    prior_mmio_write_=pc_.cpu.mmio_write_hook;
    prior_tick_=pc_.device_step_hook;
    prior_batch_limit_=pc_.device_batch_limit_hook;
    prior_batch_advance_=pc_.device_batch_advance_hook;
    prior_reset_=pc_.device_reset_hook;
    reset_devices();
    pc_.cpu.in_hook=[this](std::uint16_t p,unsigned w){
      if(keyboard_&&(p==0x60||p==0x64))return io_read(p,w);
      if(owned_port(p))return io_read(p,w);
      return prior_in_?prior_in_(p,w):std::nullopt;
    };
    pc_.cpu.out_hook=[this](std::uint16_t p,unsigned w,std::uint32_t v){
      if(keyboard_&&(p==0x60||p==0x64))return io_write(p,w,v);
      if(p==0xf0||owned_port(p))return io_write(p,w,v);
      return prior_out_&&prior_out_(p,w,v);
    };
    pc_.cpu.mmio_claim_hook=[this](std::uint64_t a){
      return mmio_claim(a)||(prior_mmio_claim_&&prior_mmio_claim_(a));
    };
    pc_.cpu.mmio_range_claim_hook=[this](std::uint64_t start,std::size_t length){
      return mmio_range_claim(start,length);
    };
    pc_.cpu.mmio_read_hook=[this](std::uint64_t a){
      if(mmio_claim(a))return mmio_byte(a);
      return prior_mmio_read_?prior_mmio_read_(a):std::nullopt;
    };
    pc_.cpu.mmio_write_hook=[this](std::uint64_t a,std::uint8_t v){
      if(mmio_claim(a))return mmio_write(a,v);
      return prior_mmio_write_&&prior_mmio_write_(a,v);
    };
    pc_.device_step_hook=[this](){
      if(prior_tick_)prior_tick_();
      poll();
    };
    pc_.device_batch_limit_hook=[this](unsigned steps){return batch_limit(steps);};
    pc_.device_batch_advance_hook=[this](unsigned steps){advance_batch(steps);};
    pc_.device_reset_hook=[this](){
      if(prior_reset_)prior_reset_();
      reset_devices();
    };
  }
  ~PcMotherboardC06(){
    // Never leave a hook pointing at a destroyed motherboard instance.
    pc_.cpu.set_x87_ignne(false);
    pc_.cpu.in_hook=std::move(prior_in_);
    pc_.cpu.out_hook=std::move(prior_out_);
    pc_.cpu.mmio_claim_hook=std::move(prior_mmio_claim_);
    pc_.cpu.mmio_range_claim_hook=std::move(prior_mmio_range_claim_);
    pc_.cpu.mmio_read_hook=std::move(prior_mmio_read_);
    pc_.cpu.mmio_write_hook=std::move(prior_mmio_write_);
    pc_.device_step_hook=std::move(prior_tick_);
    pc_.device_batch_limit_hook=std::move(prior_batch_limit_);
    pc_.device_batch_advance_hook=std::move(prior_batch_advance_);
    pc_.device_reset_hook=std::move(prior_reset_);
  }
  PcMotherboardC06(const PcMotherboardC06&)=delete;
  PcMotherboardC06& operator=(const PcMotherboardC06&)=delete;
  Evidence evidence()const{
    return {ticks_,irq_delivered_,pit_irqs_,apic_irqs_,true,true,powered_off_,ata_irq_delivered_};
  }
  // Deterministic single guest-cycle advance; no host sleeper or background
  // worker. PC's canonical run loop calls this once per guest step.
  void advance_one(){poll();}
  void enable_ps2_keyboard(){keyboard_.emplace();}
  bool inject_ps2_key(unsigned code,bool pressed){
    return keyboard_&&keyboard_->inject(code,pressed);
  }
  AtaC07* attached_ata(){return ata_.get();}
  const AtaC07* attached_ata()const{return ata_.get();}
  const AtapiCdrom* attached_atapi()const{return optical_.get();}
  bool optical_attached()const{return bool(optical_);}
};
} // namespace reverse_windows
