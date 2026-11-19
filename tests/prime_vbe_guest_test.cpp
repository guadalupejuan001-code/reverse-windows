#include "android_boot_session.hpp"
#include "guest_vbe_fabric.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using cosmic_pc::PcMachine;
using cosmic_pc::PcVideo;
using reverse_windows::AndroidBootSession;
using reverse_windows::GuestVbeFabric;
using Pixels = std::vector<std::uint32_t>;
using Clock = std::chrono::steady_clock;
constexpr auto MiB = 1024U * 1024;

void require(bool okay, const char* detail) {
  if (!okay) throw std::runtime_error(detail);
}

std::uint64_t metric(const std::string& json, const char* name) {
  const auto key = std::string("\"") + name + "\":";
  const auto found = json.find(key);
  require(found != std::string::npos, name);
  return std::stoull(json.substr(found + key.size()));
}

std::string metrics(const GuestVbeFabric& video) {
  std::ostringstream out;
  video.write_json(out);
  return out.str();
}

void run_demo(PcMachine& pc, unsigned width, unsigned height) {
  pc.boot_graphics_demo(width, height);
  pc.enable_boot_acceleration();
  for (unsigned i = 0; i < 32 && !pc.cpu.halted && !pc.cpu.fault; ++i) pc.run(50000);
  if (pc.cpu.fault) throw std::runtime_error(pc.cpu.fault->describe());
  require(pc.cpu.halted && pc.cpu.read32(cosmic_pc::kGraphicsDemoResultAddress) ==
          cosmic_pc::kGraphicsDemoResult, "The real x86 guest did not draw the VBE fixture");
}

void set_mode(PcMachine& pc, unsigned mode) {
  constexpr std::uint8_t code[]{0xcd, 0x10, 0xf4};
  pc.cpu.reset(0, 0x7c00);
  pc.cpu.a20_enabled = true;
  pc.cpu.memory.write_block(0x7c00, code, sizeof(code));
  pc.cpu.set_reg16(cosmic_pc::Cpu::EAX, 0x4f02);
  pc.cpu.set_reg16(cosmic_pc::Cpu::EBX, mode);
  pc.run(3);
  require(!pc.cpu.fault && pc.cpu.halted && pc.cpu.reg16(cosmic_pc::Cpu::EAX) == 0x004f,
          "Guest VBE mode transition failed");
}

// Same full-frame conversion and nearest-neighbor sampling as the C06 app.
// Keep the reference independent from the page mapping and delta code.
void full_frame(const PcMachine& pc, unsigned width, unsigned height,
                std::vector<std::uint8_t>& row, Pixels& result) {
  const auto info = pc.video_info();
  row.resize(std::size_t(info.width) * 4);
  result.resize(std::size_t(width) * height);
  for (unsigned y = 0; y < height; ++y) {
    pc.copy_video_rgba(0, y * info.height / height, info.width, 1, row.data(), row.size());
    for (unsigned x = 0; x < width; ++x) {
      const auto i = std::size_t(x * info.width / width) * 4;
      result[std::size_t(y) * width + x] =
          0xff000000U | (std::uint32_t(row[i]) << 16) | (std::uint32_t(row[i + 1]) << 8) | row[i + 2];
    }
  }
}

std::size_t apply(const Pixels& packet, unsigned width, unsigned height, Pixels& canvas) {
  if (packet.empty()) return 0;
  canvas.resize(std::size_t(width) * height);
  std::size_t offset = 1, pixels = 0;
  const auto count = packet[0];
  require(count && count <= xgpu::FramebufferWorkAuthority::kRectangleLimit, "Bad VBE rectangle count");
  for (unsigned n = 0; n < count; ++n) {
    require(packet.size() - offset >= 4, "Incomplete VBE rectangle header");
    const auto x = packet[offset++], y = packet[offset++];
    const auto w = packet[offset++], h = packet[offset++];
    require(x < width && y < height && w && h && w <= width - x && h <= height - y,
            "VBE rectangle outside the canvas");
    require(packet.size() - offset >= std::size_t(w) * h, "Incomplete VBE rectangle pixels");
    for (unsigned row = 0; row < h; ++row) {
      std::copy_n(packet.data() + offset, w, canvas.data() + std::size_t(y + row) * width + x);
      offset += w;
    }
    pixels += std::size_t(w) * h;
  }
  require(offset == packet.size(), "Unexpected trailing VBE pixels");
  return pixels;
}

