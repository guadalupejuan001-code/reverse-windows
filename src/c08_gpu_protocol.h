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
// The quantum plane aliases the CPU Ether aperture for its first 256 bytes.
#define RWGPU_QUANTUM_TRANSFER_BYTES 256u
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
  RWGPU_REG_DIRECT_MAX = 0x60, RWGPU_REG_BUS_MASTER = 0x64,
  RWGPU_REG_FENCE_LAST = 0x68, RWGPU_REG_IRQ_STATUS = 0x6c,
  RWGPU_REG_IRQ_ACK = 0x70, RWGPU_REG_FENCE_PREEMPT = 0x74,
  RWGPU_REG_FENCE_FAULT = 0x78, RWGPU_REG_SCHEDULED_DRAW_TICKET = 0x7c,
  RWGPU_REG_PREEMPT_LAST = 0x80, RWGPU_REG_ENGINE_RESET = 0x84,
  RWGPU_REG_QUANTUM_MAX = 0x88, RWGPU_REG_QUANTUM_BASE_LO = 0x8c,
  RWGPU_REG_QUANTUM_BASE_HI = 0x90
};
// UPLOAD, DOWNLOAD and COPY use OFFSET as source and AUX as destination.
// RAM addresses are physical and require PCI bus mastering. VRAM addresses
// are logical. LENGTH_HI extends LENGTH for these commands and SUM_INDICES.
enum RwGpuCommand {
  RWGPU_WRITE = 1, RWGPU_READ = 2, RWGPU_DRAW = 3, RWGPU_READ_COLOR = 4,
  RWGPU_UPLOAD = 5, RWGPU_DOWNLOAD = 6, RWGPU_COPY = 7, RWGPU_SUM_INDICES = 8,
  RWGPU_DRAW_VRAM = 9, RWGPU_READ_COLOR_RAM = 10, RWGPU_DRAW_RAM = 11,
  // OFFSET points at vertices and AUX at the BGRA output in guest RAM.
  // One ticket completes the draw and the entire readback.
  RWGPU_DRAW_READBACK = 12,
  // OFFSET is the physical DMA buffer, LENGTH is its size and AUX the
  // scheduler's fence. PREEMPT uses AUX for the preemption fence.
  RWGPU_SUBMIT_DMA = 13, RWGPU_PREEMPT = 14,
  // STORE/LOAD transfer staged basis bytes at OFFSET. SAVE/RESTORE transfer
  // between the shared quantum plane and persistent qSSD VRAM at that offset.
  RWGPU_QUANTUM_STORE = 15, RWGPU_QUANTUM_LOAD = 16,
  RWGPU_QUANTUM_SAVE = 17, RWGPU_QUANTUM_RESTORE = 18,
  // H uses OFFSET as a bit index. CX uses OFFSET (control) and AUX (target).
  // Both gates require LENGTH=0. Basis reads never implicitly measure a qubit.
  RWGPU_QUANTUM_H = 19, RWGPU_QUANTUM_CX = 20,
  // Explicit whole-session snapshot, including qubits outside the VRAM. All parameters zero.
  RWGPU_QUANTUM_CHECKPOINT_STATE = 21, RWGPU_QUANTUM_RESTORE_STATE = 22
};
enum RwGpuStatus { RWGPU_IDLE = 0, RWGPU_OK = 1, RWGPU_BAD_COMMAND = 2,
                  RWGPU_INVALID = 3, RWGPU_UNAVAILABLE = 4, RWGPU_FAILED = 5,
                  RWGPU_QUANTUM_UNAVAILABLE = 6, RWGPU_NO_CHECKPOINT = 7, RWGPU_CORRUPT_CHECKPOINT = 8 };
enum RwGpuCaps {
  RWGPU_CAP_VRAM = 1, RWGPU_CAP_TRIANGLES = 2, RWGPU_CAP_DIRECT_VRAM = 4,
  RWGPU_CAP_SUM_INDICES = 8, RWGPU_CAP_DRAW_VRAM = 16, RWGPU_CAP_COLOR_DMA = 32,
  RWGPU_CAP_DRAW_RAM = 64, RWGPU_CAP_COLOR_ALPHA = 128, RWGPU_CAP_DRAW_READBACK = 256,
  RWGPU_CAP_WDDM_SCHEDULER = 512, RWGPU_CAP_QUANTUM_VRAM = 1024, RWGPU_CAP_QUANTUM_STATE = 2048
};
enum RwGpuIrq { RWGPU_IRQ_COMPLETE = 1, RWGPU_IRQ_PREEMPT = 2, RWGPU_IRQ_FAULT = 4 };
#define RWGPU_PCI_MSI_CAP 0x50u
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
