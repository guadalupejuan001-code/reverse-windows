#pragma once
// C07: legacy-compatible PCI IDE primary channel, ATA PIO and exact BlockDisk.
// No emulated Windows driver, no second disk, no DMA or AHCI claims.
#include "block_disk.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace reverse_windows {
class AtaC07 {
 public:
  static constexpr std::uint16_t kData=0x1f0;
  static constexpr std::uint16_t kLast=0x1f7;
  static constexpr std::uint16_t kControl=0x3f6;
  struct Counters {
    std::uint64_t commands=0,read_sectors=0,write_sectors=0,identifies=0;
    std::uint64_t flushes=0,aborts=0,irqs_signaled=0;
    std::uint64_t identify_data_transfers=0;
  };

 private:
  BlockDisk& disk_;
  std::function<void()> irq14_;
  std::array<std::uint8_t,512> sector_{};
  enum class Transfer {Idle,Identify,Read,Write} transfer_=Transfer::Idle;
  std::uint8_t error_=1,features_=0,sector_count_=1,sector_number_=1;
  std::uint8_t cylinder_low_=0,cylinder_high_=0,drive_=0xe0,control_=0;
  std::uint8_t prev_count_=0,prev_sector_=0,prev_cyl_low_=0,prev_cyl_high_=0;
  std::uint8_t status_=0x50; // DRDY | DSC
  std::uint32_t remaining_=0;
  std::uint64_t lba_=0;
  std::size_t pos_=0;
  bool ext_=false,interrupt_pending_=false;
  Counters stats_{};

