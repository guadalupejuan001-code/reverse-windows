#pragma once

#include "android_boot_session.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <openssl/sha.h>

namespace boot_observer {
using Cpu = cosmic_pc::Cpu;
using Session = reverse_windows::AndroidBootSession;
using Pc = cosmic_pc::PcMachine;

inline std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

inline std::string digest(const std::uint8_t* bytes, std::size_t length) {
  if (!length) return {};
  std::uint8_t hashed[SHA256_DIGEST_LENGTH];
  if (!SHA256(bytes, length, hashed)) throw std::runtime_error("Could not hash the observation");
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto byte : hashed) out << std::setw(2) << unsigned(byte);
  return out.str();
}

inline std::uint64_t little(const std::uint8_t* data, unsigned count) {
  std::uint64_t number = 0;
  for (unsigned i = 0; i < count; ++i) number |= std::uint64_t(data[i]) << (i * 8);
  return number;
}

// This observer accesses only the simulator's ordinary physical RAM endpoint,
// between engine windows. CPU execution retains its own authoritative page walker.
// The diagnostic walk never marks Accessed/Dirty bits or calls guest devices.
class GuestView {
 public:
  explicit GuestView(const Cpu& cpu) : cpu_(cpu) {}

  bool enabled() const noexcept {
    return (cpu_.cr0 & (1U << 31)) && (cpu_.cr4 & (1U << 5)) &&
           !(cpu_.cr4 & (1U << 12)) && (cpu_.efer & (1U << 10));
  }

