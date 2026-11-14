#pragma once
#include <stddef.h>
#include <stdint.h>

// Little-endian PCI command ABI shared by the guest package and the VM.
// Only the doorbell commits a command. The guest driver copies data to the
// BAR staging area before ringing the doorbell.
#define RWGPU_VENDOR_ID 0x1B36u
#define RWGPU_DEVICE_ID 0xC083u
#define RWGPU_BAR_BYTES 0x80000u
#define RWGPU_BAR_DEFAULT 0xE0000000u
#define RWGPU_BAR_WINDOW_BYTES 0x02000000u
#define RWGPU_STAGE_OFFSET 0x1000u
#define RWGPU_STAGE_BYTES (256u * 1024u)
#define RWGPU_PAGE_BYTES 4096u
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
  RWGPU_REG_FORMAT = 0x3c, RWGPU_REG_MODE_WIDTH = 0x40, RWGPU_REG_MODE_HEIGHT = 0x44
};
enum RwGpuCommand { RWGPU_WRITE = 1, RWGPU_READ = 2, RWGPU_DRAW = 3, RWGPU_READ_COLOR = 4 };
enum RwGpuStatus { RWGPU_IDLE = 0, RWGPU_OK = 1, RWGPU_BAD_COMMAND = 2,
                  RWGPU_INVALID = 3, RWGPU_UNAVAILABLE = 4, RWGPU_FAILED = 5 };
enum RwGpuCaps { RWGPU_CAP_VRAM = 1, RWGPU_CAP_TRIANGLES = 2 };
// DRAW uses OFFSET as flags. READ_COLOR uses OFFSET as a byte address within
// the last completed tightly packed, top-down BGRX8888 draw.
#define RWGPU_DRAW_PRESENT_VBE UINT64_C(1)

// Colors are RGBA8 from the least to the most significant byte.
typedef struct RwGpuVertex {
  float x, y, z, w;
  uint32_t rgba;
} RwGpuVertex;
