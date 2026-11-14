#include <ntddk.h>
#include <devioctl.h>
#include <wdmsec.h>
#include "gpu_transport.h"

typedef struct RwGpuDevice {
    PDEVICE_OBJECT lower;
    PUCHAR bar;
    FAST_MUTEX serial;
    IO_REMOVE_LOCK remove;
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
    return READ_REGISTER_ULONG((volatile ULONG*)(state->bar + offset));
}

static VOID reg_write(RwGpuDevice* state, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((volatile ULONG*)(state->bar + offset), value);
}

static VOID unmap(RwGpuDevice* state) {
    ExAcquireFastMutex(&state->serial);
    if (state->bar) {
        MmUnmapIoSpace(state->bar, RWGPU_BAR_BYTES);
        state->bar = NULL;
    }
    ExReleaseFastMutex(&state->serial);
}

static NTSTATUS map_translated(RwGpuDevice* state, const CM_RESOURCE_LIST* resources) {
    if (!resources || resources->Count != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    const CM_PARTIAL_RESOURCE_LIST* list = &resources->List[0].PartialResourceList;
    for (ULONG i = 0; i < list->Count; ++i) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR* bar = &list->PartialDescriptors[i];
        if (bar->Type != CmResourceTypeMemory || bar->u.Memory.Length != RWGPU_BAR_BYTES) continue;
        ExAcquireFastMutex(&state->serial);
        state->bar = (PUCHAR)MmMapIoSpaceEx(bar->u.Memory.Start, RWGPU_BAR_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
        BOOLEAN okay = state->bar && reg_read(state, RWGPU_REG_MAGIC) == RWGPU_MAGIC &&
            reg_read(state, RWGPU_REG_ABI) == RWGPU_ABI &&
            (reg_read(state, RWGPU_REG_CAPS) & RWGPU_CAP_TRIANGLES);
        ExReleaseFastMutex(&state->serial);
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

static NTSTATUS transfer(RwGpuDevice* state, ULONG op, ULONG offset, ULONG count) {
    ULONG ticket = reg_read(state, RWGPU_REG_COMPLETED) + 1;
    if (!ticket) ticket = 1;
    reg_write(state, RWGPU_REG_OFFSET_LO, offset);
    reg_write(state, RWGPU_REG_OFFSET_HI, 0);
    reg_write(state, RWGPU_REG_LENGTH, count);
    reg_write(state, RWGPU_REG_TICKET, ticket);
    reg_write(state, RWGPU_REG_DOORBELL, op);
    return reg_read(state, RWGPU_REG_COMPLETED) == ticket && reg_read(state, RWGPU_REG_STATUS) == RWGPU_OK
        ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

static NTSTATUS draw(RwGpuDevice* state, const RwGpuDrawRequest* cmd, PUCHAR data, ULONG* written) {
    PULONG staging = (PULONG)(state->bar + RWGPU_STAGE_OFFSET);
    const ULONG vertices = cmd->vertex_count * sizeof(RwGpuVertex);
    const ULONG color = cmd->width * cmd->height * 4;
    if (vertices) WRITE_REGISTER_BUFFER_ULONG(staging, (PULONG)(data + sizeof(*cmd)), vertices / 4);
    reg_write(state, RWGPU_REG_WIDTH, cmd->width);
    reg_write(state, RWGPU_REG_HEIGHT, cmd->height);
    reg_write(state, RWGPU_REG_CLEAR_RGBA, cmd->clear_rgba);
    NTSTATUS status = transfer(state, RWGPU_DRAW, 0, vertices);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG offset = 0; offset < color;) {
        const ULONG left = color - offset;
        const ULONG chunk = left < RWGPU_STAGE_BYTES ? left : RWGPU_STAGE_BYTES;
        status = transfer(state, RWGPU_READ_COLOR, offset, chunk);
        if (!NT_SUCCESS(status)) return status;
        READ_REGISTER_BUFFER_ULONG(staging, (PULONG)(data + offset), chunk / 4);
        offset += chunk;
    }
    *written = color;
    return STATUS_SUCCESS;
}

static NTSTATUS device_control(PDEVICE_OBJECT dev, PIRP irp) {
    RwGpuDevice* state = (RwGpuDevice*)dev->DeviceExtension;
    const IO_STACK_LOCATION* req = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = IoAcquireRemoveLock(&state->remove, irp);
    if (!NT_SUCCESS(status)) return complete(irp, status, 0);
    ULONG written = 0;
    status = STATUS_INVALID_DEVICE_REQUEST;
    if (req->Parameters.DeviceIoControl.IoControlCode == RWGPU_IOCTL_DRAW) {
        PUCHAR data = irp->AssociatedIrp.SystemBuffer;
        const ULONG input = req->Parameters.DeviceIoControl.InputBufferLength;
        const ULONG output = req->Parameters.DeviceIoControl.OutputBufferLength;
        status = STATUS_INVALID_PARAMETER;
        if (data && input >= sizeof(RwGpuDrawRequest)) {
            const RwGpuDrawRequest cmd = *(const RwGpuDrawRequest*)data;
            if (cmd.version == RWGPU_ABI && cmd.width && cmd.width <= RWGPU_MAX_WIDTH &&
                cmd.height && cmd.height <= RWGPU_MAX_HEIGHT && cmd.vertex_count <= RWGPU_MAX_VERTICES &&
                cmd.vertex_count % 3 == 0 && input == sizeof(cmd) + cmd.vertex_count * sizeof(RwGpuVertex) &&
                output == cmd.width * cmd.height * 4) {
                ExAcquireFastMutex(&state->serial);
                status = state->bar ? draw(state, &cmd, data, &written) : STATUS_DEVICE_NOT_READY;
                ExReleaseFastMutex(&state->serial);
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
    ExInitializeFastMutex(&state->serial);
    IoInitializeRemoveLock(&state->remove, 'gPwR', 0, 0);
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
