#include "android_boot_session.hpp"
#include "c08_guest_gpu.hpp"
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using cosmic_pc::Cpu;
using cosmic_pc::PcMachine;
using reverse_windows::GuestGpuC08;
using reverse_windows::PcMotherboardC06;
constexpr std::size_t MiB = 1024 * 1024;
constexpr unsigned width = 640, height = 480;
constexpr std::uint32_t moved_bar = RWGPU_BAR_DEFAULT + RWGPU_BAR_BYTES;
constexpr auto last_page = RWGPU_LOGICAL_BYTES - RWGPU_PAGE_BYTES;
constexpr std::uint32_t clear = 0xff1e140aU;
constexpr std::uint32_t red = 0xff0000ffU, green = 0xff00ff00U;
constexpr std::uint32_t blue = 0xffff0000U;

void require(bool okay, const char* message) {
  if (!okay) throw std::runtime_error(message);
}

class Guest32 {
 public:
  static constexpr unsigned origin = 0xb000;
  std::vector<std::uint8_t> bytes;
  void emit(std::initializer_list<std::uint8_t> data) {
    bytes.insert(bytes.end(), data.begin(), data.end());
  }
  void word(std::uint32_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(value >> (i * 8));
  }
  void eax(std::uint32_t value) { emit({0xb8}); word(value); }
  void edx(std::uint16_t value) { emit({0x66, 0xba}); word(value, 2); }
  void write(std::uint32_t address, std::uint32_t value) {
    emit({0xc7, 0x05}); word(address); word(value); // MOV [physical],imm32
  }
  void read(std::uint32_t address, std::uint32_t output) {
    emit({0xa1}); word(address); emit({0xa3}); word(output);
  }
  void configuration(unsigned offset) {
    edx(0xcf8); eax(0x80000000U | (GuestGpuC08::pci_device << 11) | offset); emit({0xef});
    edx(0xcfc);
  }
  void cfg_write(unsigned offset, std::uint32_t value) {
    configuration(offset); eax(value); emit({0xef});
  }
  void cfg_read(unsigned offset, std::uint32_t output) {
    configuration(offset); emit({0xed, 0xa3}); word(output);
  }
  void copy(std::uint32_t destination, std::uint32_t source, unsigned length) {
    emit({0xbf}); word(destination);
    emit({0xbe}); word(source);
    emit({0xb9}); word(length / 4); emit({0xfc, 0xf3, 0xa5}); // CLD; REP MOVSD
  }
  void command(unsigned op, unsigned ticket, std::uint64_t address, unsigned size) {
    write(moved_bar + RWGPU_REG_TICKET, ticket);
    write(moved_bar + RWGPU_REG_LENGTH, size);
    write(moved_bar + RWGPU_REG_OFFSET_LO, static_cast<std::uint32_t>(address));
    write(moved_bar + RWGPU_REG_OFFSET_HI, static_cast<std::uint32_t>(address >> 32));
    write(moved_bar + RWGPU_REG_DOORBELL, op);
  }
  void run(PcMachine& pc) {
    emit({0xf4});
    require(bytes.size() < 0x1000, "Synthetic GPU command guest too large");
    pc.cpu.memory.write_block(origin, bytes.data(), bytes.size());
    pc.cpu.eip = origin;
    pc.cpu.halted = false;
    for (unsigned i = 0; i < 5 && !pc.cpu.halted && !pc.cpu.fault; ++i) pc.run(50000);
    if (pc.cpu.fault) throw std::runtime_error(pc.cpu.fault->describe());
    require(pc.cpu.halted, "GPU command guest did not halt");
  }
};

void graphics_boot(PcMachine& pc) {
  for (unsigned i = 0; i < 10 && !pc.cpu.halted && !pc.cpu.fault; ++i) pc.run(50000);
  if (pc.cpu.fault) throw std::runtime_error(pc.cpu.fault->describe());
  require(pc.cpu.halted && pc.video_info().width == width &&
          pc.cpu.read32(cosmic_pc::kGraphicsDemoResultAddress) == cosmic_pc::kGraphicsDemoResult,
          "x86 demo did not enter VBE protected mode");
}

