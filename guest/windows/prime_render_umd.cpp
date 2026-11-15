#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3d10umddi.h>
#pragma warning(pop)
#include "../../src/c08_gpu_dma.h"
#include "../../src/c08_wddm_protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

namespace {
constexpr UINT64 supported_ddi = D3DWDDM1_3_DDI_SUPPORTED;

// Only identity DXBC shaders can be lowered to the existing clip-space
// triangle renderer. Verify the token stream, including each copy and swizzle.
struct Shader {
    UINT position = UINT32_MAX;
    UINT color = UINT32_MAX;
    bool valid = false;
    UINT output_color = UINT32_MAX;
};

bool register_operand(UINT token, D3D10_SB_OPERAND_TYPE type, bool source) {
    if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(token) ||
        DECODE_D3D10_SB_OPERAND_TYPE(token) != type ||
        DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token) != D3D10_SB_OPERAND_INDEX_1D ||
        DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, token) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
        DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) != D3D10_SB_OPERAND_4_COMPONENT) return false;
    const auto mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token);
    if (source && mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE)
        return DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(token) == D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE;
    return mode == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE &&
        (DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token) == D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL ||
         (source && !DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token)));
}

Shader decode_shader(const UINT* code, bool vertex) {
    if (!code || DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(code[0]) !=
        (vertex ? D3D10_SB_VERTEX_SHADER : D3D10_SB_PIXEL_SHADER)) return {};
    const UINT words = code[1];
    if (words < 3 || words > 128) return {};
    constexpr UINT registers = 32;
    std::array<UINT, registers> moves;
    moves.fill(UINT32_MAX);
    UINT inputs = 0, outputs = 0, position = UINT32_MAX, color_input = UINT32_MAX;
    bool returned = false;
    for (UINT cursor = 2; cursor < words;) {
        const UINT* op = code + cursor;
        const UINT length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(op[0]);
        if (!length || length > words - cursor || DECODE_IS_D3D10_SB_OPCODE_EXTENDED(op[0]) || returned)
            return {};
        switch (DECODE_D3D10_SB_OPCODE_TYPE(op[0])) {
        case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
            if (length != 1) return {};
            break;
        case D3D10_SB_OPCODE_DCL_INPUT:
        case D3D10_SB_OPCODE_DCL_INPUT_PS:
            if (length != 3 || op[2] >= registers ||
                !register_operand(op[1], D3D10_SB_OPERAND_TYPE_INPUT, false)) return {};
            if (!vertex && (DECODE_D3D10_SB_OPCODE_TYPE(op[0]) != D3D10_SB_OPCODE_DCL_INPUT_PS ||
                DECODE_D3D10_SB_INPUT_INTERPOLATION_MODE(op[0]) != D3D10_SB_INTERPOLATION_LINEAR)) return {};
            inputs |= UINT{1} << op[2];
            color_input = op[2];
            break;
        case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
            if (!vertex || length != 4 || op[2] >= registers || op[3] != D3D10_SB_NAME_POSITION ||
                position != UINT32_MAX ||
                !register_operand(op[1], D3D10_SB_OPERAND_TYPE_OUTPUT, false)) return {};
            position = op[2];
            outputs |= UINT{1} << op[2];
            break;
        case D3D10_SB_OPCODE_DCL_OUTPUT:
            if (length != 3 || op[2] >= registers ||
                !register_operand(op[1], D3D10_SB_OPERAND_TYPE_OUTPUT, false)) return {};
            outputs |= UINT{1} << op[2];
            break;
        case D3D10_SB_OPCODE_MOV:
            if (length != 5 || (op[0] & 0x00fff800u) || op[2] >= registers || op[4] >= registers ||
                moves[op[2]] != UINT32_MAX ||
                !register_operand(op[1], D3D10_SB_OPERAND_TYPE_OUTPUT, false) ||
                !register_operand(op[3], D3D10_SB_OPERAND_TYPE_INPUT, true)) return {};
            moves[op[2]] = op[4];
            break;
        case D3D10_SB_OPCODE_RET:
            if (length != 1) return {};
            returned = true;
            break;
        default:
            return {};
        }
        cursor += length;
    }
    if (!returned || !inputs || !outputs) return {};
    UINT moved = 0;
    for (UINT index = 0; index < registers; ++index) {
        if (moves[index] == UINT32_MAX) continue;
        if (!(inputs & (UINT{1} << moves[index]))) return {};
        moved |= UINT{1} << index;
    }
    if (moved != outputs) return {};
    if (!vertex) {
        if (moved != 1 || inputs != (UINT{1} << color_input) || moves[0] != color_input) return {};
        return {UINT32_MAX, color_input, true};
    }
    if (position == UINT32_MAX) return {};
    const UINT output_color = outputs & ~(UINT{1} << position);
    if (!output_color || (output_color & (output_color - 1))) return {};
    for (UINT index = 0; index < registers; ++index) {
        if (output_color != (UINT{1} << index)) continue;
        if (moves[position] == moves[index]) return {};
        return {moves[position], moves[index], true, index};
    }
    return {};
}

