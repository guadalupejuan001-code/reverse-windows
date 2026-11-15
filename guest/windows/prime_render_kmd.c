#include <ntddk.h>
#include <dispmprt.h>
#include "gpu_pci_registers.h"
#include "../../src/c08_gpu_dma.h"
#include "../../src/c08_wddm_protocol.h"

#define PRIME_POOL_TAG 'kPwR'

typedef struct PrimeAdapter {
    DXGKRNL_INTERFACE dxgk;
    KMUTEX lock;
    KSPIN_LOCK submit;
    PUCHAR bar;
    BOOLEAN powered;
} PrimeAdapter;

typedef struct PrimeDevice {
    PrimeAdapter* gpu;
} PrimeDevice;

typedef struct PrimeContext {
    PrimeDevice* device;
} PrimeContext;

#include "prime_render_memory.h"

static NTSTATUS prime_add(PDEVICE_OBJECT physical, PVOID* context) {
    UNREFERENCED_PARAMETER(physical);
    PrimeAdapter* gpu = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*gpu), PRIME_POOL_TAG);
    if (!gpu) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(gpu, sizeof(*gpu));
    KeInitializeMutex(&gpu->lock, 0);
    KeInitializeSpinLock(&gpu->submit);
    *context = gpu;
    return STATUS_SUCCESS;
}

static VOID prime_lock(PrimeAdapter* gpu) {
    KeWaitForSingleObject(&gpu->lock, Executive, KernelMode, FALSE, NULL);
}

static VOID prime_unlock(PrimeAdapter* gpu) {
    KeReleaseMutex(&gpu->lock, FALSE);
}

static VOID prime_release(PrimeAdapter* gpu) {
    if (gpu->bar) {
        rwgpu_write(gpu->bar, RWGPU_REG_ENGINE_RESET, 1);
        gpu->dxgk.DxgkCbUnmapMemory(gpu->dxgk.DeviceHandle, gpu->bar);
        gpu->bar = NULL;
    }
    gpu->powered = FALSE;
}

