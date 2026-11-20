#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace reverse_windows {

class AtapiCdrom {
 public:
  using Reader = std::function<bool(std::uint64_t, std::uint8_t*, std::size_t)>;
  static constexpr std::size_t sector_bytes = 2048;
  struct Activity {
    std::uint64_t packets=0, read_sectors=0, rejected_packets=0;
    std::uint8_t last_packet=0, last_rejected_packet=0, last_sense=0, last_asc=0;
  };

  AtapiCdrom(Reader read, std::uint64_t bytes, std::function<void()> irq)
      : read_(std::move(read)), irq_(std::move(irq)), blocks_(bytes / sector_bytes) {
    if (!read_ || !blocks_ || blocks_ > std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("Capacidad ATAPI fuera de rango.");
    reset();
  }

  static bool handles(std::uint16_t port) {
    return (port >= 0x170 && port <= 0x177) || port == 0x376;
  }

  void reset() {
    control_ = drive_ = features_ = sense_ = asc_ = 0;
    activity_ = {};
    reset_task_file();
  }

  const Activity& activity() const noexcept { return activity_; }

  std::uint32_t input(std::uint16_t port, unsigned width) {
    if (drive_ & 0x10U) return 0; // No secondary slave.
    if (port == 0x170 && (width == 2 || width == 4)) {
      std::uint32_t value = 0;
      for (unsigned lane = 0; lane < width; lane += 2)
        value |= std::uint32_t(data_word()) << (8 * lane);
      return value;
    }
    if (width != 1) return 0;
    switch (port) {
      case 0x171: return error_;
      case 0x172: return reason_;
      case 0x173: return sector_number_;
      case 0x174: return byte_count_ & 255U;
      case 0x175: return byte_count_ >> 8U;
      case 0x176: return drive_;
      case 0x177: case 0x376: return status_;
      default: return 0;
    }
  }

  bool output(std::uint16_t port, unsigned width, std::uint32_t value) {
    if (port == 0x170 && (width == 2 || width == 4)) {
      if (!(drive_ & 0x10U)) {
        packet_word(static_cast<std::uint16_t>(value));
        if (width == 4) packet_word(static_cast<std::uint16_t>(value >> 16U));
      }
      return true;
    }
    if (width != 1) return false;
    const auto byte = static_cast<std::uint8_t>(value);
    if (port == 0x176) { drive_ = byte; return true; }
    if (port == 0x376) {
      const auto previous = control_;
      control_ = byte;
      if (control_ & 4U) status_ = 0x80;
      else if (previous & 4U) reset_task_file();
      return true;
    }
    if (drive_ & 0x10U || control_ & 4U) return true;
    switch (port) {
      case 0x171: features_ = byte; return true;
      case 0x172: reason_ = byte; return true;
      case 0x173: sector_number_ = byte; return true;
      case 0x174: byte_count_ = (byte_count_ & 0xff00U) | byte; return true;
      case 0x175: byte_count_ = (byte_count_ & 0x00ffU) | (unsigned(byte) << 8U); return true;
      case 0x177: command(byte); return true;
      default: return false;
    }
  }

 private:
  enum class Phase { Idle, Packet, Identify, DataIn };
  static constexpr std::uint8_t ready = 0x40, drq = 0x08;
  Reader read_;
  std::function<void()> irq_;
  const std::uint64_t blocks_;
  Phase phase_ = Phase::Idle;
  std::uint8_t control_=0, drive_=0, features_=0, status_=ready;
  std::uint8_t error_=1, reason_=1, sector_number_=1, sense_=0, asc_=0;
  unsigned byte_count_=0xeb14, limit_=0xfffe;
  std::array<std::uint8_t,12> packet_{};
  std::size_t packet_pos_=0, position_=0, phase_end_=0;
  std::vector<std::uint8_t> data_;
  std::uint32_t next_lba_=0, sectors_left_=0;
  Activity activity_;

  void reset_task_file() {
    phase_ = Phase::Idle;
    status_ = ready;
    error_ = reason_ = sector_number_ = 1;
    byte_count_ = 0xeb14; // ATAPI reset signature in cylinder registers.
    data_.clear();
    sectors_left_ = 0;
  }

  void irq() const { if (!(control_ & 2U) && irq_) irq_(); }

  void finish() {
    phase_ = Phase::Idle;
    reason_ = 3; // I/O and CoD set: command status phase.
    byte_count_ = 0;
    status_ = ready;
    error_ = 0;
    irq();
  }

  void fail(std::uint8_t key, std::uint8_t asc) {
    sense_ = key;
    asc_ = asc;
    phase_ = Phase::Idle;
    sectors_left_ = 0;
    reason_ = 3;
    byte_count_ = 0;
    error_ = static_cast<std::uint8_t>((key << 4U) | 4U);
    status_ = ready | 1U;
    irq();
  }

  void fail_packet(std::uint8_t key, std::uint8_t asc) {
    ++activity_.rejected_packets;
    activity_.last_rejected_packet = activity_.last_packet;
    activity_.last_sense = key;
    activity_.last_asc = asc;
    fail(key, asc);
  }

  static std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24U) | (std::uint32_t(p[1]) << 16U) |
           (std::uint32_t(p[2]) << 8U) | p[3];
  }

  static unsigned be16(const std::uint8_t* p) { return (unsigned(p[0]) << 8U) | p[1]; }

  static void set_be32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (24 - i * 8U));
  }

  void data_phase() {
    const auto remaining = data_.size() - position_;
    byte_count_ = static_cast<unsigned>(std::min<std::size_t>(remaining, limit_));
    phase_end_ = position_ + byte_count_;
    phase_ = Phase::DataIn;
    reason_ = 2; // I/O set, CoD clear: data from device.
    status_ = ready | drq;
    irq();
  }

  void response(std::vector<std::uint8_t> bytes, std::size_t allocation) {
    bytes.resize(std::min(bytes.size(), allocation));
    if (bytes.empty()) { finish(); return; }
    data_ = std::move(bytes);
    position_ = 0;
    data_phase();
  }

  void read_sector() {
    data_.resize(sector_bytes);
    if (!read_(std::uint64_t(next_lba_) * sector_bytes, data_.data(), data_.size())) {
      fail_packet(3, 0x11); // MEDIUM ERROR, unrecovered read error.
      return;
    }
    ++next_lba_;
    ++activity_.read_sectors;
    --sectors_left_;
    position_ = 0;
    data_phase();
  }

  void begin_read(std::uint32_t lba, std::uint32_t count) {
    if (!count) { finish(); return; }
    if (lba >= blocks_ || count > blocks_ - lba) { fail_packet(5, 0x21); return; }
    next_lba_ = lba;
    sectors_left_ = count;
    read_sector();
  }

  std::uint16_t data_word() {
    if (phase_ != Phase::Identify && phase_ != Phase::DataIn) return 0;
    std::uint16_t word = data_[position_++];
    if (position_ < phase_end_) word |= std::uint16_t(data_[position_++]) << 8U;
    if (position_ != phase_end_) return word;
    if (phase_ == Phase::Identify) {
      phase_ = Phase::Idle;
      status_ = ready;
    } else if (position_ < data_.size()) data_phase();
    else if (sectors_left_) read_sector();
    else finish();
    return word;
  }

  void identify() {
    data_.assign(512, 0);
    auto word = [this](std::size_t i, std::uint16_t value) {
      data_[i * 2] = static_cast<std::uint8_t>(value);
      data_[i * 2 + 1] = static_cast<std::uint8_t>(value >> 8U);
    };
    auto name = [&word](std::size_t start, std::size_t size, const char* text) {
      const auto length = std::char_traits<char>::length(text);
      for (std::size_t i = 0; i < size; i += 2) {
        const auto first = i < length ? text[i] : ' ';
        const auto second = i + 1 < length ? text[i+1] : ' ';
        word(start + i/2, std::uint16_t(first) << 8U | std::uint8_t(second));
      }
    };
    word(0, 0x8580); // Removable CD-ROM, 12-byte packet, no packet-ready IRQ.
    name(10, 20, "REVERSE-CD-0001");
    name(23, 8, "0.1");
    name(27, 40, "REVERSE ATAPI CD-ROM");
    word(49, 0x0200); // LBA, PIO only.
    word(53, 2);
    word(64, 3); // PIO modes 3 and 4.
    word(80, 0x003e);
    phase_ = Phase::Identify;
    position_ = 0;
    phase_end_ = data_.size();
    status_ = ready | drq;
    irq();
  }

  void command(std::uint8_t op) {
    phase_ = Phase::Idle;
    sectors_left_ = 0;
    error_ = 0;
    switch (op) {
      case 0x08: reset_task_file(); return;
      case 0xa1: identify(); return;
      case 0xa0:
        if (features_ & 3U) { fail(5, 0x24); return; } // DMA and overlap.
        limit_ = byte_count_ & 0xfffeU;
        if (!limit_) limit_ = 0xfffe;
        packet_pos_ = 0;
        phase_ = Phase::Packet;
        reason_ = 1;
        status_ = ready | drq;
        return;
      case 0xef: // PIO transfer mode or enable/disable read look-ahead.
        if (features_ == 0x03 || features_ == 0xaa || features_ == 0x55) { finish(); return; }
        break;
      case 0xec: // IDENTIFY DEVICE must return the packet device signature.
        reset_task_file();
        status_ = ready | 1U;
        error_ = 4;
        irq();
        return;
      default: break;
    }
    fail(5, 0x20);
  }

  void packet_word(std::uint16_t value) {
    if (phase_ != Phase::Packet) return;
    packet_[packet_pos_++] = static_cast<std::uint8_t>(value);
    packet_[packet_pos_++] = static_cast<std::uint8_t>(value >> 8U);
    if (packet_pos_ == packet_.size()) {
      ++activity_.packets;
      activity_.last_packet = packet_[0];
      execute_packet();
    }
  }

  void mode_sense() {
    const auto page = packet_[2] & 0x3fU;
    const auto control = packet_[2] >> 6;
    if (packet_[3] || (page != 0x01 && page != 0x2a && page != 0x3f)) {
      fail_packet(5, 0x24); // INVALID FIELD IN CDB.
      return;
    }
    if (control == 3) {
      fail_packet(5, 0x39); // SAVING PARAMETERS NOT SUPPORTED.
      return;
    }
    // MODE SENSE (10) header, data-only disc and zero block descriptors.
    std::vector<std::uint8_t> result{0,0,1,0,0,0,0,0};
    if (page == 0x01 || page == 0x3f) {
      // Read error recovery parameters are fixed. Failed reads report MEDIUM ERROR.
      result.insert(result.end(), {0x01,0x06,0,0,0,0,0,0});
    }
    if (page == 0x2a || page == 0x3f) {
      // Read-only capabilities. No writable media, audio, eject or changer.
      const auto start = result.size();
      result.resize(start + 20);
      result[start] = 0x2a;
      result[start + 1] = 18;
      if (control != 1) result[start + 13] = sector_bytes / 1024;
    }
    const auto length = result.size() - 2;
    result[0] = std::uint8_t(length >> 8);
    result[1] = std::uint8_t(length);
    response(std::move(result), be16(packet_.data() + 7));
  }

  void execute_packet() {
    switch (packet_[0]) {
      case 0x00: case 0x1e: finish(); return; // TEST UNIT READY / PREVENT ALLOW
      case 0x03: { // REQUEST SENSE, fixed format
        std::vector<std::uint8_t> result(18, 0);
        result[0] = 0x70;
        result[2] = sense_;
        result[7] = 10;
        result[12] = asc_;
        sense_ = asc_ = 0;
        response(std::move(result), packet_[4]);
        return;
      }
      case 0x12: { // Standard INQUIRY
        if (packet_[1] & 1U) { fail_packet(5, 0x24); return; }
        std::vector<std::uint8_t> result(36, 0);
        result[0] = 5; result[1] = 0x80; result[2] = 2; result[3] = 2; result[4] = 31;
        std::copy_n("REVERSE ", 8, result.begin() + 8);
        std::copy_n("Virtual CD-ROM  ", 16, result.begin() + 16);
        std::copy_n("0.1 ", 4, result.begin() + 32);
        response(std::move(result), packet_[4]);
        return;
      }
      case 0x25: { // READ CAPACITY (10)
        std::vector<std::uint8_t> result(8);
        set_be32(result.data(), static_cast<std::uint32_t>(blocks_ - 1));
        set_be32(result.data() + 4, sector_bytes);
        response(std::move(result), 8);
        return;
      }
      case 0x28: begin_read(be32(packet_.data()+2), be16(packet_.data()+7)); return;
      case 0xa8: begin_read(be32(packet_.data()+2), be32(packet_.data()+6)); return;
      case 0x2b: // SEEK (10)
        if (be32(packet_.data()+2) < blocks_) finish();
        else fail_packet(5, 0x21);
        return;
      case 0x43: { // READ TOC, format 0, one data track and lead-out.
        if ((packet_[2] & 15U) || packet_[6] > 1 || (packet_[1] & 2U)) {
          fail_packet(5, 0x24);
          return;
        }
        std::vector<std::uint8_t> result{0,18,1,1,0,0x14,1,0,0,0,0,0,
                                         0,0x14,0xaa,0,0,0,0,0};
        set_be32(result.data()+16, static_cast<std::uint32_t>(blocks_));
        response(std::move(result), be16(packet_.data()+7));
        return;
      }
      case 0x46: { // GET CONFIGURATION, CD-ROM current profile header.
        std::vector<std::uint8_t> result{0,0,0,4,0,0,0,8};
        response(std::move(result), be16(packet_.data()+7));
        return;
      }
      case 0x5a: mode_sense(); return;
      case 0x2a: case 0xaa: fail_packet(7, 0x27); return; // DATA PROTECT: read-only ISO.
      default: fail_packet(5, 0x20);
    }
  }
};
} // namespace reverse_windows