void check(const PcMachine& pc, unsigned w, unsigned h, const Pixels& canvas) {
  std::vector<std::uint8_t> row;
  Pixels expected;
  full_frame(pc, w, h, row, expected);
  require(expected == canvas, "Reconstructed graphics differ from direct guest pixels");
}

using Rect = xgpu::FramebufferRect;
void draw(PcMachine& pc, Rect area, std::uint32_t bgrx) {
  constexpr std::uint32_t code_address = 0xa000;
  static constexpr std::size_t kMaxRows = 32;
  const auto info = pc.video_info();
  std::vector<std::uint8_t> code;
  require(area.height <= kMaxRows && std::string_view(pc.cpu.mode_name()) == "protected32",
          "Unexpected synthetic painter geometry or CPU mode");
  const auto reg = [&](std::uint8_t opcode, std::uint32_t value) {
    code.push_back(opcode);
    for (unsigned i = 0; i < 4; ++i) code.push_back(value >> (8 * i));
  };
  reg(0xb8, bgrx);
  for (unsigned y = area.y; y < area.y + area.height; ++y) {
    reg(0xbf, info.physical_address + y * info.pitch + area.x * 4);
    reg(0xb9, area.width);
    code.insert(code.end(), {0xf3, 0xab}); // REP STOSD against the actual linear framebuffer.
  }
  code.push_back(0xf4);
  pc.cpu.memory.write_block(code_address, code.data(), code.size());
  pc.cpu.eip = code_address;
  pc.cpu.halted = false;
  pc.run(50000);
  if (pc.cpu.fault) throw std::runtime_error(pc.cpu.fault->describe());
  require(pc.cpu.halted, "The VBE painter did not finish");
}