struct Resource {
    DXGI_FORMAT format{};
    UINT width = 0, height = 0, usage = 0, bind = 0;
    bool buffer = false, mapped = false, initialized = false, dirty = false, pending_readback = false;
    D3DKMT_HANDLE allocation = 0;
    Resource* partner = nullptr;
    std::vector<std::uint8_t> data;
    std::vector<RwGpuVertex> triangles;
    std::uint32_t clear = 0;
    [[nodiscard]] UINT pitch() const noexcept { return buffer ? width : width * 4; }
};

struct View { Resource* resource; };
struct InputLayout { std::array<D3D10DDIARG_INPUT_ELEMENT_DESC, 2> items; };

struct Device {
    D3D10DDI_HRTDEVICE runtime_device;
    D3D10DDI_HRTCORELAYER core;
    const D3DDDI_DEVICECALLBACKS* kernel;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS* user;
    HANDLE context = nullptr;
    void* command_buffer = nullptr;
    UINT command_bytes = 0;
    D3DDDI_ALLOCATIONLIST* allocations = nullptr;
    UINT allocation_count = 0;
    D3DDDI_PATCHLOCATIONLIST* patches = nullptr;
    UINT patch_count = 0;
    Resource* vertex_buffer = nullptr;
    Resource* target = nullptr;
    Resource* depth = nullptr;
    const InputLayout* layout = nullptr;
    const Shader* vertex = nullptr;
    const Shader* pixel = nullptr;
    UINT stride = 0, offset = 0;
    D3D10_DDI_PRIMITIVE_TOPOLOGY topology{};
    D3D10_DDI_VIEWPORT viewport{};
    bool viewport_set = false, raster = false, depth_test = true, blend = true;
    void error(HRESULT hr) const { user->pfnSetErrorCb(core, hr); }
};

Device& device(D3D10DDI_HDEVICE handle) { return *static_cast<Device*>(handle.pDrvPrivate); }
Resource* resource(D3D10DDI_HRESOURCE handle) { return static_cast<Resource*>(handle.pDrvPrivate); }
Resource* view_resource(void* handle) { return handle ? static_cast<View*>(handle)->resource : nullptr; }

HRESULT submit(Device& dev, Resource& target) {
    if (!target.dirty) return S_OK;
    const auto vertex_bytes = target.triangles.size() * sizeof(RwGpuVertex);
    const auto required = sizeof(RwGpuWddmRender) + vertex_bytes;
    if (!target.allocation || !dev.command_buffer || required > dev.command_bytes ||
        !dev.allocations || !dev.allocation_count || !dev.patches || !dev.patch_count)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto* command = static_cast<std::uint8_t*>(dev.command_buffer);
    const RwGpuWddmRender render{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_RENDER, static_cast<UINT>(required)},
        target.width, target.height, target.clear, static_cast<UINT>(target.triangles.size())};
    std::memcpy(command, &render, sizeof(render));
    if (vertex_bytes) std::memcpy(command + sizeof(render), target.triangles.data(), vertex_bytes);
    dev.allocations[0] = {};
    dev.allocations[0].hAllocation = target.allocation;
    dev.allocations[0].WriteOperation = 1;
    dev.patches[0] = {};
    dev.patches[0].PatchOffset = offsetof(RwGpuDmaDraw, destination);
    D3DDDICB_RENDER request{};
    request.hContext = dev.context;
    request.CommandLength = static_cast<UINT>(required);
    request.NumAllocations = 1;
    request.NumPatchLocations = 1;
    const auto status = dev.kernel->pfnRenderCb(dev.runtime_device.handle, &request);
    if (FAILED(status)) return status;
    dev.command_buffer = request.pNewCommandBuffer;
    dev.command_bytes = request.NewCommandBufferSize;
    dev.allocations = request.pNewAllocationList;
    dev.allocation_count = request.NewAllocationListSize;
    dev.patches = request.pNewPatchLocationList;
    dev.patch_count = request.NewPatchLocationListSize;
    target.dirty = false;
    target.pending_readback = true;
    return S_OK;
}

HRESULT read_target(Device& dev, Resource& target) {
    if (!target.pending_readback) return S_OK;
    D3DDDICB_LOCK lock{};
    lock.hAllocation = target.allocation;
    lock.Flags.ReadOnly = 1;
    lock.Flags.LockEntire = 1;
    // This lock waits for the actual fence and asks VidMm for the contents
    // of the GPU-written allocation in its system memory backing.
    const auto status = dev.kernel->pfnLockCb(dev.runtime_device.handle, &lock);
    if (FAILED(status)) return status;
    if (lock.hAllocation) target.allocation = lock.hAllocation;
    if (const auto* bgra = static_cast<const std::uint8_t*>(lock.pData)) {
        for (std::size_t i = 0; i < target.data.size(); i += 4) {
            target.data[i] = bgra[i + 2];
            target.data[i + 1] = bgra[i + 1];
            target.data[i + 2] = bgra[i];
            target.data[i + 3] = bgra[i + 3];
        }
    }
    const D3DDDICB_UNLOCK unlock{1, &target.allocation};
    const auto released = dev.kernel->pfnUnlockCb(dev.runtime_device.handle, &unlock);
    if (FAILED(released)) return released;
    if (!lock.pData) return DXGI_ERROR_DEVICE_REMOVED;
    target.pending_readback = false;
    return S_OK;
}


template<class T, class... Args>
SIZE_T APIENTRY private_size(D3D10DDI_HDEVICE, Args...) { return sizeof(T); }

