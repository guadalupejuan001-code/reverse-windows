#pragma once

// VidMm owns the placements and MDL lifetimes. This miniport describes one
// linear VRAM segment and emits paging packets; the device executes them only
// when Dxgkrnl submits the corresponding DMA buffer.

typedef struct PrimeAllocation {
    UINT width, height;
    SIZE_T bytes;
} PrimeAllocation;

static BOOLEAN prime_vram_range(UINT64 offset, UINT64 bytes) {
    return offset <= RWGPU_LOGICAL_BYTES && bytes <= RWGPU_LOGICAL_BYTES - offset;
}

static NTSTATUS APIENTRY prime_create_allocation(HANDLE adapter, DXGKARG_CREATEALLOCATION* args) {
    UNREFERENCED_PARAMETER(adapter);
    if (args->NumAllocations != 1 || !args->pAllocationInfo || args->Flags.Resource)
        return STATUS_NOT_SUPPORTED;
    DXGK_ALLOCATIONINFO* info = args->pAllocationInfo;
    if (info->PrivateDriverDataSize != sizeof(RwGpuWddmAllocation) || !info->pPrivateDriverData)
        return STATUS_INVALID_PARAMETER;
    RwGpuWddmAllocation request;
    RtlCopyMemory(&request, info->pPrivateDriverData, sizeof(request));
    if (request.magic != RWGPU_WDDM_MAGIC || request.version != RWGPU_WDDM_ABI ||
        !request.width || request.width > RWGPU_MAX_WIDTH || !request.height || request.height > RWGPU_MAX_HEIGHT)
        return STATUS_INVALID_PARAMETER;
    PrimeAllocation* allocation = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*allocation), PRIME_POOL_TAG);
    if (!allocation) return STATUS_INSUFFICIENT_RESOURCES;
    allocation->width = request.width;
    allocation->height = request.height;
    allocation->bytes = ((SIZE_T)request.width * request.height * 4 + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    const UINT segment_set = 1U << (RWGPU_VIDMM_VRAM_SEGMENT - 1);
    info->Alignment = PAGE_SIZE;
    info->Size = allocation->bytes;
    info->PitchAlignedSize = 0;
    info->HintedBank.Value = 0;
    info->PreferredSegment.Value = 0;
    info->PreferredSegment.SegmentId0 = RWGPU_VIDMM_VRAM_SEGMENT;
    info->SupportedReadSegmentSet = segment_set;
    info->SupportedWriteSegmentSet = segment_set;
    info->EvictionSegmentSet = 0; // System memory backing.
    info->MaximumRenamingListLength = 0;
    info->Flags.Value = 0;
    info->Flags.CpuVisible = 1;
    info->Flags.PermanentSysMem = 1;
    info->Flags.Cached = 1;
    info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    info->hAllocation = allocation;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_destroy_allocation(HANDLE adapter, const DXGKARG_DESTROYALLOCATION* args) {
    UNREFERENCED_PARAMETER(adapter);
    for (UINT i = 0; i < args->NumAllocations; ++i)
        ExFreePoolWithTag(args->pAllocationList[i], PRIME_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_open_allocation(HANDLE handle, const DXGKARG_OPENALLOCATION* args) {
    const PrimeDevice* device = handle;
    for (UINT i = 0; i < args->NumAllocations; ++i) {
        DXGK_OPENALLOCATIONINFO* entry = &args->pOpenAllocation[i];
        const DXGKARGCB_GETHANDLEDATA request = {entry->hAllocation, DXGK_HANDLE_ALLOCATION, {0}};
        entry->hDeviceSpecificAllocation = device->gpu->dxgk.DxgkCbGetHandleData(&request);
        if (!entry->hDeviceSpecificAllocation) return STATUS_INVALID_HANDLE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_close_allocation(HANDLE handle, const DXGKARG_CLOSEALLOCATION* args) {
    // The device binding is a reference. Dxgkrnl closes it before it destroys
    // the underlying allocation, which is the only owned object.
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(args);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_describe_allocation(HANDLE handle, DXGKARG_DESCRIBEALLOCATION* args) {
    UNREFERENCED_PARAMETER(handle);
    const PrimeAllocation* allocation = args->hAllocation;
    if (!allocation) return STATUS_INVALID_HANDLE;
    args->Width = allocation->width;
    args->Height = allocation->height;
    args->Format = D3DDDIFMT_A8R8G8B8;
    args->MultisampleMethod.NumSamples = 1;
    args->MultisampleMethod.NumQualityLevels = 0;
    args->RefreshRate.Numerator = args->RefreshRate.Denominator = 0;
    args->PrivateDriverFormatAttribute = 0;
    args->Flags.Value = 0;
    args->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

static NTSTATUS prime_query_segments(const DXGKARG_QUERYADAPTERINFO* args) {
    if (!args->pOutputData) return STATUS_INVALID_PARAMETER;
    // ID zero selects physically contiguous paging buffers in system RAM.
    if (args->Type == DXGKQAITYPE_QUERYSEGMENT3) {
        if (args->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT3* out = args->pOutputData;
        out->NbSegment = 1;
        out->PagingBufferSegmentId = 0;
        out->PagingBufferSize = RWGPU_DMA_BUFFER_BYTES;
        out->PagingBufferPrivateDataSize = 0;
        if (out->pSegmentDescriptor) {
            RtlZeroMemory(out->pSegmentDescriptor, sizeof(*out->pSegmentDescriptor));
            out->pSegmentDescriptor->Size = RWGPU_LOGICAL_BYTES;
        }
    } else {
        if (args->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT* out = args->pOutputData;
        out->NbSegment = 1;
        out->PagingBufferSegmentId = 0;
        out->PagingBufferSize = RWGPU_DMA_BUFFER_BYTES;
        out->PagingBufferPrivateDataSize = 0;
        if (out->pSegmentDescriptor) {
            RtlZeroMemory(out->pSegmentDescriptor, sizeof(*out->pSegmentDescriptor));
            out->pSegmentDescriptor->Size = RWGPU_LOGICAL_BYTES;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS prime_paging_record(DXGKARG_BUILDPAGINGBUFFER* args, const VOID* record, UINT bytes) {
    if (args->DmaSize < bytes || !args->pDmaBuffer) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    RtlCopyMemory(args->pDmaBuffer, record, bytes);
    args->pDmaBuffer = (PUCHAR)args->pDmaBuffer + bytes;
    return STATUS_SUCCESS;
}

static NTSTATUS prime_build_transfer(DXGKARG_BUILDPAGINGBUFFER* args) {
    const UINT from = args->Transfer.Source.SegmentId, to = args->Transfer.Destination.SegmentId;
    const UINT64 offset = args->Transfer.TransferOffset, size = args->Transfer.TransferSize;
    if (from > RWGPU_VIDMM_VRAM_SEGMENT || to > RWGPU_VIDMM_VRAM_SEGMENT || (!from && !to))
        return STATUS_NOT_SUPPORTED;
    if (size > RWGPU_DIRECT_TRANSFER_BYTES || args->MultipassOffset) return STATUS_INVALID_PARAMETER;
    if (!size) return STATUS_SUCCESS;
    const UINT64 source = from ? (UINT64)args->Transfer.Source.SegmentAddress.QuadPart : 0;
    const UINT64 destination = to ? (UINT64)args->Transfer.Destination.SegmentAddress.QuadPart : 0;
    if ((from && !prime_vram_range(source, offset + size)) ||
        (to && (!prime_vram_range(destination, offset + size) ||
                 size > RWGPU_DIRECT_TRANSFER_BYTES - (destination + offset) % PAGE_SIZE)))
        return STATUS_INVALID_PARAMETER;
    if (from && to) {
        const RwGpuDmaCopy cmd = {{RWGPU_DMA_COPY, sizeof(cmd)}, source + offset, destination + offset, (UINT)size, 0};
        return prime_paging_record(args, &cmd, sizeof(cmd));
    }

    PMDL mdl = from ? args->Transfer.Destination.pMdl : args->Transfer.Source.pMdl;
    if (!mdl) return STATUS_INVALID_PARAMETER;
    const UINT64 mdl_pages = (UINT64)args->Transfer.MdlOffset * PAGE_SIZE;
    const UINT64 mdl_end = (UINT64)MmGetMdlByteOffset(mdl) + MmGetMdlByteCount(mdl);
    UINT64 cursor = mdl_pages ? mdl_pages : MmGetMdlByteOffset(mdl);
    if (cursor > mdl_end || size > mdl_end - cursor) return STATUS_INVALID_PARAMETER;
    const UINT ranges = (UINT)((cursor % PAGE_SIZE + size + PAGE_SIZE - 1) / PAGE_SIZE);
    const UINT dma_bytes = sizeof(RwGpuDmaTransfer) + ranges * sizeof(RwGpuDmaRange);
    if (!args->pDmaBuffer || args->DmaSize < dma_bytes) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    const PPFN_NUMBER pages = MmGetMdlPfnArray(mdl);
    RwGpuDmaTransfer cmd = {{from ? RWGPU_DMA_DOWNLOAD : RWGPU_DMA_UPLOAD, dma_bytes},
                            (from ? source : destination) + offset, (UINT)size, ranges};
    PUCHAR buffer = args->pDmaBuffer;
    RtlCopyMemory(buffer, &cmd, sizeof(cmd));
    UINT left = (UINT)size;
    for (UINT i = 0; i < ranges; ++i) {
        const UINT in_page = (UINT)(cursor % PAGE_SIZE), span = PAGE_SIZE - in_page;
        const UINT count = left < span ? left : span;
        const UINT64 pfn = pages[cursor / PAGE_SIZE];
        if (pfn > (UINT64_MAX >> PAGE_SHIFT)) return STATUS_INVALID_PARAMETER;
        const RwGpuDmaRange range = {(pfn << PAGE_SHIFT) + in_page, count, 0};
        RtlCopyMemory(buffer + sizeof(cmd) + i * sizeof(range), &range, sizeof(range));
        cursor += count;
        left -= count;
    }
    args->pDmaBuffer = buffer + dma_bytes;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_build_paging(HANDLE adapter, DXGKARG_BUILDPAGINGBUFFER* args) {
    UNREFERENCED_PARAMETER(adapter);
    switch (args->Operation) {
    case DXGK_OPERATION_TRANSFER:
    case DXGK_OPERATION_SPECIAL_LOCK_TRANSFER:
        return prime_build_transfer(args);
    case DXGK_OPERATION_FILL: {
        const UINT64 offset = (UINT64)args->Fill.Destination.SegmentAddress.QuadPart;
        const SIZE_T bytes = args->Fill.FillSize;
        if (args->Fill.Destination.SegmentId != RWGPU_VIDMM_VRAM_SEGMENT ||
            !prime_vram_range(offset, bytes) || (bytes & 3U) ||
            bytes > RWGPU_DIRECT_TRANSFER_BYTES - offset % PAGE_SIZE)
            return STATUS_INVALID_PARAMETER;
        if (!bytes) return STATUS_SUCCESS;
        const RwGpuDmaFill cmd = {{RWGPU_DMA_FILL, sizeof(cmd)}, offset, (UINT)bytes, args->Fill.FillPattern};
        return prime_paging_record(args, &cmd, sizeof(cmd));
    }
    case DXGK_OPERATION_DISCARD_CONTENT:
        // No device-side placement metadata. VidMm reinitializes recycled
        // locations through a fill or a transfer before it assigns them.
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}