void correctness() {
  PcMachine pc(2048ULL * MiB, 64 * MiB);
  GuestVbeFabric video(pc);
  run_demo(pc, 800, 600);
  const auto& watch = *pc.cpu.memory.write_watch();
  Pixels canvas;
  constexpr unsigned w = 800, h = 600;
  require(apply(video.capture(w, h), w, h, canvas) == w * h, "First graphics update must be complete");
  check(pc, w, h, canvas);
  const auto first = metrics(video);
  require(!watch.pending() && video.capture(w, h).empty() && metrics(video) == first,
          "Unchanged frames must avoid capture, reads and qSSD revisions");
  draw(pc, {222, 1, 4, 1}, 0x10996633); // Pixel 1024 starts a new physical page at x=224.
  require(watch.pages().size() == 2, "The guest REP should cross two physical pages");
  require(apply(video.capture(w, h), w, h, canvas) == 4,
          "Four changed pixels must cross pages without sending other pixels");
  require(metric(metrics(video), "read_bytes") - metric(first, "read_bytes") == 16,
          "Four adjacent pixels must read exactly 16 bytes across the two pages");
  check(pc, w, h, canvas);
  const auto serial = metric(metrics(video), "qssd_graphics_serial");
  draw(pc, {222, 1, 4, 1}, 0x80996633);
  require(watch.pending() && video.capture(w, h).empty() &&
          metric(metrics(video), "qssd_graphics_serial") == serial,
          "Writes only to the reserved X byte must not republish visible pixels");
  // One-pixel rectangles may be erased, cross the Android strip boundary,
  // and touch the last pixel of a partial physical page.
  draw(pc, {350, 31, 1, 2}, 0x00ffffff);
  draw(pc, {w - 1, h - 1, 1, 1}, 0);
  require(apply(video.capture(w, h), w, h, canvas) == 3, "Tiny and edge changes must all arrive");
  check(pc, w, h, canvas);
  const auto before_sparse = metric(metrics(video), "read_bytes");
  const auto address0 = pc.video_info().physical_address;
  const auto pages = pc.video_info().byte_size / cosmic_pc::MemoryFabric::kPageSize;
  for (unsigned i = 0; i < pages; ++i)
    pc.cpu.write8(address0 + i * cosmic_pc::MemoryFabric::kPageSize + 3, 0x5a);
  require(video.capture(w, h).empty() && metric(metrics(video), "read_bytes") - before_sparse == 4 * pages,
          "One reserved byte on each page must inspect only the intersecting pixels");
  pc.cpu.write8(pc.video_info().physical_address + pc.video_info().byte_size, 0x11);
  require(!watch.pending(), "Offscreen memory must not schedule presentation");

  draw(pc, {190, 150, 19, 5}, 0x00112233);
  require(!video.capture(w, h).empty(), "A pending packet should be available to drop");
  video.require_full();
  pc.cpu.memory.invalidate_write_watch();
  require(apply(video.capture(w, h), w, h, canvas) == w * h,
          "A dropped packet must recover with a complete update");
  check(pc, w, h, canvas);
  set_mode(pc, 0xc141);
  require(apply(video.capture(w, h), w, h, canvas) == w * h,
          "A same-size preserve-mode change must republish the layout");
  check(pc, w, h, canvas);
  set_mode(pc, 0x0003);
  require(watch.full() && !pc.video_info().graphics, "Text fallback must invalidate graphics");
  set_mode(pc, 0x4141);
  require(apply(video.capture(w, h), w, h, canvas) == w * h &&
          std::all_of(canvas.begin(), canvas.end(), [](auto p) { return p == 0xff000000; }),
          "Mode reentry must deliver every cleared pixel");

  // The 8K source is retained in RAM exactly, with the normal Android preview.
  set_mode(pc, 0x4147);
  constexpr unsigned sw = 960, sh = 540, source_w = 7680;
  require(apply(video.capture(sw, sh), sw, sh, canvas) == sw * sh, "8K geometry not sampled");
  const auto address = pc.video_info().physical_address;
  pc.cpu.write32(address + source_w * 4 + 16, 0x00bbccdd);
  require(watch.pending() && video.capture(sw, sh).empty(), "Unsampled row schedules no packet");
  pc.cpu.write32(address + (std::uint64_t(source_w) * 4312 + 7672) * 4, 0x00445566);
  require(apply(video.capture(sw, sh), sw, sh, canvas) == 1 && canvas.back() == 0xff445566,
          "The last sampled 8K pixel did not reach the mirror");
  check(pc, sw, sh, canvas);

  run_demo(pc, 1280, 720);
  apply(video.capture(sw, sh), sw, sh, canvas);
  std::mt19937 random(7);
  for (unsigned i = 0; i < 40; ++i) {
    for (unsigned n = 0; n < 4; ++n)
      draw(pc, {unsigned(random() % 1200), unsigned(random() % 700), 32, 7}, random());
    apply(video.capture(sw, sh), sw, sh, canvas);
    check(pc, sw, sh, canvas);
  }
  std::cout << "PRIME_C07_VBE_EXACT_PASS guest=protected32 pixels=exact alpha=opaque"
               " cross_page=1 same_color=1 reset=1 replay=1 offscreen=1 source_8k=1\n";
}

