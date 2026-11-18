#include "android_boot_session.hpp"
#include "c08_guest_gpu.hpp"
#include "c08_wddm_protocol.h"
#include <array>
#include <chrono>
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
  void direct_command(unsigned op, unsigned ticket, std::uint64_t from, std::uint64_t into,
                      unsigned size) {
    write(moved_bar + RWGPU_REG_AUX_LO, std::uint32_t(into));
    write(moved_bar + RWGPU_REG_AUX_HI, std::uint32_t(into >> 32));
    write(moved_bar + RWGPU_REG_LENGTH_HI, 0);
    command(op, ticket, from, size);
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
  std::uint64_t usable = 0;
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
    if (type == 1) usable += size;
    if (base < GuestGpuC08::pci_window_end && base + size > GuestGpuC08::pci_window_start) {
      require(base == RWGPU_BAR_DEFAULT && size == GuestGpuC08::pci_window_bytes && type == 2,
              "E820 exposed PCI space as usable RAM");
      saw_window = true;
    }
    token = cpu.regs[Cpu::EBX];
    require(token < 9, "E820 did not finish");
  } while (token);
  require(saw_window && usable == (64ULL << 30), "E820 did not separate PCI from 64 GiB of RAM");
  const auto fadt = reverse_windows::AcpiC06::fadt_address;
  require((cpu.read16(fadt + 109) & 0x0fU) == 0x07U,
          "FADT must allow MSI with the PCI GPU attached");
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

constexpr std::uint32_t msi_receipt = 0xe200;
constexpr unsigned msi_vector = 0x5a;

void install_gpu_msi_handler(PcMachine& pc) {
  constexpr std::uint32_t idt = 0x480000, handler = 0xf000;
  auto& cpu = pc.cpu;
  cpu.idtr = {0x7ff, idt};
  cpu.write64(idt + msi_vector * 8, (handler & 0xffffU) |
      (std::uint64_t(cpu.segments[Cpu::CS]) << 16) | (0x8eULL << 40) |
      (std::uint64_t(handler >> 16) << 48));
  Guest32 irq;
  irq.emit({0x50}); // PUSH EAX.
  irq.read(moved_bar + RWGPU_REG_IRQ_STATUS, msi_receipt);
  irq.emit({0xa3}); irq.word(moved_bar + RWGPU_REG_IRQ_ACK);
  irq.read(moved_bar + RWGPU_REG_FENCE_LAST, msi_receipt + 4);
  irq.read(moved_bar + RWGPU_REG_FENCE_FAULT, msi_receipt + 8);
  irq.read(moved_bar + RWGPU_REG_FENCE_PREEMPT, msi_receipt + 12);
  irq.read(moved_bar + RWGPU_REG_PREEMPT_LAST, msi_receipt + 16);
  irq.emit({0xff,0x05}); irq.word(msi_receipt + 20); // INC DWORD [count].
  irq.write(PcMachine::kLocalApicAddress + 0xb0, 0);
  irq.emit({0x58,0xcf}); // POP EAX; IRETD.
  cpu.memory.write_block(handler, irq.bytes.data(), irq.bytes.size());
}

void enable_gpu_msi(Guest32& driver) {
  driver.cfg_write(RWGPU_PCI_MSI_CAP + 4, PcMachine::kLocalApicAddress);
  driver.cfg_write(RWGPU_PCI_MSI_CAP + 8, msi_vector);
  driver.cfg_write(RWGPU_PCI_MSI_CAP, 1u << 16);
  driver.write(PcMachine::kLocalApicAddress + 0xf0, 0x1ff);
}

