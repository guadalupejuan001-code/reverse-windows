#include <ntddk.h>
#include <dispmprt.h>
#include "gpu_pci_registers.h"
#include "../../src/c08_wddm_protocol.h"

#define PRIME_POOL_TAG 'kPwR'

typedef struct PrimeAdapter {
    PDEVICE_OBJECT physical;
    DXGKRNL_INTERFACE dxgk;
    KMUTEX lock;
    PUCHAR bar;
    PDMA_ADAPTER adapter;
    PUCHAR dma;
    PHYSICAL_ADDRESS bus;
    ULONG dmaBytes;
    ULONG drawTicket;
    BOOLEAN powered;
} PrimeAdapter;

typedef struct PrimeDevice {
    PrimeAdapter* gpu;
} PrimeDevice;

typedef struct PrimeContext {
    PrimeDevice* device;
} PrimeContext;

static NTSTATUS prime_add(PDEVICE_OBJECT physical, PVOID* context) {
    PrimeAdapter* gpu = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*gpu), PRIME_POOL_TAG);
    if (!gpu) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(gpu, sizeof(*gpu));
    gpu->physical = physical;
    KeInitializeMutex(&gpu->lock, 0);
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
    if (gpu->dma) {
        gpu->adapter->DmaOperations->FreeCommonBuffer(
            gpu->adapter, gpu->dmaBytes, gpu->bus, gpu->dma, FALSE);
        gpu->dma = NULL;
        gpu->dmaBytes = 0;
    }
    if (gpu->adapter) {
        gpu->adapter->DmaOperations->PutDmaAdapter(gpu->adapter);
        gpu->adapter = NULL;
    }
    if (gpu->bar) {
        gpu->dxgk.DxgkCbUnmapMemory(gpu->dxgk.DeviceHandle, gpu->bar);
        gpu->bar = NULL;
    }
    gpu->powered = FALSE;
    gpu->drawTicket = 0;
}

