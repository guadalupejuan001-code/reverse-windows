#pragma once

#include "../../src/c08_gpu_protocol.h"

// Buffered IOCTL. The driver accepts only the fixed render command, bounded
// geometry and bounded readback. There are no raw register or DMA requests.
#define RWGPU_IOCTL_DRAW CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define RWGPU_DEVICE_PATH L"\\\\.\\ReversePrimeGpu"

typedef struct RwGpuDrawRequest {
  uint32_t version, width, height, clear_rgba, vertex_count;
} RwGpuDrawRequest;
