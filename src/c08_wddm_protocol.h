#pragma once
#include "c08_gpu_protocol.h"

// Driver-private WDDM commands. The packets contain no user pointer or DMA
// address. Draw output is BGRA and follows the vertices in the same packet.
// The same request returns its PCI command ticket to the caller.
#define RWGPU_WDDM_MAGIC UINT32_C(0x31445752)
#define RWGPU_WDDM_ABI 2u
enum RwGpuWddmCommand { RWGPU_WDDM_QUERY = 1, RWGPU_WDDM_DRAW = 2 };

typedef struct RwGpuWddmHeader {
  uint32_t magic, version, command, bytes;
} RwGpuWddmHeader;

typedef struct RwGpuWddmAdapter {
  RwGpuWddmHeader header;
  uint64_t logical_vram_bytes;
  uint32_t pci_id, capabilities, max_width, max_height;
} RwGpuWddmAdapter;

typedef struct RwGpuWddmFrame {
  RwGpuWddmHeader header;
  uint32_t width, height, clear_rgba, vertex_count, draw_ticket;
} RwGpuWddmFrame;

static inline int rwgpu_wddm_frame_size(const RwGpuWddmFrame* frame,
                                       uint32_t* vertices, uint32_t* pixels) {
  if (!frame->width || frame->width > RWGPU_MAX_WIDTH ||
      !frame->height || frame->height > RWGPU_MAX_HEIGHT ||
      frame->vertex_count > RWGPU_MAX_VERTICES || frame->vertex_count % 3) return 0;
  *vertices = frame->vertex_count * (uint32_t)sizeof(RwGpuVertex);
  *pixels = frame->width * frame->height * 4u;
  return frame->header.bytes == (uint32_t)sizeof(*frame) + *vertices + *pixels;
}