static NTSTATUS prime_map(PrimeAdapter* gpu, PCM_RESOURCE_LIST resources) {
    if (!resources || resources->Count != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    const CM_PARTIAL_RESOURCE_LIST* list = &resources->List[0].PartialResourceList;
    for (ULONG i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* bar = &list->PartialDescriptors[i];
        if (bar->Type != CmResourceTypeMemory || bar->u.Memory.Length != RWGPU_BAR_BYTES) continue;
        PVOID mapped = NULL;
        const NTSTATUS status = gpu->dxgk.DxgkCbMapMemory(gpu->dxgk.DeviceHandle,
            bar->u.Memory.Start, RWGPU_BAR_BYTES, FALSE, FALSE, MmNonCached, &mapped);
        if (!NT_SUCCESS(status)) return status;
        gpu->bar = mapped;
        const ULONG needed = RWGPU_CAP_DRAW_RAM | RWGPU_CAP_COLOR_DMA | RWGPU_CAP_COLOR_ALPHA;
        if (!mapped || rwgpu_read(gpu->bar, RWGPU_REG_MAGIC) != RWGPU_MAGIC ||
            rwgpu_read(gpu->bar, RWGPU_REG_ABI) != RWGPU_ABI ||
            (rwgpu_read(gpu->bar, RWGPU_REG_CAPS) & needed) != needed)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS prime_allocate_dma(PrimeAdapter* gpu) {
    DEVICE_DESCRIPTION description = {0};
    description.Version = DEVICE_DESCRIPTION_VERSION2;
    description.InterfaceType = PCIBus;
    description.Master = TRUE;
    description.Dma32BitAddresses = TRUE;
    description.Dma64BitAddresses = TRUE;
    // Prefer one common buffer for both vertices and the complete BGRA frame.
    // Systems with fragmented memory can retain the existing chunked readback.
    const ULONG geometry = RWGPU_MAX_VERTICES * (ULONG)sizeof(RwGpuVertex);
    const ULONG minimum = (geometry + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1UL);
    const ULONG preferred = rwgpu_read(gpu->bar, RWGPU_REG_CAPS) & RWGPU_CAP_DRAW_READBACK
        ? RWGPU_DIRECT_TRANSFER_BYTES : minimum;
    description.MaximumLength = preferred;
    ULONG registers = 0;
    gpu->adapter = IoGetDmaAdapter(gpu->physical, &description, &registers);
    if (!gpu->adapter) return STATUS_INSUFFICIENT_RESOURCES;
    gpu->dma = gpu->adapter->DmaOperations->AllocateCommonBuffer(
        gpu->adapter, preferred, &gpu->bus, FALSE);
    if (gpu->dma) {
        gpu->dmaBytes = preferred;
    } else if (preferred != minimum) {
        gpu->dma = gpu->adapter->DmaOperations->AllocateCommonBuffer(
            gpu->adapter, minimum, &gpu->bus, FALSE);
        if (gpu->dma) gpu->dmaBytes = minimum;
    }
    if (!gpu->dma) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
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
    if (NT_SUCCESS(status)) status = prime_allocate_dma(gpu);
    if (NT_SUCCESS(status)) {
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
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(message);
    return FALSE;
}

static VOID prime_dpc(PVOID context) { UNREFERENCED_PARAMETER(context); }
static VOID prime_reset(PVOID context) { UNREFERENCED_PARAMETER(context); }
static VOID prime_unload(VOID) {}

static NTSTATUS APIENTRY prime_create_device(HANDLE adapter, DXGKARG_CREATEDEVICE* args) {
    PrimeDevice* device = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*device), PRIME_POOL_TAG);
    if (!device) return STATUS_INSUFFICIENT_RESOURCES;
    device->gpu = adapter;
    args->hDevice = device;
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
    args->ContextInfo.DmaBufferSize = PAGE_SIZE;
    args->ContextInfo.AllocationListSize = 1;
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
    const RwGpuWddmAdapter info = {{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_QUERY, sizeof(info)},
        rwgpu_read_wide(gpu->bar, RWGPU_REG_LOGICAL_LO), (RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID,
        rwgpu_read(gpu->bar, RWGPU_REG_CAPS), RWGPU_MAX_WIDTH, RWGPU_MAX_HEIGHT, gpu->drawTicket, 0};
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
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS prime_draw(PrimeAdapter* gpu, RwGpuWddmFrame* frame, PUCHAR data) {
    uint32_t vertices, pixels;
    if (frame->draw_ticket || !rwgpu_wddm_frame_size(frame, &vertices, &pixels) || vertices > gpu->dmaBytes)
        return STATUS_INVALID_PARAMETER;
    if (!rwgpu_read(gpu->bar, RWGPU_REG_BUS_MASTER)) return STATUS_DEVICE_NOT_READY;
    const uint64_t bus = (uint64_t)gpu->bus.QuadPart;
    if (vertices) RtlCopyMemory(gpu->dma, data, vertices);
    KeMemoryBarrier();
    rwgpu_write(gpu->bar, RWGPU_REG_WIDTH, frame->width);
    rwgpu_write(gpu->bar, RWGPU_REG_HEIGHT, frame->height);
    rwgpu_write(gpu->bar, RWGPU_REG_CLEAR_RGBA, frame->clear_rgba);
    if (pixels <= gpu->dmaBytes - vertices &&
        (rwgpu_read(gpu->bar, RWGPU_REG_CAPS) & RWGPU_CAP_DRAW_READBACK)) {
        const NTSTATUS status = rwgpu_submit(gpu->bar, RWGPU_DRAW_READBACK, bus, bus + vertices, vertices);
        if (!NT_SUCCESS(status)) return status;
        if (rwgpu_read_wide(gpu->bar, RWGPU_REG_RESULT_LO) != pixels) return STATUS_DEVICE_DATA_ERROR;
        KeMemoryBarrier();
        RtlCopyMemory(data + vertices, gpu->dma + vertices, pixels);
        frame->draw_ticket = rwgpu_read(gpu->bar, RWGPU_REG_COMPLETED);
        gpu->drawTicket = frame->draw_ticket;
        return STATUS_SUCCESS;
    }
    NTSTATUS status = rwgpu_submit(gpu->bar, RWGPU_DRAW_RAM, bus, RWGPU_DRAW_COLOR_ALPHA, vertices);
    if (!NT_SUCCESS(status)) return status;
    const ULONG ticket = rwgpu_read(gpu->bar, RWGPU_REG_COMPLETED);
    for (ULONG offset = 0; offset < pixels;) {
        const ULONG remaining = pixels - offset;
        const ULONG chunk = remaining < gpu->dmaBytes ? remaining : gpu->dmaBytes;
        status = rwgpu_submit(gpu->bar, RWGPU_READ_COLOR_RAM, offset, bus, chunk);
        if (!NT_SUCCESS(status)) return status;
        if (rwgpu_read_wide(gpu->bar, RWGPU_REG_RESULT_LO) != chunk) return STATUS_DEVICE_DATA_ERROR;
        KeMemoryBarrier();
        RtlCopyMemory(data + vertices + offset, gpu->dma, chunk);
        offset += chunk;
    }
    frame->draw_ticket = ticket;
    gpu->drawTicket = ticket;
    return STATUS_SUCCESS;
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
    if (gpu->bar && gpu->powered && gpu->dma) {
        status = STATUS_INVALID_PARAMETER;
        if (header.command == RWGPU_WDDM_QUERY && header.bytes == sizeof(RwGpuWddmAdapter)) {
            RwGpuWddmAdapter info;
            prime_adapter_info(gpu, &info);
            RtlCopyMemory(args->pPrivateDriverData, &info, sizeof(info));
            status = STATUS_SUCCESS;
        } else if (header.command == RWGPU_WDDM_DRAW && header.bytes >= sizeof(RwGpuWddmFrame)) {
            RwGpuWddmFrame frame;
            RtlCopyMemory(&frame, args->pPrivateDriverData, sizeof(frame));
            status = prime_draw(gpu, &frame, (PUCHAR)args->pPrivateDriverData + sizeof(frame));
            if (NT_SUCCESS(status)) RtlCopyMemory(args->pPrivateDriverData, &frame, sizeof(frame));
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
    ddi.DxgkDdiGetNodeMetadata = prime_node;
    ddi.DxgkDdiEscape = prime_escape;
    return DxgkInitialize(driver, registry, &ddi);
}
