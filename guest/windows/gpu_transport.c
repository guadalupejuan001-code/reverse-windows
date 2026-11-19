#include <ntddk.h>
#include <devioctl.h>
#include <wdmsec.h>
#include "gpu_transport.h"
#include "gpu_pci_registers.h"

typedef struct RwGpuDevice {
    PDEVICE_OBJECT lower;
    PDEVICE_OBJECT physical;
    PUCHAR bar;
    KMUTEX serial;
    IO_REMOVE_LOCK remove;
    PDMA_ADAPTER adapter;
    PUCHAR dma;
    PHYSICAL_ADDRESS busAddress;
    ULONG dmaBytes;
    BOOLEAN published;
} RwGpuDevice;

static UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\ReversePrimeGpu");
static UNICODE_STRING linkName = RTL_CONSTANT_STRING(L"\\DosDevices\\ReversePrimeGpu");
static UNICODE_STRING access = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
static GUID deviceClass = {0xec4a2555, 0xb03e, 0x4093, {0xba, 0x54, 0x53, 0xd4, 0xaa, 0xc5, 0x61, 0xa1}};

static NTSTATUS complete(PIRP irp, NTSTATUS status, ULONG_PTR length) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = length;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS signal_forward(PDEVICE_OBJECT dev, PIRP irp, PVOID cookie) {
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)cookie, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS forward_wait(RwGpuDevice* state, PIRP irp) {
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, signal_forward, &done, TRUE, TRUE, TRUE);
    if (IoCallDriver(state->lower, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
    return irp->IoStatus.Status;
}

static NTSTATUS unlock_forward(PDEVICE_OBJECT dev, PIRP irp, PVOID cookie) {
    RwGpuDevice* state = (RwGpuDevice*)cookie;
    UNREFERENCED_PARAMETER(dev);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    IoReleaseRemoveLock(&state->remove, irp);
    return STATUS_SUCCESS;
}

static NTSTATUS forward_locked(RwGpuDevice* state, PIRP irp) {
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, unlock_forward, state, TRUE, TRUE, TRUE);
    return IoCallDriver(state->lower, irp);
}

static ULONG reg_read(RwGpuDevice* state, ULONG offset) {
    return rwgpu_read(state->bar, offset);
}

static VOID reg_write(RwGpuDevice* state, ULONG offset, ULONG value) {
    rwgpu_write(state->bar, offset, value);
}

static VOID lock_device(RwGpuDevice* state) {
    KeWaitForSingleObject(&state->serial, Executive, KernelMode, FALSE, NULL);
}

static VOID unlock_device(RwGpuDevice* state) {
    KeReleaseMutex(&state->serial, FALSE);
}

static VOID allocate_dma(RwGpuDevice* state) {
    const ULONG caps = reg_read(state, RWGPU_REG_CAPS);
    if (!(caps & RWGPU_CAP_DIRECT_VRAM)) return;
    ULONG requested = reg_read(state, RWGPU_REG_DIRECT_MAX);
    if (requested < PAGE_SIZE || requested > RWGPU_DIRECT_TRANSFER_BYTES ||
        requested % PAGE_SIZE != 0) return;
    DEVICE_DESCRIPTION description = {0};
    description.Version = DEVICE_DESCRIPTION_VERSION2;
    description.InterfaceType = PCIBus;
    description.Master = TRUE;
    description.Dma32BitAddresses = TRUE;
    description.Dma64BitAddresses = TRUE;
    description.MaximumLength = requested;
    ULONG registers = 0;
    PDMA_ADAPTER adapter = IoGetDmaAdapter(state->physical, &description, &registers);
    if (!adapter) return;
    for (; requested >= PAGE_SIZE; requested = (requested / 2) & ~(PAGE_SIZE - 1UL)) {
        PUCHAR buffer = adapter->DmaOperations->AllocateCommonBuffer(
            adapter, requested, &state->busAddress, FALSE);
        if (buffer) {
            state->adapter = adapter;
            state->dma = buffer;
            state->dmaBytes = requested;
            return;
        }
    }
    adapter->DmaOperations->PutDmaAdapter(adapter);
}

static BOOLEAN dma_ready(RwGpuDevice* state) {
    return state->dma && reg_read(state, RWGPU_REG_BUS_MASTER) != 0;
}