  static constexpr std::uint8_t kErr=0x01,kDrq=0x08,kReady=0x40,kDsc=0x10;
  bool selected_master() const {return (drive_&0x10U)==0;}
  bool lba_mode() const {return (drive_&0x40U)!=0;}
  bool irq_enabled() const {return (control_&2U)==0;}
  void signal_irq() {
    if(irq14_){irq14_();++stats_.irqs_signaled;}
  }
  void irq() {
    interrupt_pending_=true;
    if(irq_enabled())signal_irq();
  }
  void fail(std::uint8_t error) {
    transfer_=Transfer::Idle;remaining_=0;pos_=0;
    error_=error;status_=kReady|kDsc|kErr; ++stats_.aborts;
    irq();
  }
  static void word(std::array<std::uint8_t,512>& data,unsigned i,std::uint16_t v) {
    data[i*2]=std::uint8_t(v);data[i*2+1]=std::uint8_t(v>>8);
  }
  static void ata_text(std::array<std::uint8_t,512>& b,unsigned begin,
                       unsigned words,const std::string& s) {
    std::string text=s;
    text.resize(std::size_t(words)*2,' ');
    for(unsigned i=0;i<words;++i)word(b,begin+i,
      (std::uint16_t(std::uint8_t(text[i*2]))<<8)|
       std::uint8_t(text[i*2+1]));
  }
  void identify() {
    sector_.fill(0);
    word(sector_,0,0x0040); // ATA fixed disk, not ATAPI
    ata_text(sector_,10,10,"REVERSE-C07-00000001");
    ata_text(sector_,23,4,"C07");
    ata_text(sector_,27,20,"REVERSE QSSD VIRTUAL ATA DISK");
    word(sector_,47,0x8000); // No READ/WRITE MULTIPLE command support.
    word(sector_,49,1u<<9); // LBA, using the PIO data port.
    word(sector_,50,0x4000); // Valid capabilities; no extra capabilities.
    const auto capacity=disk_.sectors();
    const auto cap28=std::min<std::uint64_t>(capacity,0x0fffffffULL);
    word(sector_,60,std::uint16_t(cap28));word(sector_,61,std::uint16_t(cap28>>16));
    word(sector_,80,1u<<6); // ATA/ATAPI-6 introduced the LBA48 commands.
    constexpr std::uint16_t commands=(1u<<13)|(1u<<12)|(1u<<10); // FLUSH EXT, FLUSH, LBA48
    word(sector_,83,0x4000|commands);word(sector_,84,0x4000);
    word(sector_,86,commands);word(sector_,87,0x4000);
    for(unsigned i=0;i<4;++i)word(sector_,100+i,std::uint16_t(capacity>>(i*16)));
    word(sector_,106,0x4000); // Valid 512-byte logical and physical geometry.
    sector_[510]=0xa5; // ATA integrity word signature in the low byte.
    std::uint8_t sum=0;
    for(const auto byte:sector_)sum=std::uint8_t(sum+byte);
    sector_[511]=std::uint8_t(-sum);
    transfer_=Transfer::Identify;pos_=0;
    error_=0;status_=kReady|kDsc|kDrq;remaining_=1;
    ++stats_.identifies;
    irq();
  }
  bool within() const {
    return remaining_!=0 && lba_<=disk_.sectors() &&
           std::uint64_t(remaining_)<=disk_.sectors()-lba_;
  }
  bool advance_read() {
    if(!within()) {fail(0x10);return false;} // IDNF: address beyond disk
    try {
      if(!disk_.read(lba_,1,sector_.data())) {fail(0x40);return false;}
    }catch(const std::exception&) {fail(0x40);return false;}
    ++stats_.read_sectors;
    pos_=0;status_=kReady|kDsc|kDrq;irq();
    return true;
  }
  void finish_sector() {
    pos_=0;
    if(transfer_==Transfer::Write) {
      if(!within()) {fail(0x10);return;}
      try {
        if(!disk_.write(lba_,1,sector_.data())) {fail(0x40);return;}
      }catch(const std::exception&) {fail(0x40);return;}
      ++stats_.write_sectors;
    }
    if(remaining_)--remaining_;
    if(remaining_) {
      ++lba_;
      sector_count_=std::uint8_t(remaining_);
      if(ext_)prev_count_=std::uint8_t(remaining_>>8);
      sector_number_=std::uint8_t(lba_);
      cylinder_low_=std::uint8_t(lba_>>8);
      cylinder_high_=std::uint8_t(lba_>>16);
      if(transfer_==Transfer::Read){(void)advance_read();}
      else if(transfer_==Transfer::Write) {
        sector_.fill(0);status_=kReady|kDsc|kDrq;
        irq();
      }
    }else{
      if(transfer_==Transfer::Identify)++stats_.identify_data_transfers;
      transfer_=Transfer::Idle;
      status_=kReady|kDsc;sector_count_=0;
      // WRITE completion is an interrupt. READ/IDENTIFY signal DRQ ready.
      if(transfer_finished_write_)irq();
    }
  }
  bool transfer_finished_write_=false;
  std::uint64_t task_lba(bool ext)const {
    if(ext) return (std::uint64_t(prev_cyl_high_)<<40)|
                    (std::uint64_t(prev_cyl_low_)<<32)|
                    (std::uint64_t(prev_sector_)<<24)|
                    (std::uint64_t(cylinder_high_)<<16)|
                    (std::uint64_t(cylinder_low_)<<8)|sector_number_;
    return (std::uint64_t(drive_&15U)<<24)|
           (std::uint64_t(cylinder_high_)<<16)|
           (std::uint64_t(cylinder_low_)<<8)|sector_number_;
  }
  void execute(std::uint8_t cmd) {
    ++stats_.commands;
    interrupt_pending_=false;
    transfer_=Transfer::Idle;pos_=0;remaining_=0;
    transfer_finished_write_=false;
    if(cmd==0xec){identify();return;}
    if(cmd==0xe7||cmd==0xea) {
      bool ok=false;
      try{ok=disk_.flush();}catch(const std::exception&){ok=false;}
      if(!ok){fail(0x40);return;}
      ++stats_.flushes;error_=0;status_=kReady|kDsc;irq();return;
    }
    if(cmd==0xef) {
      // Default or explicit PIO 0–2 require the same virtual data path.
      const bool pio=features_==0x03 &&
          (sector_count_<=1 || (sector_count_>=0x08 && sector_count_<=0x0a));
      if(pio||features_==0x82||features_==0x02) {
        error_=0;status_=kReady|kDsc;irq();return;
      }
      fail(0x04);return;
    }
    const bool rd=cmd==0x20||cmd==0x24;
    const bool wr=cmd==0x30||cmd==0x34;
    if(!rd&&!wr){fail(0x04);return;}
    if(!lba_mode()){fail(0x04);return;} // CHS unsupported, not silently remapped
    ext_=cmd==0x24||cmd==0x34;
    remaining_=ext_ ? ((std::uint32_t(prev_count_)<<8)|sector_count_) : sector_count_;
    if(!remaining_) remaining_=ext_?65536U:256U;
    lba_=task_lba(ext_);
    if(!within()) {fail(0x10);return;}
    transfer_=rd?Transfer::Read:Transfer::Write;
    transfer_finished_write_=wr;
    error_=0;status_=kReady|kDsc|kDrq;
    sector_.fill(0);
    if(rd)(void)advance_read();
    // For WRITE, initial DRQ does not emit IRQ. An IRQ follows each sector.
  }
  std::optional<std::uint32_t> read_data(unsigned width) {
    if(width!=2&&width!=4)return std::nullopt;
    if(transfer_!=Transfer::Read&&transfer_!=Transfer::Identify)return std::nullopt;
    if(!(status_&kDrq)||pos_+width>512)return std::nullopt;
    std::uint32_t v=0;
    for(unsigned i=0;i<width;++i)v|=std::uint32_t(sector_[pos_++])<<(i*8);
    if(pos_==512)finish_sector();
    return v;
  }
  bool write_data(unsigned width,std::uint32_t value) {
    if(width!=2&&width!=4)return false;
    if(transfer_!=Transfer::Write||!(status_&kDrq)||pos_+width>512)return false;
    for(unsigned i=0;i<width;++i)sector_[pos_++]=std::uint8_t(value>>(i*8));
    if(pos_==512)finish_sector();
    return true;
  }

