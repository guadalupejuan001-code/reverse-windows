#pragma once
// REVERSE Windows C06: one attached motherboard for the canonical PcMachine.
// PCI configuration / 8259 PIC / PIT channel0 / RTC CMOS / ACPI PM register
// model / a bounded LAPIC and IOAPIC register subset. Not a chipset clone.
#include "c06_acpi.hpp"
#include "c08_guest_gpu.hpp"
#include "c07_ata.hpp"
#include "atapi_cdrom.hpp"
#include "ether_x86_sessions.hpp"
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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace reverse_windows {
class PcMotherboardC06 {
 public:
  using Clock = cosmic_pc::PcMachine::RunClock;
  using ClockSource = std::function<Clock::time_point()>;
  enum class CpuClock : unsigned { Standard = 1, Double = 2, Twentyfold = 20, Experimental4_5THz };
  static constexpr std::uint64_t kExperimentalTscHz = 4'500'000'000'000ULL;
  static_assert(kExperimentalTscHz % cosmic_pc::Cpu::kTscTicksPerCycle == 0);
  enum class ClockStart { Immediate, Guest32Or64 };
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
  struct StartupIpis {
    std::uint64_t init_icr_writes = 0, sipi_icr_writes = 0;
    std::uint64_t init_targets_queued = 0, sipi_targets_queued = 0, sipi_targets_applied = 0;
  };
  struct RtcEvidence {
    unsigned selected_register, register_a, register_b, register_c;
    bool nmi_masked;
    std::uint64_t irq8_assertions, data_writes;
  };
  struct ThreadedSmpStats {
    std::uint64_t dispatches = 0, guest_steps = 0;
    unsigned last_workers = 0, peak_workers = 0;
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
    void advance_without_irq(std::uint64_t clocks){
      if(clocks<counter){
        counter-=static_cast<std::uint32_t>(clocks);
      }else if(mode==0){
        counter=0;
        enabled=false;
      }else{
        counter=reload-static_cast<std::uint32_t>((clocks-counter)%reload);
      }
    }
  };

  cosmic_pc::PcMachine& pc_;
  const unsigned cpu_clock_ratio_;
  const std::uint64_t firmware_cycles_per_second_;
  std::uint64_t tsc_cycles_per_second_;
  unsigned cpu_cycle_phase_ = 0;
  std::uint64_t cpu_cycles_ = 0;
  std::uint64_t firmware_tsc_remainder_ = 0, active_tsc_remainder_ = 0;
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
  std::function<void()> prior_timestamp_read_;
  ClockSource clock_source_;
  Clock::time_point clock_sample_;
  std::uint64_t active_clock_ns_=0,active_clock_ticks_=0,clock_tick_remainder_=0;
  unsigned clock_polls_=0;
  bool active_clock_enabled_=false,active_clock_running_=false,clock_attached_=false;
  bool target_projection_hook_ = false;
  bool timestamp_hook_attached_=false;
  ClockStart clock_start_ = ClockStart::Immediate;
  bool waiting_for_guest_mode_ = false;
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
  static constexpr std::uint8_t kRtcPeriodic=0x40, kRtcIrq=0x80, kRtcSet=0x80;
  static constexpr std::array<std::uint8_t,8> kRtcCalendar{{0,2,4,6,7,8,9,0x32}};
  std::array<std::uint8_t,128> cmos_ram_{};
  std::uint8_t cmos_index_=0,rtc_a_=0x26,rtc_b_=0x02,rtc_c_=0;
  std::time_t rtc_utc_offset_=0;
  std::uint64_t rtc_period_ticks_=0,rtc_next_periodic_tick_=UINT64_MAX;
  std::uint64_t rtc_irq8_assertions_=0,rtc_data_writes_=0;
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
  static constexpr unsigned kMinParallelLoopSteps = 128;
  static constexpr unsigned kMixedParallelLoopSteps = 1024;
  static constexpr auto kParallelLoopAccess = cosmic_pc::Cpu::EtherLoopAccess::ReadOnlyRam;
  std::array<std::unique_ptr<cosmic_pc::Cpu>, kMaxProcessors> secondary_cpus_{};
  std::array<LocalApic, kMaxProcessors> apics_;
  std::array<std::uint64_t, kMaxProcessors> retired_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> ether_steps_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> state_vector_steps_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> interpreter_vector_steps_before_init_{};
  StartupIpis startup_ipis_{};
  unsigned processor_count_ = 1;
  const bool experimental_mixed_smp_;
  bool threaded_smp_enabled_ = false;
  unsigned worker_limit_ = kMaxProcessors;
  unsigned next_cpu_ = 0;
  std::optional<unsigned> faulted_processor_;
  std::shared_ptr<SmpWorkers> workers_;
  std::unique_ptr<EtherX86Sessions> ether_x86_sessions_;
  ThreadedSmpStats threaded_smp_stats_;
  std::uint64_t parallel_register_batches_ = 0, parallel_register_steps_ = 0;
  std::uint64_t parallel_register_continuations_ = 0;
  std::uint64_t parallel_ram_batches_ = 0, parallel_ram_steps_ = 0;
  std::uint64_t parallel_mixed_batches_ = 0;
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
    cmos_index_=rtc_c_=0;rtc_a_=0x26;rtc_b_=0x02;pm_control_=0x0001;powered_off_=false;
    pm_status_=pm_enable_=0;sci_active_=false;
    ferr_was_asserted_=false;pc_.cpu.set_x87_ignne(false);
    cpu_cycles_=ticks_=irq_delivered_=pit_irqs_=apic_irqs_=ata_irq_delivered_=sci_irqs_=0;
    cpu_cycle_phase_ = 0;
    firmware_tsc_remainder_ = active_tsc_remainder_ = 0;
    active_clock_enabled_=active_clock_running_=waiting_for_guest_mode_=false;
    active_clock_ns_=active_clock_ticks_=clock_tick_remainder_=0;
    clock_polls_=0;
    rtc_irq8_assertions_=rtc_data_writes_=0;
    rtc_schedule_periodic();
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
    ether_steps_before_init_.fill(0);
    state_vector_steps_before_init_.fill(0);
    interpreter_vector_steps_before_init_.fill(0);
    startup_ipis_ = {};
    faulted_processor_.reset();
    next_cpu_ = 0;
    threaded_smp_stats_ = {};
    parallel_register_batches_ = parallel_register_steps_ = 0;
    parallel_register_continuations_ = 0;
    parallel_ram_batches_ = parallel_ram_steps_ = 0;
    parallel_mixed_batches_ = 0;
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
  void rtc_schedule_periodic() {
    unsigned rate=rtc_a_&0x0fU;
    // This PC uses a 32.768 kHz crystal. Rates 1 and 2 alias rates 8 and 9.
    if(rate && rate<3)rate+=7;
    rtc_period_ticks_=0;
    rtc_next_periodic_tick_=UINT64_MAX;
    if((rtc_a_&0x70U)!=0x20 || !rate)return;
    const auto hz=32768U>>(rate-1);
    rtc_period_ticks_=(cosmic_pc::PcMachine::kVirtualCyclesPerSecond+hz-1)/hz;
    rtc_next_periodic_tick_=ticks_+rtc_period_ticks_;
  }
  void rtc_update_irq() {
    if(!(rtc_c_&rtc_b_&kRtcPeriodic)){
      rtc_c_&=~kRtcIrq;
      return;
    }
    if(rtc_c_&kRtcIrq)return;
    rtc_c_|=kRtcIrq;
    ++rtc_irq8_assertions_;
    signal_isa_irq(8);
  }
  void rtc_update_periodic() {
    if(!rtc_period_ticks_||ticks_<rtc_next_periodic_tick_)return;
    rtc_next_periodic_tick_+=((ticks_-rtc_next_periodic_tick_)/rtc_period_ticks_+1)*rtc_period_ticks_;
    rtc_c_|=kRtcPeriodic;
    rtc_update_irq();
  }
  static bool rtc_calendar_index(unsigned index) {
    return std::find(kRtcCalendar.begin(),kRtcCalendar.end(),index)!=kRtcCalendar.end();
  }
  std::uint8_t rtc_calendar_byte(unsigned index,const std::tm& current) const {
    if(index==4 && !(rtc_b_&2U)){
      const unsigned hour=current.tm_hour%12 ? current.tm_hour%12 : 12;
      return ((rtc_b_&4U)?std::uint8_t(hour):bcd(hour)) | (current.tm_hour>=12 ? 0x80U : 0);
    }
    unsigned raw=0;
    switch(index){
      case 0x00:raw=current.tm_sec;break;
      case 0x02:raw=current.tm_min;break;
      case 0x04:raw=current.tm_hour;break;
      case 0x06:raw=current.tm_wday+1;break;
      case 0x07:raw=current.tm_mday;break;
      case 0x08:raw=current.tm_mon+1;break;
      case 0x09:raw=(current.tm_year+1900)%100;break;
      case 0x32:raw=(current.tm_year+1900)/100;break;
    }
    return (rtc_b_&4U)?std::uint8_t(raw):bcd(raw);
  }
  std::tm rtc_calendar_now() const {
    const auto now=std::time(nullptr)+rtc_utc_offset_;
    std::tm current{};
#if defined(_WIN32)
    gmtime_s(&current,&now);
#else
    gmtime_r(&now,&current);
#endif
    return current;
  }
  void rtc_latch_calendar() {
    const auto current=rtc_calendar_now();
    for(auto index:kRtcCalendar)cmos_ram_[index]=rtc_calendar_byte(index,current);
  }
  void rtc_apply_calendar() {
    const auto decode=[&](unsigned index,std::uint8_t mask=0xff) {
      const int value=cmos_ram_[index]&mask;
      return (rtc_b_&4U)?value:10*(value>>4)+(value&15);
    };
    std::tm current{};
    current.tm_sec=decode(0,0x7f);
    current.tm_min=decode(2);
    current.tm_hour=decode(4,0x7f);
    if(!(rtc_b_&2U))current.tm_hour=current.tm_hour%12+((cmos_ram_[4]&0x80)?12:0);
    current.tm_mday=decode(7);
    current.tm_mon=decode(8)-1;
    current.tm_year=100*decode(0x32)+decode(9)-1900;
#if defined(_WIN32)
    const auto target=_mkgmtime(&current);
#else
    const auto target=timegm(&current);
#endif
    if(target!=std::time_t(-1))rtc_utc_offset_=target-std::time(nullptr);
  }
  std::uint8_t cmos_read() {
    const unsigned index=cmos_index_&0x7fU;
    switch(index){
      case 0x0a:{
        if((rtc_b_&kRtcSet)||(rtc_a_&0x70U)!=0x20)return rtc_a_;
        using namespace std::chrono;
        const auto now=system_clock::now();
        const bool updating=now-floor<seconds>(now)>=microseconds(999756);
        return rtc_a_|(updating?0x80U:0);
      }
      case 0x0b:return rtc_b_;
      case 0x0c:{
        rtc_update_periodic();
        return std::exchange(rtc_c_,0);
      }
      case 0x0d:return 0x80;
    }
    if((rtc_b_&kRtcSet)||!rtc_calendar_index(index))return cmos_ram_[index];
    return rtc_calendar_byte(index,rtc_calendar_now());
  }
  void cmos_write(std::uint8_t value) {
    ++rtc_data_writes_;
    const unsigned index=cmos_index_&0x7fU;
    switch(index){
      case 0x0a:
        rtc_update_periodic();
        value&=0x7fU;
        if(rtc_a_!=value){
          rtc_a_=value;
          rtc_schedule_periodic();
        }
        return;
      case 0x0b:{
        rtc_update_periodic();
        const bool was_set=(rtc_b_&kRtcSet)!=0;
        if(was_set && !(value&kRtcSet))rtc_apply_calendar();
        rtc_b_=value;
        if(!was_set && (rtc_b_&kRtcSet))rtc_latch_calendar();
        rtc_update_irq();
        return;
      }
      case 0x0c:case 0x0d:return; // Status-only registers.
    }
    const bool running_calendar=rtc_calendar_index(index)&&!(rtc_b_&kRtcSet);
    if(running_calendar)rtc_latch_calendar();
    cmos_ram_[index]=index==0 ? value&0x7fU : value;
    if(running_calendar)rtc_apply_calendar();
  }
  std::optional<std::uint32_t> io_read(std::uint16_t p,unsigned width){
    if(active_clock_running_&&timer_port(p))sync_active_clock();
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
      case 0x71:return cmos_read();
      default:return std::nullopt;
    }
  }
  bool io_write(std::uint16_t p,unsigned width,std::uint32_t value){
    if(active_clock_running_&&timer_port(p))sync_active_clock();
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
      case 0x71:cmos_write(b);return true;
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
      if (mode == 5) ++startup_ipis_.init_icr_writes;
      else ++startup_ipis_.sipi_icr_writes;
      // INIT level deassert has no effect on xAPIC startup state. Queue INIT
      // and SIPI so only the target's executor can reset its CPU or JIT cache.
      if (mode == 5 && (value & 0xc000U) == 0x8000U) return true;
      for (unsigned id = 1; id < processor_count_; ++id) {
        auto& target = apics_[id];
        if (!selected(id)) continue;
        if (mode == 5) {
          target.init_pending = true;
          target.startup_vector.reset();
          ++startup_ipis_.init_targets_queued;
        } else if ((!target.started || target.init_pending) && !target.startup_vector) {
          target.startup_vector = std::uint8_t(vector);
          ++startup_ipis_.sipi_targets_queued;
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
  Clock::time_point active_now() const {
    return clock_source_ ? clock_source_() : Clock::now();
  }
  // Accumulate units * rate / units_per_second. Splitting both factors keeps
  // the fractional product below 10^18 even for long host time intervals.
  static std::uint64_t scaled_cycles(std::uint64_t units, std::uint64_t rate,
                                     std::uint64_t units_per_second, std::uint64_t& remainder) {
    const auto part = units % units_per_second;
    const auto fraction = part * (rate % units_per_second) + remainder;
    remainder = fraction % units_per_second;
    return (units / units_per_second) * rate + part * (rate / units_per_second) + fraction / units_per_second;
  }
  void advance_firmware_tsc(unsigned steps) {
    if (tsc_cycles_per_second_ == firmware_cycles_per_second_) cpu_cycles_ += steps;
    else cpu_cycles_ += scaled_cycles(steps, tsc_cycles_per_second_, firmware_cycles_per_second_,
                                      firmware_tsc_remainder_);
  }
  void sync_active_clock() {
    if(!active_clock_running_)return;
    const auto now=active_now();
    if(now<=clock_sample_)return;
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now-clock_sample_).count();
    if(!ns)return;
    clock_sample_=now;
    active_clock_ns_+=ns;
    constexpr std::uint64_t second=1000000000U;
    constexpr std::uint64_t frequency=cosmic_pc::PcMachine::kVirtualCyclesPerSecond;
    cpu_cycles_ += scaled_cycles(ns, tsc_cycles_per_second_, second, active_tsc_remainder_);
    const auto fraction=std::uint64_t(ns)%second*frequency+clock_tick_remainder_;
    clock_tick_remainder_=fraction%second;
    const auto elapsed=std::uint64_t(ns)/second*frequency+fraction/second;
    if(!elapsed)return;

    // Virtual interrupts latch while the guest is between instruction
    // boundaries. Count PIT expirations, but queue a single pending edge.
    const auto previous=ticks_;
    ticks_+=elapsed;
    active_clock_ticks_+=elapsed;
    const auto pit_elapsed=pit_phase_+elapsed;
    const auto clocks=pit_elapsed/kPitClockDivider;
    pit_phase_=pit_elapsed%kPitClockDivider;
    if(pit_.enabled){
      if(clocks>=pit_.counter){
        pit_irqs_+=pit_.mode==0 ? 1 : 1+(clocks-pit_.counter)/pit_.reload;
        signal_isa_irq(0);
      }
      pit_.advance_without_irq(clocks);
    }
    if((speaker_control_&1)&&pit2_.enabled)pit2_.advance_without_irq(clocks);
    if(previous/AcpiC06::pm_timer_half_period!=ticks_/AcpiC06::pm_timer_half_period){
      pm_status_|=1U;
      update_sci();
    }
    if((rtc_b_&kRtcPeriodic)&&!(rtc_c_&kRtcIrq))rtc_update_periodic();
    advance_apic_timer(bsp(),elapsed);
  }
  void attach_cpu_clock() {
    if (!clock_attached_) {
      prior_clock_ = pc_.cpu.set_shared_clock(&cpu_cycles_);
      cpu_cycles_ = prior_clock_ ? *prior_clock_ : pc_.cpu.clock_cycles();
      clock_attached_ = true;
    }
  }
  void activate_clock() {
    attach_cpu_clock();
    active_tsc_remainder_ = firmware_tsc_remainder_ * 1'000'000'000ULL / firmware_cycles_per_second_;
    active_clock_enabled_ = true;
  }
  void start_clock_sample() {
    clock_sample_ = active_now();
    active_clock_running_ = true;
  }
  void observe_guest_clock_mode(const cosmic_pc::Cpu& cpu) {
    if (!waiting_for_guest_mode_ || cpu.is_real_mode()) return;
    const std::string_view mode = cpu.mode_name();
    if (mode != "protected32" && mode != "compat32" && mode != "long64") return;
    waiting_for_guest_mode_ = false;
    activate_clock();
    start_clock_sample();
  }
  struct ActiveClockRun {
    PcMotherboardC06& board;
    explicit ActiveClockRun(PcMotherboardC06& target):board(target){
      if (board.active_clock_enabled_) board.start_clock_sample();
      else board.waiting_for_guest_mode_ = board.clock_start_ == ClockStart::Guest32Or64;
    }
    ~ActiveClockRun(){
      board.sync_active_clock();
      board.active_clock_running_=false;
      board.waiting_for_guest_mode_ = false;
    }
  };
  void update_secondary_timer(LocalApic& apic) {
    advance_apic_timer(apic,ticks_-apic.last_tick);
    apic.last_tick=ticks_;
  }
  void sync_apic_clock(LocalApic& apic) {
    if(!active_clock_running_)return;
    sync_active_clock();
    if(&apic!=&bsp())update_secondary_timer(apic);
  }
  static bool timer_port(std::uint16_t port) {
    return (port>=0x40&&port<=0x43)||port==0x61||(port>=0x70&&port<=0x71)||
        (port>=AcpiC06::pm_event_port&&port<AcpiC06::pm_timer_port+4);
  }
  void poll(){
    bool device_tick = false;
    if(active_clock_running_){
      if((++clock_polls_&15U)==0)sync_active_clock();
    }else{
      advance_firmware_tsc(1);
      device_tick = ++cpu_cycle_phase_ == cpu_clock_ratio_;
      if (device_tick) {
        cpu_cycle_phase_ = 0;
        ++ticks_;
        if ((rtc_b_ & kRtcPeriodic) && !(rtc_c_ & kRtcIrq) && ticks_ >= rtc_next_periodic_tick_)
          rtc_update_periodic();
      }
    }
    auto& cpu=pc_.cpu;
    const bool ferr=cpu.x87_ferr_asserted();
    if(ferr&&!ferr_was_asserted_)signal_isa_irq(13);
    if(!ferr&&ferr_was_asserted_)cpu.set_x87_ignne(false);
    ferr_was_asserted_=ferr;
    if(device_tick&&++pit_phase_==kPitClockDivider){
      pit_phase_=0;
      if(pit_.enabled&&pit_.tick()){
        ++pit_irqs_;
        signal_isa_irq(0);
      }
      if((speaker_control_&1)&&pit2_.enabled)pit2_.tick();
    }
    if(device_tick&&(ticks_&(AcpiC06::pm_timer_half_period-1))==0){
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
    if(device_tick)advance_apic_timer(bsp(),1);
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
    std::uint64_t edge = pm_timer_edge_in();
    if (pit_.enabled) edge = std::min<std::uint64_t>(edge, pit_.counter*kPitClockDivider-pit_phase_);
    if ((rtc_b_ & kRtcPeriodic) && !(rtc_c_ & kRtcIrq))
      edge = std::min(edge, rtc_next_periodic_tick_-ticks_);
    if (bsp().timer_cycles && !(bsp().lvt[kTimer] & kLvtMasked) && lapic_on(bsp()))
      edge = std::min<std::uint64_t>(edge, bsp().timer_cycles);
    if (!edge) return std::min(1U, steps);
    if (!active_clock_running_) edge = edge * cpu_clock_ratio_ - cpu_cycle_phase_;
    return static_cast<unsigned>(std::min<std::uint64_t>(steps, edge));
  }
  void advance_batch(unsigned extra_steps) {
    if(active_clock_running_){
      if(extra_steps>=128)sync_active_clock();
      return;
    }
    // Carry the partial device tick across batches and individual instructions.
    const auto cycles = std::uint64_t(cpu_cycle_phase_) + extra_steps;
    const auto device_ticks = cycles / cpu_clock_ratio_;
    cpu_cycle_phase_ = static_cast<unsigned>(cycles % cpu_clock_ratio_);
    advance_firmware_tsc(extra_steps);
    ticks_ += device_ticks;
    const auto pit_elapsed=std::uint64_t(pit_phase_)+device_ticks;
    if(pit_.enabled)pit_.counter-=pit_elapsed/kPitClockDivider;
    // Channel 2 only drives the speaker output. It can cross expirations
    // within a batch while leaving the guest-visible counter exact.
    if((speaker_control_&1)&&pit2_.enabled)pit2_.advance_without_irq(pit_elapsed/kPitClockDivider);
    pit_phase_=pit_elapsed%kPitClockDivider;
    advance_apic_timer(bsp(),device_ticks);
  }
  unsigned advance_idle(unsigned max_extra) {
    if(!pc_.cpu.halted||!pc_.cpu.flag(cosmic_pc::Cpu::IF))return 0;
    sync_active_clock();
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
      if (mmio_claim(address)) {
        sync_apic_clock(apic);
        return mmio_byte(apic, address);
      }
      if (gpu_ && gpu_->claims(address)) return std::optional<std::uint8_t>(gpu_->read(address));
      return prior_mmio_read_ ? prior_mmio_read_(address) : std::nullopt;
    };
    cpu.mmio_write_hook = [this, &apic](std::uint64_t address, std::uint8_t value) {
      if (mmio_claim(address)) {
        sync_apic_clock(apic);
        return mmio_write(apic, address, value);
      }
      if (gpu_ && gpu_->claims(address)) return gpu_->write(address, value);
      return prior_mmio_write_ && prior_mmio_write_(address, value);
    };
    cpu.mmio_read32_hook = [this, &apic](std::uint64_t address) {
      if (mmio_claim(address)) {
        sync_apic_clock(apic);
        return mmio_register(apic, address);
      }
      if (gpu_ && gpu_->claims(address)) return std::optional<std::uint32_t>(gpu_->read32(address));
      return prior_mmio_read32_ ? prior_mmio_read32_(address) : std::nullopt;
    };
    cpu.mmio_write32_hook = [this, &apic](std::uint64_t address, std::uint32_t value)
        -> std::optional<bool> {
      if (mmio_claim(address)) {
        sync_apic_clock(apic);
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
      ether_steps_before_init_[id] += cpu.ether_arm64_stats().steps;
      const auto& interpreter = cpu.state_vector_interpreter_stats();
      state_vector_steps_before_init_[id] += cpu.ether_arm64_stats().state_vector_steps +
                                              interpreter.projected_steps;
      interpreter_vector_steps_before_init_[id] += interpreter.steps;
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
      ++startup_ipis_.sipi_targets_applied;
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

  void run_secondary(unsigned id, unsigned budget, Clock::time_point deadline,
                     bool preserve_ether_loop_entry, std::unique_lock<std::mutex>* bus = nullptr) {
    using Clock = cosmic_pc::PcMachine::RunClock;
    const bool timed = deadline != Clock::time_point::max();
    auto& apic = apics_[id];
    auto& cpu = *apic.cpu;
    unsigned dispatches = 0;
    for (unsigned used = 0; used < budget;) {
      // The workers have joined before shared RAM or devices can change.
      if ((dispatches++ & 7U) == 0) {
        if (timed && Clock::now() >= deadline) return;
        sync_active_clock();
      }
      if (faulted_processor_ || powered_off_ ||
          pc_.guest_restart() != cosmic_pc::PcMachine::GuestRestart::None) return;
      if (preserve_ether_loop_entry && used && budget - used < cosmic_pc::ether_arm64::Block::kMaxInstructions &&
          budget - used < cpu.ether_arm64_loop_length(kParallelLoopAccess)) return;
      apply_startup(apic);
      if (!apic.started) return;
      cpu.a20_enabled = pc_.cpu.a20_enabled;
      update_secondary_timer(apic);
      deliver_lapic_irq(apic);
      if (cpu.fault) {
        faulted_processor_ = id;
        return;
      }
      if (cpu.halted) return;
      observe_guest_clock_mode(cpu);
      const auto remaining = budget - used;
      const auto native_steps = cpu.try_ether_arm64_block(remaining, bus);
      if (native_steps) {
        used += native_steps;
        continue;
      }
      const bool interrupts_enabled = cpu.flag(cosmic_pc::Cpu::IF);
      if (pc_.boot_acceleration_enabled() && cpu.state_vector_batch_candidate() &&
          !(interrupts_enabled && apic.pending.highest())) {
        auto limit = batch_limit(remaining);
        if (interrupts_enabled && apic.timer_cycles && !(apic.lvt[kTimer] & kLvtMasked) && lapic_on(apic))
          limit = unsigned(std::min<std::uint64_t>(limit, apic.timer_cycles));
        if (const auto vector_steps = cpu.try_state_vector_batch(limit)) {
          used += vector_steps;
          continue;
        }
      }
      const auto result = cpu.step();
      if (result == cosmic_pc::StepResult::Faulted) faulted_processor_ = id;
      if (result != cosmic_pc::StepResult::Executed) return;
      ++used;
    }
  }

  // Keep the first job on the calling lane. Rotate the remaining jobs when
  // more vCPUs are ready than can start in the first wave.
  static unsigned rotate_smp_job(unsigned position, unsigned count, unsigned rotation) noexcept {
    return rotation && position ? 1 + (position - 1 + rotation) % (count - 1) : position;
  }

  unsigned run_parallel_ether_loops(const std::array<unsigned, kMaxProcessors>& runnable,
                                    unsigned count, unsigned budget,
                                    cosmic_pc::PcMachine::RunClock::time_point deadline) {
    if (budget < 2 * kMinParallelLoopSteps || !cosmic_pc::Cpu::ether_arm64_host() ||
        !pc_.boot_acceleration_enabled() || !pc_.guest_execution_available() ||
        (pc_.cpu.flag(cosmic_pc::Cpu::IF) && slave_.pending()) ||
        (sci_active_ && (io_redir_low_[AcpiC06::sci_irq] & 0x8000U))) return 0;

    auto limit = budget;
    std::array<unsigned, kMaxProcessors> candidates{};
    unsigned candidate_count = 0, active_count = 1;
    const auto consider = [&](unsigned id) {
      if (!apics_[id].cpu->ether_arm64_loop_length(kParallelLoopAccess)) return false;
      candidates[candidate_count++] = id;
      return true;
    };
    if (!consider(0) && !experimental_mixed_smp_) return 0;
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
      ++active_count;
      if (!consider(runnable[i]) && !experimental_mixed_smp_) return 0;
    }
    if (candidate_count < 2) return 0;
    // Bound one independent execution by the earliest device edge and the
    // aggregate budget, including the processors that need serial service.
    limit = batch_limit(std::min(limit, budget / active_count) + 1) - 1;
    if (limit < kMinParallelLoopSteps) return 0;

    std::array<unsigned, kMaxProcessors> jobs{};
    std::array<cosmic_pc::Cpu::EtherLoopWork, kMaxProcessors> work{};
    const auto prepare = [&](bool ram_only) {
      unsigned ready = 0;
      for (unsigned i = 0; i < candidate_count; ++i) {
        const auto id = candidates[i];
        const auto accepted = id
            ? apics_[id].cpu->prepare_ether_arm64_loop(limit, work[ready], kParallelLoopAccess)
            : pc_.prepare_ether_loop(limit, work[ready], kParallelLoopAccess);
        if (accepted && (!ram_only || work[ready].reads_ram())) jobs[ready++] = id;
        else if (!experimental_mixed_smp_) return 0U;
      }
      return ready;
    };
    const auto prepare_mixed = [&] {
      // Projected register rounds can be cheaper than starting workers.
      // Give distinct RAM readers a full grant before paying for a mixed batch.
      if (!experimental_mixed_smp_ || limit < kMixedParallelLoopSteps) return 0U;
      limit = kMixedParallelLoopSteps;
      return prepare(true);
    };
    auto job_count = candidate_count == active_count ? prepare(false) : 0U;
    if (job_count < active_count) job_count = prepare_mixed();
    if (job_count < 2) return 0;
    if (!workers_) workers_ = std::make_shared<SmpWorkers>(host_workers_capacity());

    // A worker publishes register state to its own vCPU and can use the
    // rest of its grant on cached register blocks. Shared writes and device
    // callbacks run at the boundary after all workers return.
    using Clock = cosmic_pc::PcMachine::RunClock;
    const bool profile_bsp = pc_.boot_hotspots() || pc_.boot_timing();
    struct Progress {
      unsigned steps = 0, ram_steps = 0, continuations = 0;
    };
    std::array<Progress, kMaxProcessors> progress{};
    const auto next_job = next_cpu_ % (job_count - 1);
    const auto lanes = workers_->try_run(std::min(job_count, host_workers_capacity()), [&](unsigned lane, unsigned count) {
      const auto rotation = count < job_count ? next_job : 0U;
      for (unsigned position = lane; position < job_count; position += count) {
        const auto job = rotate_smp_job(position, job_count, rotation);
        const auto id = jobs[job];
        auto& cpu = *apics_[id].cpu;
        auto& block = work[job];
        auto& result = progress[job];
        const bool measure = id == 0 && profile_bsp;
        bool continued = false;
        do {
          const auto start = measure ? Clock::now() : Clock::time_point{};
          block.run(deadline);
          const auto elapsed = measure ? Clock::now() - start : Clock::duration{};
          const auto completed = id == 0 ? pc_.retire_ether_loop(block, elapsed, true)
                                         : cpu.retire_ether_arm64_loop(block);
          if (!completed) break;
          result.steps += completed;
          if (block.reads_ram()) result.ram_steps += completed;
          result.continuations += continued;
          continued = result.steps < limit &&
                      cpu.prepare_parallel_ether_arm64_register_block(limit - result.steps, block);
        } while (continued);
      }
    });
    if (!lanes) return 0;
    const auto rotation = lanes < job_count ? next_job : 0U;
    if (lanes < job_count) next_cpu_ = (rotation + lanes - 1) % (job_count - 1);
    unsigned total = 0, processors = 0, longest = 0, ram_steps = 0, bsp_steps = 0;
    std::array<bool, kMaxProcessors> active_lanes{};
    std::array<bool, kMaxProcessors> serviced{};
    for (unsigned position = 0; position < job_count; ++position) {
      const auto job = rotate_smp_job(position, job_count, rotation);
      const auto& result = progress[job];
      serviced[jobs[job]] = result.steps != 0;
      active_lanes[position % lanes] |= result.steps != 0;
      if (jobs[job] == 0) bsp_steps = result.steps;
      total += result.steps;
      processors += result.steps != 0;
      longest = std::max(longest, result.steps);
      ram_steps += result.ram_steps;
      parallel_register_continuations_ += result.continuations;
    }
    if (bsp_steps) {
      pc_.finish_parallel_ether_work();
      advance_batch(bsp_steps);
    }
    if (threaded_smp_enabled_)
      record_threaded_dispatch(std::count(active_lanes.begin(), active_lanes.begin() + lanes, true), total);
    if (processors > 1) {
      parallel_mixed_batches_ += experimental_mixed_smp_ && processors < active_count;
      if (ram_steps) {
        ++parallel_ram_batches_;
        parallel_ram_steps_ += ram_steps;
      } else {
        ++parallel_register_batches_;
        parallel_register_steps_ += total;
        longest_parallel_register_slice_ = std::max(longest_parallel_register_slice_, longest);
      }
    }

    // Only now can the other processors execute memory writes and device I/O.
    if (!experimental_mixed_smp_ || !total || processors == active_count) return total;
    const auto serial_limit = std::min(limit, kMixedParallelLoopSteps);
    for (unsigned i = 0; i < count && !faulted_processor_ && pc_.guest_execution_available(); ++i) {
      const auto id = runnable[i];
      if (serviced[id] || (id && apics_[id].cpu->halted)) continue;
      if (id) run_secondary(id, serial_limit, deadline, true);
      else run_bsp(serial_limit, deadline, true);
      total += serial_limit;
    }
    return total;
  }

  void run_bsp(unsigned budget, cosmic_pc::PcMachine::RunClock::time_point deadline,
               bool preserve_ether_loop_entry = false) {
    const cosmic_pc::MemoryFabric::ExclusiveRamReads exclusive(pc_.cpu.memory);
    pc_.run(budget, deadline, preserve_ether_loop_entry);
  }

  void record_threaded_dispatch(unsigned workers, std::uint64_t steps) noexcept {
    ++threaded_smp_stats_.dispatches;
    threaded_smp_stats_.guest_steps += steps;
    threaded_smp_stats_.last_workers = workers;
    threaded_smp_stats_.peak_workers = std::max(threaded_smp_stats_.peak_workers, workers);
  }

  unsigned run_threaded_smp(const std::array<unsigned, kMaxProcessors>& runnable, unsigned count,
                            unsigned budget, Clock::time_point deadline) {
    if (!threaded_smp_enabled_ || !pc_.guest_execution_available()) return 0;

    sync_active_clock();
    std::array<unsigned, kMaxProcessors> ready{};
    unsigned jobs = 1;
    for (unsigned i = 1; i < count; ++i) {
      const auto id = runnable[i];
      auto& apic = apics_[id];
      if (apic.cpu->halted && !apic.init_pending && !apic.startup_vector) {
        update_secondary_timer(apic);
        if (!deliverable_lapic_vector(apic)) continue;
      }
      ready[jobs++] = id;
    }
    constexpr unsigned bus_steps = 64, timed_grant = 512;
    const auto quantum = std::min(kExtendedSmpQuantum,
        (active_clock_running_ ? timed_grant : kSmpQuantum) * cpu_clock_ratio_);
    const auto grant = std::min(budget / jobs, quantum);
    if (jobs < 2 || grant < kSmpQuantum) return 0;
    const bool preserve_loop_entries = cosmic_pc::Cpu::ether_arm64_host() &&
        pc_.boot_acceleration_enabled() && budget >= jobs * kMinParallelLoopSteps;

    if (!workers_) workers_ = std::make_shared<SmpWorkers>(host_workers_capacity());

    // Each CPU stays on its worker for the dispatch. The bus protects RAM,
    // devices and IRQ delivery. Only native register work can drop the lock,
    // and it validates its code again before committing the private frame.
    const cosmic_pc::MemoryFabric::ExclusiveRamReads exclusive(pc_.cpu.memory);
    std::mutex bus;
    std::array<std::uint64_t, kMaxProcessors> progress{};
    const auto next_job = next_cpu_ % (jobs - 1);
    const auto lanes = workers_->try_run(std::min(jobs, host_workers_capacity()), [&](unsigned worker, unsigned workers) {
      const auto rotation = workers < jobs ? next_job : 0U;
      for (unsigned position = worker; position < jobs; position += workers) {
        const auto id = ready[rotate_smp_job(position, jobs, rotation)];
        for (unsigned used = 0; used < grant;) {
          std::unique_lock<std::mutex> lock(bus);
          if (faulted_processor_ || !pc_.guest_execution_available() ||
              (deadline != Clock::time_point::max() && Clock::now() >= deadline)) return;
          const auto before = processor_steps(id);
          const auto steps = std::min(bus_steps, grant - used);
          // The next dispatch can admit a complete cached loop when a
          // short remainder would otherwise leave its vCPU inside it.
          const bool preserve = preserve_loop_entries && used + steps == grant;
          if (id) run_secondary(id, steps, deadline, preserve, &lock);
          else pc_.run(steps, deadline, preserve, &lock);
          const auto retired = processor_steps(id) - before;
          progress[worker] += retired;
          if (retired < steps) break;
          used += steps;
        }
      }
    });
    if (!lanes) return 0;
    if (lanes < jobs) next_cpu_ = (next_job + lanes - 1) % (jobs - 1);
    std::uint64_t retired = 0;
    unsigned active = 0;
    for (const auto steps : progress) {
      retired += steps;
      active += steps != 0;
    }
    record_threaded_dispatch(active, retired);
    const auto scheduled = jobs * grant;
    return preserve_loop_entries && retired ? std::min<std::uint64_t>(scheduled, retired) : scheduled;
  }

 public:
  explicit PcMotherboardC06(cosmic_pc::PcMachine& machine, BlockDisk* disk=nullptr,
                            AtapiCdrom::Reader optical={}, std::uint64_t optical_bytes=0,
                            const std::filesystem::path& graphics_vram={},
                            BlockDisk* data_disk=nullptr, cosmic_pc::CpuTopology topology={},
                            bool experimental_mixed_smp=false, std::shared_ptr<SmpWorkers> workers={},
                            CpuClock cpu_clock=CpuClock::Standard)
      :pc_(machine), cpu_clock_ratio_(static_cast<unsigned>(
           cpu_clock == CpuClock::Experimental4_5THz ? CpuClock::Twentyfold : cpu_clock)),
       firmware_cycles_per_second_(std::uint64_t(cosmic_pc::PcMachine::kVirtualCyclesPerSecond) * cpu_clock_ratio_),
       tsc_cycles_per_second_(cpu_clock == CpuClock::Experimental4_5THz
           ? kExperimentalTscHz / cosmic_pc::Cpu::kTscTicksPerCycle : firmware_cycles_per_second_),
       processor_count_(topology.logical_processors()),
       experimental_mixed_smp_(experimental_mixed_smp), workers_(std::move(workers)) {
    if (!topology.valid()) throw std::invalid_argument("Invalid virtual processor topology");
    if (cpu_clock != CpuClock::Standard && cpu_clock != CpuClock::Double &&
        cpu_clock != CpuClock::Twentyfold && cpu_clock != CpuClock::Experimental4_5THz)
      throw std::invalid_argument("Invalid virtual CPU clock ratio");
    prior_topology_ = pc_.cpu.topology();
    prior_apic_id_ = pc_.cpu.apic_id();
    for (unsigned id = 1; id < processor_count_; ++id) {
      auto ap = std::make_unique<cosmic_pc::Cpu>(pc_.cpu.memory);
      ap->configure_topology(topology,id);
      ap->use_windows_x64_profile(pc_.cpu.windows_x64_profile());
      ap->set_local_apic_present(true);
      ap->set_shared_clock(&cpu_cycles_);
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
        pc_,graphics_vram,[this](auto address,auto data){return signal_pci_msi(address,data);},workers_);
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
      if (target_projection_hook_ && pc_.cpu.halted && !pc_.cpu.flag(cosmic_pc::Cpu::IF) &&
          !secondary_can_run()) pc_.yield_after_instruction();
      // A guest can enter and leave protected mode inside a single Android slice.
      observe_guest_clock_mode(pc_.cpu);
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
    if (processor_count_ > 1 || cpu_clock_ratio_ != 1) {
      prior_clock_ = pc_.cpu.set_shared_clock(&cpu_cycles_);
      clock_attached_=true;
    }
  }
  ~PcMotherboardC06(){
    // Never leave a hook pointing at a destroyed motherboard instance.
    pc_.cpu.set_local_apic_present(prior_local_apic_);
    pc_.cpu.configure_topology(prior_topology_, prior_apic_id_);
    if (clock_attached_) pc_.cpu.set_shared_clock(prior_clock_);
    if (timestamp_hook_attached_) pc_.cpu.before_timestamp_read_hook=std::move(prior_timestamp_read_);
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
    if (target_projection_hook_) pc_.register_projection_limit_hook = {};
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
  StartupIpis startup_ipis() const noexcept { return startup_ipis_; }
  RtcEvidence rtc_evidence() const noexcept {
    return {cmos_index_&0x7fU,rtc_a_,rtc_b_,rtc_c_,bool(cmos_index_&0x80U),
            rtc_irq8_assertions_,rtc_data_writes_};
  }
  unsigned worker_count() const noexcept { return workers_ ? workers_->size() : 1; }
  unsigned host_workers_capacity() const noexcept { return std::min(worker_limit_, processor_count_); }
  void enable_threaded_smp(unsigned worker_limit = kMaxProcessors) noexcept {
    if (!workers_ || workers_->size() == 1) worker_limit_ = std::clamp(worker_limit, 1U, kMaxProcessors);
    threaded_smp_enabled_ = true;
  }
  bool threaded_smp_enabled() const noexcept { return threaded_smp_enabled_; }
  const ThreadedSmpStats& threaded_smp_stats() const noexcept { return threaded_smp_stats_; }
  void configure_ether_x86_sessions(unsigned workers, unsigned max_sessions_per_vcpu) {
    if (!workers || !max_sessions_per_vcpu || ether_x86_sessions_) return;
    const auto slots = std::min<std::uint64_t>(workers, std::uint64_t(processor_count_) * max_sessions_per_vcpu);
    ether_x86_sessions_ = std::make_unique<EtherX86Sessions>(
        processor_count_, static_cast<unsigned>(slots), max_sessions_per_vcpu);
  }
  std::optional<EtherX86Sessions::Ticket> submit_ether_x86_session(unsigned vcpu, EtherX86Sessions::Job job) {
    if (!ether_x86_sessions_) return std::nullopt;
    return ether_x86_sessions_->submit(vcpu, std::move(job));
  }
  EtherX86Sessions::Stats ether_x86_session_stats(unsigned vcpu) const {
    return ether_x86_sessions_ ? ether_x86_sessions_->stats(vcpu) : EtherX86Sessions::Stats{};
  }
  unsigned ether_x86_session_workers() const noexcept {
    return ether_x86_sessions_ ? ether_x86_sessions_->worker_count() : 0;
  }
  std::uint64_t parallel_register_batches() const noexcept { return parallel_register_batches_; }
  std::uint64_t parallel_register_steps() const noexcept { return parallel_register_steps_; }
  std::uint64_t parallel_register_continuations() const noexcept { return parallel_register_continuations_; }
  std::uint64_t parallel_ram_batches() const noexcept { return parallel_ram_batches_; }
  std::uint64_t parallel_ram_steps() const noexcept { return parallel_ram_steps_; }
  std::uint64_t parallel_mixed_batches() const noexcept { return parallel_mixed_batches_; }
  unsigned longest_parallel_register_slice() const noexcept { return longest_parallel_register_slice_; }
  const cosmic_pc::Cpu* faulted_cpu() const noexcept {
    return faulted_processor_ ? apics_[*faulted_processor_].cpu : nullptr;
  }
  void enable_active_clock(ClockSource source={}, ClockStart start=ClockStart::Immediate) {
    if(active_clock_enabled_)return;
    if(!timestamp_hook_attached_){
      prior_timestamp_read_=std::move(pc_.cpu.before_timestamp_read_hook);
      pc_.cpu.before_timestamp_read_hook=[this](){
        if(prior_timestamp_read_)prior_timestamp_read_();
        sync_active_clock();
      };
      for(unsigned id=1;id<processor_count_;++id)
        apics_[id].cpu->before_timestamp_read_hook=[this](){sync_active_clock();};
      timestamp_hook_attached_=true;
    }
    clock_source_=std::move(source);
    clock_start_ = start;
    if (start == ClockStart::Immediate) activate_clock();
  }
  bool active_clock_enabled() const noexcept {return active_clock_enabled_;}
  void enable_execution_target() {
    if (pc_.register_projection_limit_hook || prior_tick_ || prior_batch_limit_ || prior_batch_advance_) return;
    pc_.register_projection_limit_hook = [this](unsigned requested) {
      if (!active_clock_running_ || powered_off_ || (keyboard_ && keyboard_->needs_irq()) ||
          pc_.cpu.x87_ferr_asserted() != ferr_was_asserted_) return 0U;
      // The proven loop has neither I/O nor RAM operands. With IF clear
      // device time can be sampled after applying its atomic register state.
      return pc_.cpu.flag(cosmic_pc::Cpu::IF) ? batch_limit(requested) : requested;
    };
    target_projection_hook_ = true;
  }
  // Android serializes this with run(). Preserve the counter, including per-vCPU
  // IA32_TSC offsets. Only subsequent elapsed time accrues at the chosen rate.
  void set_experimental_tsc(bool enabled) {
    sync_active_clock();
    if (enabled) attach_cpu_clock();
    tsc_cycles_per_second_ = enabled ? kExperimentalTscHz / cosmic_pc::Cpu::kTscTicksPerCycle
                                     : firmware_cycles_per_second_;
  }
  std::uint64_t guest_tsc_hz() const noexcept {
    return tsc_cycles_per_second_ * cosmic_pc::Cpu::kTscTicksPerCycle;
  }
  std::uint64_t active_clock_elapsed_ns() const noexcept {return active_clock_ns_;}
  std::uint64_t active_clock_ticks() const noexcept {return active_clock_ticks_;}
  std::uint64_t processor_steps(unsigned id) const noexcept {
    return apics_[id].cpu->instructions + retired_before_init_[id];
  }
  std::uint64_t processor_ether_arm64_steps(unsigned id) const noexcept {
    return apics_[id].cpu->ether_arm64_stats().steps + ether_steps_before_init_[id];
  }
  std::uint64_t processor_state_vector_steps(unsigned id) const noexcept {
    const auto& cpu = *apics_[id].cpu;
    return cpu.ether_arm64_stats().state_vector_steps + cpu.state_vector_interpreter_stats().projected_steps +
        state_vector_steps_before_init_[id];
  }
  std::uint64_t processor_state_vector_interpreter_steps(unsigned id) const noexcept {
    return apics_[id].cpu->state_vector_interpreter_stats().steps + interpreter_vector_steps_before_init_[id];
  }
  struct CpuExecutionTotals {
    std::uint64_t guest_steps = 0, ether_steps = 0, projected_steps = 0;
    std::uint64_t state_vector_interpreter_steps = 0;
    unsigned processors_with_projected_steps = 0;
  };
  CpuExecutionTotals cpu_execution_totals() const noexcept {
    CpuExecutionTotals totals;
    for (unsigned id = 0; id < processor_count_; ++id) {
      const auto projected = processor_state_vector_steps(id);
      totals.guest_steps += processor_steps(id);
      totals.ether_steps += processor_ether_arm64_steps(id);
      totals.projected_steps += projected;
      totals.state_vector_interpreter_steps += processor_state_vector_interpreter_steps(id);
      totals.processors_with_projected_steps += projected != 0;
    }
    return totals;
  }
  std::uint64_t total_processor_steps() const noexcept {
    std::uint64_t steps = 0;
    for (unsigned id = 0; id < processor_count_; ++id) steps += processor_steps(id);
    return steps;
  }
  void run(unsigned budget, cosmic_pc::PcMachine::RunClock::time_point deadline=
           cosmic_pc::PcMachine::RunClock::time_point::max()) {
    const ActiveClockRun elapsed(*this);
    using Clock=cosmic_pc::PcMachine::RunClock;
    const bool timed=deadline!=Clock::time_point::max();
    if(!timed)budget=std::min(budget,cosmic_pc::PcMachine::kDefaultRunSteps);
    if (!secondary_can_run()) {
      run_bsp(budget, deadline);
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
        run_bsp(budget, deadline);
        return;
      }
      if (const auto completed = run_parallel_ether_loops(runnable, count, budget, deadline)) {
        budget -= completed;
        continue;
      }
      if (const auto scheduled = run_threaded_smp(runnable, count, budget, deadline)) {
        budget -= scheduled;
        continue;
      }
      const auto chunk = std::min(budget, count * smp_quantum(runnable, count));
      std::array<unsigned, kMaxProcessors> work{};
      for (unsigned i = 0; i < count; ++i)
        work[(next_cpu_ + i) % count] = chunk / count + (i < chunk % count);
      next_cpu_ = (next_cpu_ + chunk % count) % count;
      const bool preserve_ether_loop_entry = cosmic_pc::Cpu::ether_arm64_host() &&
          pc_.boot_acceleration_enabled() && budget >= count * kMinParallelLoopSteps;
      if (work[0]) run_bsp(work[0], deadline, preserve_ether_loop_entry);
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