#ifdef REVERSE_WINDOWS_GLES_GUEST
std::uint32_t rgb(const PcMachine& pc, unsigned x, unsigned y) {
  std::array<std::uint8_t, 4> pixel{};
  pc.copy_video_rgba(x, y, 1, 1, pixel.data(), pixel.size());
  return std::uint32_t(pixel[0]) | (std::uint32_t(pixel[1]) << 8) |
      (std::uint32_t(pixel[2]) << 16) | (std::uint32_t(pixel[3]) << 24);
}
#endif

void check_firmware(PcMachine& pc) {
  auto& cpu = pc.cpu;
  cpu.reset(0, 0x1000);
  cpu.a20_enabled = true;
  const std::uint8_t bios[]{0xcd, 0x15, 0xf4};
  cpu.memory.write_block(0x1000, bios, sizeof(bios));
  unsigned token = 0;
  bool saw_window = false;
  do {
    cpu.eip = 0x1000; cpu.halted = false;
    cpu.regs[Cpu::EAX] = 0xe820;
    cpu.regs[Cpu::EBX] = token;
    cpu.regs[Cpu::EDX] = 0x534d4150;
    cpu.regs[Cpu::ECX] = 24;
    cpu.regs[Cpu::EDI] = 0x7000;
    pc.run(3);
    require(!cpu.fault && !cpu.flag(Cpu::CF), "E820 would not deliver PCI mode memory map");
    const auto base = cpu.read64(0x7000), size = cpu.read64(0x7008);
    const auto type = cpu.read32(0x7010);
    if (base < GuestGpuC08::pci_window_end && base + size > GuestGpuC08::pci_window_start) {
      require(base == RWGPU_BAR_DEFAULT && size == GuestGpuC08::pci_window_bytes && type == 2,
              "E820 exposed PCI space as usable RAM");
      saw_window = true;
    }
    token = cpu.regs[Cpu::EBX];
    require(token < 8, "E820 did not finish");
  } while (token);
  require(saw_window, "PCI MMIO E820 reservation absent");
  const auto dsdt = reverse_windows::AcpiC06::dsdt_address;
  std::vector<std::uint8_t> aml(cpu.read32(dsdt + 4));
  cpu.memory.read_block(dsdt, aml.data(), aml.size());
  const std::uint8_t pattern[]{0x87, 0x17, 0, 0, 0x0c, 1, 0, 0, 0, 0, 0, 0, 0, 0xe0};
  require(std::search(aml.begin(), aml.end(), std::begin(pattern), std::end(pattern)) != aml.end(),
          "ACPI advertised no address resources for PCI GPU");
}

std::string gpu_stats(const GuestGpuC08& gpu) {
  std::ostringstream out;
  gpu.write_json(out);
  return out.str();
}

