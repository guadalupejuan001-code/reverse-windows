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
#include "ps2_mouse.hpp"
#include "prime_pci_clock.h"
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
  static constexpr std::uint64_t kExperimentalTscHz = RW_PRIME_COMPONENT_HZ;
  static_assert(kExperimentalTscHz % cosmic_pc::Cpu::kTscTicksPerCycle == 0);
  static constexpr std::uint64_t kRamClockHz = RW_PRIME_COMPONENT_HZ;
  static_assert(kRamClockHz % 1'000'000'000 == 0);
  static_assert(cosmic_pc::ether_arm64::kExecutionTargetHz == RW_PRIME_COMPONENT_HZ);
  static_assert(qssd::device_clock_hz == RW_PRIME_COMPONENT_HZ);
  enum class ClockStart { Immediate, Guest32Or64 };
  static constexpr std::uint64_t kLocalApic=cosmic_pc::PcMachine::kLocalApicAddress,
      kIoApic=cosmic_pc::PcMachine::kIoApicAddress;
  struct Evidence {
    std::uint64_t ticks,irq_delivered,pit_irqs,apic_irqs;
    bool pci_present,acpi_installed,powered_off;
    std::uint64_t ata_irqs=0,sci_irqs=0,atapi_irqs=0;
  };
  struct InterruptState {
    bool global_enabled, software_enabled;
    unsigned pending, in_service, priority;
    std::uint32_t timer_count;
    std::uint32_t ioapic_select, ioapic_id;
    std::uint64_t in_service_delivery_id, in_service_since_tick;
  };
  struct LapicVectorStats {
    std::uint64_t deliveries = 0, eois = 0, entry_to_eoi_ticks = 0, entry_to_eoi_steps = 0;
  };
  struct StartupIpis {
    std::uint64_t init_icr_writes = 0, sipi_icr_writes = 0;
    std::uint64_t init_targets_queued = 0, sipi_targets_queued = 0, sipi_targets_applied = 0;
  };
  struct ProcessorPosition {
    const char* mode;
    std::uint16_t cs;
    std::uint64_t rip, cr3;
    std::uint32_t flags;
  };
  struct RtcEvidence {
    unsigned selected_register, register_a, register_b, register_c;
    bool nmi_masked;
    std::uint64_t irq8_assertions, data_writes;
    std::uint64_t irq8_acknowledgements;
    std::uint32_t irq8_redirection_low;
    std::uint64_t irq8_delivered;
  };
  struct LoopTiming {
    std::uint64_t samples = 0, last_ns = 0;
    unsigned last_steps = 0;
  };
  struct RamViewStats {
    std::uint64_t blocks = 0, steps = 0, conflicts = 0;
    void add(const RamViewStats& other) noexcept {
      blocks += other.blocks;
      steps += other.steps;
      conflicts += other.conflicts;
    }
  };
  struct alignas(64) BusWaitStats {
    std::uint64_t grants = 0, samples = 0, contended = 0, wait_ns = 0, max_ns = 0;
  };
  struct ThreadedSmpStats {
    std::uint64_t dispatches = 0, guest_steps = 0;
    std::uint64_t pause_yields = 0, pause_steps = 0, pause_admissions = 0;
    unsigned last_workers = 0, peak_workers = 0;
  };
  struct ParkedAdmissionStats {
    enum class Outcome : unsigned {
      TooFewRunnable, DeviceGate, WakePending, ShortBudget, HostOrGuestUnavailable,
      Deferred, PendingApic, ApicTimerDue, MissingRegisterLoop, TooFewCandidates,
      DeadlineBeforePreparation, ShortDeviceGrant, PreparationFailed,
      DeadlineAfterPreparation, WorkersUnavailable, ZeroRetired, OneProcessorRetired,
      ParallelRetired
    };
    static constexpr std::array kOutcomeNames{
      "too_few_runnable", "device_gate", "wake_pending", "short_budget", "host_or_guest_unavailable",
      "deferred", "pending_apic", "apic_timer_due", "missing_register_loop", "too_few_candidates",
      "deadline_before_preparation", "short_device_grant", "preparation_failed",
      "deadline_after_preparation", "workers_unavailable", "zero_retired", "one_processor_retired",
      "parallel_retired"
    };
    static_assert(static_cast<unsigned>(Outcome::ParallelRetired) + 1 == kOutcomeNames.size());
    static constexpr std::uint64_t kSamplePeriod = 1021;
    std::uint64_t attempts = 0, samples = 0;
    std::uint64_t ready_processors = 0, register_candidates = 0, ram_only_candidates = 0;
    std::uint64_t no_register_candidate = 0, one_register_candidate = 0;
    std::uint64_t partial_register_candidates = 0, all_register_candidates = 0;
    std::array<std::uint64_t, kOutcomeNames.size()> outcomes{};
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
    std::uint8_t pending() const { return pending(irr); }
    std::uint8_t pending(std::uint8_t requests) const {
      const auto candidates=unsigned(requests&~mask);
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
  Ps2Mouse mouse_;
  bool aux_command_ = false, aux_test_reply_ = false;
  bool aux_ready() const {
    return keyboard_ && !(keyboard_->configuration() & 0x20) && mouse_.ready();
  }
  bool aux_irq() const {
    return aux_ready() && !keyboard_->has_data() && !aux_test_reply_ &&
           (keyboard_->configuration() & 2) && mouse_.needs_irq();
  }
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
  std::array<std::uint64_t, GuestGpuC08::pci_device + 1> pci_clock_samples_{};
  std::uint64_t ata_irq_delivered_=0;
  std::uint64_t atapi_irq_delivered_=0;
  Pit pit_{},pit2_{};
  unsigned pit_phase_=0;
  std::uint8_t speaker_control_=0;
  std::uint32_t pci_selector_=0;
  static constexpr unsigned kRtcLine = 8;
  static constexpr std::uint8_t kRtcPeriodic=0x40, kRtcIrq=0x80, kRtcSet=0x80;
  static constexpr std::array<std::uint8_t,8> kRtcCalendar{{0,2,4,6,7,8,9,0x32}};
  std::array<std::uint8_t,128> cmos_ram_{};
  std::uint8_t cmos_index_=0,rtc_a_=0x26,rtc_b_=0x02,rtc_c_=0;
  std::time_t rtc_utc_offset_=0;
  std::uint64_t rtc_period_ticks_=0,rtc_next_periodic_tick_=UINT64_MAX;
  std::uint64_t rtc_irq8_assertions_=0,rtc_data_writes_=0;
  std::uint64_t rtc_irq8_acknowledgements_=0, rtc_irq8_delivered_=0;
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
    // A nested interrupt must have a higher priority class than the interrupted one.
    struct Delivery { std::uint64_t id=0, tick=0, steps=0; };
    std::array<Delivery,16> delivery_by_priority{};
    std::bitset<256> pending_level, in_service_level, pending_ata, pending_atapi, pending_sci, pending_rtc;
    PendingWrite mmio_pending;
  };
  static constexpr auto kMaxProcessors = cosmic_pc::CpuTopology::kMaxLogical;
  static constexpr unsigned kMinParallelLoopSteps = 128;
  static constexpr unsigned kMixedParallelLoopSteps = 1024;
  static constexpr unsigned kParkedParallelLoopSteps = 2048;
  static constexpr std::chrono::nanoseconds kParkedParallelWindow = std::chrono::microseconds(100);
  static constexpr auto kParallelLoopAccess = cosmic_pc::Cpu::EtherLoopAccess::ReadOnlyRam;
  std::array<std::unique_ptr<cosmic_pc::Cpu>, kMaxProcessors> secondary_cpus_{};
  std::array<LocalApic, kMaxProcessors> apics_;
  std::array<LapicVectorStats, 256> lapic_vectors_{};
  std::array<RamViewStats, kMaxProcessors> ram_views_before_init_{};
  std::array<BusWaitStats, kMaxProcessors> bus_wait_{};
  std::array<std::uint64_t, kMaxProcessors> retired_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> ether_steps_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> off_bus_register_steps_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> state_vector_steps_before_init_{};
  std::array<std::uint64_t, kMaxProcessors> interpreter_vector_steps_before_init_{};
  StartupIpis startup_ipis_{};
  unsigned processor_count_ = 1;
  const bool experimental_mixed_smp_;
  bool threaded_smp_enabled_ = false;
  unsigned worker_limit_ = kMaxProcessors;
  unsigned next_serial_cpu_ = 0, next_native_job_ = 0, next_threaded_job_ = 0;
  std::bitset<kMaxProcessors> parked_processors_, parked_irq_processors_, wake_pending_;
  std::array<unsigned, kMaxProcessors> wake_budget_{};
  std::optional<unsigned> faulted_processor_;
  std::shared_ptr<SmpWorkers> workers_;
  std::unique_ptr<EtherX86Sessions> ether_x86_sessions_;
  ThreadedSmpStats threaded_smp_stats_;
  ParkedAdmissionStats parked_admission_stats_;
  std::uint64_t parallel_register_batches_ = 0, parallel_register_steps_ = 0;
  std::uint64_t parallel_register_continuations_ = 0;
  std::uint64_t parallel_ram_batches_ = 0, parallel_ram_steps_ = 0;
  std::uint64_t parallel_mixed_batches_ = 0;
  std::uint64_t secondary_store_batches_ = 0, secondary_store_steps_ = 0;
  std::uint64_t secondary_movaps_batches_ = 0, secondary_movaps_steps_ = 0;
  std::uint64_t secondary_rep_elements_ = 0, secondary_rep_bytes_ = 0;
  std::uint64_t secondary_scan_steps_ = 0;
  std::uint64_t secondary_shift_steps_ = 0, secondary_zero_steps_ = 0, secondary_multiply_steps_ = 0;
  std::uint64_t secondary_streaming_sse_fill_steps_ = 0;
  std::uint64_t paired_loop_blocks_ = 0, paired_loop_steps_ = 0;
  std::uint64_t worker_loop_blocks_ = 0, worker_loop_steps_ = 0;
  std::uint64_t loop_timing_attempts_ = 0;
  LoopTiming loop_timing_{};
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
    mouse_ = {}; aux_command_ = aux_test_reply_ = false;
    master_=Pic{};slave_=Pic{};slave_.vector=0x70;
    pit_=Pit{};pit2_=Pit{};pit_phase_=0;speaker_control_=0;pci_selector_=0;
    cmos_index_=rtc_c_=0;rtc_a_=0x26;rtc_b_=0x02;pm_control_=0x0001;powered_off_=false;
    pm_status_=pm_enable_=0;sci_active_=false;
    ferr_was_asserted_=false;pc_.cpu.set_x87_ignne(false);
    cpu_cycles_=ticks_=irq_delivered_=pit_irqs_=apic_irqs_=ata_irq_delivered_=atapi_irq_delivered_=sci_irqs_=0;
    cpu_cycle_phase_ = 0;
    firmware_tsc_remainder_ = active_tsc_remainder_ = 0;
    active_clock_enabled_=active_clock_running_=waiting_for_guest_mode_=false;
    active_clock_ns_=active_clock_ticks_=clock_tick_remainder_=0;
    clock_polls_=0;
    rtc_irq8_assertions_=rtc_irq8_acknowledgements_=rtc_irq8_delivered_=rtc_data_writes_=0;
    lapic_vectors_.fill({});
    rtc_schedule_periodic();
    if(ata_)ata_->reset();
    if(data_ata_)data_ata_->reset();
    if(optical_)optical_->reset();
    if(gpu_)gpu_->reset(ticks_);
    pc_.cpu.set_apic_task_priority(0);
    bsp() = LocalApic{&pc_.cpu};
    bsp().started = true;
    for (unsigned id = 1; id < processor_count_; ++id) {
      secondary_cpus_[id]->reset(0, 0);
      apics_[id] = LocalApic{secondary_cpus_[id].get()};
    }
    ram_views_before_init_.fill({});
    bus_wait_.fill({});
    retired_before_init_.fill(0);
    ether_steps_before_init_.fill(0);
    off_bus_register_steps_before_init_.fill(0);
    state_vector_steps_before_init_.fill(0);
    interpreter_vector_steps_before_init_.fill(0);
    startup_ipis_ = {};
    faulted_processor_.reset();
    next_serial_cpu_ = next_native_job_ = next_threaded_job_ = 0;
    parked_processors_.reset();
    parked_irq_processors_.reset();
    wake_pending_.reset();
    wake_budget_.fill(0);
    threaded_smp_stats_ = {};
    parked_admission_stats_ = {};
    parallel_register_batches_ = parallel_register_steps_ = 0;
    parallel_register_continuations_ = 0;
    parallel_ram_batches_ = parallel_ram_steps_ = 0;
    parallel_mixed_batches_ = 0;
    secondary_store_batches_ = secondary_store_steps_ = 0;
    secondary_movaps_batches_ = secondary_movaps_steps_ = 0;
    secondary_rep_elements_ = secondary_rep_bytes_ = 0;
    secondary_scan_steps_ = 0;
    secondary_shift_steps_ = secondary_zero_steps_ = secondary_multiply_steps_ = 0;
    secondary_streaming_sse_fill_steps_ = 0;
    paired_loop_blocks_ = paired_loop_steps_ = worker_loop_blocks_ = worker_loop_steps_ = 0;
    loop_timing_attempts_ = 0;
    loop_timing_ = {};
    longest_parallel_register_slice_ = 0;
    ioapic_select_=0;
    ioapic_id_=std::uint8_t(AcpiC06::ioapic_id(processor_count_));
    io_redir_low_.fill(0x10000U);io_redir_high_.fill(0);
    io_remote_vector_.fill(0);
    io_remote_targets_.fill(0);
    build_config(host_cfg_,0x0001,0x00);
    pci_clock_samples_.fill(0);
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
    install_firmware_input();
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
      if (offset >= RW_PRIME_PCI_CLOCK_HZ && offset < RW_PRIME_PCI_CLOCK_CYCLES + 8 &&
          (dev == 0 || (dev == 2 && ide_present()) || (dev == GuestGpuC08::pci_device && gpu_))) {
        if (offset == RW_PRIME_PCI_CLOCK_CYCLES) {
          sync_active_clock();
          pci_clock_samples_[dev] = device_clock_cycles(RW_PRIME_COMPONENT_HZ);
        }
        const auto value = offset < RW_PRIME_PCI_CLOCK_CYCLES ? RW_PRIME_COMPONENT_HZ : pci_clock_samples_[dev];
        return std::uint32_t(value >> ((offset & 7U) * 8)) & part_mask(width);
      }
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
    signal_isa_irq(kRtcLine);
  }
  void resample_rtc_level() {
    constexpr std::uint32_t level = 1U << 15, masked = 1U << 16;
    const auto route = io_redir_low_[kRtcLine];
    if ((rtc_c_ & kRtcIrq) && !io_remote_vector_[kRtcLine] &&
        (route & (level | masked)) == level && (route & 0xffU) >= 16)
      route_ioapic(kRtcLine);
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
        if (rtc_c_ & kRtcIrq) ++rtc_irq8_acknowledgements_;
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
      if(p==0x64) {
        if (status & 1U) return status;
        if (aux_test_reply_ || keyboard_->has_data()) return status | 1U;
        return aux_ready() ? status | 0x21U : status;
      }
      if(status&1U)return prior_in_(p,width);
      if(aux_test_reply_){aux_test_reply_=false;return 0;}
      if(keyboard_->has_data())return keyboard_->read_data();
      return aux_ready() ? mouse_.read() : 0xffU;
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
        // The refresh divider changes state every 2^26 RAM cycles (~14.9 us).
        const bool refresh = (ram_clock_cycles() & (1ULL << 26)) != 0;
        return speaker_control_|(out?0x20U:0U)|(refresh?0x10U:0U);
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
      if(aux_command_){
        if(p!=0x60)return false;
        aux_command_=false;
        return mouse_.command(data);
      }
      if(p==0x64 && !keyboard_->controller_expects_data()){
        if(data==0xd4){aux_command_=true;return true;}
        if(data==0xa9){aux_test_reply_=true;return true;}
        if(data==0xa8){
          const auto config=keyboard_->configuration() & ~0x20U;
          if(!prior_out_(0x64,1,0x60)||!prior_out_(0x60,1,config))return false;
          keyboard_->controller_command(data);return true;
        }
      }
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
  std::uint32_t ioapic_redirection_low(unsigned line) const {
    return io_redir_low_[line] | (io_remote_vector_[line] ? 1U << 14 : 0);
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
        return (ioapic_select_&1)?io_redir_high_[pin]:ioapic_redirection_low(pin);
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
        if(pin==kRtcLine)resample_rtc_level();
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
  static bool local_timer_irq_armed(const LocalApic& apic) {
    return apic.timer_cycles && !(apic.lvt[kTimer] & kLvtMasked) && lapic_on(apic);
  }
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
  std::uint8_t cascaded_pic_requests(unsigned slave_irq) const {
    constexpr std::uint8_t cascade = 1U << 2;
    if (slave_irq) return master_.irr | cascade;
    return master_.isr & cascade ? master_.irr : std::uint8_t(master_.irr & ~cascade);
  }
  bool interrupt_pending() const {
    return deliverable_lapic_vector(bsp()) ||
           (pic_connected() && master_.pending(cascaded_pic_requests(slave_.pending())));
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
    const auto& entry = apic.delivery_by_priority[vector >> 4];
    auto& stats = lapic_vectors_[vector];
    ++stats.eois;
    stats.entry_to_eoi_ticks += ticks_ - entry.tick;
    stats.entry_to_eoi_steps += apic.cpu->instructions - entry.steps;
    apic.in_service.reset(vector);
    if(!apic.in_service_level.test(vector))return;
    apic.in_service_level.reset(vector);
    for (unsigned line = 0; line < io_remote_vector_.size(); ++line) {
      if (io_remote_vector_[line] != vector) continue;
      io_remote_targets_[line] &= ~(1U << apic.cpu->apic_id());
      if (!io_remote_targets_[line]) {
        io_remote_vector_[line] = 0;
        if (line == kRtcLine) resample_rtc_level();
      }
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
      if (line == 15) apic.pending_atapi.set(vector);
      if (line == AcpiC06::sci_irq) apic.pending_sci.set(vector);
      if (line == kRtcLine) apic.pending_rtc.set(vector);
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
      if (apic.pending_atapi[vector]) ++atapi_irq_delivered_;
      if (apic.pending_sci[vector]) ++sci_irqs_;
      if (apic.pending_rtc[vector]) ++rtc_irq8_delivered_;
      apic.pending_ata.reset(vector);
      apic.pending_atapi.reset(vector);
      apic.pending_sci.reset(vector);
      apic.pending_rtc.reset(vector);
      ++apic_irqs_; ++irq_delivered_;
      ++lapic_vectors_[vector].deliveries;
      apic.delivery_by_priority[vector >> 4] = {apic_irqs_, ticks_, apic.cpu->instructions};
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
    refresh_parked_processors();
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
  void poll(bool deliver_bsp = true){
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
    // A held input can gain a destination after the guest enables or retargets its LAPIC.
    resample_rtc_level();
    if(sci_active_&&(io_redir_low_[AcpiC06::sci_irq]&0x8000U))deliver_sci();
    if(keyboard_&&keyboard_->needs_irq()&&!(prior_in_(0x64,1).value_or(0)&1U)){
      signal_isa_irq(1);
      keyboard_->mark_irq();
    }
    if(aux_irq() && !(prior_in_(0x64,1).value_or(0)&1U)){
      signal_isa_irq(12);
      mouse_.mark_irq();
    }
    if(device_tick)advance_apic_timer(bsp(),1);
    // The slave's INT output reaches the master independently of CPU IF or
    // LAPIC acceptance. The guest can read both IRRs with interrupts disabled.
    const auto slave_irq=slave_.pending();
    master_.irr = cascaded_pic_requests(slave_irq);
    if(!deliver_bsp||cpu.fault||!cpu.flag(cosmic_pc::Cpu::IF))return;
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
          if(sn==7)++atapi_irq_delivered_;
          if(sn==AcpiC06::sci_irq-8)++sci_irqs_;
          if(sn==kRtcLine-8)++rtc_irq8_delivered_;
        }
      }else if(cpu.raise_external_irq(std::uint8_t(master_.vector+n))){
        master_.accepted(n);++irq_delivered_;
      }
    }
  }

  // The current step has already polled the board. Stop the batch before
  // another tick could change the interrupt or timer state visible to the CPU.
  unsigned batch_limit(unsigned steps) const {
    if (pc_.cpu.flag(cosmic_pc::Cpu::IF) && interrupt_pending()) return std::min(steps, 1U);
    return device_batch_limit(steps);
  }

  std::uint64_t ticks_to_device_edge() const {
    std::uint64_t edge = pm_timer_edge_in();
    if (pit_.enabled) edge = std::min<std::uint64_t>(edge, pit_.counter*kPitClockDivider-pit_phase_);
    if ((rtc_b_ & kRtcPeriodic) && !(rtc_c_ & kRtcIrq))
      edge = std::min(edge, rtc_next_periodic_tick_-ticks_);
    if (local_timer_irq_armed(bsp()))
      edge = std::min<std::uint64_t>(edge, bsp().timer_cycles);
    return edge;
  }

  unsigned device_batch_limit(unsigned steps) const {
    if(prior_tick_ || powered_off_ || pc_.cpu.x87_ferr_asserted()!=ferr_was_asserted_ ||
       (keyboard_ && keyboard_->needs_irq()) || aux_irq()) return std::min(steps, 1U);
    auto edge = ticks_to_device_edge();
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
      ram_views_before_init_[id] = processor_ram_views(id);
      retired_before_init_[id] += cpu.instructions;
      ether_steps_before_init_[id] += cpu.ether_arm64_stats().steps;
      off_bus_register_steps_before_init_[id] += cpu.ether_arm64_stats().off_bus_register_steps;
      const auto& interpreter = cpu.state_vector_interpreter_stats();
      state_vector_steps_before_init_[id] += cpu.ether_arm64_stats().state_vector_steps +
                                              interpreter.projected_steps;
      interpreter_vector_steps_before_init_[id] += interpreter.steps;
      cpu.reset_for_startup_ipi(0, 0);
      apic = LocalApic{&cpu};
      apic.startup_vector = sipi;
      for (unsigned line = 0; line < io_remote_targets_.size(); ++line) {
        io_remote_targets_[line] &= ~(1U << id);
        if (!io_remote_targets_[line]) io_remote_vector_[line] = 0;
      }
    }
    if (apic.startup_vector) {
      cpu.reset_for_startup_ipi(std::uint16_t(*apic.startup_vector) << 8, 0);
      cpu.a20_enabled = pc_.cpu.a20_enabled;
      apic.started = true;
      apic.startup_vector.reset();
      apic.last_tick = ticks_;
      ++startup_ipis_.sipi_targets_applied;
    }
  }
  static constexpr unsigned kSmpQuantum = 32;
  static constexpr unsigned kExtendedSmpQuantum = 4096;

  bool refresh_parked_processor(unsigned id) {
    const auto& apic = apics_[id];
    const bool parked = active_clock_running_ && apic.started && apic.cpu->halted &&
                        !apic.init_pending && !apic.startup_vector;
    parked_processors_.set(id, parked);
    parked_irq_processors_.set(id, parked && apic.cpu->flag(cosmic_pc::Cpu::IF));
    return parked;
  }

  bool parked_needs_short_grant() const {
    return parked_irq_processors_.any() || (parked_processors_[0] && prior_tick_);
  }

  void refresh_parked_processors() {
    parked_processors_.reset();
    parked_irq_processors_.reset();
    if (!active_clock_running_) return;
    for (unsigned id = 0; id < processor_count_; ++id)
      refresh_parked_processor(id);
  }

  // The dispatch owns this snapshot. Its CPUs have no worker, so their timers
  // can be sampled under the device bus without touching another worker's CPU.
  // Queue a wake; only the target's executor may enter its interrupt handler.
  void poll_parked_processors(bool poll_devices = false) {
    if (!active_clock_running_ || parked_processors_.none()) return;
    sync_active_clock();
    if (parked_processors_[0] && prior_tick_) prior_tick_();
    if (parked_processors_[0] || poll_devices) poll(false);
    for (unsigned id = 0, mask = parked_processors_.to_ulong(); mask; ++id, mask >>= 1) {
      if (!(mask & 1U)) continue;
      auto& apic = apics_[id];
      if (id) update_secondary_timer(apic);
      if (!wake_pending_[id] && (apic.init_pending || apic.startup_vector ||
          (parked_irq_processors_[id] &&
           (id ? deliverable_lapic_vector(apic) != 0 : interrupt_pending())))) {
        wake_pending_.set(id);
        wake_budget_[id] = kSmpQuantum;
      }
    }
  }

  unsigned smp_quantum(const std::array<unsigned, kMaxProcessors>& runnable, unsigned count) const {
    for (unsigned slot = 0; slot < count; ++slot) {
      const auto& apic = apics_[runnable[slot]];
      const auto& cpu = *apic.cpu;
      if (!apic.started || apic.init_pending || apic.startup_vector || local_timer_irq_armed(apic) ||
          cpu.halted || cpu.flag(cosmic_pc::Cpu::IF | cosmic_pc::Cpu::TF) ||
          !cpu.is_long_mode() || !cpu.ether_arm64_block_candidate()) return kSmpQuantum;
    }
    return std::max(kSmpQuantum, batch_limit(kExtendedSmpQuantum));
  }

  unsigned secondary_batch_limit(const LocalApic& apic, unsigned remaining) const {
    const bool accepts_irq = apic.cpu->flag(cosmic_pc::Cpu::IF);
    // A masked request stays in IRR. A translated block cannot change guest
    // interrupt priority or issue an EOI without returning to the device bus.
    if (accepts_irq && deliverable_lapic_vector(apic)) return 1;
    auto limit = device_batch_limit(remaining);
    if (accepts_irq && local_timer_irq_armed(apic))
      limit = unsigned(std::min<std::uint64_t>(limit, apic.timer_cycles));
    return limit;
  }

  static bool grouped_loop_candidate(const cosmic_pc::Cpu& cpu) {
    return cpu.scan_loop_batch_candidate() || cpu.shift_loop_batch_candidate() ||
           cpu.zero_loop_batch_candidate() || cpu.wide_multiply_loop_batch_candidate() ||
           cpu.streaming_sse_fill_loop_batch_candidate();
  }

  unsigned retire_secondary_loop(cosmic_pc::Cpu& cpu, unsigned limit) {
    if (cpu.scan_loop_batch_candidate()) {
      const auto steps = cpu.try_scan_loop_batch(limit);
      secondary_scan_steps_ += steps;
      return steps;
    }
    if (cpu.shift_loop_batch_candidate()) {
      const auto steps = cpu.try_shift_loop_batch(limit);
      secondary_shift_steps_ += steps;
      return steps;
    }
    if (cpu.zero_loop_batch_candidate()) {
      const auto steps = cpu.try_zero_loop_batch(limit);
      secondary_zero_steps_ += steps;
      return steps;
    }
    const bool multiply = cpu.wide_multiply_loop_batch_candidate();
    if (!multiply && !cpu.streaming_sse_fill_loop_batch_candidate()) return 0;
    const auto steps = unsigned(cpu.try_counter_loop_batch(limit));
    if (multiply) secondary_multiply_steps_ += steps;
    else secondary_streaming_sse_fill_steps_ += steps;
    return steps;
  }

  unsigned shared_loop_limit(unsigned budget) const {
    auto limit = batch_limit(budget);
    for (unsigned id = 1; id < processor_count_; ++id) {
      const auto& apic = apics_[id];
      if (apic.init_pending || apic.startup_vector) return 1;
      if (!apic.started || !apic.cpu->flag(cosmic_pc::Cpu::IF)) continue;
      if (deliverable_lapic_vector(apic)) return 1;
      if (local_timer_irq_armed(apic)) {
        const auto elapsed = ticks_ - apic.last_tick;
        if (elapsed >= apic.timer_cycles) return 1;
        limit = unsigned(std::min<std::uint64_t>(limit, apic.timer_cycles - elapsed));
      }
    }
    return limit;
  }

  static constexpr unsigned kMinCooperativeGrant = 12; // Two iterations of the six-instruction scan.

  unsigned run_shared_loop(cosmic_pc::Cpu& cpu, unsigned grant) {
    const auto limit = shared_loop_limit(grant);
    if (limit < kMinCooperativeGrant) return 0;
    // Calls hold the shared bus. Sample execution time separately from the
    // time spent waiting for a lane; failed candidates publish no latency.
    const bool sample = (loop_timing_attempts_++ & 63U) == 0;
    const auto start = sample ? Clock::now() : Clock::time_point{};
    const auto steps = retire_secondary_loop(cpu, limit);
    if (sample && steps) {
      ++loop_timing_.samples;
      loop_timing_.last_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
      loop_timing_.last_steps = steps;
    }
    return steps;
  }

  // The AP has reached a checked RAM or register loop. Commit one block
  // on the calling lane while it owns the bus, then service the BSP before
  // allowing another block. An IPI or an MMIO operation returns to dispatch.
  unsigned run_paired_loop(unsigned id, unsigned budget, Clock::time_point deadline,
                           bool& register_peer) {
    const bool previous_register_peer = register_peer;
    register_peer = false;
    auto& apic = apics_[id];
    auto& primary = pc_.cpu;
    if (!active_clock_running_ || !pc_.boot_acceleration_enabled() || !pc_.guest_execution_available() ||
        !apic.started || apic.init_pending || apic.startup_vector || apic.cpu->halted ||
        !grouped_loop_candidate(*apic.cpu) || (primary.halted && primary.flag(cosmic_pc::Cpu::IF)))
      return 0;
    const bool parked = primary.halted;
    const unsigned peer_limit = previous_register_peer ? 32768U : kExtendedSmpQuantum;
    const unsigned grant = parked ? std::min(65536U, budget) : std::min(peer_limit, budget / 2);
    if (grant < kMinCooperativeGrant) return 0;
    apic.cpu->a20_enabled = primary.a20_enabled;
    update_secondary_timer(apic);
    const cosmic_pc::MemoryFabric::ExclusiveRamReads exclusive(primary.memory);
    const auto steps = run_shared_loop(*apic.cpu, grant);
    if (!steps) return 0;
    ++paired_loop_blocks_;
    paired_loop_steps_ += steps;
    const auto before = primary.instructions;
    const auto start_ip = primary.eip;
    const auto counters_before = pc_.boot_acceleration().counter_loop_steps;
    // A complete AP iteration may leave a few steps of its share. Use them
    // on the peer so the next call starts at the validated AP backedge.
    const unsigned peer = parked ? 1U : steps + std::min(2U * (grant - steps), 16U);
    run_bsp(std::min(budget - steps, peer), deadline);
    // Widen the next pair only after the canonical BSP decoder has retired
    // this same short register countdown for an entire share (at most two
    // scalar boundary instructions). A polling loop that reads shared RAM
    // keeps its normal turn, so it can send an IPI as its condition changes.
    const auto peer_steps = primary.instructions - before;
    const auto counter_steps = pc_.boot_acceleration().counter_loop_steps - counters_before;
    if (budget - steps > peer_steps && counter_steps >= 128 && counter_steps + 2 >= peer_steps &&
        primary.eip == start_ip && primary.is_long_mode() && !primary.halted &&
        !primary.flag(cosmic_pc::Cpu::IF | cosmic_pc::Cpu::TF) && pc_.guest_execution_available()) {
      if (!primary.counter_loop_batch_candidate()) run_bsp(1, deadline);
      const auto& branch = primary.last_decoded_bytes();
      register_peer = primary.counter_loop_batch_candidate() && branch.size() == 2 &&
                      branch[0] == 0x75 && branch[1] >= 0xf9;
    }
    return steps + unsigned(primary.instructions - before);
  }

  void run_secondary(unsigned id, unsigned budget, Clock::time_point deadline,
                     bool preserve_ether_loop_entry, std::unique_lock<std::mutex>* bus = nullptr) {
    using Clock = cosmic_pc::PcMachine::RunClock;
    const bool timed = deadline != Clock::time_point::max();
    auto& apic = apics_[id];
    auto& cpu = *apic.cpu;
    unsigned dispatches = 0;
    std::optional<std::uint64_t> rejected_vector_ip;
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
      if (pc_.boot_acceleration_enabled() && cpu.rep_batch_candidate()) {
        if (const auto elements = cpu.try_rep_batch(secondary_batch_limit(apic, remaining))) {
          secondary_rep_elements_ += elements;
          secondary_rep_bytes_ += cpu.last_rep_batch_bytes();
          used += elements;
          continue;
        }
      }
      if (pc_.boot_acceleration_enabled() && grouped_loop_candidate(cpu)) {
        if (const auto steps = retire_secondary_loop(cpu, secondary_batch_limit(apic, remaining))) {
          used += steps;
          // Leave a validated backedge intact for the next bus grant.
          if (active_clock_running_ && budget - used < kMinCooperativeGrant && grouped_loop_candidate(cpu)) return;
          continue;
        }
      }
      // Apply the same one-page XMM store coalescer as the boot CPU. Shared
      // writes stay inside the scheduler's bus grant in threaded mode.
      using StoreBatch = cosmic_pc::Cpu::XmmStoreBatch;
      const auto store_kind = pc_.boot_acceleration_enabled() ? cpu.xmm_store_batch_candidate() : StoreBatch::None;
      if (store_kind != StoreBatch::None) {
        if (const auto stores = cpu.try_xmm_store_batch(secondary_batch_limit(apic, remaining), store_kind)) {
          if (store_kind == StoreBatch::Aligned) {
            ++secondary_movaps_batches_;
            secondary_movaps_steps_ += stores;
          } else {
            ++secondary_store_batches_;
            secondary_store_steps_ += stores;
          }
          used += stores;
          continue;
        }
      }
      if (remaining > 1 && cpu.ether_arm64_block_candidate()) {
        // A scheduler turn can span a local timer edge. Native blocks do not
        // poll the APIC internally, so bound their grant when IF is set.
        const auto limit = cpu.flag(cosmic_pc::Cpu::IF) ? secondary_batch_limit(apic, remaining) : remaining;
        if (const auto native_steps = cpu.try_ether_arm64_block(limit, bus)) {
          used += native_steps;
          continue;
        }
      }
      if (pc_.boot_acceleration_enabled() && cpu.state_vector_batch_candidate() &&
          rejected_vector_ip != cpu.eip) {
        const auto entry = cpu.eip;
        if (const auto vector_steps = cpu.try_state_vector_batch(secondary_batch_limit(apic, remaining))) {
          used += vector_steps;
          continue;
        }
        // Do not decode the same rejected loop on every backedge of this grant.
        // Scalar execution remains exact, and the next grant retries changed code or state.
        rejected_vector_ip = entry;
      }
      const auto result = cpu.step();
      if (result == cosmic_pc::StepResult::Faulted) faulted_processor_ = id;
      if (result != cosmic_pc::StepResult::Executed) return;
      ++used;
    }
  }

  // Keep the BSP on the calling lane when present. Rotate every AP that
  // shares physical workers with other ready processors.
  static unsigned rotate_smp_job(unsigned position, unsigned count, unsigned rotation,
                                 unsigned fixed_jobs) noexcept {
    if (position < fixed_jobs) return position;
    return fixed_jobs + (position - fixed_jobs + rotation) % (count - fixed_jobs);
  }

  Clock::time_point parked_parallel_deadline(Clock::time_point start) const {
    auto edge = ticks_to_device_edge();
    for (unsigned id = 1; id < processor_count_; ++id) {
      const auto& apic = apics_[id];
      if (!apic.started || !apic.cpu->flag(cosmic_pc::Cpu::IF)) continue;
      if (deliverable_lapic_vector(apic)) return start;
      if (!local_timer_irq_armed(apic)) continue;
      const auto elapsed = ticks_ - apic.last_tick;
      if (elapsed >= apic.timer_cycles) return start;
      edge = std::min(edge, apic.timer_cycles - elapsed);
    }
    constexpr std::uint64_t second = 1'000'000'000ULL;
    constexpr auto frequency = cosmic_pc::PcMachine::kVirtualCyclesPerSecond;
    // Leave a device tick for the clock sample at the dispatch boundary.
    if (edge <= 1) return start;
    const auto until_edge = ((edge - 1) * second - clock_tick_remainder_) / frequency;
    return start + std::min(kParkedParallelWindow, std::chrono::nanoseconds(until_edge));
  }

  ParkedAdmissionStats* sample_parked_admission(const std::array<unsigned, kMaxProcessors>& runnable,
                                                 unsigned count) noexcept {
    auto& stats = parked_admission_stats_;
    if (stats.attempts++ % ParkedAdmissionStats::kSamplePeriod) return nullptr;
    ++stats.samples;
    // A cache-only snapshot includes later peers even when admission rejects
    // the first peer. It does not translate guest addresses or prepare work.
    unsigned ready = 0, registers = 0;
    for (unsigned i = 0; i < count; ++i) {
      const auto& cpu = *apics_[runnable[i]].cpu;
      if (cpu.halted) continue;
      ++ready;
      if (cpu.ether_arm64_loop_length(cosmic_pc::Cpu::EtherLoopAccess::Registers)) ++registers;
      else if (cpu.ether_arm64_loop_length(kParallelLoopAccess)) ++stats.ram_only_candidates;
    }
    stats.ready_processors += ready;
    stats.register_candidates += registers;
    if (registers == 0) ++stats.no_register_candidate;
    else if (registers == 1) ++stats.one_register_candidate;
    else if (registers < ready) ++stats.partial_register_candidates;
    else ++stats.all_register_candidates;
    return &stats;
  }

  unsigned run_parallel_ether_loops(const std::array<unsigned, kMaxProcessors>& runnable,
                                    unsigned count, unsigned budget,
                                    cosmic_pc::PcMachine::RunClock::time_point deadline,
                                    bool& defer_parked_parallel) {
    const bool watch_parked_irqs = parked_irq_processors_.any();
    auto* sample = watch_parked_irqs ? sample_parked_admission(runnable, count) : nullptr;
    using Outcome = ParkedAdmissionStats::Outcome;
    const auto record = [sample](Outcome outcome) {
      if (sample) ++sample->outcomes[static_cast<unsigned>(outcome)];
      return 0U;
    };
    if (count < 2) return record(Outcome::TooFewRunnable);
    if (prior_tick_) return record(Outcome::DeviceGate);
    if (wake_pending_.any()) return record(Outcome::WakePending);
    if (budget < 2 * kMinParallelLoopSteps) return record(Outcome::ShortBudget);
    if (!cosmic_pc::Cpu::ether_arm64_host() || !pc_.boot_acceleration_enabled() || !pc_.guest_execution_available())
      return record(Outcome::HostOrGuestUnavailable);
    if (sci_active_ && (io_redir_low_[AcpiC06::sci_irq] & 0x18000U) == 0x8000U &&
        !io_remote_targets_[AcpiC06::sci_irq]) return record(Outcome::DeviceGate);
    if (watch_parked_irqs && defer_parked_parallel) return record(Outcome::Deferred);
    // A read-only RAM loop can still block on page backing. While an IRQ
    // target sleeps, admit only bounded native work on private registers.
    const auto access = watch_parked_irqs ? cosmic_pc::Cpu::EtherLoopAccess::Registers : kParallelLoopAccess;
    const bool allow_mixed = experimental_mixed_smp_ && !watch_parked_irqs;
    auto limit = watch_parked_irqs ? std::min(budget, kParkedParallelLoopSteps) : budget;
    std::array<unsigned, kMaxProcessors> candidates{};
    unsigned candidate_count = 0, active_count = 0;
    const auto consider = [&](unsigned id) {
      if (!apics_[id].cpu->ether_arm64_loop_length(access)) return false;
      candidates[candidate_count++] = id;
      return true;
    };
    for (unsigned i = 0; i < count; ++i) {
      const auto id = runnable[i];
      const auto& apic = apics_[id];
      const auto& cpu = *apic.cpu;
      if (apic.init_pending || apic.startup_vector ||
          (cpu.flag(cosmic_pc::Cpu::IF) && deliverable_lapic_vector(apic))) return record(Outcome::PendingApic);
      if (id && local_timer_irq_armed(apic)) {
        const auto elapsed = ticks_ - apic.last_tick;
        if (elapsed >= apic.timer_cycles) return record(Outcome::ApicTimerDue);
        limit = static_cast<unsigned>(std::min<std::uint64_t>(limit, apic.timer_cycles - elapsed - 1));
      }
      if (cpu.halted) continue;
      ++active_count;
      if (!consider(id) && !allow_mixed) return record(Outcome::MissingRegisterLoop);
    }
    if (candidate_count < 2) return record(Outcome::TooFewCandidates);
    auto work_deadline = deadline;
    if (watch_parked_irqs) {
      const auto sample_start = Clock::now();
      poll_parked_processors(true);
      if (wake_pending_.any()) return record(Outcome::WakePending);
      work_deadline = std::min(deadline, parked_parallel_deadline(sample_start));
      if (Clock::now() >= work_deadline) return record(Outcome::DeadlineBeforePreparation);
    }
    // Bound one independent execution by the earliest device edge and the
    // aggregate budget, including the processors that need serial service.
    limit = batch_limit(std::min(limit, budget / active_count) + 1) - 1;
    if (limit < kMinParallelLoopSteps) return record(Outcome::ShortDeviceGrant);

    std::array<unsigned, kMaxProcessors> jobs{};
    std::array<cosmic_pc::Cpu::EtherLoopWork, kMaxProcessors> work{};
    const auto prepare = [&](bool ram_only) {
      unsigned ready = 0;
      for (unsigned i = 0; i < candidate_count; ++i) {
        const auto id = candidates[i];
        const auto accepted = id
            ? apics_[id].cpu->prepare_ether_arm64_loop(limit, work[ready], access)
            : pc_.prepare_ether_loop(limit, work[ready], access);
        if (accepted && (!ram_only || work[ready].reads_ram())) jobs[ready++] = id;
        else if (!allow_mixed) return 0U;
      }
      return ready;
    };
    const auto prepare_mixed = [&] {
      // Projected register rounds can be cheaper than starting workers.
      // Give distinct RAM readers a full grant before paying for a mixed batch.
      if (!allow_mixed || limit < kMixedParallelLoopSteps) return 0U;
      limit = kMixedParallelLoopSteps;
      return prepare(true);
    };
    auto job_count = candidate_count == active_count ? prepare(false) : 0U;
    if (job_count < active_count) job_count = prepare_mixed();
    if (job_count < 2) return record(Outcome::PreparationFailed);
    if (watch_parked_irqs && Clock::now() >= work_deadline) return record(Outcome::DeadlineAfterPreparation);
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
    const unsigned fixed_jobs = jobs[0] == 0;
    const auto next_job = next_native_job_ % (job_count - fixed_jobs);
    const auto lanes = workers_->try_run(std::min(job_count, host_workers_capacity()), [&](unsigned lane, unsigned count) {
      const auto rotation = count < job_count ? next_job : 0U;
      for (unsigned position = lane; position < job_count; position += count) {
        const auto job = rotate_smp_job(position, job_count, rotation, fixed_jobs);
        const auto id = jobs[job];
        auto& cpu = *apics_[id].cpu;
        auto& block = work[job];
        auto& result = progress[job];
        const bool measure = id == 0 && profile_bsp;
        bool continued = false;
        do {
          const auto start = measure ? Clock::now() : Clock::time_point{};
          block.run(work_deadline);
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
    if (!lanes) return record(Outcome::WorkersUnavailable);
    const auto rotation = lanes < job_count ? next_job : 0U;
    if (lanes < job_count)
      next_native_job_ = (rotation + lanes - fixed_jobs) % (job_count - fixed_jobs);
    unsigned total = 0, processors = 0, longest = 0, ram_steps = 0, bsp_steps = 0;
    std::array<bool, kMaxProcessors> active_lanes{};
    std::array<bool, kMaxProcessors> serviced{};
    for (unsigned position = 0; position < job_count; ++position) {
      const auto job = rotate_smp_job(position, job_count, rotation, fixed_jobs);
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
    record(processors > 1 ? Outcome::ParallelRetired :
           processors == 1 ? Outcome::OneProcessorRetired : Outcome::ZeroRetired);
    if (watch_parked_irqs) {
      // Worker retirement is private. Poll the devices at the join before
      // RAM maintenance or another guest grant.
      if (bsp_steps) advance_batch(bsp_steps);
      poll_parked_processors(true);
      if (bsp_steps && wake_pending_.none()) pc_.finish_parallel_ether_work();
      // A lane can miss the time window. Serve the skipped vCPU through
      // normal threaded grants for the rest of this run() call.
      if (processors < active_count) defer_parked_parallel = true;
    } else if (bsp_steps) {
      pc_.finish_parallel_ether_work();
      advance_batch(bsp_steps);
    }
    if (threaded_smp_enabled_)
      record_threaded_dispatch(std::count(active_lanes.begin(), active_lanes.begin() + lanes, true), total);
    if (processors > 1) {
      parallel_mixed_batches_ += allow_mixed && processors < active_count;
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
    if (!allow_mixed || !total || processors == active_count) return total;
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

  unsigned run_threaded_smp(const std::array<unsigned, kMaxProcessors>& ready, unsigned jobs,
                            unsigned budget, Clock::time_point deadline, bool cooperative_pauses) {
    if (!threaded_smp_enabled_ || !pc_.guest_execution_available()) return 0;

    constexpr unsigned bus_steps = 64, timed_grant = 512;
    const auto quantum = std::min(kExtendedSmpQuantum,
        (active_clock_running_ ? timed_grant : kSmpQuantum) * cpu_clock_ratio_);
    const auto grant = std::min(budget / jobs, quantum);
    if (jobs < 2 || grant < kSmpQuantum) return 0;
    const bool preserve_loop_entries = cosmic_pc::Cpu::ether_arm64_host() &&
        pc_.boot_acceleration_enabled() && budget >= jobs * kMinParallelLoopSteps;

    if (!workers_) workers_ = std::make_shared<SmpWorkers>(host_workers_capacity());

    // Each CPU stays on its worker for the dispatch. The bus protects RAM,
    // devices and IRQ delivery. Native blocks can release it for private
    // registers or a validated private view of fixed-address RAM words.
    const cosmic_pc::MemoryFabric::ExclusiveRamReads exclusive(pc_.cpu.memory);
    std::mutex bus;
    bool parked_during_dispatch = false;
    std::array<std::uint64_t, kMaxProcessors> progress{};
    const unsigned fixed_jobs = ready[0] == 0;
    const auto next_job = next_threaded_job_ % (jobs - fixed_jobs);
    const auto lanes = workers_->try_run(std::min(jobs, host_workers_capacity()), [&](unsigned worker, unsigned workers) {
      const auto rotation = workers < jobs ? next_job : 0U;
      for (unsigned position = worker; position < jobs; position += workers) {
        const auto id = ready[rotate_smp_job(position, jobs, rotation, fixed_jobs)];
        for (unsigned used = 0; used < grant;) {
          std::unique_lock<std::mutex> lock(bus, std::defer_lock);
          auto& wait = bus_wait_[id];
          if ((wait.grants++ & 63U) == 0) {
            ++wait.samples;
            if (!lock.try_lock()) {
              const auto started = Clock::now();
              lock.lock();
              const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
              ++wait.contended;
              wait.wait_ns += ns;
              wait.max_ns = std::max(wait.max_ns, std::uint64_t(ns));
            }
          } else {
            lock.lock();
          }
          if (faulted_processor_ || !pc_.guest_execution_available() ||
              (deadline != Clock::time_point::max() && Clock::now() >= deadline)) return;
          if (wake_pending_.any()) return;
          const auto remaining = grant - used;
          auto& apic = apics_[id];
          const bool short_grant = parked_needs_short_grant();
          const bool pause_turn = cooperative_pauses && apic.cpu->pause_wait_hint();
          if (id && remaining > bus_steps && !short_grant && !pause_turn &&
              active_clock_running_ && pc_.boot_acceleration_enabled() &&
              apic.started && !apic.init_pending && !apic.startup_vector && !apic.cpu->halted &&
              grouped_loop_candidate(*apic.cpu)) {
            sync_active_clock();
            update_secondary_timer(apic);
            apic.cpu->a20_enabled = pc_.cpu.a20_enabled;
            // Other ready CPUs retain their shares. Only a validated, non-I/O
            // operation can hold the bus past its ordinary 64-step slice.
            if (const auto bulk = run_shared_loop(*apic.cpu, remaining)) {
              ++worker_loop_blocks_;
              worker_loop_steps_ += bulk;
              progress[worker] += bulk;
              used += bulk;
              continue;
            }
          }
          const auto before = processor_steps(id);
          const auto steps = std::min(short_grant || pause_turn ? kSmpQuantum : bus_steps, remaining);
          // The next dispatch can admit a complete cached loop when a
          // short remainder would otherwise leave its vCPU inside it.
          const bool preserve = preserve_loop_entries && used + steps == grant;
          if (id) run_secondary(id, steps, deadline, preserve, &lock);
          else pc_.run(steps, deadline, preserve, &lock);
          const auto retired = processor_steps(id) - before;
          progress[worker] += retired;
          parked_during_dispatch |= refresh_parked_processor(id);
          poll_parked_processors();
          if (wake_pending_.any()) return;
          if (parked_processors_[id]) break;
          if (!retired) break;
          // Keep polling a real condition on every admitted turn, but let
          // a runnable peer proceed before spending another grant waiting.
          if (cooperative_pauses && apic.cpu->pause_wait_hint()) {
            if (retired < remaining) {
              ++threaded_smp_stats_.pause_yields;
              threaded_smp_stats_.pause_steps += retired;
            }
            break;
          }
          used += unsigned(retired);
          if (retired < steps && !(id && steps < remaining && active_clock_running_ &&
                                  grouped_loop_candidate(*apic.cpu))) break;
        }
      }
    });
    if (!lanes) return 0;
    if (lanes < jobs) next_threaded_job_ = (next_job + lanes - fixed_jobs) % (jobs - fixed_jobs);
    std::uint64_t retired = 0;
    unsigned active = 0;
    for (const auto steps : progress) {
      retired += steps;
      active += steps != 0;
    }
    record_threaded_dispatch(active, retired);
    const auto scheduled = jobs * grant;
    if (cooperative_pauses || wake_pending_.any() || parked_during_dispatch)
      return static_cast<unsigned>(retired);
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
    const auto storage_clock = [this](std::uint64_t hz) {
      sync_active_clock();
      return device_clock_cycles(hz);
    };
    if(disk)ata_=std::make_unique<AtaC07>(*disk,[this](){signal_isa_irq(14);},AtaC07::Unit::Master,storage_clock);
    if(data_disk)data_ata_=std::make_unique<AtaC07>(
        *data_disk,[this](){signal_isa_irq(14);},AtaC07::Unit::Slave,storage_clock);
    if(optical)optical_=std::make_unique<AtapiCdrom>(
        std::move(optical),optical_bytes,[this](){signal_isa_irq(15);});
    if(!graphics_vram.empty())gpu_=std::make_unique<GuestGpuC08>(
        pc_,graphics_vram,[this](auto address,auto data){return signal_pci_msi(address,data);},workers_,
        [this]{return device_clock_cycles(RW_PRIME_COMPONENT_HZ);});
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
          (!secondary_can_run() || (active_clock_running_ && !prior_tick_))) pc_.yield_after_instruction();
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
    return {ticks_,irq_delivered_,pit_irqs_,apic_irqs_,true,true,powered_off_,ata_irq_delivered_,sci_irqs_,atapi_irq_delivered_};
  }
  const auto& lapic_vector_stats() const noexcept { return lapic_vectors_; }
  InterruptState interrupt_state()const{
    const auto vector = bsp().in_service.highest();
    const auto delivery = bsp().delivery_by_priority[vector >> 4]; // Class zero is never delivered.
    return {pc_.cpu.apic_global_enabled(),lapic_on(bsp()),bsp().pending.highest(),
            vector,processor_priority(bsp()),timer_count(bsp()),ioapic_select_,ioapic_id_,
            delivery.id,delivery.tick};
  }
  bool secondary_can_run() const noexcept {
    for (unsigned id = 1; id < processor_count_; ++id)
      if (needs_service(apics_[id])) return true;
    return false;
  }
  const char* processor_state(unsigned id) const {
    const auto& apic = apics_.at(id);
    if (!apic.cpu) throw std::out_of_range("Unknown vCPU");
    if (apic.init_pending || apic.startup_vector) return "startup_pending";
    if (!apic.started) return "wait_for_sipi";
    return apic.cpu->halted ? "halted" : "running";
  }
  ProcessorPosition processor_position(unsigned id) const {
    const auto* cpu = apics_.at(id).cpu;
    if (!cpu) throw std::out_of_range("Unknown vCPU");
    return {cpu->mode_name(), cpu->segments[cosmic_pc::Cpu::CS], cpu->eip, cpu->cr3, cpu->eflags};
  }
  unsigned processors_started() const noexcept {
    return static_cast<unsigned>(std::count_if(apics_.begin(), apics_.begin() + processor_count_,
        [](const LocalApic& apic) { return apic.started; }));
  }
  StartupIpis startup_ipis() const noexcept { return startup_ipis_; }
  RtcEvidence rtc_evidence() const noexcept {
    return {cmos_index_&0x7fU,rtc_a_,rtc_b_,rtc_c_,bool(cmos_index_&0x80U),
            rtc_irq8_assertions_,rtc_data_writes_,rtc_irq8_acknowledgements_,
            ioapic_redirection_low(kRtcLine),rtc_irq8_delivered_};
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
  const ParkedAdmissionStats& parked_admission_stats() const noexcept { return parked_admission_stats_; }
  std::uint64_t parallel_register_steps() const noexcept { return parallel_register_steps_; }
  std::uint64_t parallel_register_continuations() const noexcept { return parallel_register_continuations_; }
  std::uint64_t parallel_ram_batches() const noexcept { return parallel_ram_batches_; }
  std::uint64_t parallel_ram_steps() const noexcept { return parallel_ram_steps_; }
  std::uint64_t secondary_store_batches() const noexcept { return secondary_store_batches_; }
  std::uint64_t secondary_store_steps() const noexcept { return secondary_store_steps_; }
  std::uint64_t secondary_movaps_batches() const noexcept { return secondary_movaps_batches_; }
  std::uint64_t secondary_movaps_steps() const noexcept { return secondary_movaps_steps_; }
  std::uint64_t secondary_rep_elements() const noexcept { return secondary_rep_elements_; }
  std::uint64_t secondary_rep_bytes() const noexcept { return secondary_rep_bytes_; }
  std::uint64_t secondary_scan_steps() const noexcept { return secondary_scan_steps_; }
  std::uint64_t secondary_shift_steps() const noexcept { return secondary_shift_steps_; }
  std::uint64_t secondary_zero_steps() const noexcept { return secondary_zero_steps_; }
  std::uint64_t secondary_multiply_steps() const noexcept { return secondary_multiply_steps_; }
  std::uint64_t secondary_streaming_sse_fill_steps() const noexcept { return secondary_streaming_sse_fill_steps_; }
  std::uint64_t parallel_mixed_batches() const noexcept { return parallel_mixed_batches_; }
  std::uint64_t paired_loop_blocks() const noexcept { return paired_loop_blocks_; }
  std::uint64_t paired_loop_steps() const noexcept { return paired_loop_steps_; }
  const LoopTiming& loop_timing() const noexcept { return loop_timing_; }
  std::uint64_t worker_loop_blocks() const noexcept { return worker_loop_blocks_; }
  std::uint64_t worker_loop_steps() const noexcept { return worker_loop_steps_; }
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
      if (!active_clock_running_ || powered_off_ || ((keyboard_ && keyboard_->needs_irq()) || aux_irq()) ||
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
  std::uint64_t ata_clock_hz() const noexcept {
    return ide_present() ? AtaC07::kControllerClockHz : 0;
  }
  std::uint64_t device_clock_cycles(std::uint64_t hz) const noexcept {
    // Derive the free-running oscillator on demand, once for the entire board.
    // The firmware phase stops where active VM time takes over, even mid-tick.
    const auto firmware_steps = (ticks_ - active_clock_ticks_) * cpu_clock_ratio_ + cpu_cycle_phase_;
    std::uint64_t firmware_remainder = 0, active_remainder = 0;
    const auto firmware = scaled_cycles(firmware_steps, hz, firmware_cycles_per_second_, firmware_remainder);
    return firmware + scaled_cycles(active_clock_ns_, hz, 1'000'000'000, active_remainder);
  }
  std::uint64_t ram_clock_cycles() const noexcept {
    return device_clock_cycles(kRamClockHz);
  }
  std::uint64_t active_clock_elapsed_ns() const noexcept {return active_clock_ns_;}
  std::uint64_t active_clock_ticks() const noexcept {return active_clock_ticks_;}
  std::uint64_t processor_steps(unsigned id) const noexcept {
    return apics_[id].cpu->instructions + retired_before_init_[id];
  }
  std::uint64_t processor_crc32c_retired(unsigned id) const noexcept {
    return apics_[id].cpu->crc32c_retired;
  }
  const cosmic_pc::Cpu::TscAccessStats& processor_tsc_access_stats(unsigned id) const noexcept {
    return apics_[id].cpu->tsc_access_stats();
  }
  const BusWaitStats& processor_bus_wait(unsigned id) const noexcept { return bus_wait_[id]; }
  RamViewStats processor_ram_views(unsigned id) const noexcept {
    auto result = ram_views_before_init_[id];
    const auto& cpu = apics_[id].cpu->ether_arm64_stats();
    result.add({cpu.off_bus_ram_blocks, cpu.off_bus_ram_steps, cpu.ram_view_conflicts});
    return result;
  }
  std::uint64_t processor_ether_arm64_steps(unsigned id) const noexcept {
    return apics_[id].cpu->ether_arm64_stats().steps + ether_steps_before_init_[id];
  }
  std::uint64_t processor_off_bus_register_steps(unsigned id) const noexcept {
    return apics_[id].cpu->ether_arm64_stats().off_bus_register_steps + off_bus_register_steps_before_init_[id];
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
    std::uint64_t state_vector_interpreter_steps = 0, crc32c_retired = 0;
    unsigned processors_with_projected_steps = 0;
    RamViewStats ram_views;
    cosmic_pc::Cpu::TscAccessStats tsc_access;
    std::uint64_t bus_samples = 0, bus_contended = 0, bus_wait_ns = 0;
  };
  CpuExecutionTotals cpu_execution_totals() const noexcept {
    CpuExecutionTotals totals;
    for (unsigned id = 0; id < processor_count_; ++id) {
      totals.ram_views.add(processor_ram_views(id));
      const auto& wait = bus_wait_[id];
      totals.bus_samples += wait.samples;
      totals.bus_contended += wait.contended;
      totals.bus_wait_ns += wait.wait_ns;
      const auto projected = processor_state_vector_steps(id);
      const auto& tsc = processor_tsc_access_stats(id);
      totals.tsc_access.rdtsc += tsc.rdtsc;
      totals.tsc_access.read_tsc += tsc.read_tsc;
      totals.tsc_access.write_tsc += tsc.write_tsc;
      totals.tsc_access.read_adjust += tsc.read_adjust;
      totals.tsc_access.write_adjust += tsc.write_adjust;
      totals.guest_steps += processor_steps(id);
      totals.crc32c_retired += processor_crc32c_retired(id);
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
    // Firmware input pauses the whole VM even if a secondary CPU is first
    // in the ready rotation.
    if (!pc_.guest_execution_available()) return;
    const ActiveClockRun elapsed(*this);
    using Clock=cosmic_pc::PcMachine::RunClock;
    const bool timed=deadline!=Clock::time_point::max();
    if(!timed)budget=std::min(budget,cosmic_pc::PcMachine::kDefaultRunSteps);
    refresh_parked_processors();
    if (!secondary_can_run() && wake_pending_.none()) {
      run_bsp(budget, deadline);
      refresh_parked_processor(0);
      poll_parked_processors();
      return;
    }
    // The budget counts work across the whole VM. Small calls keep a rotating
    // position so that every runnable processor gets a turn.
    bool register_peer = false;
    bool pause_admitted = false;
    bool defer_parked_parallel = false;
    while (budget && !faulted_processor_ && !powered_off_ && pc_.guest_execution_available()) {
      if(timed&&Clock::now()>=deadline)return;
      refresh_parked_processors();
      poll_parked_processors();
      // A wake survives a short run() or an exhausted host deadline. Service
      // it before starting another long worker grant, including with one lane.
      for (unsigned id = 0; id < processor_count_; ++id) {
        if (!wake_pending_[id]) continue;
        if (!budget || (timed && Clock::now() >= deadline)) return;
        if (!needs_service(apics_[id])) { wake_pending_.reset(id); continue; }
        parked_processors_.reset(id);
        parked_irq_processors_.reset(id);
        const auto before = processor_steps(id);
        // A run(1) can enter an IRQ and stop before its handler completes.
        // Carry the rest of the bounded wake grant into the next run.
        auto& wake_budget = wake_budget_[id];
        const auto grant = std::min(budget, wake_budget);
        if (id) run_secondary(id, grant, deadline, false);
        else run_bsp(grant, deadline);
        refresh_parked_processor(id);
        const auto retired = static_cast<unsigned>(processor_steps(id) - before);
        if (retired) {
          budget -= retired;
          wake_budget -= retired;
          if (!wake_budget || parked_processors_[id]) wake_pending_.reset(id);
        } else if (apics_[id].cpu->halted || !needs_service(apics_[id])) {
          wake_pending_.reset(id);
        } else return;
        if (faulted_processor_ || !pc_.guest_execution_available() || powered_off_) return;
      }
      if (!budget) return;
      std::array<unsigned, kMaxProcessors> runnable{};
      unsigned count = 0;
      bool sleeping = parked_irq_processors_.any();
      if (!active_clock_running_ || !pc_.cpu.halted) runnable[count++] = 0;
      sync_active_clock();
      for (unsigned id = 1; id < processor_count_; ++id) {
        auto& apic = apics_[id];
        if (!needs_service(apic)) continue;
        if (apic.cpu->halted && !apic.init_pending && !apic.startup_vector) {
          update_secondary_timer(apic);
          if (!deliverable_lapic_vector(apic)) { sleeping = true; continue; }
        }
        runnable[count++] = id;
      }
      // Firmware time advances through BSP steps, retaining its old polling
      // policy. Active time lets sleeping CPUs return their entire shares;
      // threaded dispatches sample their events inside the worker grant.
      if (!count) {
        const auto grant = std::min(budget, kSmpQuantum);
        const auto before = pc_.cpu.instructions;
        run_bsp(grant, deadline);
        refresh_parked_processor(0);
        poll_parked_processors();
        const auto retired = static_cast<unsigned>(pc_.cpu.instructions - before);
        budget -= wake_pending_.any() ? retired : grant;
        continue;
      }
      const auto dispatch_budget = sleeping && !active_clock_running_
          ? std::min(budget, count * kSmpQuantum) : budget;
      if (count == 2 && runnable[0] == 0 && !sleeping) {
        const auto id = runnable[1];
        if (active_clock_running_ && pc_.boot_acceleration_enabled() && apics_[id].startup_vector &&
            dispatch_budget >= 2 * kSmpQuantum && pc_.guest_execution_available()) {
          // A SIPI starts in 16-bit firmware. Service its first bounded slice
          // here before waking workers for two running instruction streams.
          const auto grant = std::min(dispatch_budget / 2, 128U);
          const auto before = total_processor_steps();
          register_peer = false;
          run_bsp(grant, deadline);
          if (!pc_.guest_execution_available() || powered_off_) return;
          run_secondary(id, grant, deadline, false);
          const auto retired = unsigned(total_processor_steps() - before);
          if (retired) { budget -= retired; continue; }
        }
        if (const auto retired = run_paired_loop(runnable[1], dispatch_budget, deadline, register_peer)) {
          budget -= retired;
          if (timed && pc_.cpu.halted && !pc_.cpu.flag(cosmic_pc::Cpu::IF) && budget < kMinCooperativeGrant &&
              grouped_loop_candidate(*apics_[runnable[1]].cpu)) return;
          continue;
        }
      }
      // Preserve the validated single-AP loop path when the parked BSP cannot
      // accept interrupts. A checked non-I/O loop cannot send a startup IPI.
      if (count == 1 && runnable[0] != 0 && !sleeping) {
        if (const auto retired = run_paired_loop(runnable[0], dispatch_budget, deadline, register_peer)) {
          budget -= retired;
          continue;
        }
      }
      register_peer = false;
      if (count == 1) {
        const auto grant = parked_needs_short_grant() ? std::min(dispatch_budget, kSmpQuantum) : dispatch_budget;
        const auto id = runnable[0];
        const auto before = processor_steps(id);
        if (id) run_secondary(id, grant, deadline, false);
        else run_bsp(grant, deadline);
        refresh_parked_processor(id);
        poll_parked_processors();
        const auto retired = static_cast<unsigned>(processor_steps(id) - before);
        budget -= wake_pending_.any() ? retired : grant;
        if (!sleeping && !parked_processors_.any()) return;
        continue;
      }
      // A quiet pair keeps its normal dispatch. In a large group of
      // pollers, the existing serial rotation admits ready CPUs through one
      // short turn before a worker can exhaust the window on an early job.
      const bool cooperative_pauses = timed && active_clock_running_ && threaded_smp_enabled_ && count > 2 &&
          std::count_if(runnable.begin(), runnable.begin() + count, [&](unsigned id) {
            return apics_[id].cpu->pause_wait_hint();
          }) >= 2;
      const bool pause_admission = cooperative_pauses && !pause_admitted && count > host_workers_capacity();
      if (pause_admission) {
        pause_admitted = true;
      } else {
        if (const auto completed = run_parallel_ether_loops(runnable, count, dispatch_budget, deadline,
                                                           defer_parked_parallel)) {
          budget -= completed;
          continue;
        }
        if (wake_pending_.any()) continue;
        if (const auto scheduled = run_threaded_smp(runnable, count, dispatch_budget, deadline, cooperative_pauses)) {
          budget -= scheduled;
          continue;
        }
      }
      if (wake_pending_.any()) continue;
      const auto quantum = pause_admission || parked_needs_short_grant()
          ? kSmpQuantum : smp_quantum(runnable, count);
      const auto chunk = std::min(dispatch_budget, count * quantum);
      const auto first_ready = std::lower_bound(runnable.begin(), runnable.begin() + count, next_serial_cpu_);
      const auto start = static_cast<unsigned>(first_ready - runnable.begin()) % count;
      const auto extra = chunk % count;
      const bool preserve_ether_loop_entry = cosmic_pc::Cpu::ether_arm64_host() &&
          pc_.boot_acceleration_enabled() && budget >= count * kMinParallelLoopSteps;
      const auto before = total_processor_steps();
      bool parked_during_round = false;
      bool admission_serviced = false;
      const auto grants = std::min(chunk, count);
      unsigned i = 0;
      for (; i < grants; ++i) {
        if (timed && Clock::now() >= deadline) return;
        poll_parked_processors();
        if (wake_pending_.any()) break;
        const auto slot = (start + i) % count;
        const auto id = runnable[slot];
        const auto work = chunk / count + (i < extra);
        next_serial_cpu_ = runnable[(slot + 1) % count];
        const auto started = pause_admission && !admission_serviced ? processor_steps(id) : 0;
        if (id) run_secondary(id, work, deadline, preserve_ether_loop_entry);
        else run_bsp(work, deadline, preserve_ether_loop_entry);
        if (pause_admission && !admission_serviced && processor_steps(id) != started) {
          ++threaded_smp_stats_.pause_admissions;
          admission_serviced = true;
        }
        parked_during_round |= refresh_parked_processor(id);
        poll_parked_processors();
        if (faulted_processor_ || !pc_.guest_execution_available() || powered_off_) return;
        if (wake_pending_.any()) break;
      }
      if (i == grants) next_serial_cpu_ = runnable[(start + extra) % count];
      budget -= pause_admission || wake_pending_.any() || parked_during_round
          ? static_cast<unsigned>(total_processor_steps() - before) : chunk;
    }
  }
  // Deterministic single guest-cycle advance; no host sleeper or background
  // worker. PC's canonical run loop calls this once per guest step.
  void advance_one(){poll();}
  void enable_ps2_keyboard(){keyboard_.emplace();install_firmware_input();}
  void install_firmware_input(){AcpiC06::install(pc_.cpu,ide_present(),bool(gpu_),bool(keyboard_));}
  void pointer(int dx,int dy,unsigned buttons){if(keyboard_)mouse_.input(dx,dy,buttons);}
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