#ifdef REVERSE_WINDOWS_GLES_GUEST
void verify_scheduled_dma(PcMachine& pc, PcMotherboardC06& board,
                          const RwGpuVertex* vertices, unsigned count,
                          const std::array<std::uint8_t, 4096>& reference) {
  constexpr auto receipt = msi_receipt;
  constexpr std::uint32_t read_first = 0x510000;
  constexpr std::uint64_t command_ram = 0x1'0050'0000ULL;
  constexpr std::uint64_t read_second = command_ram + 0x30000;
  constexpr std::uint64_t target = 0x1900010000ULL, scratch = target + RWGPU_PAGE_BYTES;
  constexpr unsigned half = RWGPU_PAGE_BYTES / 2;
  auto& cpu = pc.cpu;
  install_gpu_msi_handler(pc);
  const auto store = [&](std::uint64_t address, const auto& value) {
    cpu.memory.write_block(address, reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
  };
  const auto queue = [&](std::uint32_t offset, const void* data, unsigned size) {
    cpu.memory.write_block(command_ram + offset, static_cast<const std::uint8_t*>(data), size);
  };
  struct Transfer {
    RwGpuDmaTransfer command;
    std::array<RwGpuDmaRange, 2> range;
  };
  const auto transfer = [&](unsigned op, std::uint64_t vram) {
    return Transfer{{{op, sizeof(Transfer)}, vram, RWGPU_PAGE_BYTES, 2},
                    {{{read_first, half, 0}, {read_second, half, 0}}}};
  };
  cpu.memory.write_block(read_first, reference.data(), half);
  cpu.memory.write_block(read_second, reference.data() + half, half);
  const auto upload = transfer(RWGPU_DMA_UPLOAD, scratch);
  const RwGpuDmaCopy copy{{RWGPU_DMA_COPY, sizeof(copy)}, scratch, target, RWGPU_PAGE_BYTES, 0};
  const RwGpuDmaFill fill{{RWGPU_DMA_FILL, sizeof(fill)}, scratch, RWGPU_PAGE_BYTES, 0x5aa51effU};
  queue(0, &upload, sizeof(upload));
  queue(sizeof(upload), &copy, sizeof(copy));
  queue(sizeof(upload) + sizeof(copy), &fill, sizeof(fill));
  const unsigned draw_bytes = rwgpu_dma_draw_bytes(count);
  std::vector<std::uint8_t> draw_packet(draw_bytes);
  const RwGpuDmaDraw draw{{RWGPU_DMA_DRAW, draw_bytes}, target, 32, 32, 0x741e140aU, count};
  std::memcpy(draw_packet.data(), &draw, sizeof(draw));
  std::memcpy(draw_packet.data() + sizeof(draw), vertices, count * sizeof(RwGpuVertex));
  queue(0x1000, draw_packet.data(), draw_bytes);
  const auto download = transfer(RWGPU_DMA_DOWNLOAD, target);
  store(command_ram + 0x2000, download);
  auto bad = download;
  bad.range[1].ram = PcMachine::kLocalApicAddress;
  store(command_ram + 0x3000, bad);
  const auto copied = transfer(RWGPU_DMA_DOWNLOAD, scratch);
  store(command_ram + 0x4000, copied);

  Guest32 driver;
  driver.cfg_read(4, receipt + 24);
  driver.cfg_read(0x34, receipt + 28);
  driver.cfg_read(RWGPU_PCI_MSI_CAP, receipt + 32);
  enable_gpu_msi(driver);
  const auto submit = [&](unsigned ticket, unsigned offset, unsigned fence, unsigned bytes) {
    driver.direct_command(RWGPU_SUBMIT_DMA, ticket, command_ram + offset, fence, bytes);
    driver.emit({0xfb,0xf4,0xfa}); // STI; HLT until the MSI; CLI.
  };
  submit(29, 0, 101, sizeof(upload) + sizeof(copy) + sizeof(fill));
  driver.read(moved_bar + RWGPU_REG_STATUS, receipt + 36);
  submit(30, 0x4000, 102, sizeof(copied));
  driver.read(read_first, receipt + 40);
  submit(31, 0x1000, 103, draw_bytes);
  submit(32, 0x2000, 104, sizeof(download));
  submit(33, 0x3000, 105, sizeof(bad));
  driver.read(moved_bar + RWGPU_REG_STATUS, receipt + 44);
  driver.direct_command(RWGPU_PREEMPT, 34, 0, 205, 0);
  driver.emit({0xfb,0xf4,0xfa});
  driver.read(moved_bar + RWGPU_REG_SCHEDULED_DRAW_TICKET, receipt + 48);
  driver.run(pc);
  std::array<std::uint8_t, RWGPU_PAGE_BYTES> pixels{};
  cpu.memory.read_block(read_first, pixels.data(), half);
  cpu.memory.read_block(read_second, pixels.data() + half, half);
  require(cpu.read32(receipt) == RWGPU_IRQ_PREEMPT && cpu.read32(receipt + 4) == 104 &&
          cpu.read32(receipt + 8) == 105 && cpu.read32(receipt + 12) == 205 &&
          cpu.read32(receipt + 16) == 104 && cpu.read32(receipt + 20) == 6 &&
          (cpu.read32(receipt + 24) & 0x00100000U) && cpu.read32(receipt + 28) == RWGPU_PCI_MSI_CAP &&
          cpu.read32(receipt + 32) == 5 && cpu.read32(receipt + 36) == RWGPU_OK &&
          cpu.read32(receipt + 40) == 0x5aa51effU && cpu.read32(receipt + 44) == RWGPU_INVALID &&
          cpu.read32(receipt + 48) == 31 && board.interrupt_state().in_service == 0 &&
          pixels == reference && !pc.cpu.memory.write_watch()->pending(),
          "VidMm DMA data, MSI fences, fault, or preemption diverged");
  std::cout << "PRIME_C08_SCHEDULED_DMA_PASS msi=6 fence=104 fault=105 preempt=205 pixels=4096"
               " pin=system-ram vbe_stores=0 high_dma=1\n";
}
#else
void verify_preempt_msi(PcMachine& pc, PcMotherboardC06& board) {
  constexpr unsigned preempt_fence = 205, preempt_ticket = 11;
  install_gpu_msi_handler(pc);
  Guest32 driver;
  enable_gpu_msi(driver);
  driver.direct_command(RWGPU_PREEMPT, preempt_ticket, 0, preempt_fence, 0);
  driver.emit({0xfb,0xf4,0xfa}); // STI; HLT until the MSI; CLI.
  driver.read(moved_bar + RWGPU_REG_COMPLETED, msi_receipt + 24);
  driver.run(pc);
  require(pc.cpu.read32(msi_receipt) == RWGPU_IRQ_PREEMPT &&
          pc.cpu.read32(msi_receipt + 12) == preempt_fence &&
          pc.cpu.read32(msi_receipt + 20) == 1 &&
          pc.cpu.read32(msi_receipt + 24) == preempt_ticket &&
          board.interrupt_state().in_service == 0,
          "PrimeVGC preempt MSI did not reach the guest handler");
  std::cout << "PRIME_C08_PREEMPT_MSI_PASS fence=" << preempt_fence << " fadt_msi=1\n";
}
#endif

void verify(const std::filesystem::path& volume) {
  std::array<std::uint8_t, RWGPU_PAGE_BYTES> payload{}, actual{};
  for (unsigned i = 0; i < payload.size(); ++i) payload[i] = (i * 17) ^ (i >> 8);
  {
    PcMachine pc(PcMachine::high_ram_address_space(64ULL << 30, true), 64 * MiB);
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
    const auto offset = RWGPU_LOGICAL_BYTES - 2 * RWGPU_PAGE_BYTES;
    const auto size = 2 * RWGPU_PAGE_BYTES;
    driver.read(moved_bar + RWGPU_REG_VRAM_BATCH_BYTES, 0xe03c);
    driver.copy(moved_bar + RWGPU_STAGE_OFFSET, source, size);
    driver.command(RWGPU_WRITE, 4, offset, size);
    driver.read(moved_bar + RWGPU_REG_STATUS, 0xe040);
    driver.command(RWGPU_READ, 5, offset, size);
    driver.read(moved_bar + RWGPU_REG_STATUS, 0xe044);
    driver.copy(output + size, moved_bar + RWGPU_STAGE_OFFSET, size);
    driver.run(pc);
    const auto stage = moved_bar + RWGPU_STAGE_OFFSET;
    const auto first_word = std::uint32_t(payload[0]) | (std::uint32_t(payload[1]) << 8) |
        (std::uint32_t(payload[2]) << 16) | (std::uint32_t(payload[3]) << 24);
    require(pc.cpu.mmio_read32_hook(stage) == first_word &&
            pc.cpu.mmio_write32_hook(stage, first_word).value_or(false),
            "The GPU staging buffer must retain little-endian dword transfers");
    require(pc.cpu.read32(0xe000) == GuestGpuC08::pci_id && pc.cpu.read32(0xe004) == 0x03020001 &&
             pc.cpu.read32(0xe008) == ~(RWGPU_BAR_BYTES - 1) &&
             pc.cpu.read32(0xe00c) == RWGPU_MAGIC && pc.cpu.read64(0xe010) == RWGPU_LOGICAL_BYTES,
             "PCI enumeration or 64-bit VRAM registers do not match guest ABI");
    require(pc.cpu.read32(0xe018) == RWGPU_OK && pc.cpu.read32(0xe01c) == RWGPU_OK &&
             pc.cpu.read32(0xe020) == RWGPU_INVALID, "GPU VRAM page commands did not validate");
    pc.cpu.memory.read_block(output, actual.data(), actual.size());
    require(actual == payload, "PCI read did not copy exact 120 GB boundary data");
    pc.cpu.memory.read_block(output + size, actual.data(), actual.size());
    require(actual == payload, "Batched PCI read lost the first page");
    pc.cpu.memory.read_block(output + size + RWGPU_PAGE_BYTES, actual.data(), actual.size());
    require(pc.cpu.read32(0xe03c) == RWGPU_VRAM_BATCH_BYTES && pc.cpu.read32(0xe040) == RWGPU_OK &&
            pc.cpu.read32(0xe044) == RWGPU_OK &&
            actual == std::array<std::uint8_t, RWGPU_PAGE_BYTES>{},
            "Batched PCI read lost the second page or limit");
    Guest32 restore_last;
    restore_last.copy(moved_bar + RWGPU_STAGE_OFFSET, source, payload.size());
    restore_last.command(RWGPU_WRITE, 6, last_page, payload.size());
    restore_last.direct_command(RWGPU_DOWNLOAD, 7, last_page, 0x1c000, payload.size());
    restore_last.read(moved_bar + RWGPU_REG_STATUS, 0xe048);
    restore_last.cfg_write(4, 6);
    restore_last.direct_command(RWGPU_DOWNLOAD, 8, last_page, 0x1c000, payload.size());
    restore_last.read(moved_bar + RWGPU_REG_STATUS, 0xe04c);
    restore_last.run(pc);
    pc.cpu.memory.read_block(0x1c000, actual.data(), actual.size());
    require(pc.cpu.read32(0xe048) == RWGPU_UNAVAILABLE && pc.cpu.read32(0xe04c) == RWGPU_OK &&
            actual == payload, "Direct DMA must require bus mastering and use the staged VRAM backing");

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
    constexpr std::uint64_t geometry_offset = 0x1900000000ULL;
    draw.direct_command(RWGPU_UPLOAD, 9, source, geometry_offset, RWGPU_PAGE_BYTES);
    draw.read(moved_bar + RWGPU_REG_STATUS, 0xe050);
    draw.write(moved_bar + RWGPU_REG_WIDTH, width);
    draw.write(moved_bar + RWGPU_REG_HEIGHT, height);
    draw.write(moved_bar + RWGPU_REG_CLEAR_RGBA, clear);
    draw.direct_command(RWGPU_DRAW_VRAM, 10, geometry_offset, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    draw.read(moved_bar + RWGPU_REG_STATUS, 0xe024);
    draw.read(moved_bar + RWGPU_REG_COMPLETED, 0xe028);
    draw.run(pc);
    require(pc.cpu.read32(0xe050) == RWGPU_OK, "Direct upload of 3D vertices failed");
#ifdef REVERSE_WINDOWS_GLES_GUEST
    require(pc.cpu.read32(0xe024) == RWGPU_OK && pc.cpu.read32(0xe028) == 10,
            "REgraphics did not finish the real guest draw from PrimeVGC VRAM");
    require(rgb(pc, 320, 240) == red && rgb(pc, 320, 63) == green &&
            rgb(pc, 34, 81) == blue && rgb(pc, 639, 479) == clear,
            "Guest GPU did not preserve clip position, depth, color or clear");
    Guest32 retrieve;
    constexpr auto color_target = 0x200000U;
    constexpr auto color_bytes = width * height * 4;
    retrieve.direct_command(RWGPU_READ_COLOR_RAM, 11, 0, color_target, color_bytes);
    retrieve.read(moved_bar + RWGPU_REG_STATUS, 0xe054);
    retrieve.read(moved_bar + RWGPU_REG_RESULT_LO, 0xe05c);
    retrieve.direct_command(RWGPU_READ_COLOR_RAM, 12, color_bytes - 3, color_target, 4);
    retrieve.read(moved_bar + RWGPU_REG_STATUS, 0xe058);
    retrieve.run(pc);
    std::vector<std::uint8_t> via_dma(color_bytes), via_vbe(color_bytes);
    pc.cpu.memory.read_block(color_target, via_dma.data(), via_dma.size());
    pc.cpu.memory.read_block(pc.video_info().physical_address, via_vbe.data(), via_vbe.size());
    require(pc.cpu.read32(0xe054) == RWGPU_OK && pc.cpu.read32(0xe058) == RWGPU_INVALID &&
            pc.cpu.read32(0xe05c) == color_bytes && via_dma == via_vbe,
            "PCI color DMA did not deliver the exact BGRX frame directly to guest RAM");
    const auto& packet = mirror.capture(width, height);
    require(!packet.empty(), "C07 did not receive the 3D framebuffer stores");
    const auto first = gpu_stats(*board.attached_gpu());
    require(first.find("\"source_triangles\":4,\"submitted_triangles\":3,\"vertex_bytes\":180") !=
            std::string::npos && first.find("\"graphics_serial\":2") != std::string::npos,
            "GPU Virtual did not select and version the exact 3D work");
    // The developer miniport submits the same bounded RAM render used here.
    constexpr unsigned frame_bytes = sizeof(RwGpuWddmFrame) + sizeof(vertices) + color_bytes;
    RwGpuWddmFrame frame{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_DRAW, frame_bytes},
                        width, height, clear, sizeof(vertices) / sizeof(vertices[0]), 0};
    uint32_t encoded_bytes = 0, output_bytes = 0;
    require(rwgpu_wddm_frame_size(&frame, &encoded_bytes, &output_bytes) &&
            encoded_bytes == sizeof(vertices) && output_bytes == color_bytes,
            "WDDM frame packet does not match its vertices and color payload");
    frame.header.bytes -= 1;
    require(!rwgpu_wddm_frame_size(&frame, &encoded_bytes, &output_bytes), "Truncated frame packet accepted");
    frame.header.bytes += 1;
    frame.width = UINT32_MAX;
    require(!rwgpu_wddm_frame_size(&frame, &encoded_bytes, &output_bytes), "Oversized frame packet accepted");

    Guest32 repeat;
    repeat.cfg_write(4, 2);
    repeat.direct_command(RWGPU_DRAW_RAM, 13, source, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    repeat.read(moved_bar + RWGPU_REG_STATUS, 0xe060);
    repeat.cfg_write(4, 6);
    repeat.direct_command(RWGPU_DRAW_RAM, 14, moved_bar, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    repeat.read(moved_bar + RWGPU_REG_STATUS, 0xe064);
    repeat.direct_command(RWGPU_DRAW_RAM, 15, source, RWGPU_DRAW_PRESENT_VBE, sizeof(vertices));
    repeat.read(moved_bar + RWGPU_REG_STATUS, 0xe02c);
    repeat.run(pc);
    require(pc.cpu.read32(0xe060) == RWGPU_UNAVAILABLE && pc.cpu.read32(0xe064) == RWGPU_INVALID &&
            pc.cpu.read32(0xe02c) == RWGPU_OK && mirror.capture(width, height).empty(),
            "RAM draw must reject invalid DMA and preserve the exact C07 frame");
    pc.cpu.memory.read_block(pc.video_info().physical_address, via_dma.data(), via_dma.size());
    require(via_dma == via_vbe, "Render from guest memory differs from render from VRAM");
    std::cout << "PRIME_C08_REGRAPHICS_PASS " << gpu_stats(*board.attached_gpu()) << '\n';
    Guest32 offscreen;
    offscreen.write(moved_bar + RWGPU_REG_WIDTH, 32);
    offscreen.write(moved_bar + RWGPU_REG_HEIGHT, 32);
    offscreen.command(RWGPU_DRAW, 16, 0, 0);
    offscreen.read(moved_bar + RWGPU_REG_STATUS, 0xe030);
    offscreen.command(RWGPU_READ_COLOR, 17, (32 * 32 - 1) * 4, 4);
    offscreen.read(moved_bar + RWGPU_REG_STATUS, 0xe034);
    offscreen.read(moved_bar + RWGPU_STAGE_OFFSET, 0xe038);
    offscreen.run(pc);
    require(pc.cpu.read32(0xe030) == RWGPU_OK && pc.cpu.read32(0xe034) == RWGPU_OK &&
            pc.cpu.read32(0xe038) == 0x000a141eU && !pc.cpu.memory.write_watch()->pending(),
            "Offscreen draw did not return exact BGRX or changed the boot display");
    Guest32 alpha;
    alpha.write(moved_bar + RWGPU_REG_CLEAR_RGBA, 0x741e140aU);
    alpha.direct_command(RWGPU_DRAW_RAM, 18, source, RWGPU_DRAW_COLOR_ALPHA, sizeof(vertices));
    alpha.read(moved_bar + RWGPU_REG_STATUS, 0xe068);
    alpha.read(moved_bar + RWGPU_REG_COMPLETED, 0xe06c);
    alpha.direct_command(RWGPU_READ_COLOR_RAM, 19, 0, color_target, 32 * 32 * 4);
    alpha.read(moved_bar + RWGPU_REG_STATUS, 0xe070);
    alpha.direct_command(RWGPU_DRAW_RAM, 20, source, RWGPU_DRAW_PRESENT_VBE | RWGPU_DRAW_COLOR_ALPHA,
                         sizeof(vertices));
    alpha.read(moved_bar + RWGPU_REG_STATUS, 0xe074);
    alpha.run(pc);
    require(pc.cpu.read32(0xe068) == RWGPU_OK && pc.cpu.read32(0xe06c) == 18 &&
            pc.cpu.read32(0xe070) == RWGPU_OK && pc.cpu.read32(0xe074) == RWGPU_INVALID &&
            pc.cpu.read32(color_target + (32 * 32 - 1) * 4) == 0x740a141eU &&
            pc.cpu.read32(color_target + (16 * 32 + 16) * 4) == 0xffff0000U &&
            gpu_stats(*board.attached_gpu()).find("\"alpha_draw_ticket\":18,") != std::string::npos &&
            !pc.cpu.memory.write_watch()->pending(),
            "WDDM offscreen readback must preserve alpha without changing the boot display");
    std::cout << "PRIME_C08_BGRA_DMA_PASS alpha_ticket=18 vbe_stores=0 " << gpu_stats(*board.attached_gpu()) << '\n';
    std::array<std::uint8_t, RWGPU_PAGE_BYTES> reference{};
    pc.cpu.memory.read_block(color_target, reference.data(), reference.size());

    constexpr auto frame_target = 0x400000U;
    constexpr unsigned chunk_size = RWGPU_MAX_VERTICES * sizeof(RwGpuVertex);
    static_assert(color_bytes / chunk_size == 5 && color_bytes % chunk_size == 0);
    Guest32 paired;
    paired.write(moved_bar + RWGPU_REG_WIDTH, width);
    paired.write(moved_bar + RWGPU_REG_HEIGHT, height);
    paired.read(moved_bar + RWGPU_REG_CAPS, 0xe078);
    paired.direct_command(RWGPU_DRAW_RAM, 21, source, RWGPU_DRAW_COLOR_ALPHA, sizeof(vertices));
    for (unsigned chunk = 0; chunk < 5; ++chunk) {
      const auto offset = chunk * chunk_size;
      paired.direct_command(RWGPU_READ_COLOR_RAM, 22 + chunk, offset, color_target + offset, chunk_size);
      paired.read(moved_bar + RWGPU_REG_STATUS, 0xe0a0 + 4 * chunk);
    }
    paired.direct_command(RWGPU_DRAW_READBACK, 27, source, moved_bar, sizeof(vertices));
    paired.read(moved_bar + RWGPU_REG_STATUS, 0xe07c);
    paired.direct_command(RWGPU_DRAW_READBACK, 28, source, frame_target, sizeof(vertices));
    paired.read(moved_bar + RWGPU_REG_STATUS, 0xe080);
    paired.read(moved_bar + RWGPU_REG_COMPLETED, 0xe084);
    paired.read(moved_bar + RWGPU_REG_RESULT_LO, 0xe088);
    paired.run(pc);
    for (unsigned chunk = 0; chunk < 5; ++chunk)
      require(pc.cpu.read32(0xe0a0 + 4 * chunk) == RWGPU_OK, "Legacy BGRA readback failed");
    pc.cpu.memory.read_block(color_target, via_dma.data(), via_dma.size());
    pc.cpu.memory.read_block(frame_target, via_vbe.data(), via_vbe.size());
    const auto frame_stats = gpu_stats(*board.attached_gpu());
    require((pc.cpu.read32(0xe078) & RWGPU_CAP_DRAW_READBACK) && pc.cpu.read32(0xe07c) == RWGPU_INVALID &&
            pc.cpu.read32(0xe080) == RWGPU_OK && pc.cpu.read32(0xe084) == 28 &&
            pc.cpu.read32(0xe088) == color_bytes && via_dma == via_vbe &&
            pc.cpu.read32(frame_target + color_bytes - 4) == 0x740a141eU &&
            frame_stats.find("\"frame_dma_completions\":1,") != std::string::npos &&
            frame_stats.find("\"alpha_draw_ticket\":28,") != std::string::npos &&
            !pc.cpu.memory.write_watch()->pending(),
            "Single-ticket readback did not deliver the same BGRA frame or corrupted VGA");
    std::cout << "PRIME_C08_SINGLE_DMA_PASS bytes=" << color_bytes << " legacy_commands=6 commands=1\n";
    verify_scheduled_dma(pc, board, vertices, std::size(vertices), reference);
#else
    require(pc.cpu.read32(0xe024) == RWGPU_UNAVAILABLE && pc.cpu.read32(0xe028) == 10,
            "Headless build must disclose its absent renderer");
    verify_preempt_msi(pc, board);
    std::cout << "PRIME_C08_RENDER_UNAVAILABLE no_egl=1\n";
#endif
    pc.reset();
    require(!pc.cpu.mmio_claim_hook(moved_bar), "Guest reset must turn BAR decode off");
  }
  using Vram = reverse_windows::GpuVram;
  Vram reopened(volume);
  actual.fill(255);
  require(reopened.read(last_page, actual.data(), actual.size()) == Vram::Result::Complete && actual == payload,
          "Reopening QSSD did not recover the final VRAM page");
  require(reopened.read(last_page - 2 * RWGPU_PAGE_BYTES, actual.data(), actual.size()) == Vram::Result::Complete &&
            actual == std::array<std::uint8_t, RWGPU_PAGE_BYTES>{},
          "A sparse adjacent page is not zero");
  std::cout << "PRIME_C08_PCI_VRAM_PASS logical_bytes=" << RWGPU_LOGICAL_BYTES
            << " last_byte=" << RWGPU_LOGICAL_BYTES - 1 << " backing_bytes=" << reopened.journal_bytes()
            << " e820=1 acpi=1 restart=1 exact=1\n";
}

void measure_vram(const std::filesystem::path& path) {
  constexpr unsigned batch_bytes = RWGPU_VRAM_BATCH_BYTES;
  constexpr unsigned batches = 128;
  constexpr auto first = RWGPU_LOGICAL_BYTES - std::uint64_t(batches) * batch_bytes;
  constexpr std::size_t bytes = std::size_t(batches) * batch_bytes;
  std::vector<std::uint8_t> expected(bytes), copy(bytes);
  std::uint32_t state = 0x1431edfa;
  for (auto& byte : expected) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    byte = state >> 19;
  }
  using Clock = std::chrono::steady_clock;
  using Vram = reverse_windows::GpuVram;
  std::chrono::nanoseconds durable_time{};
  std::uint64_t commits = 0;
  {
    // Exercise existing C08 installations whose QSSD header rounded to 112 GiB.
    reverse_windows::QssdDisk::create(path, Vram::kLegacyBackingBytes / reverse_windows::QssdDisk::kGiB);
    Vram vram(path);
    const auto started = Clock::now();
    for (std::size_t i = 0; i < bytes; i += batch_bytes)
      require(vram.write(first + i, expected.data() + i, batch_bytes) == Vram::Result::Complete,
              "Large QSSD VRAM batch write failed");
    durable_time = Clock::now() - started;
    commits = vram.commits();
  }
  const auto started = Clock::now();
  Vram vram(path);
  for (std::size_t i = 0; i < bytes; i += batch_bytes)
    require(vram.read(first + i, copy.data() + i, batch_bytes) == Vram::Result::Complete,
            "Large QSSD VRAM batch read failed");
  const auto reload = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
  require(copy == expected && commits == batches &&
          vram.read(RWGPU_LOGICAL_BYTES - RWGPU_PAGE_BYTES, copy.data(), batch_bytes) == Vram::Result::InvalidRange,
          "Batched VRAM failed durability or boundary validation");
  std::cout << "PRIME_C08_VRAM_BATCH_PASS bytes=" << bytes << " offset=" << first
            << " commits=" << commits << " write_ns=" << durable_time.count()
            << " reopen_and_read_ns=" << reload.count() << " exact=1\n";
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
    measure_vram(remove.path / "large-vram.rqvd");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
