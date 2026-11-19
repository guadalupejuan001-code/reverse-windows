#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace reverse_windows {

// The auxiliary port reports relative motion and button transitions as three-byte packets.
class Ps2Mouse final {
  struct Motion { int x, y; unsigned buttons; };
  std::deque<Motion> motion_;
  std::deque<std::uint8_t> bytes_;
  unsigned buttons_ = 0, resolution_ = 2, rate_ = 100;
  int parameter_ = 0;
  std::uint8_t last_ = 0xfe;
  bool streaming_ = false, remote_ = false, scaling_ = false, wrap_ = false, signaled_ = false;

  void defaults() {
    motion_.clear();
    buttons_ = 0; resolution_ = 2; rate_ = 100; parameter_ = 0;
    streaming_ = remote_ = scaling_ = wrap_ = false;
  }
  static int scaled(int value) {
    const int mag = value < 0 ? -value : value;
    constexpr int low[]{0,1,1,3,6,9};
    const int out = mag < 6 ? low[mag] : mag * 2;
    return value < 0 ? -out : out;
  }
  void packet() {
    Motion m{0,0,buttons_};
    if (!motion_.empty()) { m = motion_.front(); motion_.pop_front(); }
    const int x = scaling_ ? scaled(m.x) : m.x;
    const int y = scaling_ ? scaled(m.y) : m.y;
    bytes_.push_back(8 | m.buttons | (x < 0 ? 16 : 0) | (y < 0 ? 32 : 0)
                     | (x < -256 || x > 255 ? 64 : 0) | (y < -256 || y > 255 ? 128 : 0));
    bytes_.push_back(static_cast<std::uint8_t>(std::clamp(x,-256,255)));
    bytes_.push_back(static_cast<std::uint8_t>(std::clamp(y,-256,255)));
  }
 public:
  void input(int dx, int dy, unsigned buttons) {
    buttons &= 7;
    if (!dx && !dy && buttons == buttons_) return;
    const bool changed = buttons != buttons_;
    buttons_ = buttons;
    if (!streaming_ && !remote_) return;
    if (motion_.empty() || changed) {
      if (motion_.size() == 64) motion_.pop_front();
      motion_.push_back({0,0,buttons});
    }
    auto& m = motion_.back();
    m.x = std::clamp(m.x + std::clamp(dx,-32767,32767), -32767,32767);
    m.y = std::clamp(m.y + std::clamp(dy,-32767,32767), -32767,32767);
  }
  bool ready() const { return !bytes_.empty() || (streaming_ && !remote_ && !motion_.empty()); }
  bool needs_irq() const { return ready() && !signaled_; }
  void mark_irq() { signaled_ = true; }
  std::uint8_t read() {
    if (bytes_.empty() && ready()) packet();
    if (bytes_.empty()) return 0xff;
    last_ = bytes_.front(); bytes_.pop_front(); signaled_ = false;
    return last_;
  }
  bool command(std::uint8_t value) {
    if (bytes_.size() > 60) return false;
    if (wrap_ && value != 0xec && value != 0xff) { bytes_.push_back(value); return true; }
    if (value == 0xfe && !parameter_) { bytes_.push_back(last_); return true; }
    if (parameter_) {
      const bool valid = parameter_ == 0xf3 || value <= 3;
      if (valid) {
        if (parameter_ == 0xe8) resolution_ = value; else rate_ = value;
        parameter_ = 0;
      }
      bytes_.push_back(valid ? 0xfa : 0xfe);
      return true;
    }
    bytes_.push_back(0xfa);
    switch (value) {
      case 0xff: defaults(); bytes_.push_back(0xaa); bytes_.push_back(0); break;
      case 0xf6: defaults(); break;
      case 0xf5: streaming_ = false; motion_.clear(); break;
      case 0xf4: streaming_ = true; break;
      case 0xf3: case 0xe8: parameter_ = value; break;
      case 0xf2: bytes_.push_back(0); break;
      case 0xf0: remote_ = true; break;
      case 0xee: wrap_ = true; break;
      case 0xec: wrap_ = false; break;
      case 0xeb: packet(); break;
      case 0xea: remote_ = false; break;
      case 0xe9: bytes_.push_back((remote_ ? 64 : 0) | (streaming_ ? 32 : 0) |
                     (scaling_ ? 16 : 0) | ((buttons_ & 1) << 2) |
                     ((buttons_ & 2) >> 1) | ((buttons_ & 4) >> 1));
                 bytes_.push_back(resolution_); bytes_.push_back(rate_); break;
      case 0xe7: scaling_ = true; break;
      case 0xe6: scaling_ = false; break;
      default: bytes_.back() = 0xfe;
    }
    return true;
  }
};
} // namespace reverse_windows