static NTSTATUS prime_map(PrimeAdapter* gpu, PCM_RESOURCE_LIST resources) {
    if (!resources || resources->Count != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    const CM_PARTIAL_RESOURCE_LIST* list = &resources->List[0].PartialResourceList;
    BOOLEAN has_msi = FALSE;
    for (ULONG i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* resource = &list->PartialDescriptors[i];
        if (resource->Type == CmResourceTypeInterrupt && (resource->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
            has_msi = TRUE;
    }
    if (!has_msi) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (ULONG i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* bar = &list->PartialDescriptors[i];
        if (bar->Type != CmResourceTypeMemory || bar->u.Memory.Length != RWGPU_BAR_BYTES) continue;
        PVOID mapped = NULL;
        const NTSTATUS status = gpu->dxgk.DxgkCbMapMemory(gpu->dxgk.DeviceHandle,
            bar->u.Memory.Start, RWGPU_BAR_BYTES, FALSE, FALSE, MmNonCached, &mapped);
        if (!NT_SUCCESS(status)) return status;
        gpu->bar = mapped;
        const ULONG needed = RWGPU_CAP_WDDM_SCHEDULER;
        if (!mapped || rwgpu_read(gpu->bar, RWGPU_REG_MAGIC) != RWGPU_MAGIC ||
            rwgpu_read(gpu->bar, RWGPU_REG_ABI) != RWGPU_ABI ||
            (rwgpu_read(gpu->bar, RWGPU_REG_CAPS) & needed) != needed)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS prime_start(PVOID context, PDXGK_START_INFO start_info, PDXGKRNL_INTERFACE dxgk,
                            PULONG sources, PULONG children) {
    UNREFERENCED_PARAMETER(start_info);
    PrimeAdapter* gpu = context;
    DXGK_DEVICE_INFO device = {0};
    NTSTATUS status = dxgk->DxgkCbGetDeviceInformation(dxgk->DeviceHandle, &device);
    if (!NT_SUCCESS(status)) return status;
    prime_lock(gpu);
    gpu->dxgk = *dxgk;
    status = prime_map(gpu, device.TranslatedResourceList);
    if (NT_SUCCESS(status)) {
        rwgpu_write(gpu->bar, RWGPU_REG_ENGINE_RESET, 1);
        gpu->powered = TRUE;
        *sources = 0;
        *children = 0;
    } else {
        prime_release(gpu);
    }
    prime_unlock(gpu);
    return status;
}

static NTSTATUS prime_stop(PVOID context) {
    PrimeAdapter* gpu = context;
    prime_lock(gpu);
    prime_release(gpu);
    prime_unlock(gpu);
    return STATUS_SUCCESS;
}

static NTSTATUS prime_remove(PVOID context) {
    prime_stop(context);
    ExFreePoolWithTag(context, PRIME_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS prime_power(PVOID context, ULONG uid, DEVICE_POWER_STATE state, POWER_ACTION action) {
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(action);
    PrimeAdapter* gpu = context;
    prime_lock(gpu);
    gpu->powered = state == PowerDeviceD0;
    prime_unlock(gpu);
    return STATUS_SUCCESS;
}

static NTSTATUS prime_children(PVOID context, PDXGK_CHILD_DESCRIPTOR children, ULONG bytes) {
    UNREFERENCED_PARAMETER(context);
    if (bytes < sizeof(*children)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(children, sizeof(*children)); // This adapter has zero video outputs.
    return STATUS_SUCCESS;
}

static NTSTATUS prime_child_status(PVOID context, PDXGK_CHILD_STATUS child, BOOLEAN nondestructive) {
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(nondestructive);
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS prime_descriptor(PVOID context, ULONG child, PDXGK_DEVICE_DESCRIPTOR descriptor) {
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(child);
    UNREFERENCED_PARAMETER(descriptor);
    return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
}

static NTSTATUS prime_control(PVOID context, ULONG source, PVIDEO_REQUEST_PACKET packet) {
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(packet);
    return STATUS_NOT_SUPPORTED;
}

static BOOLEAN prime_isr(PVOID context, ULONG message) {
    PrimeAdapter* gpu = context;
    if (message != 0 || !gpu->bar || !gpu->powered) return FALSE;
    const ULONG irq = rwgpu_read(gpu->bar, RWGPU_REG_IRQ_STATUS);
    if (!irq) return FALSE;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA event = {0};
    const ULONG completed = rwgpu_read(gpu->bar, RWGPU_REG_FENCE_LAST);
    const ULONG preempt = rwgpu_read(gpu->bar, RWGPU_REG_FENCE_PREEMPT);
    const ULONG preempt_last = rwgpu_read(gpu->bar, RWGPU_REG_PREEMPT_LAST);
    const ULONG fault = rwgpu_read(gpu->bar, RWGPU_REG_FENCE_FAULT);
    rwgpu_write(gpu->bar, RWGPU_REG_IRQ_ACK, irq);
    if (irq & RWGPU_IRQ_COMPLETE) {
        event.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        event.DmaCompleted.SubmissionFenceId = completed;
        gpu->dxgk.DxgkCbNotifyInterrupt(gpu->dxgk.DeviceHandle, &event);
    }
    if (irq & RWGPU_IRQ_PREEMPT) {
        RtlZeroMemory(&event, sizeof(event));
        event.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
        event.DmaPreempted.PreemptionFenceId = preempt;
        event.DmaPreempted.LastCompletedFenceId = preempt_last;
        gpu->dxgk.DxgkCbNotifyInterrupt(gpu->dxgk.DeviceHandle, &event);
    }
    if (irq & RWGPU_IRQ_FAULT) {
        RtlZeroMemory(&event, sizeof(event));
        event.InterruptType = DXGK_INTERRUPT_DMA_FAULTED;
        event.DmaFaulted.FaultedFenceId = fault;
        event.DmaFaulted.Status = STATUS_DEVICE_DATA_ERROR;
        gpu->dxgk.DxgkCbNotifyInterrupt(gpu->dxgk.DeviceHandle, &event);
    }
    gpu->dxgk.DxgkCbQueueDpc(gpu->dxgk.DeviceHandle);
    return TRUE;
}

static VOID prime_dpc(PVOID context) {
    PrimeAdapter* gpu = context;
    gpu->dxgk.DxgkCbNotifyDpc(gpu->dxgk.DeviceHandle);
}
static VOID prime_reset(PVOID context) {
    PrimeAdapter* gpu = context;
    if (gpu->bar) rwgpu_write(gpu->bar, RWGPU_REG_ENGINE_RESET, 1);
}
static VOID prime_unload(VOID) {}

static NTSTATUS APIENTRY prime_create_device(HANDLE adapter, DXGKARG_CREATEDEVICE* args) {
    PrimeDevice* device = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*device), PRIME_POOL_TAG);
    if (!device) return STATUS_INSUFFICIENT_RESOURCES;
    device->gpu = adapter;
    args->hDevice = device;
    if (args->pInfo) {
        RtlZeroMemory(args->pInfo, sizeof(*args->pInfo));
        args->pInfo->DmaBufferSize = RWGPU_DMA_BUFFER_BYTES;
        args->pInfo->AllocationListSize = 1;
        args->pInfo->PatchLocationListSize = 1;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_destroy_device(HANDLE device) {
    ExFreePoolWithTag(device, PRIME_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_create_context(HANDLE device, DXGKARG_CREATECONTEXT* args) {
    if (args->NodeOrdinal != 0 || args->PrivateDriverDataSize != 0) return STATUS_INVALID_PARAMETER;
    PrimeContext* context = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*context), PRIME_POOL_TAG);
    if (!context) return STATUS_INSUFFICIENT_RESOURCES;
    context->device = device;
    RtlZeroMemory(&args->ContextInfo, sizeof(args->ContextInfo));
    args->ContextInfo.DmaBufferSize = RWGPU_DMA_BUFFER_BYTES;
    args->ContextInfo.AllocationListSize = 1;
    args->ContextInfo.PatchLocationListSize = 1;
    args->hContext = context;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_destroy_context(HANDLE context) {
    ExFreePoolWithTag(context, PRIME_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_node(HANDLE adapter, UINT ordinal, DXGKARG_GETNODEMETADATA* out) {
    UNREFERENCED_PARAMETER(adapter);
    if (ordinal != 0) return STATUS_INVALID_PARAMETER;
    const WCHAR name[] = L"PrimeVGC";
    RtlZeroMemory(out, sizeof(*out));
    out->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlCopyMemory(out->FriendlyName, name, sizeof(name));
    return STATUS_SUCCESS;
}

static VOID prime_adapter_info(PrimeAdapter* gpu, RwGpuWddmAdapter* out) {
    const ULONG draw = rwgpu_read(gpu->bar, RWGPU_REG_SCHEDULED_DRAW_TICKET);
    const RwGpuWddmAdapter info = {{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_QUERY, sizeof(info)},
        rwgpu_read_wide(gpu->bar, RWGPU_REG_LOGICAL_LO), (RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID,
        rwgpu_read(gpu->bar, RWGPU_REG_CAPS), RWGPU_MAX_WIDTH, RWGPU_MAX_HEIGHT, draw, draw};
    *out = info;
}

static NTSTATUS APIENTRY prime_query(HANDLE adapter, const DXGKARG_QUERYADAPTERINFO* args) {
    PrimeAdapter* gpu = adapter;
    if (args->Type == DXGKQAITYPE_UMDRIVERPRIVATE) {
        if (!args->pOutputData || args->OutputDataSize != sizeof(RwGpuWddmAdapter))
            return STATUS_INVALID_PARAMETER;
        prime_lock(gpu);
        const BOOLEAN ready = gpu->bar && gpu->powered;
        if (ready) prime_adapter_info(gpu, args->pOutputData);
        prime_unlock(gpu);
        return ready ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    if (args->Type == DXGKQAITYPE_DRIVERCAPS) {
        if (!args->pOutputData || args->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
            return STATUS_BUFFER_TOO_SMALL;
        DXGK_DRIVERCAPS* caps = args->pOutputData;
        RtlZeroMemory(caps, sizeof(*caps));
        caps->WDDMVersion = DXGKDDI_WDDMv1_3;
        caps->HighestAcceptableAddress.QuadPart = MAXLONGLONG;
        caps->SupportNonVGA = TRUE;
        caps->SchedulingCaps.MultiEngineAware = TRUE;
        caps->SchedulingCaps.PreemptionAware = TRUE;
        caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        return STATUS_SUCCESS;
    }
    if (args->Type == DXGKQAITYPE_QUERYSEGMENT || args->Type == DXGKQAITYPE_QUERYSEGMENT3)
        return prime_query_segments(args);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY prime_render(HANDLE context, DXGKARG_RENDER* args) {
    UNREFERENCED_PARAMETER(context);
    C_ASSERT(sizeof(RwGpuWddmRender) == sizeof(RwGpuDmaDraw));
    const UINT bytes = args->CommandLength;
    if (!args->pCommand || !args->pDmaBuffer || !args->pAllocationList || args->AllocationListSize != 1 ||
        !args->pPatchLocationListIn || args->PatchLocationListInSize != 1 || !args->pPatchLocationListOut ||
        args->MultipassOffset || bytes < sizeof(RwGpuWddmRender) || bytes > RWGPU_DMA_BUFFER_BYTES)
        return STATUS_INVALID_PARAMETER;
    const UINT padded = (bytes + 7U) & ~7U;
    if (padded > args->DmaSize || !args->PatchLocationListOutSize)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    PUCHAR output = args->pDmaBuffer;
    D3DDDI_PATCHLOCATIONLIST location;
    // Windows can pass a user VA. Snapshot each input once and validate
    // the copy before publishing any DMA or patch-list output pointers.
    __try {
        RtlCopyMemory(output, args->pCommand, bytes);
        RtlCopyMemory(&location, args->pPatchLocationListIn, sizeof(location));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return (NTSTATUS)GetExceptionCode();
    }
    RwGpuWddmRender render;
    RtlCopyMemory(&render, output, sizeof(render));
    const DXGK_ALLOCATIONLIST* entry = args->pAllocationList;
    const PrimeAllocation* allocation = entry->hDeviceSpecificAllocation;
    if (!allocation || !entry->WriteOperation ||
        render.header.magic != RWGPU_WDDM_MAGIC || render.header.version != RWGPU_WDDM_ABI ||
        render.header.command != RWGPU_WDDM_RENDER || render.header.bytes != bytes ||
        render.width != allocation->width || render.height != allocation->height ||
        render.vertices > RWGPU_MAX_VERTICES || render.vertices % 3 ||
        bytes != sizeof(render) + render.vertices * sizeof(RwGpuVertex) ||
        location.AllocationIndex != 0 || location.Value != 0 || location.AllocationOffset != 0 ||
        location.DriverId != 0 || location.SplitOffset != 0 ||
        location.PatchOffset != FIELD_OFFSET(RwGpuDmaDraw, destination))
        return STATUS_INVALID_PARAMETER;
    const RwGpuDmaDraw draw = {{RWGPU_DMA_DRAW, padded}, UINT64_MAX,
        render.width, render.height, render.clear_rgba, render.vertices};
    RtlCopyMemory(output, &draw, sizeof(draw));
    RtlZeroMemory(output + bytes, padded - bytes);
    *args->pPatchLocationListOut++ = location;
    args->pDmaBuffer = output + padded;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_patch(HANDLE adapter, const DXGKARG_PATCH* args) {
    UNREFERENCED_PARAMETER(adapter);
    if (args->Flags.Paging || args->Flags.NullRendering) return STATUS_SUCCESS;
    const UINT start = args->DmaBufferSubmissionStartOffset;
    const UINT end = args->DmaBufferSubmissionEndOffset;
    if (!args->pDmaBuffer || start > end || end > args->DmaBufferSize ||
        end - start < sizeof(RwGpuDmaDraw) || !args->pAllocationList || args->AllocationListSize != 1 ||
        !args->pPatchLocationList || args->PatchLocationListSubmissionLength != 1 ||
        args->PatchLocationListSubmissionStart >= args->PatchLocationListSize)
        return STATUS_INVALID_PARAMETER;
    const D3DDDI_PATCHLOCATIONLIST* place = &args->pPatchLocationList[args->PatchLocationListSubmissionStart];
    const DXGK_ALLOCATIONLIST* entry = args->pAllocationList;
    const PrimeAllocation* allocation = entry->hDeviceSpecificAllocation;
    const UINT64 offset = (UINT64)entry->PhysicalAddress.QuadPart;
    if (!allocation || place->AllocationIndex != 0 ||
        place->PatchOffset != start + FIELD_OFFSET(RwGpuDmaDraw, destination) ||
        (entry->SegmentId && (entry->SegmentId != RWGPU_VIDMM_VRAM_SEGMENT ||
            !prime_vram_range(offset, allocation->bytes) || offset % PAGE_SIZE)))
        return STATUS_INVALID_PARAMETER;
    // Initial patching can precede residency. Keep an invalid address until
    // VidMm supplies the segment and current placement.
    const UINT64 destination = entry->SegmentId ? offset : UINT64_MAX;
    RtlCopyMemory((PUCHAR)args->pDmaBuffer + place->PatchOffset, &destination, sizeof(destination));
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_submit(HANDLE adapter, const DXGKARG_SUBMITCOMMAND* args) {
    PrimeAdapter* gpu = adapter;
    if (!gpu->bar || !gpu->powered) return STATUS_DEVICE_NOT_READY;
    const UINT start = args->DmaBufferSubmissionStartOffset, end = args->DmaBufferSubmissionEndOffset;
    ULONG bytes = 0;
    UINT64 address = 0;
    if (!args->Flags.NullRendering && !args->Flags.ContextSwitch) {
        const UINT64 base = (UINT64)args->DmaBufferPhysicalAddress.QuadPart;
        // A rejected packet is reported through the device's fault ISR and
        // that submission's fence, keeping it separate from success.
        if (start > end || end > args->DmaBufferSize || args->DmaBufferSegmentId ||
            args->EngineOrdinal || args->NodeOrdinal || start > UINT64_MAX - base) {
            bytes = RWGPU_DMA_BUFFER_BYTES + 8;
        } else if (start != end) {
            bytes = end - start;
            address = base + start;
        }
    }
    KeAcquireSpinLockAtDpcLevel(&gpu->submit);
    (void)rwgpu_submit(gpu->bar, RWGPU_SUBMIT_DMA, address, args->SubmissionFenceId, bytes);
    KeReleaseSpinLockFromDpcLevel(&gpu->submit);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_preempt(HANDLE adapter, const DXGKARG_PREEMPTCOMMAND* args) {
    PrimeAdapter* gpu = adapter;
    if (!gpu->bar || !gpu->powered || args->NodeOrdinal || args->EngineOrdinal)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLockAtDpcLevel(&gpu->submit);
    const NTSTATUS status = rwgpu_submit(gpu->bar, RWGPU_PREEMPT, 0, args->PreemptionFenceId, 0);
    KeReleaseSpinLockFromDpcLevel(&gpu->submit);
    return status;
}

static NTSTATUS APIENTRY prime_fence(HANDLE adapter, DXGKARG_QUERYCURRENTFENCE* args) {
    const PrimeAdapter* gpu = adapter;
    if (!gpu->bar || args->NodeOrdinal || args->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    args->CurrentFence = rwgpu_read(gpu->bar, RWGPU_REG_FENCE_LAST);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_reset_timeout(HANDLE adapter) {
    prime_reset(adapter);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY prime_restart_timeout(HANDLE adapter) {
    const PrimeAdapter* gpu = adapter;
    return gpu->bar && gpu->powered ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static NTSTATUS APIENTRY prime_escape(HANDLE adapter, const DXGKARG_ESCAPE* args) {
    if (!args->Flags.HardwareAccess || !args->pPrivateDriverData ||
        args->PrivateDriverDataSize < sizeof(RwGpuWddmHeader)) return STATUS_INVALID_PARAMETER;
    RwGpuWddmHeader header;
    RtlCopyMemory(&header, args->pPrivateDriverData, sizeof(header));
    if (header.magic != RWGPU_WDDM_MAGIC || header.version != RWGPU_WDDM_ABI ||
        header.bytes != args->PrivateDriverDataSize) return STATUS_INVALID_PARAMETER;
    PrimeAdapter* gpu = adapter;
    const PrimeDevice* device = args->hDevice;
    const PrimeContext* context = args->hContext;
    if ((device && device->gpu != gpu) || (context && (!device || context->device != device)))
        return STATUS_INVALID_PARAMETER;
    prime_lock(gpu);
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    if (gpu->bar && gpu->powered) {
        status = STATUS_INVALID_PARAMETER;
        if (header.command == RWGPU_WDDM_QUERY && header.bytes == sizeof(RwGpuWddmAdapter)) {
            RwGpuWddmAdapter info;
            prime_adapter_info(gpu, &info);
            RtlCopyMemory(args->pPrivateDriverData, &info, sizeof(info));
            status = STATUS_SUCCESS;
        }
    }
    prime_unlock(gpu);
    return status;
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    DRIVER_INITIALIZATION_DATA ddi = {0};
    ddi.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    ddi.DxgkDdiAddDevice = prime_add;
    ddi.DxgkDdiStartDevice = prime_start;
    ddi.DxgkDdiStopDevice = prime_stop;
    ddi.DxgkDdiRemoveDevice = prime_remove;
    ddi.DxgkDdiDispatchIoRequest = prime_control;
    ddi.DxgkDdiInterruptRoutine = prime_isr;
    ddi.DxgkDdiDpcRoutine = prime_dpc;
    ddi.DxgkDdiQueryChildRelations = prime_children;
    ddi.DxgkDdiQueryChildStatus = prime_child_status;
    ddi.DxgkDdiQueryDeviceDescriptor = prime_descriptor;
    ddi.DxgkDdiSetPowerState = prime_power;
    ddi.DxgkDdiResetDevice = prime_reset;
    ddi.DxgkDdiUnload = prime_unload;
    ddi.DxgkDdiQueryAdapterInfo = prime_query;
    ddi.DxgkDdiCreateDevice = prime_create_device;
    ddi.DxgkDdiDestroyDevice = prime_destroy_device;
    ddi.DxgkDdiCreateContext = prime_create_context;
    ddi.DxgkDdiDestroyContext = prime_destroy_context;
    ddi.DxgkDdiCreateAllocation = prime_create_allocation;
    ddi.DxgkDdiDestroyAllocation = prime_destroy_allocation;
    ddi.DxgkDdiOpenAllocation = prime_open_allocation;
    ddi.DxgkDdiCloseAllocation = prime_close_allocation;
    ddi.DxgkDdiDescribeAllocation = prime_describe_allocation;
    ddi.DxgkDdiBuildPagingBuffer = prime_build_paging;
    ddi.DxgkDdiRender = prime_render;
    ddi.DxgkDdiPatch = prime_patch;
    ddi.DxgkDdiSubmitCommand = prime_submit;
    ddi.DxgkDdiPreemptCommand = prime_preempt;
    ddi.DxgkDdiQueryCurrentFence = prime_fence;
    ddi.DxgkDdiResetFromTimeout = prime_reset_timeout;
    ddi.DxgkDdiRestartFromTimeout = prime_restart_timeout;
    ddi.DxgkDdiGetNodeMetadata = prime_node;
    ddi.DxgkDdiEscape = prime_escape;
    return DxgkInitialize(driver, registry, &ddi);
}
