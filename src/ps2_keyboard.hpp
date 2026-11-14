#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>

namespace reverse_windows {

// Optional keyboard device on top of the existing PC/AT controller. The
// controller still owns its configuration and A20, including 0xD0/0xD1.
class Ps2Keyboard final {
 public:
  static constexpr std::size_t kQueueLimit = 64;

  void reset() { *this = Ps2Keyboard{}; }

  void controller_command(std::uint8_t command) {
    if (command == 0x60 || command == 0xd1) controller_pending_ = command;
    if (command == 0xad) config_ |= 0x10;
    if (command == 0xae) config_ &= ~0x10;
  }

  bool controller_expects_data() const noexcept { return controller_pending_ != 0; }

  void controller_data(std::uint8_t data) {
    if (controller_pending_ == 0x60) config_ = data;
    controller_pending_ = 0;
  }

  bool has_data() const noexcept { return !responses_.empty() || !keys_.empty(); }
  bool needs_irq() const noexcept { return (config_ & 1) && has_data() && !signaled_; }
  void mark_irq() noexcept { signaled_ = true; }

  // Real-mode guests can enable IRQ1 or explicitly request scan data for polling.
  bool guest_uses_scancodes() const noexcept { return (config_ & 1) || scanning_requested_; }

  std::uint8_t read_data() {
    auto& queue = responses_.empty() ? keys_ : responses_;
    if (queue.empty()) return 0xff;
    last_byte_ = queue.front();
    queue.pop_front();
    signaled_ = false;
    return last_byte_;
  }

  bool inject(unsigned code, bool pressed) {
    if (!enabled_ || (config_ & 0x10) || (code > 0xff && (code & ~255U) != 0xe000U))
      return false;
    const auto input = static_cast<std::uint8_t>(code);
    const bool extended = (code >> 8) == 0xe0;
    if (!input || input >= set2_.size()) return false;
    const bool translated = (config_ & 0x40) || scan_set_ == 1;
    const auto output = translated ? input : (extended ? extended_set2(input) : set2_[input]);
    if (!output || keys_.size() + 3 > kQueueLimit) return false;
    if (extended) keys_.push_back(0xe0);
    if (!pressed && !translated) keys_.push_back(0xf0);
    keys_.push_back(pressed || !translated ? output : static_cast<std::uint8_t>(output | 0x80));
    return true;
  }

  bool command(std::uint8_t value) {
    // Reserve the maximum response length before changing command state.
    if (responses_.size() + 3 > kQueueLimit) return false;
    if (parameter_ != 0) {
      const auto command = parameter_;
      parameter_ = 0;
      if (command == 0xf0) {
        if (!value) return respond({0xfa, scan_set_});
        if (value > 2) return respond({0xfe});
        scan_set_ = value;
      }
      return respond({0xfa});
    }
    switch (value) {
      case 0xed: case 0xf0: case 0xf3:
        parameter_ = value;
        return respond({0xfa});
      case 0xee: return respond({0xee}); // Echo.
      case 0xf2: return respond({0xfa, 0xab, 0x83});
      case 0xf4: enabled_ = true; scanning_requested_ = true; return respond({0xfa});
      case 0xf5: defaults(); enabled_ = false; return respond({0xfa});
      case 0xf6: defaults(); enabled_ = true; return respond({0xfa});
      case 0xfe: return respond({last_byte_});
      case 0xff: defaults(); enabled_ = true; return respond({0xfa, 0xaa});
      default: return respond({0xfe}); // Unsupported command requests resend.
    }
  }

 private:
  inline static constexpr std::array<std::uint8_t, 0x59> set2_{{
    0,0x76,0x16,0x1e,0x26,0x25,0x2e,0x36,0x3d,0x3e,0x46,0x45,0x4e,0x55,0x66,0x0d,
    0x15,0x1d,0x24,0x2d,0x2c,0x35,0x3c,0x43,0x44,0x4d,0x54,0x5b,0x5a,0x14,0x1c,0x1b,
    0x23,0x2b,0x34,0x33,0x3b,0x42,0x4b,0x4c,0x52,0x0e,0x12,0x5d,0x1a,0x22,0x21,0x2a,
    0x32,0x31,0x3a,0x41,0x49,0x4a,0x59,0x7c,0x11,0x29,0x58,0x05,0x06,0x04,0x0c,0x03,
    0x0b,0x83,0x0a,0x01,0x09,0x77,0x7e,0x6c,0x75,0x7d,0x7b,0x6b,0x73,0x74,0x79,0x69,
    0x72,0x7a,0x70,0x71,0,0,0x61,0x78,0x07
  }};

  static std::uint8_t extended_set2(std::uint8_t value) {
    switch (value) {
      case 0x1c: case 0x1d: case 0x35: case 0x38: case 0x47: case 0x48:
      case 0x49: case 0x4b: case 0x4d: case 0x4f: case 0x50: case 0x51:
      case 0x52: case 0x53:
        return set2_[value];
      default: return 0; // E0 events outside the current key map.
    }
  }

  void defaults() {
    scan_set_ = 2;
    scanning_requested_ = false;
    keys_.clear();
  }

  bool respond(std::initializer_list<std::uint8_t> bytes) {
    responses_.insert(responses_.end(), bytes.begin(), bytes.end());
    return true;
  }

  std::deque<std::uint8_t> keys_, responses_;
  std::uint8_t config_ = 4, controller_pending_ = 0, scan_set_ = 2;
  std::uint8_t parameter_ = 0, last_byte_ = 0xfe;
  bool enabled_ = true, signaled_ = false, scanning_requested_ = false;
};

} // namespace reverse_windows