void android_session() {
  auto name = (std::filesystem::temp_directory_path() / "reverse-windows-vbe-XXXXXX").string();
  require(::mkdtemp(name.data()), "Cannot create the VBE guest directory");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  } cleanup{name};
  const auto volume = cleanup.path / "boot.rqvd";
  const auto boot = cosmic_pc::graphics_boot_disk(1280, 720);
  AndroidBootSession::create_disk(volume, 1);
  {
    reverse_windows::QssdDisk disk(volume);
    require(disk.write(0, 1, boot.data()) && disk.flush(), "Could not write VBE boot sector");
  }
  AndroidBootSession session(volume, 0, AndroidBootSession::BootSource::Disk);
  std::array<std::uint8_t, 4000> text;
  const auto vga = session.capture_video(text.data(), text.size());
  require(vga.text_bytes == text.size() && !vga.graphics && !session.video_pending(),
          "Android session must deliver initial VGA and consume its dirtiness");
  Pixels canvas;
  unsigned frames = 0;
  for (unsigned i = 0; i < 64 && !session.is_stopped(); ++i) {
    if (session.step(2048) == AndroidBootSession::Step::Fault)
      throw std::runtime_error(session.diagnostic());
    if (!session.video_pending()) continue;
    const auto frame = session.capture_video(text.data(), text.size());
    if (!frame.graphics) continue;
    require(frame.width == 960 && frame.height == 540, "Android preview geometry changed");
    apply(*frame.graphics, frame.width, frame.height, canvas);
    ++frames;
  }
  require(session.is_stopped() && frames > 1 && !session.video_pending(),
          "Android guest did not reach the final VBE picture");
  PcMachine reference(2048ULL * MiB, 64 * MiB);
  run_demo(reference, 1280, 720);
  check(reference, 960, 540, canvas);
  const auto diagnostic = session.diagnostic();
  const auto vbe = diagnostic.find("\"vbe\":{");
  require(vbe != std::string::npos && metric(diagnostic.substr(vbe), "frames") == frames &&
          metric(diagnostic.substr(vbe), "qssd_graphics_serial") == frames + 1,
          "Native Android statistics must account for every delivered revision");
  std::cout << "PRIME_C07_ANDROID_VBE_PASS frames=" << frames << " qssd=1 pixels=exact\n";
}

std::uint64_t nanoseconds(Clock::duration elapsed) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

void measure() {
  constexpr unsigned width = 960, height = 540, slots = 180, interval = 6, paint_interval = 12;
  constexpr auto frame_bytes = std::size_t(width) * height * 4;
  PcMachine pc(2048ULL * MiB, 64 * MiB);
  GuestVbeFabric video(pc);
  run_demo(pc, 1280, 720);
  Pixels full, previous, actual;
  std::vector<std::uint8_t> row;
  Clock::duration baseline{}, update{};
  std::uint64_t total_legacy_bytes = 0, total_legacy_reads = 0;
  unsigned legacy_frames = 0;
  for (unsigned tick = 0; tick < slots; ++tick) {
    if (tick && tick % paint_interval == 0) {
      const auto color = tick * 0x00010203;
      draw(pc, {286, 126, 48, 20}, color);
      draw(pc, {894, 424, 16, 10}, ~color);
    }
    if (tick % interval == 0) {
      const auto start = Clock::now();
      full_frame(pc, width, height, row, full);
      previous = full;
      baseline += Clock::now() - start;
      ++legacy_frames;
      total_legacy_bytes += frame_bytes;
      total_legacy_reads += 1280 * 4 * height;
    }
    const auto start = Clock::now();
    if (pc.cpu.memory.write_watch()->pending())
      apply(video.capture(width, height), width, height, actual);
    update += Clock::now() - start;
    require(actual == previous, "C06 and C07 must present equal pixels at each display opportunity");
  }
  const auto data = metrics(video);
  const auto frames = metric(data, "frames"), pixels = metric(data, "sent_pixels");
  const auto packet_bytes = metric(data, "packet_bytes") + frames * 12; // JNI dimension/strip header.
  const auto read_bytes = metric(data, "read_bytes");
  require(legacy_frames == slots / interval && frames == slots / paint_interval &&
          metric(data, "captures") == frames && pixels < total_legacy_bytes / 4 &&
          packet_bytes < total_legacy_bytes / 10 && read_bytes < total_legacy_reads / 10,
          "Sparse display work did not contract as expected");
  std::cout << "PRIME_C07_VBE_BENCH slots=" << slots << " legacy_frames=" << legacy_frames
            << " frames=" << frames << " legacy_bytes=" << total_legacy_bytes
            << " packet_bytes=" << packet_bytes << " legacy_read_bytes=" << total_legacy_reads
            << " read_bytes=" << read_bytes << " legacy_ns=" << nanoseconds(baseline)
            << " delta_ns=" << nanoseconds(update) << " pixels=exact\n";
}
} // namespace

int main() {
  try {
    correctness();
    android_session();
    measure();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
