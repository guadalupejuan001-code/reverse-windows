#pragma once
#include "../../src/c08_gpu_protocol.h"

static inline ULONG rwgpu_read(PUCHAR bar, ULONG offset) {
    return READ_REGISTER_ULONG((volatile ULONG*)(bar + offset));
}

static inline VOID rwgpu_write(PUCHAR bar, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((volatile ULONG*)(bar + offset), value);
}

static inline uint64_t rwgpu_read_wide(PUCHAR bar, ULONG offset) {
    const ULONG low = rwgpu_read(bar, offset);
    const ULONG high = rwgpu_read(bar, offset + 4);
    return ((uint64_t)high << 32) | low;
}

static inline VOID rwgpu_write_wide(PUCHAR bar, ULONG offset, uint64_t value) {
    rwgpu_write(bar, offset, (ULONG)value);
    rwgpu_write(bar, offset + 4, (ULONG)(value >> 32));
}

// The caller holds exclusive access to the adapter for the entire submission.
static inline NTSTATUS rwgpu_submit(PUCHAR bar, ULONG command, uint64_t source,
                                    uint64_t destination, ULONG count) {
    ULONG ticket = rwgpu_read(bar, RWGPU_REG_COMPLETED) + 1;
    if (!ticket) ticket = 1;
    rwgpu_write_wide(bar, RWGPU_REG_OFFSET_LO, source);
    rwgpu_write_wide(bar, RWGPU_REG_AUX_LO, destination);
    rwgpu_write(bar, RWGPU_REG_LENGTH, count);
    rwgpu_write(bar, RWGPU_REG_LENGTH_HI, 0);
    rwgpu_write(bar, RWGPU_REG_TICKET, ticket);
    rwgpu_write(bar, RWGPU_REG_DOORBELL, command);
    if (rwgpu_read(bar, RWGPU_REG_COMPLETED) != ticket) return STATUS_DEVICE_DATA_ERROR;
    const ULONG status = rwgpu_read(bar, RWGPU_REG_STATUS);
    if (status == RWGPU_OK) return STATUS_SUCCESS;
    if (status == RWGPU_UNAVAILABLE) return STATUS_DEVICE_NOT_READY;
    if (status == RWGPU_QUANTUM_UNAVAILABLE) return STATUS_DEVICE_NOT_READY;
    if (status == RWGPU_INVALID) return STATUS_INVALID_PARAMETER;
    return STATUS_DEVICE_DATA_ERROR;
}