 public:
  explicit AtaC07(BlockDisk& disk,std::function<void()> signal_irq14)
    :disk_(disk),irq14_(std::move(signal_irq14)) {
    if(disk_.sectors()==0||disk_.bytes()!=disk_.sectors()*512ULL ||
       disk_.sectors() >= (1ULL<<48))
      throw std::invalid_argument("C07 ATA requires 512-byte sector BlockDisk within LBA48");
  }
  AtaC07(const AtaC07&)=delete;
  AtaC07& operator=(const AtaC07&)=delete;
  void reset() {
    transfer_=Transfer::Idle;sector_.fill(0);pos_=0;remaining_=0;
    error_=1;features_=0;sector_count_=1;sector_number_=1;
    cylinder_low_=cylinder_high_=prev_count_=prev_sector_=prev_cyl_low_=prev_cyl_high_=0;
    drive_=0xe0;control_=0;status_=kReady|kDsc;
    lba_=0;ext_=false;interrupt_pending_=false;transfer_finished_write_=false;
  }
  bool handles(std::uint16_t port) const {
    return (port>=0x1f0&&port<=0x1f7)||port==0x3f6;
  }
  std::optional<std::uint32_t> input(std::uint16_t p,unsigned width) {
    if(!handles(p))return std::nullopt;
    if(!selected_master())return 0; // The empty slave has no task-file registers.
    if(p==kData)return read_data(width);
    if(width!=1)return std::nullopt;
    switch(p) {
      case 0x1f1:return error_;
      case 0x1f2:return sector_count_;
      case 0x1f3:return sector_number_;
      case 0x1f4:return cylinder_low_;
      case 0x1f5:return cylinder_high_;
      case 0x1f6:return drive_;
      case 0x1f7:interrupt_pending_=false;return status_;
      case 0x3f6:return status_; // alternate status never acknowledges IRQ
      default:return std::nullopt;
    }
  }
  bool output(std::uint16_t p,unsigned width,std::uint32_t value) {
    if(!handles(p))return false;
    if(p==kData)return selected_master() ? write_data(width,value) : (width==2||width==4);
    if(width!=1)return false;
    const auto v=std::uint8_t(value);
    if(p==0x1f6){drive_=v;return true;}
    if(p!=kControl&&(!selected_master()||(control_&4U)))return true;
    switch(p) {
      case 0x1f1:features_=v;return true;
      case 0x1f2:prev_count_=sector_count_;sector_count_=v;return true;
      case 0x1f3:prev_sector_=sector_number_;sector_number_=v;return true;
      case 0x1f4:prev_cyl_low_=cylinder_low_;cylinder_low_=v;return true;
      case 0x1f5:prev_cyl_high_=cylinder_high_;cylinder_high_=v;return true;
      case 0x1f7:execute(v);return true;
      case 0x3f6:{
        const auto was_reset=(control_&4U)!=0;
        const auto was_enabled=irq_enabled();
        control_=v;
        if(control_&4U){reset();control_=v;status_=0x80;return true;}
        if(was_reset){reset();control_=v;}
        else if(!was_enabled&&irq_enabled()&&interrupt_pending_)signal_irq();
        return true;
      }
      default:return false;
    }
  }
  Counters counters()const{return stats_;}
  bool irq_pending()const{return interrupt_pending_;}
  bool ready()const{return selected_master() && (status_&kReady)!=0;}
  std::uint8_t status()const{return selected_master()?status_:0;}
  std::uint8_t error()const{return selected_master()?error_:0;}
};
} // namespace reverse_windows