template<class T, class Handle>
void APIENTRY destroy_private(D3D10DDI_HDEVICE, Handle handle) {
    static_cast<T*>(handle.pDrvPrivate)->~T();
}

void APIENTRY create_resource(D3D10DDI_HDEVICE handle, const D3D11DDIARG_CREATERESOURCE* args,
                              D3D10DDI_HRESOURCE output, D3D10DDI_HRTRESOURCE) {
    auto& dev = device(handle);
    auto& out = *new (output.pDrvPrivate) Resource{};
    if (!args || !args->pMipInfoList || args->MipLevels != 1 || args->ArraySize != 1 ||
        args->SampleDesc.Count != 1 || args->SampleDesc.Quality || args->pPrimaryDesc || args->MiscFlags) {
        dev.error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    out.width = args->pMipInfoList[0].TexelWidth;
    out.height = args->pMipInfoList[0].TexelHeight;
    out.format = args->Format;
    out.usage = args->Usage;
    out.bind = args->BindFlags;
    out.buffer = args->ResourceDimension == D3D10DDIRESOURCE_BUFFER;
    if (!out.width || (out.buffer && (out.width > RWGPU_MAX_VERTICES * sizeof(RwGpuVertex) ||
        out.bind != D3D10_DDI_BIND_VERTEX_BUFFER || out.usage != D3D10_DDI_USAGE_IMMUTABLE ||
        !args->pInitialDataUP || !args->pInitialDataUP[0].pSysMem))) {
        dev.error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    if (!out.buffer && (args->ResourceDimension != D3D10DDIRESOURCE_TEXTURE2D ||
        !out.height || out.width > RWGPU_MAX_WIDTH || out.height > RWGPU_MAX_HEIGHT ||
        args->pInitialDataUP ||
        !((out.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
           ((out.bind == D3D10_DDI_BIND_RENDER_TARGET && out.usage == D3D10_DDI_USAGE_DEFAULT) ||
            (!out.bind && out.usage == D3D10_DDI_USAGE_STAGING && args->MapFlags == D3D10_DDI_CPU_ACCESS_READ))) ||
          (out.format == DXGI_FORMAT_D24_UNORM_S8_UINT && out.bind == D3D10_DDI_BIND_DEPTH_STENCIL &&
           out.usage == D3D10_DDI_USAGE_DEFAULT)))) {
        dev.error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    try {
        if (out.buffer) {
            const auto* data = static_cast<const std::uint8_t*>(args->pInitialDataUP[0].pSysMem);
            out.data.assign(data, data + out.width);
            out.initialized = true;
        } else if (out.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            out.data.resize(std::size_t(out.pitch()) * out.height);
            if (out.bind == D3D10_DDI_BIND_RENDER_TARGET) {
                RwGpuWddmAllocation desc{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, out.width, out.height};
                D3DDDI_ALLOCATIONINFO allocation{};
                allocation.pPrivateDriverData = &desc;
                allocation.PrivateDriverDataSize = sizeof(desc);
                D3DDDICB_ALLOCATE request{};
                request.NumAllocations = 1;
                request.pAllocationInfo = &allocation;
                const auto hr = dev.kernel->pfnAllocateCb(dev.runtime_device.handle, &request);
                if (FAILED(hr) || !allocation.hAllocation) {
                    dev.error(FAILED(hr) ? hr : DXGI_ERROR_DEVICE_REMOVED);
                    return;
                }
                out.allocation = allocation.hAllocation;
            }
        }
    } catch (const std::bad_alloc&) {
        dev.error(E_OUTOFMEMORY);
    }
}

void APIENTRY destroy_resource(D3D10DDI_HDEVICE handle, D3D10DDI_HRESOURCE value) {
    auto& dev = device(handle);
    auto* current = resource(value);
    if (dev.vertex_buffer == current) dev.vertex_buffer = nullptr;
    if (dev.target == current) dev.target = nullptr;
    if (dev.depth == current) dev.depth = nullptr;
    if (current->partner) current->partner->partner = nullptr;
    if (current->allocation) {
        const D3DDDICB_DEALLOCATE request{nullptr, 1, &current->allocation};
        const auto hr = dev.kernel->pfnDeallocateCb(dev.runtime_device.handle, &request);
        if (FAILED(hr)) dev.error(hr);
    }
    current->~Resource();
}

template<class Definition, class ViewHandle, class RuntimeHandle, DXGI_FORMAT Format>
void APIENTRY create_view(D3D10DDI_HDEVICE handle, const Definition* args, ViewHandle out, RuntimeHandle) {
    auto* source = args ? resource(args->hDrvResource) : nullptr;
    auto* view = new (out.pDrvPrivate) View{nullptr};
    if (source && args->Format == Format && source->format == Format &&
        args->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D && args->Tex2D.MipSlice == 0 &&
        args->Tex2D.FirstArraySlice == 0 && args->Tex2D.ArraySize == 1) {
        if constexpr (std::is_same_v<Definition, D3D11DDIARG_CREATEDEPTHSTENCILVIEW>) {
            if (args->Flags) { device(handle).error(DXGI_ERROR_UNSUPPORTED); return; }
        }
        view->resource = source;
        return;
    }
    device(handle).error(DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY create_layout(D3D10DDI_HDEVICE handle, const D3D10DDIARG_CREATEELEMENTLAYOUT* args,
                            D3D10DDI_HELEMENTLAYOUT out, D3D10DDI_HRTELEMENTLAYOUT) {
    auto* layout = new (out.pDrvPrivate) InputLayout{};
    if (!args || args->NumElements != layout->items.size() || !args->pVertexElements) {
        device(handle).error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    std::copy_n(args->pVertexElements, layout->items.size(), layout->items.begin());
    for (const auto& element : layout->items) {
        if (element.InputSlot || element.InputSlotClass != D3D10_DDI_INPUT_PER_VERTEX_DATA ||
            element.InstanceDataStepRate) {
            device(handle).error(DXGI_ERROR_UNSUPPORTED);
            return;
        }
    }
}

template<bool Vertex>
void APIENTRY create_shader(D3D10DDI_HDEVICE handle, const UINT* code, D3D10DDI_HSHADER out,
                            D3D10DDI_HRTSHADER, const D3D11_1DDIARG_STAGE_IO_SIGNATURES*) {
    auto* shader = new (out.pDrvPrivate) Shader(decode_shader(code, Vertex));
    if (!shader->valid) device(handle).error(DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY bind_layout(D3D10DDI_HDEVICE h, D3D10DDI_HELEMENTLAYOUT l) {
    device(h).layout = static_cast<const InputLayout*>(l.pDrvPrivate);
}
void APIENTRY bind_vs(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER shader) {
    device(h).vertex = static_cast<const Shader*>(shader.pDrvPrivate);
}
void APIENTRY bind_ps(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER shader) {
    device(h).pixel = static_cast<const Shader*>(shader.pDrvPrivate);
}
void APIENTRY bind_topology(D3D10DDI_HDEVICE h, D3D10_DDI_PRIMITIVE_TOPOLOGY topology) {
    device(h).topology = topology;
}

void APIENTRY bind_vertices(D3D10DDI_HDEVICE handle, UINT start, UINT count,
                             const D3D10DDI_HRESOURCE* slots, const UINT* stride, const UINT* offset) {
    auto& dev = device(handle);
    for (UINT i = 0; i < count; ++i) {
        if (start + i) {
            if (resource(slots[i])) dev.error(DXGI_ERROR_UNSUPPORTED);
            continue;
        }
        dev.vertex_buffer = resource(slots[i]);
        dev.stride = dev.vertex_buffer ? stride[i] : 0;
        dev.offset = dev.vertex_buffer ? offset[i] : 0;
    }
}

void APIENTRY bind_targets(D3D10DDI_HDEVICE handle, const D3D10DDI_HRENDERTARGETVIEW* views,
                           UINT count, UINT, D3D10DDI_HDEPTHSTENCILVIEW depth,
                           const D3D11DDI_HUNORDEREDACCESSVIEW* unordered, const UINT*, UINT,
                           UINT unordered_count, UINT, UINT) {
    auto& dev = device(handle);
    dev.target = count ? view_resource(views[0].pDrvPrivate) : nullptr;
    dev.depth = view_resource(depth.pDrvPrivate);
    for (UINT i = 1; i < count; ++i)
        if (views[i].pDrvPrivate) dev.error(DXGI_ERROR_UNSUPPORTED);
    for (UINT i = 0; i < unordered_count; ++i)
        if (unordered[i].pDrvPrivate) dev.error(DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY viewports(D3D10DDI_HDEVICE h, UINT count, UINT, const D3D10_DDI_VIEWPORT* view) {
    auto& dev = device(h);
    dev.viewport_set = count == 1;
    if (dev.viewport_set) dev.viewport = view[0];
}

template<class Definition, class Handle, class RuntimeHandle>
void APIENTRY create_state(D3D10DDI_HDEVICE, const Definition* args, Handle out, RuntimeHandle) {
    new (out.pDrvPrivate) Definition(*args);
}

void APIENTRY bind_raster(D3D10DDI_HDEVICE h, D3D10DDI_HRASTERIZERSTATE state) {
    const auto* r = static_cast<const D3D11_1_DDI_RASTERIZER_DESC*>(state.pDrvPrivate);
    device(h).raster = r && r->FillMode == D3D10_DDI_FILL_SOLID && r->CullMode == D3D10_DDI_CULL_NONE &&
        r->DepthClipEnable && !r->ScissorEnable && !r->DepthBias && !r->DepthBiasClamp &&
        !r->SlopeScaledDepthBias && !r->MultisampleEnable && !r->AntialiasedLineEnable && !r->ForcedSampleCount;
}

void APIENTRY bind_depth(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILSTATE state, UINT) {
    const auto* d = static_cast<const D3D10_DDI_DEPTH_STENCIL_DESC*>(state.pDrvPrivate);
    device(h).depth_test = !d || (d->DepthEnable && d->DepthWriteMask == D3D10_DDI_DEPTH_WRITE_MASK_ALL &&
        d->DepthFunc == D3D10_DDI_COMPARISON_LESS && !d->StencilEnable);
}

void APIENTRY bind_blend(D3D10DDI_HDEVICE h, D3D10DDI_HBLENDSTATE state, const FLOAT*, UINT mask) {
    const auto* b = static_cast<const D3D11_1_DDI_BLEND_DESC*>(state.pDrvPrivate);
    device(h).blend = mask == UINT32_MAX && (!b || (!b->AlphaToCoverageEnable &&
        !b->RenderTarget[0].BlendEnable && !b->RenderTarget[0].LogicOpEnable &&
        b->RenderTarget[0].RenderTargetWriteMask == D3D10_DDI_COLOR_WRITE_ENABLE_ALL));
}

void APIENTRY clear_color(D3D10DDI_HDEVICE h, D3D10DDI_HRENDERTARGETVIEW v, FLOAT color[4]) {
    auto* target = view_resource(v.pDrvPrivate);
    if (!target || target->data.empty()) { device(h).error(E_INVALIDARG); return; }
    std::uint32_t rgba = 0;
    for (unsigned channel = 0; channel < 4; ++channel) {
        const float value = std::isfinite(color[channel]) ? std::clamp(color[channel], 0.f, 1.f) : 0.f;
        rgba |= std::uint32_t(std::lround(value * 255.f)) << (channel * 8);
    }
    target->clear = rgba;
    target->triangles.clear();
    target->initialized = true;
    target->dirty = false;
    for (std::size_t i = 0; i < target->data.size(); i += 4)
        std::memcpy(target->data.data() + i, &rgba, sizeof(rgba));
}

void APIENTRY clear_depth(D3D10DDI_HDEVICE h, D3D10DDI_HDEPTHSTENCILVIEW v, UINT flags, FLOAT z, UINT8) {
    auto& dev = device(h);
    auto* depth = view_resource(v.pDrvPrivate);
    if (!depth || flags != D3D10_DDI_CLEAR_DEPTH || z != 1.f ||
        (depth->partner && !depth->partner->triangles.empty())) {
        dev.error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    if (depth->partner) {
        depth->partner->partner = nullptr;
        depth->partner = nullptr;
    }
    depth->initialized = true;
}

void APIENTRY draw(D3D10DDI_HDEVICE handle, UINT count, UINT first) {
    if (!count) return;
    auto& dev = device(handle);
    auto* rt = dev.target;
    auto* vb = dev.vertex_buffer;
    const auto& viewport = dev.viewport;
    if (!rt || !rt->initialized || !vb || !vb->buffer || !vb->initialized ||
        !dev.depth || !dev.depth->initialized || (dev.depth->partner && dev.depth->partner != rt) ||
        (rt->triangles.empty() ? rt->partner != nullptr : rt->partner != dev.depth) ||
        dev.depth->width != rt->width ||
        dev.depth->height != rt->height || !dev.depth_test || !dev.blend || !dev.raster ||
        !dev.viewport_set || viewport.TopLeftX != 0.f || viewport.TopLeftY != 0.f ||
        viewport.MinDepth != 0.f || viewport.MaxDepth != 1.f ||
        viewport.Width != static_cast<float>(rt->width) || viewport.Height != static_cast<float>(rt->height) ||
        dev.topology != D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST ||
        !dev.vertex || !dev.pixel || !dev.vertex->valid || !dev.pixel->valid ||
        dev.vertex->output_color != dev.pixel->color || !dev.layout || !dev.stride ||
        count % 3 || count > RWGPU_MAX_VERTICES - rt->triangles.size()) {
        dev.error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    const auto& inputs = dev.layout->items;
    const auto position = std::find_if(inputs.begin(), inputs.end(), [&](const auto& item) {
        return item.InputRegister == dev.vertex->position && item.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    });
    const auto color = std::find_if(inputs.begin(), inputs.end(), [&](const auto& item) {
        return item.InputRegister == dev.vertex->color && item.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    });
    const auto last = std::uint64_t(first) + count - 1;
    if (position == inputs.end() || color == inputs.end() || dev.offset > vb->data.size() ||
        last > (vb->data.size() - dev.offset) / dev.stride) {
        dev.error(E_INVALIDARG);
        return;
    }
    const auto tail = vb->data.size() - dev.offset - last * dev.stride;
    if (std::uint64_t(position->AlignedByteOffset) + 16 > tail ||
        std::uint64_t(color->AlignedByteOffset) + 4 > tail) {
        dev.error(E_INVALIDARG);
        return;
    }
    const auto old_count = rt->triangles.size();
    try { rt->triangles.resize(old_count + count); }
    catch (const std::bad_alloc&) { dev.error(E_OUTOFMEMORY); return; }
    for (UINT index = 0; index < count; ++index) {
        const auto* data = vb->data.data() + dev.offset + (std::size_t(first) + index) * dev.stride;
        auto& vertex = rt->triangles[old_count + index];
        std::memcpy(&vertex.x, data + position->AlignedByteOffset, 16);
        std::memcpy(&vertex.rgba, data + color->AlignedByteOffset, 4);
    }
    rt->partner = dev.depth;
    dev.depth->partner = rt;
    rt->dirty = true;
}

void APIENTRY copy_resource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE out, D3D10DDI_HRESOURCE in) {
    auto* src = resource(in);
    auto* dst = resource(out);
    if (!src || !dst || !src->initialized || dst->mapped || src->mapped ||
        src->width != dst->width || src->height != dst->height || src->format != dst->format ||
        src->data.empty() || src->data.size() != dst->data.size() || dst->usage != D3D10_DDI_USAGE_STAGING) {
        device(h).error(DXGI_ERROR_UNSUPPORTED);
        return;
    }
    auto& dev = device(h);
    HRESULT result = submit(dev, *src);
    if (SUCCEEDED(result)) result = read_target(dev, *src);
    if (FAILED(result)) { dev.error(result); return; }
    std::copy(src->data.begin(), src->data.end(), dst->data.begin());
    dst->initialized = true;
}

void APIENTRY map_resource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE r, UINT subresource,
                           D3D10_DDI_MAP mode, UINT, D3D10DDI_MAPPED_SUBRESOURCE* map_data) {
    auto* src = resource(r);
    *map_data = {};
    if (!src || !src->initialized || src->mapped || src->usage != D3D10_DDI_USAGE_STAGING ||
        subresource || mode != D3D10_DDI_MAP_READ) {
        device(h).error(E_INVALIDARG);
        return;
    }
    src->mapped = true;
    map_data->pData = src->data.data();
    map_data->RowPitch = src->pitch();
    map_data->DepthPitch = static_cast<UINT>(src->data.size());
}

void APIENTRY unmap_resource(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE r, UINT) {
    if (auto* src = resource(r)) src->mapped = false;
}

void APIENTRY destroy_device(D3D10DDI_HDEVICE h) {
    auto& dev = device(h);
    D3DDDICB_DESTROYCONTEXT context{dev.context};
    dev.kernel->pfnDestroyContextCb(dev.runtime_device.handle, &context);
    dev.~Device();
}

template<class... Args>
void APIENTRY unsupported(D3D10DDI_HDEVICE h, Args...) { device(h).error(DXGI_ERROR_UNSUPPORTED); }

template<class Handle>
void APIENTRY null_bindings(D3D10DDI_HDEVICE h, UINT, UINT n, const Handle* handles) {
    for (UINT i = 0; i < n; ++i)
        if (handles[i].pDrvPrivate) { device(h).error(DXGI_ERROR_UNSUPPORTED); break; }
}

void APIENTRY null_constants(D3D10DDI_HDEVICE h, UINT start, UINT n,
                             const D3D10DDI_HRESOURCE* handles, const UINT*, const UINT*) {
    null_bindings(h, start, n, handles);
}

void APIENTRY null_shader(D3D10DDI_HDEVICE h, D3D10DDI_HSHADER s) {
    if (s.pDrvPrivate) device(h).error(DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY null_index(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE r, DXGI_FORMAT, UINT) {
    if (r.pDrvPrivate) device(h).error(DXGI_ERROR_UNSUPPORTED);
}

void APIENTRY format(D3D10DDI_HDEVICE, DXGI_FORMAT f, UINT* result) {
    *result = 0;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            *result = D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET | D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
            break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            *result = D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
            break;
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
            *result = D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
            break;
        default: break;
    }
}

void APIENTRY sample_quality(D3D10DDI_HDEVICE, DXGI_FORMAT f, UINT samples, UINT, UINT* out) {
    *out = samples == 1 && (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT);
}

void APIENTRY counter_info(D3D10DDI_HDEVICE, D3D10DDI_COUNTER_INFO* out) { *out = {}; }
BOOL APIENTRY flush(D3D10DDI_HDEVICE h, UINT) {
    auto& dev = device(h);
    if (!dev.target) return FALSE;
    const HRESULT result = submit(dev, *dev.target);
    if (FAILED(result)) dev.error(result);
    return SUCCEEDED(result);
}
BOOL APIENTRY is_busy(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE) { return FALSE; }
void APIENTRY relocate(D3D10DDI_HDEVICE, D3DWDDM1_3DDI_DEVICEFUNCS*) {}
void APIENTRY hazard(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE) {}

template<class T>
HRESULT APIENTRY unsupported_dxgi(T*) { return DXGI_ERROR_UNSUPPORTED; }

DXGI1_3_DDI_BASE_FUNCTIONS dxgi_functions() {
    DXGI1_3_DDI_BASE_FUNCTIONS cb{};
    cb.pfnPresent = unsupported_dxgi;
    cb.pfnPresent1 = unsupported_dxgi;
    cb.pfnSetDisplayMode = unsupported_dxgi;
    cb.pfnResolveSharedResource = unsupported_dxgi;
    return cb;
}

D3DWDDM1_3DDI_DEVICEFUNCS render_functions() {
    D3DWDDM1_3DDI_DEVICEFUNCS cb{};
    cb.pfnDestroyDevice = destroy_device;
    cb.pfnCalcPrivateResourceSize = private_size<Resource>;
    cb.pfnCreateResource = create_resource;
    cb.pfnDestroyResource = destroy_resource;
    cb.pfnCalcPrivateRenderTargetViewSize = private_size<View>;
    cb.pfnCreateRenderTargetView = create_view<D3D10DDIARG_CREATERENDERTARGETVIEW,
        D3D10DDI_HRENDERTARGETVIEW, D3D10DDI_HRTRENDERTARGETVIEW, DXGI_FORMAT_R8G8B8A8_UNORM>;
    cb.pfnDestroyRenderTargetView = destroy_private<View>;
    cb.pfnCalcPrivateDepthStencilViewSize = private_size<View>;
    cb.pfnCreateDepthStencilView = create_view<D3D11DDIARG_CREATEDEPTHSTENCILVIEW,
        D3D10DDI_HDEPTHSTENCILVIEW, D3D10DDI_HRTDEPTHSTENCILVIEW, DXGI_FORMAT_D24_UNORM_S8_UINT>;
    cb.pfnDestroyDepthStencilView = destroy_private<View>;
    cb.pfnCalcPrivateElementLayoutSize = private_size<InputLayout>;
    cb.pfnCreateElementLayout = create_layout;
    cb.pfnDestroyElementLayout = destroy_private<InputLayout>;
    cb.pfnCalcPrivateShaderSize = private_size<Shader>;
    cb.pfnCreateVertexShader = create_shader<true>;
    cb.pfnCreatePixelShader = create_shader<false>;
    cb.pfnDestroyShader = destroy_private<Shader>;
    cb.pfnCalcPrivateRasterizerStateSize = private_size<D3D11_1_DDI_RASTERIZER_DESC>;
    cb.pfnCreateRasterizerState = create_state<D3D11_1_DDI_RASTERIZER_DESC>;
    cb.pfnDestroyRasterizerState = destroy_private<D3D11_1_DDI_RASTERIZER_DESC>;
    cb.pfnCalcPrivateDepthStencilStateSize = private_size<D3D10_DDI_DEPTH_STENCIL_DESC>;
    cb.pfnCreateDepthStencilState = create_state<D3D10_DDI_DEPTH_STENCIL_DESC>;
    cb.pfnDestroyDepthStencilState = destroy_private<D3D10_DDI_DEPTH_STENCIL_DESC>;
    cb.pfnCalcPrivateBlendStateSize = private_size<D3D11_1_DDI_BLEND_DESC>;
    cb.pfnCreateBlendState = create_state<D3D11_1_DDI_BLEND_DESC>;
    cb.pfnDestroyBlendState = destroy_private<D3D11_1_DDI_BLEND_DESC>;
    cb.pfnIaSetTopology = bind_topology;
    cb.pfnIaSetInputLayout = bind_layout;
    cb.pfnIaSetVertexBuffers = bind_vertices;
    cb.pfnIaSetIndexBuffer = null_index;
    cb.pfnVsSetShader = bind_vs;
    cb.pfnPsSetShader = bind_ps;
    cb.pfnGsSetShader = cb.pfnHsSetShader = cb.pfnDsSetShader = cb.pfnCsSetShader = null_shader;
    cb.pfnVsSetShaderResources = cb.pfnPsSetShaderResources = cb.pfnGsSetShaderResources =
        cb.pfnHsSetShaderResources = cb.pfnDsSetShaderResources = cb.pfnCsSetShaderResources = null_bindings;
    cb.pfnVsSetSamplers = cb.pfnPsSetSamplers = cb.pfnGsSetSamplers =
        cb.pfnHsSetSamplers = cb.pfnDsSetSamplers = cb.pfnCsSetSamplers = null_bindings;
    cb.pfnVsSetConstantBuffers = cb.pfnPsSetConstantBuffers = cb.pfnGsSetConstantBuffers =
        cb.pfnHsSetConstantBuffers = cb.pfnDsSetConstantBuffers = cb.pfnCsSetConstantBuffers = null_constants;
    cb.pfnSetRenderTargets = bind_targets;
    cb.pfnSetRasterizerState = bind_raster;
    cb.pfnSetDepthStencilState = bind_depth;
    cb.pfnSetBlendState = bind_blend;
    cb.pfnSetViewports = viewports;
    cb.pfnSetScissorRects = +[](D3D10DDI_HDEVICE, UINT, UINT, const D3D10_DDI_RECT*) {};
    cb.pfnClearRenderTargetView = clear_color;
    cb.pfnClearDepthStencilView = clear_depth;
    cb.pfnDraw = draw;
    cb.pfnDrawIndexed = unsupported;
    cb.pfnDrawInstanced = unsupported;
    cb.pfnDrawIndexedInstanced = unsupported;
    cb.pfnResourceCopy = copy_resource;
    cb.pfnResourceMap = cb.pfnStagingResourceMap = map_resource;
    cb.pfnResourceUnmap = cb.pfnStagingResourceUnmap = unmap_resource;
    cb.pfnResourceIsStagingBusy = is_busy;
    cb.pfnResourceReadAfterWriteHazard = hazard;
    cb.pfnCheckFormatSupport = format;
    cb.pfnCheckMultisampleQualityLevels = sample_quality;
    cb.pfnCheckCounterInfo = counter_info;
    cb.pfnCheckCounter = unsupported;
    cb.pfnRelocateDeviceFuncs = relocate;
    cb.pfnFlush = flush;
    return cb;
}

SIZE_T APIENTRY device_size(D3D10DDI_HADAPTER, const D3D10DDIARG_CALCPRIVATEDEVICESIZE*) {
    return sizeof(Device);
}

HRESULT APIENTRY create_device(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE* args) {
    (void)adapter;
    if (!args || args->Interface != D3DWDDM1_3_DDI_INTERFACE_VERSION ||
        D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(args->Flags) != D3D11_1DDI_3DPIPELINELEVEL_9_1)
        return DXGI_ERROR_UNSUPPORTED;
    if (!args->hDrvDevice.pDrvPrivate || !args->pWDDM1_3DeviceFuncs ||
        !args->p11UMCallbacks || !args->p11UMCallbacks->pfnSetErrorCb || !args->pKTCallbacks ||
        !args->pKTCallbacks->pfnCreateContextCb || !args->pKTCallbacks->pfnDestroyContextCb ||
        !args->pKTCallbacks->pfnAllocateCb || !args->pKTCallbacks->pfnDeallocateCb ||
        !args->pKTCallbacks->pfnRenderCb || !args->pKTCallbacks->pfnLockCb ||
        !args->pKTCallbacks->pfnUnlockCb || !args->DXGIBaseDDI.pDXGIDDIBaseFunctions4)
        return E_INVALIDARG;
    D3DDDICB_CREATECONTEXT request{};
    const HRESULT hr = args->pKTCallbacks->pfnCreateContextCb(args->hRTDevice.handle, &request);
    if (FAILED(hr)) return hr;
    if (!request.hContext) return DXGI_ERROR_DEVICE_REMOVED;
    if (!request.pCommandBuffer || !request.CommandBufferSize || !request.pAllocationList ||
        !request.AllocationListSize || !request.pPatchLocationList || !request.PatchLocationListSize) {
        const D3DDDICB_DESTROYCONTEXT context{request.hContext};
        args->pKTCallbacks->pfnDestroyContextCb(args->hRTDevice.handle, &context);
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    new (args->hDrvDevice.pDrvPrivate) Device{args->hRTDevice, args->hRTCoreLayer, args->pKTCallbacks,
        args->p11UMCallbacks, request.hContext, request.pCommandBuffer, request.CommandBufferSize,
        request.pAllocationList, request.AllocationListSize, request.pPatchLocationList, request.PatchLocationListSize};
    *args->pWDDM1_3DeviceFuncs = render_functions();
    *args->DXGIBaseDDI.pDXGIDDIBaseFunctions4 = dxgi_functions();
    return S_OK;
}

HRESULT APIENTRY close_adapter(D3D10DDI_HADAPTER) {
    return S_OK;
}

HRESULT APIENTRY versions(D3D10DDI_HADAPTER, UINT32* count, UINT64* out) {
    if (!count) return E_INVALIDARG;
    if (out && !*count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *count = 1;
    if (out) *out = supported_ddi;
    return S_OK;
}

HRESULT APIENTRY caps(D3D10DDI_HADAPTER, const D3D10_2DDIARG_GETCAPS* args) {
    if (!args || !args->pData) return E_INVALIDARG;
    switch (args->Type) {
        case D3D11DDICAPS_3DPIPELINESUPPORT:
            if (args->DataSize != sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)) return E_INVALIDARG;
            static_cast<D3D11DDI_3DPIPELINESUPPORT_CAPS*>(args->pData)->Caps =
                D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11_1DDI_3DPIPELINELEVEL_9_1);
            return S_OK;
        case D3D11DDICAPS_THREADING:
        case D3D11DDICAPS_SHADER:
            if (args->DataSize != sizeof(UINT)) return E_INVALIDARG;
            *static_cast<UINT*>(args->pData) = 0;
            return S_OK;
        case D3D11_1DDICAPS_D3D11_OPTIONS:
            if (args->DataSize != sizeof(D3D11_1DDI_D3D11_OPTIONS_DATA)) return E_INVALIDARG;
            *static_cast<D3D11_1DDI_D3D11_OPTIONS_DATA*>(args->pData) = {};
            return S_OK;
        case D3D11_1DDICAPS_ARCHITECTURE_INFO:
            if (args->DataSize != sizeof(D3D11_1DDI_ARCHITECTURE_INFO_DATA)) return E_INVALIDARG;
            *static_cast<D3D11_1DDI_ARCHITECTURE_INFO_DATA*>(args->pData) = {};
            return S_OK;
        default:
            return E_NOTIMPL;
    }
}
}

extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER* args) {
    if (!args || !args->pAdapterFuncs_2 || !args->pAdapterCallbacks ||
        !args->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
    RwGpuWddmAdapter adapter{};
    D3DDDICB_QUERYADAPTERINFO query{&adapter, sizeof(adapter)};
    const HRESULT status = args->pAdapterCallbacks->pfnQueryAdapterInfoCb(args->hRTAdapter.handle, &query);
    if (FAILED(status)) return status;
    if (adapter.header.magic != RWGPU_WDDM_MAGIC || adapter.header.version != RWGPU_WDDM_ABI ||
        adapter.header.command != RWGPU_WDDM_QUERY || adapter.header.bytes != sizeof(adapter) ||
        adapter.pci_id != ((RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID) ||
        adapter.logical_vram_bytes != RWGPU_LOGICAL_BYTES ||
        !(adapter.capabilities & RWGPU_CAP_WDDM_SCHEDULER)) return DXGI_ERROR_UNSUPPORTED;

    const D3D10_2DDI_ADAPTERFUNCS callbacks{device_size, create_device, close_adapter, versions, caps};
    *args->pAdapterFuncs_2 = callbacks;
    args->hAdapter.pDrvPrivate = args->hRTAdapter.handle;
    return S_OK;
}
