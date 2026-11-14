#pragma once
// C07: legacy-compatible PCI IDE primary channel, ATA PIO and exact BlockDisk.
#include "block_disk.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
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
  static constexpr unsigned kMaxMultipleSectors=16;
  struct Counters {
    std::uint64_t commands=0,read_sectors=0,write_sectors=0,identifies=0;
    std::uint64_t flushes=0,aborts=0,irqs_signaled=0;
    std::uint64_t identify_data_transfers=0;
    std::uint64_t batched_pio_bytes=0;
  };
  struct Backend {
    std::uint64_t reads=0,writes=0,flushes=0;
    std::chrono::nanoseconds read_time{},write_time{},flush_time{};
  };
  const Backend& backend() const noexcept { return backend_; }

 private:
  BlockDisk& disk_;
  std::function<void()> irq14_;
  std::array<std::uint8_t,kMaxMultipleSectors*512> buffer_{};
  enum class Transfer {Idle,Identify,Read,Write} transfer_=Transfer::Idle;
  std::uint8_t error_=1,features_=0,sector_count_=1,sector_number_=1;
  std::uint8_t cylinder_low_=0,cylinder_high_=0,drive_=0xe0,control_=0;
  std::uint8_t prev_count_=0,prev_sector_=0,prev_cyl_low_=0,prev_cyl_high_=0;
  std::uint8_t status_=0x50; // DRDY | DSC
  std::uint32_t remaining_=0;
  std::uint64_t lba_=0;
  std::size_t pos_=0;
  unsigned multiple_count_=0,group_sectors_=1,active_sectors_=1;
  bool ext_=false,interrupt_pending_=false;
  Counters stats_{};
  Backend backend_{};

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
    std::array<std::uint8_t,512> identify_data{};
    word(identify_data,0,0x0040); // Fixed disk.
    ata_text(identify_data,10,10,"REVERSE-C07-00000001");
    ata_text(identify_data,23,4,"C06P");
    ata_text(identify_data,27,20,"REVERSE QSSD VIRTUAL ATA DISK");
    word(identify_data,47,0x8000|kMaxMultipleSectors);
    word(identify_data,49,1u<<9); // LBA, using the PIO data port.
    word(identify_data,50,0x4000); // Valid capabilities; no extra capabilities.
    word(identify_data,59,std::uint16_t(0x0100U|multiple_count_));
    const auto capacity=disk_.sectors();
    const auto cap28=std::min<std::uint64_t>(capacity,0x0fffffffULL);
    word(identify_data,60,std::uint16_t(cap28));word(identify_data,61,std::uint16_t(cap28>>16));
    word(identify_data,80,1u<<6); // ATA/ATAPI-6 introduced the LBA48 commands.
    constexpr std::uint16_t commands=(1u<<13)|(1u<<12)|(1u<<10); // FLUSH EXT, FLUSH, LBA48
    word(identify_data,83,0x4000|commands);word(identify_data,84,0x4000);
    word(identify_data,86,commands);word(identify_data,87,0x4000);
    for(unsigned i=0;i<4;++i)word(identify_data,100+i,std::uint16_t(capacity>>(i*16)));
    word(identify_data,106,0x4000); // Valid 512-byte logical and physical geometry.
    identify_data[510]=0xa5; // ATA integrity word signature in the low byte.
    std::uint8_t sum=0;
    for(const auto byte:identify_data)sum=std::uint8_t(sum+byte);
    identify_data[511]=std::uint8_t(-sum);
    std::copy(identify_data.begin(),identify_data.end(),buffer_.begin());
    transfer_=Transfer::Identify;pos_=0;
    error_=0;status_=kReady|kDsc|kDrq;remaining_=active_sectors_=1;
    ++stats_.identifies;
    irq();
  }
  bool within() const {
    const auto limit=ext_?disk_.sectors():std::min<std::uint64_t>(disk_.sectors(),1ULL<<28);
    return remaining_!=0&&lba_<limit&&std::uint64_t(remaining_)<=limit-lba_;
  }
  bool advance_read() {
    active_sectors_=std::min<unsigned>(group_sectors_,remaining_);
    const auto started=std::chrono::steady_clock::now();
    bool ok=false;
    try {ok=disk_.read(lba_,static_cast<std::uint16_t>(active_sectors_),buffer_.data());}
    catch(const std::exception&) {}
    backend_.read_time+=std::chrono::steady_clock::now()-started;
    ++backend_.reads;
    if(!ok){
      // ATA MULTIPLE still presents the failing block with ERR and DRQ.
      // Bytes from an unreadable backing block are unavailable.
      if(multiple_transfer_){
        std::fill_n(buffer_.begin(),active_sectors_*512,0);
        read_error_=true;error_=0x40;status_=kReady|kDsc|kErr|kDrq;pos_=0;
        ++stats_.aborts;irq();
        return true;
      }
      fail(0x40);return false;
    }
    stats_.read_sectors+=active_sectors_;
    pos_=0;status_=kReady|kDsc|kDrq;irq();
    return true;
  }
  void finish_group() {
    pos_=0;
    const bool writing=transfer_==Transfer::Write;
    if (read_error_) {
      transfer_=Transfer::Idle;remaining_=0;status_=kReady|kDsc|kErr;
      return;
    }
    if(writing) {
      const auto started=std::chrono::steady_clock::now();
      bool ok=false;
      try {ok=disk_.write(lba_,static_cast<std::uint16_t>(active_sectors_),buffer_.data());}
      catch(const std::exception&) {}
      backend_.write_time+=std::chrono::steady_clock::now()-started;
      ++backend_.writes;
      if(!ok){fail(0x40);return;}
      stats_.write_sectors+=active_sectors_;
    }
    remaining_-=active_sectors_;
    if(remaining_) {
      lba_+=active_sectors_;
      sector_count_=std::uint8_t(remaining_);
      if(ext_)prev_count_=std::uint8_t(remaining_>>8);
      sector_number_=std::uint8_t(lba_);
      cylinder_low_=std::uint8_t(lba_>>8);
      cylinder_high_=std::uint8_t(lba_>>16);
      if(transfer_==Transfer::Read){(void)advance_read();}
      else if(writing) {
        active_sectors_=std::min<unsigned>(group_sectors_,remaining_);
        status_=kReady|kDsc|kDrq;
        irq();
      }
    }else{
      if(transfer_==Transfer::Identify)++stats_.identify_data_transfers;
      transfer_=Transfer::Idle;
      status_=kReady|kDsc;sector_count_=0;
      // WRITE completion is an interrupt. READ/IDENTIFY signal DRQ ready.
      if(writing)irq();
    }
  }
  bool read_error_=false;
  bool multiple_transfer_=false;
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
    read_error_=multiple_transfer_=false;
    if(cmd==0xec){identify();return;}
    if(cmd==0xe7||cmd==0xea) {
      const auto started=std::chrono::steady_clock::now();
      bool ok=false;
      try{ok=disk_.flush();}catch(const std::exception&){ok=false;}
      backend_.flush_time+=std::chrono::steady_clock::now()-started;
      ++backend_.flushes;
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
    if(cmd==0xc6) { // SET MULTIPLE MODE
      const unsigned count=sector_count_;
      if(count>kMaxMultipleSectors||(count&&(count&(count-1)))){fail(0x04);return;}
      multiple_count_=count;error_=0;status_=kReady|kDsc;irq();return;
    }
    const bool multiple=cmd==0xc4||cmd==0xc5||cmd==0x29||cmd==0x39;
    const bool rd=cmd==0x20||cmd==0x24||cmd==0xc4||cmd==0x29;
    const bool wr=cmd==0x30||cmd==0x34||cmd==0xc5||cmd==0x39;
    if(!rd&&!wr){fail(0x04);return;}
    if(multiple&&!multiple_count_){fail(0x04);return;}
    if(!lba_mode()){fail(0x04);return;} // CHS unsupported, not silently remapped
    ext_=cmd==0x24||cmd==0x34||cmd==0x29||cmd==0x39;
    multiple_transfer_=multiple;
    remaining_=ext_ ? ((std::uint32_t(prev_count_)<<8)|sector_count_) : sector_count_;
    if(!remaining_) remaining_=ext_?65536U:256U;
    lba_=task_lba(ext_);
    if(!within()) {fail(0x10);return;}
    transfer_=rd?Transfer::Read:Transfer::Write;
    group_sectors_=multiple?multiple_count_:1;
    active_sectors_=std::min<unsigned>(group_sectors_,remaining_);
    error_=0;status_=kReady|kDsc|kDrq;
    if(rd)(void)advance_read();
    // For WRITE, initial DRQ does not emit IRQ. An IRQ follows each sector.
  }
  std::optional<std::uint32_t> read_data(unsigned width) {
    if(width!=2&&width!=4)return std::nullopt;
    if(transfer_!=Transfer::Read&&transfer_!=Transfer::Identify)return std::nullopt;
    if(!(status_&kDrq)||pos_+width>active_sectors_*512)return std::nullopt;
    std::uint32_t v=0;
    for(unsigned i=0;i<width;++i)v|=std::uint32_t(buffer_[pos_++])<<(i*8);
    if(pos_==active_sectors_*512)finish_group();
    return v;
  }
  bool write_data(unsigned width,std::uint32_t value) {
    if(width!=2&&width!=4)return false;
    if(transfer_!=Transfer::Write||!(status_&kDrq)||pos_+width>active_sectors_*512)return false;
    for(unsigned i=0;i<width;++i)buffer_[pos_++]=std::uint8_t(value>>(i*8));
    if(pos_==active_sectors_*512)finish_group();
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
  void reset(bool device_only=false) {
    transfer_=Transfer::Idle;pos_=0;remaining_=0;multiple_count_=0;group_sectors_=active_sectors_=1;
    error_=1;features_=0;sector_count_=1;sector_number_=1;
    cylinder_low_=cylinder_high_=prev_count_=prev_sector_=prev_cyl_low_=prev_cyl_high_=0;
    drive_=0xe0;control_=0;status_=kReady|kDsc;
    lba_=0;ext_=false;interrupt_pending_=false;read_error_=multiple_transfer_=false;
    if(!device_only)backend_={};
  }
  bool handles(std::uint16_t port) const {
    return (port>=0x1f0&&port<=0x1f7)||port==0x3f6;
  }
  unsigned pio_batch_limit(unsigned width,bool input,unsigned elements) const noexcept {
    if((width!=2&&width!=4)||!selected_master()||!(status_&kDrq)||
       transfer_!=(input?Transfer::Read:Transfer::Write))return 0;
    const auto available=(active_sectors_*512-pos_)/width;
    return available>1?std::min(elements,unsigned(available-1)):0;
  }
  bool pio_batch_transfer(unsigned width,bool input,std::uint8_t* bytes,unsigned elements) noexcept {
    if(elements<2||pio_batch_limit(width,input,elements)!=elements)return false;
    const auto size=width*elements;
    if(input)std::memcpy(bytes,buffer_.data()+pos_,size);
    else std::memcpy(buffer_.data()+pos_,bytes,size);
    pos_+=size;
    stats_.batched_pio_bytes+=size;
    return true;
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
        if(control_&4U){reset(true);control_=v;status_=0x80;return true;}
        if(was_reset){reset(true);control_=v;}
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
