#pragma once
#include <stddef.h>
#include <stdint.h>

// Little-endian PCI command ABI shared by the guest package and the VM.
// Only a full doorbell write with the next ticket commits a command.
#define RWGPU_VENDOR_ID 0x1B36u
#define RWGPU_DEVICE_ID 0xC083u
#define RWGPU_BAR_BYTES 0x80000u
#define RWGPU_BAR_DEFAULT 0xE0000000u
#define RWGPU_BAR_WINDOW_BYTES 0x02000000u
#define RWGPU_STAGE_OFFSET 0x1000u
#define RWGPU_STAGE_BYTES (256u * 1024u)
#define RWGPU_PAGE_BYTES 4096u
#define RWGPU_VRAM_BATCH_BYTES (15u * RWGPU_PAGE_BYTES)
#define RWGPU_DIRECT_TRANSFER_BYTES (2048u * RWGPU_PAGE_BYTES)
#define RWGPU_LOGICAL_BYTES UINT64_C(120000000000)
#define RWGPU_MAGIC 0x55504752u
#define RWGPU_ABI 1u
#define RWGPU_MAX_VERTICES 12288u
#define RWGPU_MAX_WIDTH 1920u
#define RWGPU_MAX_HEIGHT 1080u

enum RwGpuRegister {
  RWGPU_REG_MAGIC = 0x00, RWGPU_REG_ABI = 0x04,
  RWGPU_REG_LOGICAL_LO = 0x08, RWGPU_REG_LOGICAL_HI = 0x0c,
  RWGPU_REG_CAPS = 0x10, RWGPU_REG_STATUS = 0x14,
  RWGPU_REG_OFFSET_LO = 0x18, RWGPU_REG_OFFSET_HI = 0x1c,
  RWGPU_REG_LENGTH = 0x20, RWGPU_REG_WIDTH = 0x24, RWGPU_REG_HEIGHT = 0x28,
  RWGPU_REG_CLEAR_RGBA = 0x2c, RWGPU_REG_TICKET = 0x30,
  RWGPU_REG_DOORBELL = 0x34, RWGPU_REG_COMPLETED = 0x38,
  RWGPU_REG_FORMAT = 0x3c, RWGPU_REG_MODE_WIDTH = 0x40, RWGPU_REG_MODE_HEIGHT = 0x44,
  RWGPU_REG_VRAM_BATCH_BYTES = 0x48,
  RWGPU_REG_AUX_LO = 0x4c, RWGPU_REG_AUX_HI = 0x50,
  RWGPU_REG_LENGTH_HI = 0x54, RWGPU_REG_RESULT_LO = 0x58, RWGPU_REG_RESULT_HI = 0x5c,
  RWGPU_REG_DIRECT_MAX = 0x60, RWGPU_REG_BUS_MASTER = 0x64
};
// UPLOAD, DOWNLOAD and COPY use OFFSET as source and AUX as destination.
// RAM addresses are physical and require PCI bus mastering. VRAM addresses
// are logical. LENGTH_HI extends LENGTH for these commands and SUM_INDICES.
enum RwGpuCommand {
  RWGPU_WRITE = 1, RWGPU_READ = 2, RWGPU_DRAW = 3, RWGPU_READ_COLOR = 4,
  RWGPU_UPLOAD = 5, RWGPU_DOWNLOAD = 6, RWGPU_COPY = 7, RWGPU_SUM_INDICES = 8,
  RWGPU_DRAW_VRAM = 9, RWGPU_READ_COLOR_RAM = 10, RWGPU_DRAW_RAM = 11
};
enum RwGpuStatus { RWGPU_IDLE = 0, RWGPU_OK = 1, RWGPU_BAD_COMMAND = 2,
                  RWGPU_INVALID = 3, RWGPU_UNAVAILABLE = 4, RWGPU_FAILED = 5 };
enum RwGpuCaps {
  RWGPU_CAP_VRAM = 1, RWGPU_CAP_TRIANGLES = 2, RWGPU_CAP_DIRECT_VRAM = 4,
  RWGPU_CAP_SUM_INDICES = 8, RWGPU_CAP_DRAW_VRAM = 16, RWGPU_CAP_COLOR_DMA = 32,
  RWGPU_CAP_DRAW_RAM = 64, RWGPU_CAP_COLOR_ALPHA = 128
};
// DRAW uses OFFSET as flags. DRAW_VRAM and DRAW_RAM use OFFSET as the source
// in VRAM or guest physical RAM and AUX as flags. READ_COLOR and READ_COLOR_RAM
// address the last tightly packed, top-down draw. READ_COLOR_RAM uses AUX for
// the physical RAM target.
#define RWGPU_DRAW_PRESENT_VBE UINT64_C(1)
// Offscreen output with real alpha, in BGRA order. VBE remains BGRX.
#define RWGPU_DRAW_COLOR_ALPHA UINT64_C(2)

// Colors are RGBA8 from the least to the most significant byte.
typedef struct RwGpuVertex {
  float x, y, z, w;
  uint32_t rgba;
} RwGpuVertex;