void verify(const std::filesystem::path& volume) {
  std::array<std::uint8_t, RWGPU_PAGE_BYTES> payload{}, actual{};
  for (unsigned i = 0; i < payload.size(); ++i) payload[i] = (i * 17) ^ (i >> 8);
  {
    PcMachine pc(4096ULL * MiB, 64 * MiB);
    pc.boot_graphics_demo(width, height);
    pc.enable_boot_acceleration();
    PcMotherboardC06 board(pc, nullptr, {}, 0, volume);
    require(board.attached_gpu() && !std::filesystem::exists(volume),
            "GPU backing must be lazy");
    check_firmware(pc);
    pc.reset();
    graphics_boot(pc);
    const auto source = 0x16000;
    const auto output = 0x18000;
    pc.cpu.memory.write_block(source, payload.data(), payload.size());
    Guest32 driver;
    driver.cfg_read(0, 0xe000);
    driver.cfg_read(8, 0xe004);
    driver.cfg_write(0x10, UINT32_MAX);
    driver.cfg_read(0x10, 0xe008);
    driver.cfg_write(0x10, moved_bar);
    driver.cfg_write(4, 2);
    driver.read(moved_bar + RWGPU_REG_MAGIC, 0xe00c);
    driver.read(moved_bar + RWGPU_REG_LOGICAL_LO, 0xe010);
    driver.read(moved_bar + RWGPU_REG_LOGICAL_HI, 0xe014);
    driver.copy(moved_bar + RWGPU_STAGE_OFFSET, source, payload.size());
    driver.command(RWGPU_WRITE, 1, last_page, payload.size());
    driver.read(moved_bar + RWGPU_REG_STATUS, 0xe018);
    driver.command(RWGPU_READ, 2, last_page, payload.size());
    driver.read(moved_bar + RWGPU_REG_STATUS, 0xe01c);
    driver.copy(output, moved_bar + RWGPU_STAGE_OFFSET, payload.size());
    driver.command(RWGPU_READ, 3, RWGPU_LOGICAL_BYTES, payload.size());
    driver.read(moved_bar + RWGPU_REG_STATUS, 0xe020);
    driver.run(pc);
    require(pc.cpu.read32(0xe000) == GuestGpuC08::pci_id && pc.cpu.read32(0xe004) == 0x03020001 &&
             pc.cpu.read32(0xe008) == ~(RWGPU_BAR_BYTES - 1) &&
             pc.cpu.read32(0xe00c) == RWGPU_MAGIC && pc.cpu.read64(0xe010) == RWGPU_LOGICAL_BYTES,
             "PCI enumeration or 64-bit VRAM registers do not match guest ABI");
    require(pc.cpu.read32(0xe018) == RWGPU_OK && pc.cpu.read32(0xe01c) == RWGPU_OK &&
             pc.cpu.read32(0xe020) == RWGPU_INVALID, "GPU VRAM page commands did not validate");
    pc.cpu.memory.read_block(output, actual.data(), actual.size());
    require(actual == payload, "PCI read did not copy exact 120 GB boundary data");

    // Real x86 submits near red before far green. Reversing depth semantics
    // therefore produces visibly different pixels. Blue is partly clipped.
    const RwGpuVertex vertices[]{
        {-.7f,-.55f,.2f,1,red}, {.7f,-.55f,.2f,1,red}, {0,.65f,.2f,1,red},
        {-.85f,-.7f,.8f,1,green}, {.85f,-.7f,.8f,1,green}, {0,.85f,.8f,1,green},
        {-.65f,.4f,.3f,1,blue}, {-1.3f,.65f,.3f,1,blue}, {-.7f,.95f,.3f,1,blue},
        {2,0,.5f,1,red}, {2.1f,.5f,.5f,1,red}, {2.2f,-.5f,.5f,1,red}
    };
    pc.cpu.memory.write_block(source, reinterpret_cast<const std::uint8_t*>(vertices), sizeof(vertices));
    reverse_windows::GuestVbeFabric mirror(pc);
    (void)mirror.capture(width, height);
    Guest32 draw;
    draw.copy(moved_bar + RWGPU_STAGE_OFFSET, source, sizeof(vertices));
    draw.write(moved_bar + RWGPU_REG_WIDTH, width);
    draw.write(moved_bar + RWGPU_REG_HEIGHT, height);
    draw.write(moved_bar + RWGPU_REG_CLEAR_RGBA, clear);
    draw.command(RWGPU_DRAW, 4, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    draw.read(moved_bar + RWGPU_REG_STATUS, 0xe024);
    draw.read(moved_bar + RWGPU_REG_COMPLETED, 0xe028);
    draw.run(pc);
#ifdef REVERSE_WINDOWS_GLES_GUEST
    require(pc.cpu.read32(0xe024) == RWGPU_OK && pc.cpu.read32(0xe028) == 4,
            "REgraphics did not finish the real guest draw");
    require(rgb(pc, 320, 240) == red && rgb(pc, 320, 63) == green &&
            rgb(pc, 34, 81) == blue && rgb(pc, 639, 479) == clear,
            "Guest GPU did not preserve clip position, depth, color or clear");
    const auto& packet = mirror.capture(width, height);
    require(!packet.empty(), "C07 did not receive the 3D framebuffer stores");
    const auto first = gpu_stats(*board.attached_gpu());
    require(first.find("\"source_triangles\":4,\"submitted_triangles\":3,\"vertex_bytes\":180") !=
            std::string::npos && first.find("\"graphics_serial\":2") != std::string::npos,
            "GPU Virtual did not select and version the exact 3D work");
    Guest32 repeat;
    repeat.command(RWGPU_DRAW, 5, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    repeat.read(moved_bar + RWGPU_REG_STATUS, 0xe02c);
    repeat.run(pc);
    require(pc.cpu.read32(0xe02c) == RWGPU_OK && mirror.capture(width, height).empty(),
            "Identical GPU output should not be retransmitted by C07");
    std::cout << "PRIME_C08_REGRAPHICS_PASS " << gpu_stats(*board.attached_gpu()) << '\n';
    Guest32 offscreen;
    offscreen.write(moved_bar + RWGPU_REG_WIDTH, 32);
    offscreen.write(moved_bar + RWGPU_REG_HEIGHT, 32);
    offscreen.command(RWGPU_DRAW, 6, 0, 0);
    offscreen.read(moved_bar + RWGPU_REG_STATUS, 0xe030);
    offscreen.command(RWGPU_READ_COLOR, 7, (32 * 32 - 1) * 4, 4);
    offscreen.read(moved_bar + RWGPU_REG_STATUS, 0xe034);
    offscreen.read(moved_bar + RWGPU_STAGE_OFFSET, 0xe038);
    offscreen.run(pc);
    require(pc.cpu.read32(0xe030) == RWGPU_OK && pc.cpu.read32(0xe034) == RWGPU_OK &&
            pc.cpu.read32(0xe038) == 0x000a141eU && !pc.cpu.memory.write_watch()->pending(),
            "Offscreen draw did not return exact BGRX or changed the boot display");
#else
    require(pc.cpu.read32(0xe024) == RWGPU_UNAVAILABLE && pc.cpu.read32(0xe028) == 4,
            "Headless build must disclose its absent renderer");
    std::cout << "PRIME_C08_RENDER_UNAVAILABLE no_egl=1\n";
#endif
    pc.reset();
    require(!pc.cpu.mmio_claim_hook(moved_bar), "Guest reset must turn BAR decode off");
  }
  reverse_windows::GraphicsMemory reopened(volume);
  actual.fill(255);
  require(reopened.read(last_page, actual.data(), actual.size()) && actual == payload,
          "Reopening QSSD did not recover the final VRAM page");
  require(reopened.read(last_page - RWGPU_PAGE_BYTES, actual.data(), actual.size()) &&
            actual == std::array<std::uint8_t, RWGPU_PAGE_BYTES>{},
          "A sparse adjacent page is not zero");
  std::cout << "PRIME_C08_PCI_VRAM_PASS logical_bytes=" << RWGPU_LOGICAL_BYTES
            << " last_byte=" << RWGPU_LOGICAL_BYTES - 1 << " backing_bytes=" << reopened.backing_bytes()
            << " e820=1 acpi=1 restart=1 exact=1\n";
}
}  // namespace

int main() {
  try {
    auto name = (std::filesystem::temp_directory_path() / "reverse-windows-gpu-XXXXXX").string();
    require(::mkdtemp(name.data()), "Could not create test directory");
    struct RemoveDirectory {
      std::filesystem::path path;
      ~RemoveDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } remove{name};
    verify(remove.path / "gpu.rqvd");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
