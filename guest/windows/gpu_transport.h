#pragma once

#include "../../src/c08_gpu_protocol.h"

// Only the driver programs bus addresses from its DMA common buffer. All
// offsets supplied by clients are relative to virtual VRAM.
#define RWGPU_IOCTL_DRAW CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define RWGPU_IOCTL_GET_DMA CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
// Upload data is passed in DeviceIoControl's MDL buffer (lpOutBuffer).
#define RWGPU_IOCTL_UPLOAD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_IN_DIRECT, FILE_WRITE_DATA)
#define RWGPU_IOCTL_DOWNLOAD CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_OUT_DIRECT, FILE_WRITE_DATA)
#define RWGPU_IOCTL_DRAW_VRAM CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_OUT_DIRECT, FILE_WRITE_DATA)
#define RWGPU_DEVICE_PATH L"\\\\.\\ReversePrimeGpu"

typedef struct RwGpuDrawRequest {
  uint32_t version, width, height, clear_rgba, vertex_count;
} RwGpuDrawRequest;

typedef struct RwGpuDmaInfo {
  uint64_t logical_bytes;
  uint32_t transfer_bytes, capabilities;
} RwGpuDmaInfo;

typedef struct RwGpuVramTransfer {
  uint64_t offset;
  uint32_t version, bytes;
} RwGpuVramTransfer;

typedef struct RwGpuDrawVramRequest {
  uint64_t offset;
  RwGpuDrawRequest draw;
  uint32_t reserved;
} RwGpuDrawVramRequest;
