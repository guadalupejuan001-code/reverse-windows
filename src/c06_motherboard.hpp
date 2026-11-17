#pragma once
// REVERSE Windows C06: one attached motherboard for the canonical PcMachine.
// PCI configuration / 8259 PIC / PIT channel0 / RTC CMOS / ACPI PM register
// model / a bounded LAPIC and IOAPIC register subset. Not a chipset clone.
#include "c06_acpi.hpp"
#include "c08_guest_gpu.hpp"
#include "c07_ata.hpp"
#include "atapi_cdrom.hpp"
#include "pc_machine.hpp"
#include "ps2_keyboard.hpp"
#include "smp_workers.hpp"
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
    std::uint64_t ata_irqs=0,sci_irqs=0;
  };
  struct InterruptState {
    bool global_enabled, software_enabled;
    unsigned pending, in_service, priority;
    std::uint32_t timer_count;
    std::uint32_t ioapic_select, ioapic_id;
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
      if(!candidates)return 0;
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
      auto selected_mode=(v>>1)&7U;
      if(selected_mode>=6)selected_mode-=4;
      if((v&1)|| (selected_mode!=0&&selected_mode!=2&&selected_mode!=3))return false;
      access=(v>>4)&3;mode=selected_mode;
      enabled=false;phase=0;read_phase=0;latch_valid=false;return true;
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
      if(counter) --counter;
      if(counter)return false;
      if(mode==0)enabled=false;
      else counter=reload;
      return true;
    }
  };

  cosmic_pc::PcMachine& pc_;
  std::optional<Ps2Keyboard> keyboard_;
  std::function<std::optional<std::uint32_t>(std::uint16_t,unsigned)> prior_in_;
  std::function<bool(std::uint16_t,unsigned,std::uint32_t)> prior_out_;
  decltype(cosmic_pc::Cpu::pio_batch_limit_hook) prior_pio_batch_limit_;
  decltype(cosmic_pc::Cpu::pio_batch_transfer_hook) prior_pio_batch_transfer_;
  std::function<bool(std::uint64_t)> prior_mmio_claim_;
  std::function<bool(std::uint64_t,std::size_t)> prior_mmio_range_claim_;
  std::function<std::optional<std::uint8_t>(std::uint64_t)> prior_mmio_read_;
  std::function<bool(std::uint64_t,std::uint8_t)> prior_mmio_write_;
  decltype(cosmic_pc::Cpu::mmio_read32_hook) prior_mmio_read32_;
  decltype(cosmic_pc::Cpu::mmio_write32_hook) prior_mmio_write32_;
  std::function<void()> prior_tick_;
  std::function<unsigned(unsigned)> prior_batch_limit_;
  std::function<void(unsigned)> prior_batch_advance_;
  std::function<unsigned(unsigned)> prior_idle_advance_;
  std::function<void()> prior_reset_;
  Pic master_{},slave_{};
  std::unique_ptr<AtaC07> ata_;
  std::unique_ptr<AtaC07> data_ata_;
  std::unique_ptr<AtapiCdrom> optical_;
  std::unique_ptr<GuestGpuC08> gpu_;
  bool prior_pci_reservation_=false;
  std::array<std::uint8_t,256> ata_cfg_{};
  std::uint64_t ata_irq_delivered_=0;
  Pit pit_{},pit2_{};
  unsigned pit_phase_=0;
  std::uint8_t speaker_control_=0;
  std::uint32_t pci_selector_=0;
  std::uint8_t cmos_index_=0,rtc_b_=2;
  std::array<std::uint8_t,256> host_cfg_{},isa_cfg_{};
  std::uint16_t pm_control_=0x0001; // SCI_EN: firmware starts in ACPI mode.
  std::uint16_t pm_status_=0,pm_enable_=0;
  bool sci_active_=false;
  bool powered_off_=false;
  bool ferr_was_asserted_=false;
  std::uint64_t ticks_=0,irq_delivered_=0,pit_irqs_=0,apic_irqs_=0,sci_irqs_=0;
  static constexpr std::uint32_t kLvtMasked=1U<<16;
  static constexpr unsigned kTimer=0,kLint0=3,kLint1=4,kError=5;
  // Local state is private to one x86 logical processor. The I/O APIC,
  // devices, E820 map, and Ether-backed RAM belong to the whole machine.
  struct PendingWrite {
    std::uint64_t aligned = 0;
    std::uint32_t value = 0;
    std::uint8_t bytes = 0;
  };
  // IRR and ISR are read on every eligible CPU tick. Recompute the priority
  // only when an interrupt is queued or the current highest vector is removed.
  class ApicPriorityVectors {
   public:
    void set(unsigned vector) {
      bits_.set(vector);
      highest_ = std::max(highest_, vector);
    }
    void reset(unsigned vector) {
      bits_.reset(vector);
      if (vector != highest_) return;
      highest_ = 0;
      if (bits_.none()) return;
      while (vector > 16) {
        --vector;
        if (bits_[vector]) {
          highest_ = vector;
          return;
        }
      }
    }
    unsigned highest() const noexcept { return highest_; }
    const std::bitset<256>& bits() const noexcept { return bits_; }

   private:
    std::bitset<256> bits_;
    unsigned highest_ = 0;
  };
  struct LocalApic {
    explicit LocalApic(cosmic_pc::Cpu* processor = nullptr) : cpu(processor) {
      lvt.fill(kLvtMasked);
    }
    cosmic_pc::Cpu* cpu;
    bool started = false, init_pending = false;
    std::optional<std::uint8_t> startup_vector;
    std::uint32_t svr = 0xff, ldr = 0, dfr = 0xffffffffU;
    std::uint32_t esr = 0, errors = 0, icr_low = 0, icr_high = 0;
    std::array<std::uint32_t,6> lvt{};
    std::uint32_t timer_reload = 0, divider = 0;
    std::uint64_t timer_cycles = 0, last_tick = 0;
    ApicPriorityVectors pending, in_service;
    std::bitset<256> pending_level, in_service_level, pending_ata, pending_sci;
    PendingWrite mmio_pending;
  };
  static constexpr auto kMaxProcessors = cosmic_pc::CpuTopology::kMaxLogical;
  static constexpr unsigned kMinParallelRegisterSteps = 128;
  std::array<std::unique_ptr<cosmic_pc::Cpu>, kMaxProcessors> secondary_cpus_{};
  std::array<LocalApic, kMaxProcessors> apics_;
  std::array<std::uint64_t, kMaxProcessors> retired_before_init_{};
  unsigned processor_count_ = 1;
  unsigned next_cpu_ = 0;
  std::optional<unsigned> faulted_processor_;
  std::unique_ptr<SmpWorkers> workers_;
  std::uint64_t parallel_register_batches_ = 0, parallel_register_steps_ = 0;
  unsigned longest_parallel_register_slice_ = 0;
  bool prior_local_apic_=false;
  const std::uint64_t* prior_clock_ = nullptr;
  cosmic_pc::CpuTopology prior_topology_;
  unsigned prior_apic_id_ = 0;
  std::uint32_t ioapic_select_=0;
  std::uint8_t ioapic_id_=0;
  std::array<std::uint32_t,24> io_redir_low_{},io_redir_high_{};
  std::array<std::uint8_t,24> io_remote_vector_{};
  std::array<std::uint32_t,24> io_remote_targets_{};
  LocalApic& bsp() noexcept { return apics_[0]; }
  const LocalApic& bsp() const noexcept { return apics_[0]; }

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
    b[0x0e]=0; // Single-function bridge.
  }
  void reset_devices(){
    if(keyboard_) keyboard_->reset();
    master_=Pic{};slave_=Pic{};slave_.vector=0x70;
    pit_=Pit{};pit2_=Pit{};pit_phase_=0;speaker_control_=0;pci_selector_=0;
    cmos_index_=0;rtc_b_=2;pm_control_=0x0001;powered_off_=false;
    pm_status_=pm_enable_=0;sci_active_=false;
    ferr_was_asserted_=false;pc_.cpu.set_x87_ignne(false);
    ticks_=irq_delivered_=pit_irqs_=apic_irqs_=ata_irq_delivered_=sci_irqs_=0;
    if(ata_)ata_->reset();
    if(data_ata_)data_ata_->reset();
    if(optical_)optical_->reset();
    if(gpu_)gpu_->reset();
    pc_.cpu.set_apic_task_priority(0);
    bsp() = LocalApic{&pc_.cpu};
    bsp().started = true;
    for (unsigned id = 1; id < processor_count_; ++id) {
      secondary_cpus_[id]->reset(0, 0);
      apics_[id] = LocalApic{secondary_cpus_[id].get()};
    }
    retired_before_init_.fill(0);
    faulted_processor_.reset();
    next_cpu_ = 0;
    parallel_register_batches_ = parallel_register_steps_ = 0;
    longest_parallel_register_slice_ = 0;
    ioapic_select_=0;
    ioapic_id_=std::uint8_t(AcpiC06::ioapic_id(processor_count_));
    io_redir_low_.fill(0x10000U);io_redir_high_.fill(0);
    io_remote_vector_.fill(0);
    io_remote_targets_.fill(0);
    build_config(host_cfg_,0x0001,0x00);
    build_config(isa_cfg_,0x0002,0x01);
    if(ide_present()){
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
    AcpiC06::install(pc_.cpu,ide_present(),bool(gpu_));
  }
  bool ide_present()const noexcept{return bool(ata_)||bool(optical_);}
  AtaC07* selected_ata() const noexcept {
    return data_ata_&&data_ata_->selected()?data_ata_.get():ata_.get();
  }
  static bool ide_port(std::uint16_t port) noexcept {
    return (port>=AcpiC06::primary_ide_ports&&port<AcpiC06::primary_ide_ports+8)||
           port==AcpiC06::primary_ide_control||AtapiCdrom::handles(port);
  }
  bool owned_port(std::uint16_t p) const {
    return ide_port(p)||p==0x20||p==0x21||p==0xa0||p==0xa1||
           (p>=0x40&&p<=0x43)||p==0x61||(p>=0x70&&p<=0x71)||
           (p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)||
           (p>=AcpiC06::pm_event_port&&p<AcpiC06::pm_timer_port+4);
  }
  unsigned selected_pci_device() const noexcept {
    if(!(pci_selector_ & 0x80000000U) ||
       ((pci_selector_>>16)&255U)!=0 || ((pci_selector_>>8)&7U)!=0)
      return 32;
    return (pci_selector_>>11)&31;
  }
  std::array<std::uint8_t,256>* selected_pci(unsigned dev){
    if(dev==0)return &host_cfg_;
    if(dev==1)return &isa_cfg_;
    if(dev==2&&ide_present())return &ata_cfg_;
    return nullptr;
  }
  std::optional<std::uint32_t> pci_read(std::uint16_t p,unsigned width){
    if(p==0xcf8&&width==4)return pci_selector_;
    if(p>=0xcfc&&p+width<=0xd00&&(width==1||width==2||width==4)){
      const unsigned offset=(pci_selector_&0xfcU)+(p-0xcfc);
      if(offset+width>256)return part_mask(width);
      const auto dev=selected_pci_device();
      if (gpu_ && dev==GuestGpuC08::pci_device) {
        const auto value=gpu_->config_word(offset&~3U);
        return (value>>((offset&3U)*8))&part_mask(width);
      }
      const auto config=selected_pci(dev);
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
      const unsigned offset=(pci_selector_&0xfcU)+(p-0xcfc);
      if(offset+width>256)return true;
      const auto dev=selected_pci_device();
      if(gpu_ && dev==GuestGpuC08::pci_device) {
        if(gpu_->config_write(offset,width,value))pc_.cpu.memory.invalidate_page_tables();
        return true;
      }
      const auto config=selected_pci(dev);
      if(!config)return true; // non-existent devices ignore writes
      // Fixed-function bridges implement only the low decode bits. IDE
      // has compatibility I/O ports and no memory BARs or bus mastering.
      for(unsigned i=0;i<width;++i){
        const unsigned at=offset+i;
        if(at==4)(*config)[4]=std::uint8_t(value>>(8*i))&(config==&ata_cfg_?1U:3U);
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
    if(ide_port(p)){
      if(ata_cfg_[4]&1U){
        if(ata_&&ata_->handles(p))return selected_ata()->input(p,width);
        if(optical_&&optical_->handles(p))return optical_->input(p,width);
      }
      return UINT32_MAX; // Inactive or empty channel: open bus.
    }
    if(p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)return pci_read(p,width);
    if(p==AcpiC06::pm_timer_port&&width==4)return pm_timer();
    if(p==AcpiC06::pm_control_port&&width==2)return pm_control_;
    if(p==AcpiC06::pm_event_port&&(width==2||width==4))
      return pm_status_|(width==4?std::uint32_t(pm_enable_)<<16:0);
    if(p==AcpiC06::pm_event_port+2&&width==2)return pm_enable_;
    if(width!=1)return std::nullopt;
    switch(p){
      case 0x20:return master_.status();
      case 0x21:return master_.mask;
      case 0xa0:return slave_.status();
      case 0xa1:return slave_.mask;
      case 0x40:return pit_.read();
      case 0x42:return pit2_.read();
      case 0x61:{
        const bool gate=(speaker_control_&1)!=0;
        const bool out=pit2_.mode==0 ? !pit2_.enabled :
            !gate||!pit2_.enabled||pit2_.counter>pit2_.reload/2;
        return speaker_control_|(out?0x20U:0U)|(((ticks_/54U)&1U)?0x10U:0U);
      }
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
    if(ide_port(p)){
      if(ata_cfg_[4]&1U){
        if(ata_&&ata_->handles(p)){
          if(data_ata_&&width==1&&(p==AtaC07::kDrive||p==AtaC07::kControl)){
            // IDE device selection and software reset are shared by both units.
            data_ata_->output(p,width,value);
            return ata_->output(p,width,value);
          }
          return selected_ata()->output(p,width,value);
        }
        if(optical_&&optical_->handles(p))return optical_->output(p,width,value);
      }
      return width==1||width==2||width==4; // No device receives the command.
    }
    if(p>=AcpiC06::pci_config_ports&&p<AcpiC06::pci_config_ports+8)return pci_write(p,width,value);
    if(p==AcpiC06::pm_control_port&&width==2){
      pm_control_=std::uint16_t((value&0x1c00U)|0x0001U); // SLP_TYP and read-only SCI_EN.
      if((value&0x2000)&&((value>>10)&7)==5){
        powered_off_=true;
        pc_.power_off();
      }
      return true;
    }
    if(p==AcpiC06::pm_event_port&&(width==2||width==4)){
      pm_status_&=~std::uint16_t(value&1U); // TMR_STS is write-one-to-clear.
      if(width==4)pm_enable_=std::uint16_t((value>>16)&1U);
      update_sci();
      return true;
    }
    if(p==AcpiC06::pm_event_port+2&&width==2){
      pm_enable_=std::uint16_t(value&1U);
      update_sci();
      return true;
    }
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
      case 0xa0:slave_.command(b);update_sci();return true;
      case 0xa1:slave_.data(b);return true;
      case 0x40:pit_.data(b);return true;
      case 0x42:pit2_.data(b);return true;
      case 0x43:
        if((b>>6)==2) return pit2_.command(b&0x3fU);
        return pit_.command(b);
      case 0x61:
        if(!(speaker_control_&1)&&(b&1)&&pit2_.mode!=0&&pit2_.enabled)pit2_.counter=pit2_.reload;
        speaker_control_=b&3U;
        return true;
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
        (gpu_ && gpu_->claims(start,length)) ||
        (prior_mmio_claim_ && (!prior_mmio_range_claim_ || prior_mmio_range_claim_(start,length))));
  }
  static std::uint32_t vector_register(const std::bitset<256>& vectors,unsigned offset) {
    const unsigned first=(offset&0x70U)*2;
    std::uint32_t result=0;
    for(unsigned i=0;i<32;++i)
      if(vectors[first+i])result|=1U<<i;
    return result;
  }
  static unsigned processor_priority(const LocalApic& apic) {
    const auto tpr=apic.cpu->apic_task_priority();
    const auto service=apic.in_service.highest()&0xf0U;
    return (tpr&0xf0U)>=service?tpr:service;
  }
  static unsigned timer_divisor(const LocalApic& apic) {
    const auto index=(apic.divider&3U)|((apic.divider&8U)>>1);
    return 1U<<((index+1)&7U);
  }
  static std::uint32_t timer_count(const LocalApic& apic) {
    const auto divisor=timer_divisor(apic);
    return std::uint32_t((apic.timer_cycles+divisor-1)/divisor);
  }
  static std::optional<unsigned> lvt_index(unsigned offset) {
    if(offset>=0x320&&offset<=0x370&&(offset&15U)==0)
      return (offset-0x320)/16;
    return std::nullopt;
  }
  std::optional<std::uint32_t> mmio_register(const LocalApic& apic, std::uint64_t base) const {
    if(base>=kLocalApic&&base<kLocalApic+4096) {
      if(!apic.cpu->apic_global_enabled())return std::nullopt;
      const auto r=unsigned(base-kLocalApic);
      if(r>=0x100&&r<=0x170&&(r&15U)==0)return vector_register(apic.in_service.bits(),r);
      if(r>=0x180&&r<=0x1f0&&(r&15U)==0)
        return vector_register(apic.pending_level|apic.in_service_level,r);
      if(r>=0x200&&r<=0x270&&(r&15U)==0)return vector_register(apic.pending.bits(),r);
      if(const auto lvt=lvt_index(r))return apic.lvt[*lvt];
      switch(r) {
        case 0x20:return apic.cpu->apic_id() << 24;
        case 0x30:return 0x00050014U; // Version 0x14, six LVT entries.
        case 0x80:return apic.cpu->apic_task_priority();
        case 0xa0:return processor_priority(apic);
        case 0xb0:return 0; // EOI write-only
        case 0xd0:return apic.ldr;
        case 0xe0:return apic.dfr;
        case 0xf0:return apic.svr;
        case 0x280:return apic.esr;
        case 0x300:return apic.icr_low;
        case 0x310:return apic.icr_high;
        case 0x380:return apic.timer_reload;
        case 0x390:return timer_count(apic);
        case 0x3e0:return apic.divider;
        default:return std::nullopt;
      }
    }
    if(base>=kIoApic&&base<kIoApic+4096) {
      const auto r=unsigned(base-kIoApic);
      if(r==0)return ioapic_select_;
      if(r!=0x10)return std::nullopt;
      if(ioapic_select_==0||ioapic_select_==2)return std::uint32_t(ioapic_id_)<<24;
      if(ioapic_select_==1)return 0x00170011U; // version and max RTE 23
      if(ioapic_select_>=0x10&&ioapic_select_<0x40) {
        const unsigned pin=(ioapic_select_-0x10)/2;
        return (ioapic_select_&1)?io_redir_high_[pin]:
            io_redir_low_[pin]|(io_remote_vector_[pin]?1U<<14:0);
      }
    }
    return std::nullopt;
  }
  std::optional<std::uint8_t> mmio_byte(const LocalApic& apic, std::uint64_t a) const{
    const auto reg=mmio_register(apic, a&~std::uint64_t{3});
    if(!reg)return std::nullopt;
    return std::uint8_t((*reg>>((a&3)*8))&255U);
  }
  bool mmio_commit(LocalApic& apic, std::uint64_t base,std::uint32_t v) {
    if(base>=kLocalApic&&base<kLocalApic+4096){
      if(!apic.cpu->apic_global_enabled())return false;
      const auto r=unsigned(base-kLocalApic);
      if(const auto lvt=lvt_index(r)){
        if(*lvt==kTimer&&(v&0x40000U))return false; // No TSC deadline.
        const std::uint32_t writable=*lvt==kTimer?0x300ffU:
            (*lvt==kLint0||*lvt==kLint1)?0x1a7ffU:
            (*lvt==kError?0x100ffU:0x107ffU);
        apic.lvt[*lvt]=(v&writable)|(lapic_on(apic)?0:kLvtMasked);
        return true;
      }
      switch(r){
        case 0x80:apic.cpu->set_apic_task_priority(std::uint8_t(v));return true;
        case 0xb0:complete_lapic_irq(apic);return true;
        case 0xd0:apic.ldr=v&0xff000000U;return true;
        case 0xe0:apic.dfr=(v&0xf0000000U)|0x0fffffffU;return true;
        case 0xf0:
          apic.svr=v&0x1ffU;
          if(!lapic_on(apic))for(auto& lvt:apic.lvt)lvt|=kLvtMasked;
          if(sci_active_)deliver_sci();
          return true;
        case 0x280:apic.esr=std::exchange(apic.errors,0);return true;
        case 0x300:return send_local_ipi(apic,v);
        case 0x310:apic.icr_high=v&0xff000000U;return true;
        case 0x380:
          apic.timer_reload=v;apic.timer_cycles=std::uint64_t(v)*timer_divisor(apic);return true;
        case 0x3e0:{
          const auto count=timer_count(apic);
          apic.divider=v&0x0bU;apic.timer_cycles=std::uint64_t(count)*timer_divisor(apic);return true;
        }
        default:return false;
      }
    }
    if(base>=kIoApic&&base<kIoApic+4096){
      const auto r=unsigned(base-kIoApic);
      if(r==0){ioapic_select_=v&0xffU;return true;}
      if(r!=0x10)return false;
      // C06 advertises an eight-bit APIC ID for the I/O controller in the MADT.
      if(ioapic_select_==0){ioapic_id_=std::uint8_t(v>>24);return true;}
      if(ioapic_select_==1||ioapic_select_==2)return true; // Version and arbitration are read-only.
      if(ioapic_select_>=0x10&&ioapic_select_<0x40){
        const unsigned pin=(ioapic_select_-0x10)/2;
        if(ioapic_select_&1)io_redir_high_[pin]=v&0xff000000U;
        else io_redir_low_[pin]=v&0x1afffU;
        if(pin==AcpiC06::sci_irq&&sci_active_)deliver_sci();
        return true;
      }
      return false;
    }
    return false;
  }
  bool mmio_write(LocalApic& apic, std::uint64_t a,std::uint8_t byte) {
    const auto base=a&~std::uint64_t{3};
    if(a%4==0)apic.mmio_pending={base,0,0};
    if(apic.mmio_pending.aligned!=base||apic.mmio_pending.bytes!=(a&3))
      return false; // no partial/out-of-order MMIO register commits
    apic.mmio_pending.value|=std::uint32_t(byte)<<((a&3)*8);
    ++apic.mmio_pending.bytes;
    if((a&3)==3){
      const auto value=apic.mmio_pending.value;apic.mmio_pending={};
      return mmio_commit(apic, base,value);
    }
    return true;
  }
  static bool lapic_on(const LocalApic& apic){return apic.cpu->apic_global_enabled()&&(apic.svr&0x100U);}
  bool pic_connected() const {
    const auto& apic = bsp();
    const auto lint=apic.lvt[kLint0];
    return !lapic_on(apic)||(lint&(kLvtMasked|0x700U))==0x700U; // ExtINT.
  }
  static unsigned deliverable_lapic_vector(const LocalApic& apic) {
    if(!lapic_on(apic))return 0;
    const auto vector=apic.pending.highest();
    if(!vector)return 0;
    return (vector&0xf0U)>(processor_priority(apic)&0xf0U)?vector:0;
  }
  bool interrupt_pending()const {
    return (pic_connected()&&master_.pending())||deliverable_lapic_vector(bsp());
  }
  static bool lapic_destination(const LocalApic& apic, std::uint32_t command,std::uint32_t destination) {
    const auto id=destination>>24;
    if(!(command&(1U<<11)))return id==apic.cpu->apic_id()||id==255;
    const auto logical=apic.ldr>>24;
    if(id==255)return true;
    if((apic.dfr>>28)==15)return (id&logical)!=0;
    return (id&0xf0U)==(logical&0xf0U)&&(id&logical&0x0fU)!=0;
  }
  std::uint32_t interrupt_targets(std::uint32_t command, std::uint32_t destination,
                                  unsigned mode) const {
    std::uint32_t mask = 0;
    unsigned best = kMaxProcessors;
    for (unsigned id = 0; id < processor_count_; ++id) {
      const auto& apic = apics_[id];
      if (!apic.started || apic.init_pending || !lapic_on(apic) ||
          !lapic_destination(apic, command, destination)) continue;
      if (mode != 1) mask |= 1U << id;
      else if (best == kMaxProcessors || processor_priority(apic) < processor_priority(apics_[best]))
        best = id;
    }
    return best == kMaxProcessors ? mask : 1U << best;
  }
  bool send_local_ipi(LocalApic& source, std::uint32_t value) {
    const unsigned mode=(value>>8)&7U,shorthand=(value>>18)&3U,vector=value&255U;
    const auto sender = source.cpu->apic_id();
    const auto selected = [&](unsigned id) {
      return shorthand == 1 ? id == sender : shorthand == 2 ? true :
             shorthand == 3 ? id != sender : lapic_destination(apics_[id], value, source.icr_high);
    };
    if (mode > 1 && mode != 5 && mode != 6) {
      for (unsigned id = 0; id < processor_count_; ++id)
        if (selected(id)) return false;
    }
    source.icr_low=value&0x000ccfffU;
    if (mode == 5 || mode == 6) {
      // INIT level deassert has no effect on xAPIC startup state. Queue INIT
      // and SIPI so only the target's executor can reset its CPU or JIT cache.
      if (mode == 5 && (value & 0xc000U) == 0x8000U) return true;
      for (unsigned id = 1; id < processor_count_; ++id) {
        auto& target = apics_[id];
        if (!selected(id)) continue;
        if (mode == 5) {
          target.init_pending = true;
          target.startup_vector.reset();
        } else if ((!target.started || target.init_pending) && !target.startup_vector) {
          target.startup_vector = std::uint8_t(vector);
          if (sender == 0) pc_.yield_after_instruction();
        }
      }
      return true;
    }
    unsigned best = kMaxProcessors;
    for (unsigned id = 0; id < processor_count_; ++id) {
      auto& target = apics_[id];
      if (!selected(id) || !target.started || target.init_pending || !lapic_on(target)) continue;
      if (vector < 16) { signal_apic_error(source, 1U << 5); break; }
      if (mode == 0) target.pending.set(vector);
      else if (best == kMaxProcessors || processor_priority(target) < processor_priority(apics_[best]))
        best = id;
    }
    if (best != kMaxProcessors) apics_[best].pending.set(vector);
    return true;
  }
  bool signal_pci_msi(std::uint32_t address, std::uint16_t data) {
    if ((address & 0xfff00ff3U) != 0xfee00000U) return false;
    const unsigned vector = data & 255U, mode = (data >> 8) & 7U;
    if (mode > 1 || (data & 0xb800U) || vector < 16) return false;
    const auto targets = interrupt_targets((address & 4U) ? 1U << 11 : 0,
                                           ((address >> 12) & 255U) << 24, mode);
    for (unsigned id = 0; id < processor_count_; ++id)
      if (targets & (1U << id)) apics_[id].pending.set(vector);
    return targets != 0;
  }
  static void signal_apic_error(LocalApic& apic, std::uint32_t bit) {
    apic.errors|=bit;
    const auto lvt=apic.lvt[kError];
    if(lapic_on(apic)&&!(lvt&kLvtMasked)&&(lvt&255U)>=16)apic.pending.set(lvt&255U);
  }
  void complete_lapic_irq(LocalApic& apic) {
    const auto vector=apic.in_service.highest();
    if(!vector)return;
    apic.in_service.reset(vector);
    if(!apic.in_service_level.test(vector))return;
    apic.in_service_level.reset(vector);
    for (unsigned line = 0; line < io_remote_vector_.size(); ++line) {
      if (io_remote_vector_[line] != vector) continue;
      io_remote_targets_[line] &= ~(1U << apic.cpu->apic_id());
      if (!io_remote_targets_[line]) io_remote_vector_[line] = 0;
    }
    if(sci_active_)deliver_sci();
  }
  void signal_isa_irq(unsigned line) {
    // Keep the legacy PIC edge even when the guest uses the I/O APIC.
    if(line>=8)slave_.irr|=std::uint8_t(1U<<(line-8));
    else master_.irr|=std::uint8_t(1U<<line);
    route_ioapic(line);
  }
  void route_ioapic(unsigned line) {
    const auto redirect=io_redir_low_[line];
    const unsigned vector=redirect&255U,mode=(redirect>>8)&7U;
    if ((redirect & (1U << 16)) || mode > 1) return;
    const auto targets = interrupt_targets(redirect, io_redir_high_[line], mode);
    if (!targets) return;
    if (vector < 16) { signal_apic_error(bsp(), 1U << 6); return; }
    const bool level=(redirect&(1U<<15))!=0;
    if (level) {
      if (io_remote_vector_[line]) return;
      io_remote_vector_[line] = std::uint8_t(vector);
      io_remote_targets_[line] = targets;
    }
    for (unsigned id = 0; id < processor_count_; ++id) {
      if (!(targets & (1U << id))) continue;
      auto& apic = apics_[id];
      apic.pending.set(vector);
      if (level) apic.pending_level.set(vector);
      if (line == 14) apic.pending_ata.set(vector);
      if (line == AcpiC06::sci_irq) apic.pending_sci.set(vector);
    }
  }
  void deliver_sci() {
    const auto mask=std::uint8_t(1U<<(AcpiC06::sci_irq-8));
    if(sci_active_&&!(slave_.isr&mask))slave_.irr|=mask;
    route_ioapic(AcpiC06::sci_irq);
  }
  void update_sci() {
    sci_active_=(pm_status_&pm_enable_&1U)!=0;
    if(sci_active_)deliver_sci();
    else slave_.irr&=std::uint8_t(~(1U<<(AcpiC06::sci_irq-8)));
  }
  std::uint32_t pm_timer()const noexcept {
    return std::uint32_t(ticks_)&AcpiC06::pm_timer_mask;
  }
  static constexpr unsigned kPitClockDivider=cosmic_pc::PcMachine::kPitClockDivider;
  unsigned pm_timer_edge_in()const noexcept {
    return AcpiC06::pm_timer_half_period-(pm_timer()&(AcpiC06::pm_timer_half_period-1));
  }
  static void advance_apic_timer(LocalApic& apic, std::uint64_t cycles) {
    if (!apic.cpu->apic_global_enabled() || !apic.timer_cycles || !cycles) return;
    if (cycles < apic.timer_cycles) {
      apic.timer_cycles -= cycles;
      return;
    }
    const auto lvt = apic.lvt[kTimer];
    if (lapic_on(apic) && !(lvt & kLvtMasked)) {
      if ((lvt & 255U) >= 16) apic.pending.set(lvt & 255U);
      else signal_apic_error(apic, 1U << 6);
    }
    const auto period = (lvt & 0x20000U) ? std::uint64_t(apic.timer_reload)*timer_divisor(apic) : 0;
    apic.timer_cycles = period ? period - (cycles - apic.timer_cycles) % period : 0;
  }
  bool deliver_lapic_irq(LocalApic& apic) {
    const auto vector = deliverable_lapic_vector(apic);
    if (!vector) return false;
    if (apic.cpu->raise_external_irq(std::uint8_t(vector))) {
      apic.in_service.set(vector);
      apic.pending.reset(vector);
      apic.in_service_level.set(vector, apic.pending_level[vector]);
      apic.pending_level.reset(vector);
      if (apic.pending_ata[vector]) ++ata_irq_delivered_;
      if (apic.pending_sci[vector]) ++sci_irqs_;
      apic.pending_ata.reset(vector);
      apic.pending_sci.reset(vector);
      ++apic_irqs_; ++irq_delivered_;
    }
    return true;
  }
  void poll(){
    ++ticks_;
    auto& cpu=pc_.cpu;
    const bool ferr=cpu.x87_ferr_asserted();
    if(ferr&&!ferr_was_asserted_)signal_isa_irq(13);
    if(!ferr&&ferr_was_asserted_)cpu.set_x87_ignne(false);
    ferr_was_asserted_=ferr;
    if(++pit_phase_==kPitClockDivider){
      pit_phase_=0;
      if(pit_.enabled&&pit_.tick()){
        ++pit_irqs_;
        signal_isa_irq(0);
      }
      if((speaker_control_&1)&&pit2_.enabled)pit2_.tick();
    }
    if((ticks_&(AcpiC06::pm_timer_half_period-1))==0){
      pm_status_|=1U;
      update_sci();
    }
    // A level interrupt stays asserted if the guest enables its LAPIC
    // after the PM event arose.
    if(sci_active_&&(io_redir_low_[AcpiC06::sci_irq]&0x8000U))deliver_sci();
    if(keyboard_&&keyboard_->needs_irq()&&!(prior_in_(0x64,1).value_or(0)&1U)){
      signal_isa_irq(1);
      keyboard_->mark_irq();
    }
    advance_apic_timer(bsp(),1);
    // The slave's INT output reaches the master independently of CPU IF or
    // LAPIC acceptance. The guest can read both IRRs with interrupts disabled.
    const auto slave_irq=slave_.pending();
    if(slave_irq)master_.irr|=std::uint8_t(1U<<2);
    else if(!(master_.isr&(1U<<2)))master_.irr&=std::uint8_t(~(1U<<2));
    if(cpu.fault||!cpu.flag(cosmic_pc::Cpu::IF))return;
    if (deliver_lapic_irq(bsp())) return;
    // 8259 cascade: a pending slave IRQ presents IRQ2 at the master PIC.
    // Deliver the SLAVE vector, not master IRQ2's vector. Both PICs must EOI.
    const auto master_irq=pic_connected()?master_.pending():0;
    if(master_irq){
      const unsigned n=master_irq-1;
      if(n==2&&slave_irq){
        const unsigned sn=slave_irq-1;
        if(cpu.raise_external_irq(std::uint8_t(slave_.vector+sn))){
          slave_.accepted(sn);master_.accepted(2);
          ++irq_delivered_;if(sn==6)++ata_irq_delivered_;
          if(sn==AcpiC06::sci_irq-8)++sci_irqs_;
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
       (pc_.cpu.flag(cosmic_pc::Cpu::IF)&&interrupt_pending())) return 1;
    if(pit_.enabled)steps=std::min(steps,pit_.counter*kPitClockDivider-pit_phase_);
    if((speaker_control_&1)&&pit2_.enabled)
      steps=std::min(steps,pit2_.counter*kPitClockDivider-pit_phase_);
    steps=std::min(steps,pm_timer_edge_in());
    if(pc_.cpu.apic_global_enabled()&&bsp().timer_cycles&&bsp().timer_cycles<steps)
      steps=unsigned(bsp().timer_cycles);
    return steps;
  }
  void advance_batch(unsigned extra_steps) {
    ticks_+=extra_steps;
    const auto pit_elapsed=pit_phase_+extra_steps;
    if(pit_.enabled)pit_.counter-=pit_elapsed/kPitClockDivider;
    if((speaker_control_&1)&&pit2_.enabled)pit2_.counter-=pit_elapsed/kPitClockDivider;
    pit_phase_=pit_elapsed%kPitClockDivider;
    if(pc_.cpu.apic_global_enabled()&&bsp().timer_cycles)bsp().timer_cycles-=extra_steps;
  }
  unsigned advance_idle(unsigned max_extra) {
    if(!pc_.cpu.halted||!pc_.cpu.flag(cosmic_pc::Cpu::IF))return 0;
    const auto extra=batch_limit(max_extra+1)-1;
    advance_batch(extra);
    return extra;
  }

  void attach_mmio(cosmic_pc::Cpu& cpu, LocalApic& apic) {
    cpu.mmio_claim_hook = [this](std::uint64_t address) {
      return mmio_claim(address) || (gpu_ && gpu_->claims(address)) ||
             (prior_mmio_claim_ && prior_mmio_claim_(address));
    };
    cpu.mmio_range_claim_hook = [this](std::uint64_t start, std::size_t length) {
      return mmio_range_claim(start, length);
    };
    cpu.mmio_read_hook = [this, &apic](std::uint64_t address) {
      if (mmio_claim(address)) return mmio_byte(apic, address);
      if (gpu_ && gpu_->claims(address)) return std::optional<std::uint8_t>(gpu_->read(address));
      return prior_mmio_read_ ? prior_mmio_read_(address) : std::nullopt;
    };
    cpu.mmio_write_hook = [this, &apic](std::uint64_t address, std::uint8_t value) {
      if (mmio_claim(address)) return mmio_write(apic, address, value);
      if (gpu_ && gpu_->claims(address)) return gpu_->write(address, value);
      return prior_mmio_write_ && prior_mmio_write_(address, value);
    };
    cpu.mmio_read32_hook = [this, &apic](std::uint64_t address) {
      if (mmio_claim(address)) return mmio_register(apic, address);
      if (gpu_ && gpu_->claims(address)) return std::optional<std::uint32_t>(gpu_->read32(address));
      return prior_mmio_read32_ ? prior_mmio_read32_(address) : std::nullopt;
    };
    cpu.mmio_write32_hook = [this, &apic](std::uint64_t address, std::uint32_t value)
        -> std::optional<bool> {
      if (mmio_claim(address)) {
        apic.mmio_pending = {};
        return mmio_commit(apic, address, value);
      }
      if (gpu_ && gpu_->claims(address)) {
        if (gpu_->write32(address, value)) return true;
        return std::nullopt;
      }
      return prior_mmio_write32_ ? prior_mmio_write32_(address, value) : std::nullopt;
    };
  }

  static bool needs_service(const LocalApic& apic) noexcept {
    return apic.init_pending || apic.startup_vector ||
           (apic.started && (!apic.cpu->halted || apic.cpu->flag(cosmic_pc::Cpu::IF)));
  }
  void apply_startup(LocalApic& apic) {
    auto& cpu = *apic.cpu;
    if (apic.init_pending) {
      const auto id = cpu.apic_id();
      const auto sipi = apic.startup_vector;
      retired_before_init_[id] += cpu.instructions;
      cpu.reset(0, 0);
      apic = LocalApic{&cpu};
      apic.startup_vector = sipi;
      for (unsigned line = 0; line < io_remote_targets_.size(); ++line) {
        io_remote_targets_[line] &= ~(1U << id);
        if (!io_remote_targets_[line]) io_remote_vector_[line] = 0;
      }
    }
    if (apic.startup_vector) {
      cpu.reset(std::uint16_t(*apic.startup_vector) << 8, 0);
      cpu.a20_enabled = pc_.cpu.a20_enabled;
      apic.started = true;
      apic.startup_vector.reset();
      apic.last_tick = ticks_;
    }
  }
  static constexpr unsigned kSmpQuantum = 32;
  static constexpr unsigned kExtendedSmpQuantum = 4096;

  unsigned smp_quantum(const std::array<unsigned, kMaxProcessors>& runnable, unsigned count) const {
    for (unsigned slot = 0; slot < count; ++slot) {
      const auto& apic = apics_[runnable[slot]];
      const auto& cpu = *apic.cpu;
      if (!apic.started || apic.init_pending || apic.startup_vector || apic.timer_cycles ||
          apic.pending.highest() || cpu.halted || cpu.flag(cosmic_pc::Cpu::IF | cosmic_pc::Cpu::TF) ||
          !cpu.is_long_mode() || !cpu.ether_arm64_block_candidate()) return kSmpQuantum;
    }
    return std::max(kSmpQuantum, batch_limit(kExtendedSmpQuantum));
  }

  void run_secondary(unsigned id, unsigned budget, cosmic_pc::PcMachine::RunClock::time_point deadline,
                     bool preserve_ether_loop_entry) {
    using Clock = cosmic_pc::PcMachine::RunClock;
    const bool timed = deadline != Clock::time_point::max();
    auto& apic = apics_[id];
    auto& cpu = *apic.cpu;
    unsigned dispatches = 0;
    for (unsigned used = 0; used < budget;) {
      // The register workers have joined before shared RAM or devices run.
      if (timed && (dispatches++ & 7U) == 0 && Clock::now() >= deadline) return;
      if (faulted_processor_ || powered_off_ ||
          pc_.guest_restart() != cosmic_pc::PcMachine::GuestRestart::None) return;
      if (preserve_ether_loop_entry && used && budget - used < cosmic_pc::ether_arm64::Block::kMaxInstructions &&
          budget - used < cpu.ether_arm64_register_loop_length()) return;
      apply_startup(apic);
      if (!apic.started) return;
      cpu.a20_enabled = pc_.cpu.a20_enabled;
      advance_apic_timer(apic, ticks_ - apic.last_tick);
      apic.last_tick = ticks_;
      deliver_lapic_irq(apic);
      if (cpu.fault) {
        faulted_processor_ = id;
        return;
      }
      if (cpu.halted) return;
      const auto native_steps = cpu.try_ether_arm64_block(budget - used);
      if (native_steps) {
        used += native_steps;
        continue;
      }
      const auto result = cpu.step();
      if (result == cosmic_pc::StepResult::Faulted) faulted_processor_ = id;
      if (result != cosmic_pc::StepResult::Executed) return;
      ++used;
    }
  }

  unsigned run_parallel_register_loops(const std::array<unsigned, kMaxProcessors>& runnable,
                                       unsigned count, unsigned budget,
                                       cosmic_pc::PcMachine::RunClock::time_point deadline) {
    if (!pc_.boot_acceleration_enabled() || !pc_.cpu.ether_arm64_register_loop_length() ||
        (pc_.cpu.flag(cosmic_pc::Cpu::IF) && slave_.pending()) ||
        (sci_active_ && (io_redir_low_[AcpiC06::sci_irq] & 0x8000U))) return 0;

    auto limit = budget;
    std::array<unsigned, kMaxProcessors> jobs{};
    unsigned job_count = 1;
    for (unsigned i = 1; i < count; ++i) {
      const auto& apic = apics_[runnable[i]];
      const auto& cpu = *apic.cpu;
      if (apic.init_pending || apic.startup_vector ||
          (cpu.flag(cosmic_pc::Cpu::IF) && deliverable_lapic_vector(apic))) return 0;
      if (cpu.apic_global_enabled() && apic.timer_cycles) {
        const auto elapsed = ticks_ - apic.last_tick;
        if (elapsed >= apic.timer_cycles) return 0;
        limit = static_cast<unsigned>(std::min<std::uint64_t>(limit, apic.timer_cycles - elapsed - 1));
      }
      if (cpu.halted) continue;
      if (!cpu.ether_arm64_register_loop_length()) return 0;
      jobs[job_count++] = runnable[i];
    }
    if (job_count < 2) return 0;
    // Bound one independent execution by the earliest device edge and the
    // caller's aggregate budget. The serial quantum does not apply here.
    limit = batch_limit(std::min(limit, budget / job_count) + 1) - 1;
    if (limit < kMinParallelRegisterSteps) return 0;

    std::array<cosmic_pc::Cpu::EtherRegisterWork, kMaxProcessors> work{};
    if (!pc_.prepare_ether_register_loop(limit, work[0])) return 0;
    for (unsigned i = 1; i < job_count; ++i)
      if (!apics_[jobs[i]].cpu->prepare_ether_arm64_register_loop(limit, work[i])) return 0;
    if (!workers_) workers_ = std::make_unique<SmpWorkers>(processor_count_);
    if (workers_->prepare(job_count) != job_count) return 0;

    // All shared validation is finished. The workers only read native code
    // and write their own frames until every job has returned.
    using Clock = cosmic_pc::PcMachine::RunClock;
    const bool profile_bsp = pc_.boot_hotspots() || pc_.boot_timing();
    Clock::duration bsp_elapsed{};
    workers_->run(job_count, [&](unsigned job) {
      const bool measure = job == 0 && profile_bsp;
      const auto start = measure ? Clock::now() : Clock::time_point{};
      work[job].run(deadline);
      if (measure) bsp_elapsed = Clock::now() - start;
    });
    unsigned total = 0, processors = 0, longest = 0;
    for (unsigned i = 1; i < job_count; ++i) {
      const auto completed = apics_[jobs[i]].cpu->retire_ether_arm64_register_loop(work[i]);
      total += completed;
      processors += completed != 0;
      longest = std::max(longest, completed);
    }
    const auto bsp_completed = pc_.retire_ether_register_loop(work[0], bsp_elapsed);
    advance_batch(bsp_completed);
    total += bsp_completed;
    processors += bsp_completed != 0;
    if (processors > 1) {
      ++parallel_register_batches_;
      parallel_register_steps_ += total;
      longest_parallel_register_slice_ = std::max({longest_parallel_register_slice_, longest, bsp_completed});
    }
    return total;
  }

 public:
  explicit PcMotherboardC06(cosmic_pc::PcMachine& machine, BlockDisk* disk=nullptr,
                            AtapiCdrom::Reader optical={}, std::uint64_t optical_bytes=0,
                            const std::filesystem::path& graphics_vram={},
                            BlockDisk* data_disk=nullptr, cosmic_pc::CpuTopology topology={})
      :pc_(machine), processor_count_(topology.logical_processors()) {
    if (!topology.valid()) throw std::invalid_argument("Invalid virtual processor topology");
    prior_topology_ = pc_.cpu.topology();
    prior_apic_id_ = pc_.cpu.apic_id();
    for (unsigned id = 1; id < processor_count_; ++id) {
      auto ap = std::make_unique<cosmic_pc::Cpu>(pc_.cpu.memory);
      ap->configure_topology(topology,id);
      ap->use_windows_x64_profile(pc_.cpu.windows_x64_profile());
      ap->set_local_apic_present(true);
      ap->set_shared_clock(&ticks_);
      ap->enable_page_translation_cache();
      ap->enable_ether_arm64_blocks();
#if defined(MUNDO_X_PC_ETHER_NEXT)
      ap->enable_ether_next();
#endif
      secondary_cpus_[id] = std::move(ap);
    }
    pc_.cpu.configure_topology(topology,0);
    if(data_disk&&!disk)throw std::invalid_argument("ATA data disk requires a primary disk");
    if(disk)ata_=std::make_unique<AtaC07>(*disk,[this](){signal_isa_irq(14);});
    if(data_disk)data_ata_=std::make_unique<AtaC07>(
        *data_disk,[this](){signal_isa_irq(14);},AtaC07::Unit::Slave);
    if(optical)optical_=std::make_unique<AtapiCdrom>(
        std::move(optical),optical_bytes,[this](){signal_isa_irq(15);});
    if(!graphics_vram.empty())gpu_=std::make_unique<GuestGpuC08>(
        pc_,graphics_vram,[this](auto address,auto data){return signal_pci_msi(address,data);});
    // The machine must be booted/mounted first. ACPI goes into reserved BIOS
    // memory and is never presented by E820 as allocatable guest RAM.
    prior_in_=pc_.cpu.in_hook;prior_out_=pc_.cpu.out_hook;
    prior_pio_batch_limit_=pc_.cpu.pio_batch_limit_hook;
    prior_pio_batch_transfer_=pc_.cpu.pio_batch_transfer_hook;
    prior_mmio_claim_=pc_.cpu.mmio_claim_hook;
    prior_mmio_range_claim_=pc_.cpu.mmio_range_claim_hook;
    prior_mmio_read_=pc_.cpu.mmio_read_hook;
    prior_mmio_write_=pc_.cpu.mmio_write_hook;
    prior_mmio_read32_=pc_.cpu.mmio_read32_hook;
    prior_mmio_write32_=pc_.cpu.mmio_write32_hook;
    prior_tick_=pc_.device_step_hook;
    prior_batch_limit_=pc_.device_batch_limit_hook;
    prior_batch_advance_=pc_.device_batch_advance_hook;
    prior_idle_advance_=pc_.device_idle_advance_hook;
    prior_reset_=pc_.device_reset_hook;
    reset_devices();
    prior_pci_reservation_=pc_.pci_mmio_reserved();
    if(gpu_)pc_.reserve_pci_mmio(true);
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
    if(ata_){
      pc_.cpu.pio_batch_limit_hook=[this](std::uint16_t p,unsigned width,bool input,unsigned count){
        if(p==AtaC07::kData&&(ata_cfg_[4]&1U))
          return selected_ata()->pio_batch_limit(width,input,count);
        return 0U;
      };
      pc_.cpu.pio_batch_transfer_hook=[this](std::uint16_t p,unsigned width,bool input,
                                             std::uint8_t* data,unsigned count){
        return p==AtaC07::kData&&(ata_cfg_[4]&1U)&&
               selected_ata()->pio_batch_transfer(width,input,data,count);
      };
    }
    attach_mmio(pc_.cpu, bsp());
    for (unsigned id = 1; id < processor_count_; ++id) {
      auto& ap = *secondary_cpus_[id];
      ap.in_hook = pc_.cpu.in_hook;
      ap.out_hook = pc_.cpu.out_hook;
      ap.pio_batch_limit_hook = pc_.cpu.pio_batch_limit_hook;
      ap.pio_batch_transfer_hook = pc_.cpu.pio_batch_transfer_hook;
      attach_mmio(ap, apics_[id]);
    }
    pc_.device_step_hook=[this](){
      if(prior_tick_)prior_tick_();
      poll();
    };
    pc_.device_batch_limit_hook=[this](unsigned steps){return batch_limit(steps);};
    pc_.device_batch_advance_hook=[this](unsigned steps){advance_batch(steps);};
    pc_.device_idle_advance_hook=[this](unsigned steps){return advance_idle(steps);};
    pc_.device_reset_hook=[this](){
      if(prior_reset_)prior_reset_();
      reset_devices();
    };
    prior_local_apic_=pc_.cpu.local_apic_present();
    pc_.cpu.set_local_apic_present(true);
    if (processor_count_ > 1) prior_clock_ = pc_.cpu.set_shared_clock(&ticks_);
  }
  ~PcMotherboardC06(){
    // Never leave a hook pointing at a destroyed motherboard instance.
    pc_.cpu.set_local_apic_present(prior_local_apic_);
    pc_.cpu.configure_topology(prior_topology_, prior_apic_id_);
    if (processor_count_ > 1) pc_.cpu.set_shared_clock(prior_clock_);
    pc_.reserve_pci_mmio(prior_pci_reservation_);
    pc_.cpu.set_x87_ignne(false);
    pc_.cpu.in_hook=std::move(prior_in_);
    pc_.cpu.out_hook=std::move(prior_out_);
    pc_.cpu.pio_batch_limit_hook=std::move(prior_pio_batch_limit_);
    pc_.cpu.pio_batch_transfer_hook=std::move(prior_pio_batch_transfer_);
    pc_.cpu.mmio_claim_hook=std::move(prior_mmio_claim_);
    pc_.cpu.mmio_range_claim_hook=std::move(prior_mmio_range_claim_);
    pc_.cpu.mmio_read_hook=std::move(prior_mmio_read_);
    pc_.cpu.mmio_write_hook=std::move(prior_mmio_write_);
    pc_.cpu.mmio_read32_hook=std::move(prior_mmio_read32_);
    pc_.cpu.mmio_write32_hook=std::move(prior_mmio_write32_);
    pc_.device_step_hook=std::move(prior_tick_);
    pc_.device_batch_limit_hook=std::move(prior_batch_limit_);
    pc_.device_batch_advance_hook=std::move(prior_batch_advance_);
    pc_.device_idle_advance_hook=std::move(prior_idle_advance_);
    pc_.device_reset_hook=std::move(prior_reset_);
  }
  PcMotherboardC06(const PcMotherboardC06&)=delete;
  PcMotherboardC06& operator=(const PcMotherboardC06&)=delete;
  Evidence evidence()const{
    return {ticks_,irq_delivered_,pit_irqs_,apic_irqs_,true,true,powered_off_,ata_irq_delivered_,sci_irqs_};
  }
  InterruptState interrupt_state()const{
    return {pc_.cpu.apic_global_enabled(),lapic_on(bsp()),bsp().pending.highest(),
            bsp().in_service.highest(),processor_priority(bsp()),timer_count(bsp()),
            ioapic_select_,ioapic_id_};
  }
  bool secondary_can_run() const noexcept {
    for (unsigned id = 1; id < processor_count_; ++id)
      if (needs_service(apics_[id])) return true;
    return false;
  }
  unsigned processors_started() const noexcept {
    return static_cast<unsigned>(std::count_if(apics_.begin(), apics_.begin() + processor_count_,
        [](const LocalApic& apic) { return apic.started; }));
  }
  unsigned worker_count() const noexcept { return workers_ ? workers_->size() : 1; }
  std::uint64_t parallel_register_batches() const noexcept { return parallel_register_batches_; }
  std::uint64_t parallel_register_steps() const noexcept { return parallel_register_steps_; }
  unsigned longest_parallel_register_slice() const noexcept { return longest_parallel_register_slice_; }
  const cosmic_pc::Cpu* faulted_cpu() const noexcept {
    return faulted_processor_ ? apics_[*faulted_processor_].cpu : nullptr;
  }
  std::uint64_t processor_steps(unsigned id) const noexcept {
    return apics_[id].cpu->instructions + retired_before_init_[id];
  }
  std::uint64_t total_processor_steps() const noexcept {
    std::uint64_t steps = 0;
    for (unsigned id = 0; id < processor_count_; ++id) steps += processor_steps(id);
    return steps;
  }
  void run(unsigned budget, cosmic_pc::PcMachine::RunClock::time_point deadline=
           cosmic_pc::PcMachine::RunClock::time_point::max()) {
    using Clock=cosmic_pc::PcMachine::RunClock;
    const bool timed=deadline!=Clock::time_point::max();
    if(!timed)budget=std::min(budget,cosmic_pc::PcMachine::kDefaultRunSteps);
    if (!secondary_can_run()) {
      pc_.run(budget,deadline);
      return;
    }
    // The budget counts work across the whole VM. Small calls keep a rotating
    // position so that every runnable processor gets a turn.
    while (budget && !faulted_processor_ && !pc_.cpu.fault && !powered_off_ &&
           pc_.guest_restart() == cosmic_pc::PcMachine::GuestRestart::None) {
      if(timed&&Clock::now()>=deadline)return;
      std::array<unsigned, kMaxProcessors> runnable{};
      unsigned count = 1;
      for (unsigned id = 1; id < processor_count_; ++id)
        if (needs_service(apics_[id])) runnable[count++] = id;
      if (count == 1) {
        pc_.run(budget,deadline);
        return;
      }
      if (const auto completed = run_parallel_register_loops(runnable, count, budget, deadline)) {
        budget -= completed;
        continue;
      }
      const auto chunk = std::min(budget, count * smp_quantum(runnable, count));
      std::array<unsigned, kMaxProcessors> work{};
      for (unsigned i = 0; i < count; ++i)
        work[(next_cpu_ + i) % count] = chunk / count + (i < chunk % count);
      next_cpu_ = (next_cpu_ + chunk % count) % count;
      const bool preserve_ether_loop_entry = cosmic_pc::Cpu::ether_arm64_host() &&
          pc_.boot_acceleration_enabled() && budget >= count * kMinParallelRegisterSteps;
      if (work[0]) pc_.run(work[0], deadline, preserve_ether_loop_entry);
      if (pc_.cpu.fault || pc_.waiting_key || powered_off_ ||
          pc_.guest_restart() != cosmic_pc::PcMachine::GuestRestart::None) return;
      for (unsigned i = 1; i < count; ++i)
        if (work[i]) run_secondary(runnable[i], work[i], deadline, preserve_ether_loop_entry);
      budget -= chunk;
    }
  }
  // Deterministic single guest-cycle advance; no host sleeper or background
  // worker. PC's canonical run loop calls this once per guest step.
  void advance_one(){poll();}
  void enable_ps2_keyboard(){keyboard_.emplace();}
  bool guest_uses_ps2_scancodes()const noexcept {
    return keyboard_&&keyboard_->guest_uses_scancodes();
  }
  bool inject_ps2_key(unsigned code,bool pressed){
    return keyboard_&&keyboard_->inject(code,pressed);
  }
  AtaC07* attached_ata(){return ata_.get();}
  const GuestGpuC08* attached_gpu() const noexcept {return gpu_.get();}
  void set_gpu_cache_enabled(bool enabled) noexcept {
    if (gpu_) gpu_->set_vram_cache_enabled(enabled);
  }
  const AtaC07* attached_ata()const{return ata_.get();}
  const AtaC07* attached_data_ata()const{return data_ata_.get();}
  const AtapiCdrom* attached_atapi()const{return optical_.get();}
  bool optical_attached()const{return bool(optical_);}
};
} // namespace reverse_windows
