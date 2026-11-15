#pragma once
#include "c08_gpu_protocol.h"

// DMA command format emitted by the kernel miniport after checking the
// user-mode commands and letting VidMm resolve their allocation addresses.
#define RWGPU_DMA_BUFFER_BYTES (256u * 1024u)
#define RWGPU_DMA_MAX_RANGES (RWGPU_DIRECT_TRANSFER_BYTES / RWGPU_PAGE_BYTES + 1)
#define RWGPU_VIDMM_VRAM_SEGMENT 1u

enum RwGpuDmaOpcode {
  RWGPU_DMA_UPLOAD = 1, RWGPU_DMA_DOWNLOAD, RWGPU_DMA_COPY, RWGPU_DMA_FILL, RWGPU_DMA_DRAW
};

typedef struct RwGpuDmaHeader {
  uint32_t opcode, record_bytes;
} RwGpuDmaHeader;

typedef struct RwGpuDmaRange {
  uint64_t ram;
  uint32_t bytes, reserved;
} RwGpuDmaRange;

typedef struct RwGpuDmaTransfer {
  RwGpuDmaHeader header;
  uint64_t vram;
  uint32_t bytes, ranges;
} RwGpuDmaTransfer;

typedef struct RwGpuDmaCopy {
  RwGpuDmaHeader header;
  uint64_t source, destination;
  uint32_t bytes, reserved;
} RwGpuDmaCopy;

typedef struct RwGpuDmaFill {
  RwGpuDmaHeader header;
  uint64_t destination;
  uint32_t bytes, pattern;
} RwGpuDmaFill;

typedef struct RwGpuDmaDraw {
  RwGpuDmaHeader header;
  uint64_t destination;
  uint32_t width, height, clear_rgba, vertices;
} RwGpuDmaDraw;

static inline uint32_t rwgpu_dma_draw_bytes(uint32_t vertices) {
  return (uint32_t)(sizeof(RwGpuDmaDraw) + vertices * sizeof(RwGpuVertex) + 7u) & ~7u;
}