  bool read(std::uint64_t virtual_address, std::uint8_t* data, std::size_t bytes) const {
    if (!enabled() || !bytes || bytes > 4096 || virtual_address > UINT64_MAX - (bytes - 1)) return false;
    try {
      while (bytes) {
        const auto physical = address(virtual_address);
        const auto take = std::min(bytes, std::size_t(4096 - (virtual_address & 4095)));
        if (!physical || !cpu_.try_read_ram_block(*physical, data, static_cast<unsigned>(take))) return false;
        data += take;
        virtual_address += take;
        bytes -= take;
      }
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

 private:
  const Cpu& cpu_;
  static constexpr auto kPhysical = UINT64_C(0x000ffffffffff000);

  std::optional<std::uint64_t> address(std::uint64_t va) const {
    if ((va >> 47) != 0 && (va >> 47) != 0x1ffff) return std::nullopt;
    auto table = cpu_.cr3 & kPhysical;
    for (unsigned shift : {39U, 30U, 21U, 12U}) {
      std::array<std::uint8_t, 8> bytes{};
      if (!cpu_.try_read_ram_block(table + ((va >> shift) & 511) * 8, bytes.data(), bytes.size())) return std::nullopt;
      const auto entry = little(bytes.data(), bytes.size());
      if (!(entry & 1U)) return std::nullopt;
      if (shift == 39 && (entry & 128U)) return std::nullopt;
      if (shift == 12 || ((shift == 21 || shift == 30) && (entry & 128U))) {
        const auto mask = (UINT64_C(1) << shift) - 1;
        return ((entry & kPhysical) & ~mask) | (va & mask);
      }
      table = entry & kPhysical;
    }
    return std::nullopt;
  }
};

inline std::string basename(const std::uint8_t* data, std::size_t size) {
  const auto end = std::find(data, data + size, std::uint8_t{0});
  if (end == data || end == data + size) return {};
  std::string path(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(end));
  const auto separator = path.find_last_of("/\\");
  if (separator != std::string::npos) path.erase(0, separator + 1);
  const std::string allowed = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-";
  if (path.empty() || path.size() > 96 || path.find_first_not_of(allowed) != std::string::npos) return {};
  return path;
}

struct Module {
  std::uint64_t base = 0, rva = 0;
  std::uint64_t timestamp = 0, image_size = 0, entrypoint_rva = 0;
  std::string header_sha256, export_name, pdb_basename, debug_id_sha256;
  bool named() const noexcept { return !export_name.empty() || !pdb_basename.empty(); }
};

inline std::optional<Module> containing_image(const GuestView& view, std::uint64_t base, std::uint64_t ip) {
  std::array<std::uint8_t, 64> dos{};
  if (!view.read(base, dos.data(), dos.size()) || dos[0] != 'M' || dos[1] != 'Z') return std::nullopt;
  const auto nt_offset = little(dos.data() + 60, 4);
  if (nt_offset < dos.size() || nt_offset > 4096) return std::nullopt;
  std::array<std::uint8_t, 24> nt{};
  if (!view.read(base + nt_offset, nt.data(), nt.size()) || little(nt.data(), 4) != 0x4550 ||
      little(nt.data() + 4, 2) != 0x8664) return std::nullopt;
  const auto sections = little(nt.data() + 6, 2);
  const auto optional_size = little(nt.data() + 20, 2);
  if (!sections || sections > 96 || optional_size < 112 || optional_size > 512) return std::nullopt;
  std::array<std::uint8_t, 512> optional{};
  const auto optional_va = base + nt_offset + nt.size();
  if (!view.read(optional_va, optional.data(), optional_size) || little(optional.data(), 2) != 0x20b)
    return std::nullopt;
  const auto image_size = little(optional.data() + 56, 4);
  const auto rip_rva = ip - base;
  if (!image_size || image_size > (UINT64_C(1) << 29) || rip_rva >= image_size ||
      base > UINT64_MAX - image_size) return std::nullopt;
  const auto in_image = [&](std::uint64_t offset, std::uint64_t length) {
    return offset && offset < image_size && length <= image_size - offset;
  };
  bool executable = false;
  for (std::uint64_t i = 0; i < sections; ++i) {
    const auto offset = nt_offset + nt.size() + optional_size + 40 * i;
    std::array<std::uint8_t, 40> section{};
    if (!in_image(offset, section.size()) || !view.read(base + offset, section.data(), section.size()))
      return std::nullopt;
    const auto begin = little(section.data() + 12, 4);
    const auto length = std::max(little(section.data() + 8, 4), little(section.data() + 16, 4));
    if ((little(section.data() + 36, 4) & 0x20000000U) && in_image(begin, length) &&
        begin <= rip_rva && rip_rva - begin < length) executable = true;
  }
  if (!executable) return std::nullopt;
  Module module;
  module.base = base;
  module.rva = rip_rva;
  module.timestamp = little(nt.data() + 8, 4);
  module.image_size = image_size;
  module.entrypoint_rva = little(optional.data() + 16, 4);
  module.header_sha256 = digest(optional.data(), optional_size);
  const auto directories = little(optional.data() + 108, 4);
  if (directories && optional_size >= 120) {
    const auto exports = little(optional.data() + 112, 4);
    std::array<std::uint8_t, 40> entry{};
    if (in_image(exports, entry.size()) && view.read(base + exports, entry.data(), entry.size())) {
      const auto at = little(entry.data() + 12, 4);
      std::array<std::uint8_t, 128> name{};
      if (in_image(at, name.size()) && view.read(base + at, name.data(), name.size()))
        module.export_name = basename(name.data(), name.size());
    }
  }
  if (directories > 6 && optional_size >= 168) {
    const auto debug = little(optional.data() + 160, 4);
    const auto length = little(optional.data() + 164, 4);
    if (in_image(debug, length) && length >= 28 && length <= 28 * 32) {
      for (std::uint64_t offset = 0; offset + 28 <= length; offset += 28) {
        std::array<std::uint8_t, 28> entry{};
        if (!view.read(base + debug + offset, entry.data(), entry.size()) || little(entry.data() + 12, 4) != 2)
          continue;
        const auto payload_size = little(entry.data() + 16, 4);
        const auto payload_rva = little(entry.data() + 20, 4);
        std::array<std::uint8_t, 512> payload{};
        if (payload_size < 25 || payload_size > payload.size() || !in_image(payload_rva, payload_size) ||
            !view.read(base + payload_rva, payload.data(), payload_size) || little(payload.data(), 4) != 0x53445352)
          continue;
        module.pdb_basename = basename(payload.data() + 24, payload_size - 24);
        module.debug_id_sha256 = digest(payload.data() + 4, 20);
        if (!module.pdb_basename.empty()) break;
      }
    }
  }
  return module;
}

inline std::optional<Module> image_at_rip(const GuestView& view, std::uint64_t ip) {
  if (!view.enabled()) return std::nullopt;
  const auto page = ip & ~UINT64_C(4095);
  constexpr auto kSearchLimit = UINT64_C(32) << 20;
  for (std::uint64_t distance = 0; distance <= std::min(page, kSearchLimit); distance += 4096)
    if (auto module = containing_image(view, page - distance, ip)) return module;
  return std::nullopt;
}

struct Frame { unsigned width = 0, height = 0; std::string kind, sha256, file, error; };

inline Frame capture(Session& session, const std::filesystem::path& directory, const std::string& label) {
  std::array<std::uint8_t, 4000> text{};
  session.retry_video();
  const auto frame = session.capture_video(text.data(), text.size());
  Frame result{frame.width, frame.height, "unavailable", {}, {}, {}};
  if (frame.text_bytes) {
    if (frame.text_bytes > text.size()) throw std::runtime_error("Text capture exceeded its buffer");
    result.kind = "guest_text";
    result.sha256 = digest(text.data(), frame.text_bytes);
    return result;
  }
  if (!frame.graphics || !frame.width || !frame.height) return result;
  const auto pixels = std::size_t(frame.width) * frame.height;
  const auto& packet = *frame.graphics;
  if (packet.empty() || packet[0] > pixels) throw std::runtime_error("Invalid frame rectangle count");
  std::vector<std::uint8_t> rgb(pixels * 3), covered(pixels);
  std::size_t at = 1;
  for (unsigned i = 0; i < packet[0]; ++i) {
    if (packet.size() - at < 4) throw std::runtime_error("Incomplete frame rectangle");
    const auto x = packet[at++], y = packet[at++], width = packet[at++], height = packet[at++];
    if (!width || !height || x >= frame.width || y >= frame.height || width > frame.width - x ||
        height > frame.height - y || std::size_t(width) * height > packet.size() - at)
      throw std::runtime_error("Out-of-range frame rectangle");
    for (unsigned row = 0; row < height; ++row) {
      for (unsigned column = 0; column < width; ++column) {
        const auto pixel = packet[at++];
        const auto index = std::size_t(y + row) * frame.width + x + column;
        rgb[index * 3] = std::uint8_t(pixel >> 16);
        rgb[index * 3 + 1] = std::uint8_t(pixel >> 8);
        rgb[index * 3 + 2] = std::uint8_t(pixel);
        covered[index] = 1;
      }
    }
  }
  if (at != packet.size() || std::find(covered.begin(), covered.end(), 0) != covered.end())
    throw std::runtime_error("Requested full frame has missing RGB pixels");
  result.kind = "guest_vbe_rgb";
  result.sha256 = digest(rgb.data(), rgb.size());
  result.file = label + ".ppm";
  std::ofstream out(directory / result.file, std::ios::binary);
  out << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
  if (!out) throw std::runtime_error("Could not write the temporary RGB frame");
  return result;
}

inline bool sample(Session& session, std::ostream& out, const std::filesystem::path& directory,
                   const std::string& label) {
  const auto& cpu = session.diagnostic_cpu();
  const GuestView view(cpu);
  const auto module = image_at_rip(view, cpu.eip);
  std::array<std::uint8_t, 16> opcodes{};
  const auto code = view.read(cpu.eip, opcodes.data(), opcodes.size()) ? digest(opcodes.data(), opcodes.size()) : "";
  Frame frame;
  try { frame = capture(session, directory, label); }
  catch (const std::exception& error) { frame.kind = "capture_error"; frame.error = error.what(); }
  out << "{\"label\":" << Pc::quote(label) << ",\"steps\":" << session.session_steps()
      << ",\"cpu_mode\":" << Pc::quote(cpu.mode_name()) << ",\"cpl\":" << cpu.cpl()
      << ",\"cr3\":" << Pc::quote(hex(cpu.cr3)) << ",\"rip\":" << Pc::quote(hex(cpu.eip))
      << ",\"rip_prefix_sha256\":" << Pc::quote(code) << ",\"frame\":{\"kind\":" << Pc::quote(frame.kind)
      << ",\"width\":" << frame.width << ",\"height\":" << frame.height << ",\"rgb_or_text_sha256\":"
      << Pc::quote(frame.sha256) << ",\"temporary_source\":" << Pc::quote(frame.file)
      << ",\"error\":" << Pc::quote(frame.error) << "},\"module\":";
  if (module) {
    out << "{\"image_base\":" << Pc::quote(hex(module->base)) << ",\"rip_rva\":" << Pc::quote(hex(module->rva))
        << ",\"machine\":\"0x8664\",\"timestamp\":" << module->timestamp
        << ",\"size_of_image\":" << module->image_size
        << ",\"entrypoint_rva\":" << Pc::quote(hex(module->entrypoint_rva))
        << ",\"pe_optional_header_sha256\":" << Pc::quote(module->header_sha256) << ",\"export_name\":"
        << Pc::quote(module->export_name) << ",\"pdb_basename\":" << Pc::quote(module->pdb_basename)
        << ",\"codeview_identifier_sha256\":" << Pc::quote(module->debug_id_sha256) << '}';
  } else out << "null";
  if (cpu.fault) {
    const auto& fault = *cpu.fault;
    out << ",\"terminal_fault\":{\"code\":" << unsigned(fault.code)
        << ",\"reason\":" << Pc::quote(fault.reason) << ",\"error_code\":" << fault.error_code
        << ",\"linear_address\":" << Pc::quote(hex(fault.linear_address)) << '}';
  }
  out << "}\n" << std::flush;
  if (!out) throw std::runtime_error("Could not persist the boot observation");
  return module && module->named();
}

inline Session::Step trace(Session& session, std::ostream& out) {
  auto& cpu = session.diagnostic_cpu();
  auto previous = std::move(cpu.step_trace_hook);
  auto observed = 0U;
  const auto restore = [&] { cpu.step_trace_hook = std::move(previous); };
  cpu.step_trace_hook = [&](const Cpu::StepTrace& instruction) {
    if (observed++ >= 64) return;
    const auto& bytes = instruction.decoded_bytes;
    out << "{\"step\":" << instruction.sequence << ",\"cpu_mode\":" << Pc::quote(instruction.mode_before)
        << ",\"cs\":" << instruction.cs_before << ",\"rip\":" << Pc::quote(hex(instruction.rip_before))
        << ",\"opcode\":" << instruction.opcode << ",\"bytes_sha256\":" << Pc::quote(digest(bytes.data(), bytes.size()))
        << ",\"outcome\":" << Pc::quote(instruction.outcome) << ",\"vector\":" << instruction.delivered_vector << "}\n";
  };
  Session::Step result;
  try { result = session.step(64); }
  catch (...) { restore(); throw; }
  restore();
  out.flush();
  if (!out) throw std::runtime_error("Could not write bounded diagnostic trace");
  return result;
}
}