static VOID unmap(RwGpuDevice* state) {
    lock_device(state);
    if (state->dma) {
        state->adapter->DmaOperations->FreeCommonBuffer(
            state->adapter, state->dmaBytes, state->busAddress, state->dma, FALSE);
        state->adapter->DmaOperations->PutDmaAdapter(state->adapter);
        state->adapter = NULL;
        state->dma = NULL;
        state->dmaBytes = 0;
    }
    if (state->bar) {
        MmUnmapIoSpace(state->bar, RWGPU_BAR_BYTES);
        state->bar = NULL;
    }
    unlock_device(state);
}

static NTSTATUS map_translated(RwGpuDevice* state, const CM_RESOURCE_LIST* resources) {
    if (!resources || resources->Count != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    const CM_PARTIAL_RESOURCE_LIST* list = &resources->List[0].PartialResourceList;
    for (ULONG i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* bar = &list->PartialDescriptors[i];
        if (bar->Type != CmResourceTypeMemory || bar->u.Memory.Length != RWGPU_BAR_BYTES) continue;
        lock_device(state);
        state->bar = (PUCHAR)MmMapIoSpaceEx(bar->u.Memory.Start, RWGPU_BAR_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
        BOOLEAN okay = state->bar && reg_read(state, RWGPU_REG_MAGIC) == RWGPU_MAGIC &&
            reg_read(state, RWGPU_REG_ABI) == RWGPU_ABI &&
            (reg_read(state, RWGPU_REG_CAPS) & RWGPU_CAP_TRIANGLES);
        if (okay) allocate_dma(state);
        unlock_device(state);
        if (!okay) {
            unmap(state);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

static NTSTATUS dispatch_pnp(PDEVICE_OBJECT dev, PIRP irp) {
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    PIO_STACK_LOCATION req = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    if (!NT_SUCCESS(status)) return complete(irp, status, 0);
    if (req->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        IoReleaseRemoveLockAndWait(&state->remove, irp);
        if (state->published) IoDeleteSymbolicLink(&linkName);
        unmap(state);
        PDEVICE_OBJECT lower = state->lower;
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(lower, irp);
        IoDetachDevice(lower);
        IoDeleteDevice(dev);
        return status;
    }
    if (req->MinorFunction == IRP_MN_START_DEVICE) {
        status = forward_wait(state, irp);
        if (NT_SUCCESS(status))
            status = map_translated(state, req->Parameters.StartDevice.AllocatedResourcesTranslated);
        if (NT_SUCCESS(status) && !state->published) {
            status = IoCreateSymbolicLink(&linkName, &name);
            if (NT_SUCCESS(status)) state->published = TRUE;
            else unmap(state);
        }
        IoReleaseRemoveLock(&state->remove, irp);
        return complete(irp, status, 0);
    }
    if (req->MinorFunction == IRP_MN_SURPRISE_REMOVAL || req->MinorFunction == IRP_MN_STOP_DEVICE) {
        if (state->published) IoDeleteSymbolicLink(&linkName);
        state->published = FALSE;
        unmap(state);
    }
    return forward_locked(state, irp);
}

static uint64_t read_wide(RwGpuDevice* state, ULONG reg) {
    return rwgpu_read_wide(state->bar, reg);
}

static NTSTATUS transfer(RwGpuDevice* state, ULONG op, uint64_t offset, uint64_t aux, ULONG count) {
    return rwgpu_submit(state->bar, op, offset, aux, count);
}

static VOID set_scene(RwGpuDevice* state, const RwGpuDrawRequest* cmd) {
    reg_write(state, RWGPU_REG_WIDTH, cmd->width);
    reg_write(state, RWGPU_REG_HEIGHT, cmd->height);
    reg_write(state, RWGPU_REG_CLEAR_RGBA, cmd->clear_rgba);
}

static NTSTATUS draw(RwGpuDevice* state, const RwGpuDrawRequest* cmd, PUCHAR data, ULONG* written) {
    PULONG staging = (PULONG)(state->bar + RWGPU_STAGE_OFFSET);
    const ULONG vertices = cmd->vertex_count * sizeof(RwGpuVertex);
    const ULONG color = cmd->width * cmd->height * 4;
    if (vertices) WRITE_REGISTER_BUFFER_ULONG(staging, (PULONG)(data + sizeof(*cmd)), vertices / 4);
    set_scene(state, cmd);
    NTSTATUS status = transfer(state, RWGPU_DRAW, 0, 0, vertices);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG offset = 0; offset < color;) {
        const ULONG left = color - offset;
        const ULONG chunk = left < RWGPU_STAGE_BYTES ? left : RWGPU_STAGE_BYTES;
        status = transfer(state, RWGPU_READ_COLOR, offset, 0, chunk);
        if (!NT_SUCCESS(status)) return status;
        READ_REGISTER_BUFFER_ULONG(staging, (PULONG)(data + offset), chunk / 4);
        offset += chunk;
    }
    *written = color;
    return STATUS_SUCCESS;
}

static BOOLEAN valid_scene(const RwGpuDrawRequest* cmd) {
    return cmd->version == RWGPU_ABI && cmd->width && cmd->width <= RWGPU_MAX_WIDTH &&
        cmd->height && cmd->height <= RWGPU_MAX_HEIGHT && cmd->vertex_count <= RWGPU_MAX_VERTICES &&
        cmd->vertex_count % 3 == 0;
}

static BOOLEAN valid_vram(RwGpuDevice* state, uint64_t offset, ULONG size) {
    const uint64_t capacity = read_wide(state, RWGPU_REG_LOGICAL_LO);
    return offset <= capacity && size <= capacity - offset;
}

static NTSTATUS quantum_request(RwGpuDevice* state, PUCHAR data, ULONG input, ULONG output, ULONG* written) {
    if (!data || input < sizeof(RwGpuQuantumRequest)) return STATUS_INVALID_PARAMETER;
    const RwGpuQuantumRequest cmd = *(const RwGpuQuantumRequest*)data;
    if (cmd.version != RWGPU_ABI || cmd.reserved || cmd.operation < RWGPU_QUANTUM_STORE ||
        cmd.operation > RWGPU_QUANTUM_CX || cmd.bytes > RWGPU_QUANTUM_TRANSFER_BYTES ||
        input != sizeof(cmd) + (cmd.operation == RWGPU_QUANTUM_STORE ? cmd.bytes : 0) ||
        output != (cmd.operation == RWGPU_QUANTUM_LOAD ? cmd.bytes : 0)) return STATUS_INVALID_PARAMETER;
    if (!state->bar) return STATUS_DEVICE_NOT_READY;
    if (!(reg_read(state, RWGPU_REG_CAPS) & RWGPU_CAP_QUANTUM_VRAM)) return STATUS_NOT_SUPPORTED;
    if (cmd.operation >= RWGPU_QUANTUM_H) {
        const uint64_t bits = RWGPU_LOGICAL_BYTES * 8;
        if (cmd.bytes || cmd.offset >= bits ||
            (cmd.operation == RWGPU_QUANTUM_H ? cmd.auxiliary != 0 :
                cmd.auxiliary >= bits || cmd.auxiliary == cmd.offset)) return STATUS_INVALID_PARAMETER;
    } else if (!cmd.bytes || cmd.auxiliary || !valid_vram(state, cmd.offset, cmd.bytes)) {
        return STATUS_INVALID_PARAMETER;
    }
    PUCHAR staging = state->bar + RWGPU_STAGE_OFFSET;
    if (cmd.operation == RWGPU_QUANTUM_STORE)
        WRITE_REGISTER_BUFFER_UCHAR(staging, data + sizeof(cmd), cmd.bytes);
    const NTSTATUS status = transfer(state, cmd.operation, cmd.offset, cmd.auxiliary, cmd.bytes);
    if (NT_SUCCESS(status) && cmd.operation == RWGPU_QUANTUM_LOAD) {
        READ_REGISTER_BUFFER_UCHAR(staging, data, cmd.bytes);
        *written = cmd.bytes;
    }
    return status;
}

static NTSTATUS checked_dma(RwGpuDevice* state, ULONG command, uint64_t from, uint64_t to,
                            ULONG bytes) {
    NTSTATUS status = transfer(state, command, from, to, bytes);
    if (NT_SUCCESS(status) && read_wide(state, RWGPU_REG_RESULT_LO) != bytes)
        return STATUS_DEVICE_DATA_ERROR;
    return status;
}

static NTSTATUS transfer_vram(RwGpuDevice* state, const RwGpuVramTransfer* cmd,
                              PUCHAR buffer, BOOLEAN upload, ULONG* written) {
    if (!dma_ready(state)) return STATUS_DEVICE_NOT_READY;
    if (cmd->bytes > state->dmaBytes || !valid_vram(state, cmd->offset, cmd->bytes) ||
        (upload && cmd->bytes > reg_read(state, RWGPU_REG_DIRECT_MAX) - cmd->offset % RWGPU_PAGE_BYTES))
        return STATUS_INVALID_PARAMETER;
    const uint64_t bus = (uint64_t)state->busAddress.QuadPart;
    if (upload) {
        RtlCopyMemory(state->dma, buffer, cmd->bytes);
        KeMemoryBarrier();
        return checked_dma(state, RWGPU_UPLOAD, bus, cmd->offset, cmd->bytes);
    }
    const NTSTATUS status = checked_dma(state, RWGPU_DOWNLOAD, cmd->offset, bus, cmd->bytes);
    if (NT_SUCCESS(status)) {
        KeMemoryBarrier();
        RtlCopyMemory(buffer, state->dma, cmd->bytes);
        *written = cmd->bytes;
    }
    return status;
}

static NTSTATUS draw_vram(RwGpuDevice* state, const RwGpuDrawVramRequest* cmd,
                          PUCHAR buffer, ULONG bytes, ULONG* written) {
    const ULONG caps = reg_read(state, RWGPU_REG_CAPS);
    const ULONG vertices = cmd->draw.vertex_count * sizeof(RwGpuVertex);
    if (!dma_ready(state) || !(caps & RWGPU_CAP_DRAW_VRAM) || !(caps & RWGPU_CAP_COLOR_DMA))
        return STATUS_DEVICE_NOT_READY;
    if (!valid_vram(state, cmd->offset, vertices)) return STATUS_INVALID_PARAMETER;
    set_scene(state, &cmd->draw);
    NTSTATUS status = transfer(state, RWGPU_DRAW_VRAM, cmd->offset, 0, vertices);
    if (!NT_SUCCESS(status)) return status;
    const uint64_t bus = (uint64_t)state->busAddress.QuadPart;
    for (ULONG offset = 0; offset < bytes;) {
        const ULONG remaining = bytes - offset;
        const ULONG chunk = remaining < state->dmaBytes ? remaining : state->dmaBytes;
        status = checked_dma(state, RWGPU_READ_COLOR_RAM, offset, bus, chunk);
        if (!NT_SUCCESS(status)) return status;
        KeMemoryBarrier();
        RtlCopyMemory(buffer + offset, state->dma, chunk);
        offset += chunk;
    }
    *written = bytes;
    return STATUS_SUCCESS;
}

static NTSTATUS device_control(PDEVICE_OBJECT dev, PIRP irp) {
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    const IO_STACK_LOCATION* req = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    if (!NT_SUCCESS(status)) return complete(irp, status, 0);
    const ULONG code = req->Parameters.DeviceIoControl.IoControlCode;
    const ULONG input = req->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output = req->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR data = irp->AssociatedIrp.SystemBuffer;
    ULONG written = 0;
    status = STATUS_INVALID_DEVICE_REQUEST;
    if (code == RWGPU_IOCTL_DRAW) {
        status = STATUS_INVALID_PARAMETER;
        if (data && input >= sizeof(RwGpuDrawRequest)) {
            const RwGpuDrawRequest cmd = *(const RwGpuDrawRequest*)data;
            if (valid_scene(&cmd) && input == sizeof(cmd) + cmd.vertex_count * sizeof(RwGpuVertex) &&
                output == cmd.width * cmd.height * 4) {
                lock_device(state);
                status = state->bar ? draw(state, &cmd, data, &written) : STATUS_DEVICE_NOT_READY;
                unlock_device(state);
            }
        }
    } else if (code == RWGPU_IOCTL_QUANTUM) {
        lock_device(state);
        status = quantum_request(state, data, input, output, &written);
        unlock_device(state);
    } else if (code == RWGPU_IOCTL_GET_DMA) {
        status = STATUS_INVALID_PARAMETER;
        if (data && input == 0 && output == sizeof(RwGpuDmaInfo)) {
            lock_device(state);
            status = STATUS_DEVICE_NOT_READY;
            if (state->bar && dma_ready(state)) {
                const RwGpuDmaInfo info = {read_wide(state, RWGPU_REG_LOGICAL_LO),
                                          state->dmaBytes, reg_read(state, RWGPU_REG_CAPS)};
                memcpy(data, &info, sizeof(info));
                written = sizeof(info);
                status = STATUS_SUCCESS;
            }
            unlock_device(state);
        }
    } else if (code == RWGPU_IOCTL_UPLOAD || code == RWGPU_IOCTL_DOWNLOAD || code == RWGPU_IOCTL_DRAW_VRAM) {
        status = STATUS_INVALID_PARAMETER;
        const ULONG header = code == RWGPU_IOCTL_DRAW_VRAM ? sizeof(RwGpuDrawVramRequest) :
                                                            sizeof(RwGpuVramTransfer);
        if (data && input == header && output && output <= RWGPU_DIRECT_TRANSFER_BYTES &&
            irp->MdlAddress && output <= MmGetMdlByteCount(irp->MdlAddress)) {
            const ULONG protection = MdlMappingNoExecute |
                (code == RWGPU_IOCTL_UPLOAD ? MdlMappingNoWrite : 0);
            PUCHAR buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority | protection);
            if (!buffer) status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                lock_device(state);
                if (!state->bar) status = STATUS_DEVICE_NOT_READY;
                else if (code == RWGPU_IOCTL_DRAW_VRAM) {
                    const RwGpuDrawVramRequest cmd = *(const RwGpuDrawVramRequest*)data;
                    if (!cmd.reserved && valid_scene(&cmd.draw) && output == cmd.draw.width * cmd.draw.height * 4)
                        status = draw_vram(state, &cmd, buffer, output, &written);
                } else {
                    const RwGpuVramTransfer cmd = *(const RwGpuVramTransfer*)data;
                    if (cmd.version == RWGPU_ABI && cmd.bytes == output)
                        status = transfer_vram(state, &cmd, buffer, code == RWGPU_IOCTL_UPLOAD, &written);
                }
                unlock_device(state);
            }
        }
    }
    IoReleaseRemoveLock(&state->remove, irp);
    return complete(irp, status, written);
}

static NTSTATUS pass(PDEVICE_OBJECT dev, PIRP irp) {
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    return NT_SUCCESS(status) ? forward_locked(state, irp) : complete(irp, status, 0);
}

static NTSTATUS power(PDEVICE_OBJECT dev, PIRP irp) {
    PoStartNextPowerIrp(irp);
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    if (!NT_SUCCESS(status)) return complete(irp, status, 0);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, unlock_forward, state, TRUE, TRUE, TRUE);
    return PoCallDriver(state->lower, irp);
}

static NTSTATUS file_request(PDEVICE_OBJECT dev, PIRP irp) {
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    if (NT_SUCCESS(status)) IoReleaseRemoveLock(&state->remove, irp);
    return complete(irp, status, 0);
}

static VOID unload(PDRIVER_OBJECT driver) { UNREFERENCED_PARAMETER(driver); }

static NTSTATUS add(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
    PDEVICE_OBJECT dev = NULL;
    NTSTATUS status = IoCreateDeviceSecure(driver, sizeof(RwGpuDevice), &name, FILE_DEVICE_UNKNOWN,
                                           FILE_DEVICE_SECURE_OPEN, FALSE, &access, &deviceClass, &dev);
    if (!NT_SUCCESS(status)) return status;
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    KeInitializeMutex(&state->serial, 0);
    IoInitializeRemoveLock(&state->remove, 'gPwR', 0, 0);
    state->physical = pdo;
    status = IoAttachDeviceToDeviceStackSafe(dev, pdo, &state->lower);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(dev);
        return status;
    }
    dev->Flags |= DO_BUFFERED_IO;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    UNREFERENCED_PARAMETER(registry);
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = pass;
    driver->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    driver->MajorFunction[IRP_MJ_POWER] = power;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = device_control;
    driver->MajorFunction[IRP_MJ_CREATE] = file_request;
    driver->MajorFunction[IRP_MJ_CLOSE] = file_request;
    driver->MajorFunction[IRP_MJ_CLEANUP] = file_request;
    driver->DriverExtension->AddDevice = add;
    driver->DriverUnload = unload;
    return STATUS_SUCCESS;
}
