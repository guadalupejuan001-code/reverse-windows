#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
typedef _Return_type_success_(return >= 0) LONG NTSTATUS;
#pragma warning(push)
#pragma warning(disable : 4201)
#include <d3d10umddi.h>
#pragma warning(pop)
#include "../../src/c08_wddm_protocol.h"
#include "gpu_scene_shaders.h"
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
constexpr UINT width = 16, height = 16, clear = 0x7f1e140a;
constexpr UINT color_bytes = width * height * 4;
constexpr UINT rgba = 0x90785634, bgra = 0x90345678;
RwGpuVertex geometry[]{{-1.f,-1.f,.5f,1.f,0x800000ff},{1.f,-1.f,.5f,1.f,0x8000ff00},
    {0.f,1.f,.5f,1.f,0x80ff0000},{-.5f,-.5f,.25f,1.f,0x400000ff},
    {.5f,-.5f,.25f,1.f,0x4000ff00},{0.f,.5f,.25f,1.f,0x40ff0000}};

struct Runtime {
    HRESULT error = S_OK;
    unsigned draws = 0, contexts = 0;
    bool supplies_ticket = true;
};

HRESULT APIENTRY query(HANDLE, const D3DDDICB_QUERYADAPTERINFO* info) {
    if (info->PrivateDriverDataSize != sizeof(RwGpuWddmAdapter)) return E_INVALIDARG;
    const RwGpuWddmAdapter out{{RWGPU_WDDM_MAGIC, RWGPU_WDDM_ABI, RWGPU_WDDM_QUERY, sizeof(out)},
        RWGPU_LOGICAL_BYTES, (RWGPU_DEVICE_ID << 16) | RWGPU_VENDOR_ID,
        RWGPU_CAP_DRAW_RAM | RWGPU_CAP_COLOR_DMA | RWGPU_CAP_COLOR_ALPHA, RWGPU_MAX_WIDTH, RWGPU_MAX_HEIGHT};
    std::memcpy(info->pPrivateDriverData, &out, sizeof(out));
    return S_OK;
}

HRESULT APIENTRY create_context(HANDLE device, D3DDDICB_CREATECONTEXT* context) {
    auto& rt = *static_cast<Runtime*>(device);
    if (rt.contexts || context->NodeOrdinal || context->PrivateDriverDataSize) return E_INVALIDARG;
    context->hContext = &rt.contexts;
    ++rt.contexts;
    return S_OK;
}

HRESULT APIENTRY destroy_context(HANDLE device, const D3DDDICB_DESTROYCONTEXT* context) {
    auto& rt = *static_cast<Runtime*>(device);
    if (rt.contexts != 1 || context->hContext != &rt.contexts) return E_INVALIDARG;
    --rt.contexts;
    return S_OK;
}

void APIENTRY set_error(D3D10DDI_HRTCORELAYER core, HRESULT error) {
    static_cast<Runtime*>(core.handle)->error = error;
}

// Stand-in for the runtime callback. Validate the packet and return known
// BGRA bytes so the test can distinguish UMD decoding from actual GPU rendering.
HRESULT APIENTRY escape(HANDLE adapter, const D3DDDICB_ESCAPE* request) {
    auto& rt = *static_cast<Runtime*>(adapter);
    if (request->hDevice != &rt || request->hContext != &rt.contexts || !request->Flags.HardwareAccess ||
        request->PrivateDriverDataSize != sizeof(RwGpuWddmFrame) + sizeof(geometry) + color_bytes)
        return E_INVALIDARG;
    auto* data = static_cast<std::uint8_t*>(request->pPrivateDriverData);
    RwGpuWddmFrame frame{};
    std::memcpy(&frame, data, sizeof(frame));
    uint32_t vertices = 0, pixels = 0;
    if (frame.header.magic != RWGPU_WDDM_MAGIC || frame.header.version != RWGPU_WDDM_ABI ||
        frame.header.command != RWGPU_WDDM_DRAW || !rwgpu_wddm_frame_size(&frame, &vertices, &pixels) ||
        frame.draw_ticket || frame.width != width || frame.height != height || frame.clear_rgba != clear ||
        vertices != sizeof(geometry) || pixels != color_bytes ||
        std::memcmp(data + sizeof(frame), geometry, sizeof(geometry))) return E_INVALIDARG;
    ++rt.draws;
    if (rt.supplies_ticket) frame.draw_ticket = rt.draws;
    std::memcpy(data, &frame, sizeof(frame));
    for (UINT offset = 0; offset < pixels; offset += sizeof(bgra))
        std::memcpy(data + sizeof(frame) + vertices + offset, &bgra, sizeof(bgra));
    return S_OK;
}

void require(bool condition, const char* operation) {
    if (!condition) throw std::runtime_error(operation);
}

void check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return;
    std::fprintf(stderr, "%s: 0x%08lx\n", operation, static_cast<unsigned long>(hr));
    throw std::runtime_error(operation);
}

std::vector<UINT> compile(const char* hlsl, const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
    const HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr, "main", profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    check(hr, profile);
    const auto* bytes = static_cast<const std::uint8_t*>(shader->GetBufferPointer());
    const auto size = shader->GetBufferSize();
    const auto read = [&](std::size_t offset) {
        require(offset <= size && size - offset >= sizeof(UINT), "short DXBC container");
        UINT value;
        std::memcpy(&value, bytes + offset, sizeof(value));
        return value;
    };
    require(read(0) == 0x43425844, "invalid DXBC container magic");
    const UINT chunks = read(28);
    require(chunks <= (size - 32) / 4, "short DXBC index");
    for (UINT i = 0; i < chunks; ++i) {
        const auto offset = std::size_t(read(32 + 4 * i));
        const UINT kind = read(offset);
        if (kind != 0x52444853 && kind != 0x58454853) continue;
        const UINT length = read(offset + 4);
        require(length >= 8 && length % 4 == 0 && length <= size - offset - 8 &&
            read(offset + 12) == length / 4, "invalid DXBC shader length");
        std::vector<UINT> code(length / 4);
        std::memcpy(code.data(), bytes + offset + 8, length);
        return code;
    }
    throw std::runtime_error("tokenized shader chunk not found");
}

template<class Handle>
struct Storage {
    std::vector<std::max_align_t> bytes;
    Handle value;
    explicit Storage(SIZE_T size) : bytes((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)),
        value{bytes.data()} { require(size != 0, "missing UMD object size"); }
};

void exercise(PFND3D10DDI_OPENADAPTER open) {
    Runtime runtime;
    D3DDDI_ADAPTERCALLBACKS host{};
    host.pfnQueryAdapterInfoCb = query;
    D3D10_2DDI_ADAPTERFUNCS api{};
    D3D10DDIARG_OPENADAPTER adapter{};
    adapter.hRTAdapter.handle = &runtime;
    adapter.pAdapterCallbacks = &host;
    adapter.pAdapterFuncs_2 = &api;
    adapter.Interface = D3DWDDM1_3_DDI_INTERFACE_VERSION;
    check(open(&adapter), "OpenAdapter10_2");
    const auto a = adapter.hAdapter;
    D3DDDI_DEVICECALLBACKS kernel{};
    kernel.pfnEscapeCb = escape;
    kernel.pfnCreateContextCb = create_context;
    kernel.pfnDestroyContextCb = destroy_context;
    D3D11DDI_CORELAYER_DEVICECALLBACKS user{};
    user.pfnSetErrorCb = set_error;
    D3DWDDM1_3DDI_DEVICEFUNCS f{};
    DXGI1_3_DDI_BASE_FUNCTIONS dxgi{};
    Storage<D3D10DDI_HDEVICE> storage(api.pfnCalcPrivateDeviceSize(a, nullptr));
    const auto d = storage.value;
    D3D10DDIARG_CREATEDEVICE device{};
    device.Interface = D3DWDDM1_3_DDI_INTERFACE_VERSION;
    device.Flags = D3D11_1DDI_3DPIPELINELEVEL_9_1 << D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT;
    device.hRTDevice.handle = device.hRTCoreLayer.handle = &runtime;
    device.hDrvDevice = d;
    device.pKTCallbacks = &kernel;
    device.p11UMCallbacks = &user;
    device.pWDDM1_3DeviceFuncs = &f;
    device.DXGIBaseDDI.pDXGIDDIBaseFunctions4 = &dxgi;
    check(api.pfnCreateDevice(a, &device), "CreateDevice");

    const auto vs = compile(scene_vs, "vs_4_0_level_9_1");
    const auto ps = compile(scene_ps, "ps_4_0_level_9_1");
    const auto inverted = compile("float4 main(float4 c : COLOR0) : SV_Target0 {return c.bgra;}",
        "ps_4_0_level_9_1");
    D3D11_1DDIARG_STAGE_IO_SIGNATURES signature{};
    Storage<D3D10DDI_HSHADER> vertex(f.pfnCalcPrivateShaderSize(d, vs.data(), &signature));
    Storage<D3D10DDI_HSHADER> pixel(f.pfnCalcPrivateShaderSize(d, ps.data(), &signature));
    Storage<D3D10DDI_HSHADER> rejected(f.pfnCalcPrivateShaderSize(d, inverted.data(), &signature));
    f.pfnCreateVertexShader(d, vs.data(), vertex.value, {}, &signature);
    check(runtime.error, "vertex passthrough");
    f.pfnCreatePixelShader(d, ps.data(), pixel.value, {}, &signature);
    check(runtime.error, "pixel passthrough");
    f.pfnCreatePixelShader(d, inverted.data(), rejected.value, {}, &signature);
    require(runtime.error == DXGI_ERROR_UNSUPPORTED, "swizzled shader must be rejected");
    runtime.error = S_OK;

    D3D10DDI_MIPINFO dims{sizeof(geometry), 1, 1, sizeof(geometry), 1, 1};
    const D3D10_DDIARG_SUBRESOURCE_UP vertices{geometry, sizeof(geometry), sizeof(geometry)};
    D3D11DDIARG_CREATERESOURCE res{};
    res.pMipInfoList = &dims;
    res.pInitialDataUP = &vertices;
    res.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
    res.Usage = D3D10_DDI_USAGE_IMMUTABLE;
    res.BindFlags = D3D10_DDI_BIND_VERTEX_BUFFER;
    res.SampleDesc.Count = res.MipLevels = res.ArraySize = 1;
    Storage<D3D10DDI_HRESOURCE> buffer(f.pfnCalcPrivateResourceSize(d, &res));
    f.pfnCreateResource(d, &res, buffer.value, {});
    check(runtime.error, "vertex buffer");
    dims = {width, height, 1, width, height, 1};
    res.pInitialDataUP = nullptr;
    res.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
    res.Usage = D3D10_DDI_USAGE_DEFAULT;
    res.BindFlags = D3D10_DDI_BIND_RENDER_TARGET;
    res.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Storage<D3D10DDI_HRESOURCE> color(f.pfnCalcPrivateResourceSize(d, &res));
    f.pfnCreateResource(d, &res, color.value, {});
    D3D10DDIARG_CREATERENDERTARGETVIEW color_args{};
    color_args.hDrvResource = color.value;
    color_args.ResourceDimension = res.ResourceDimension;
    color_args.Format = res.Format;
    color_args.Tex2D.ArraySize = 1;
    Storage<D3D10DDI_HRENDERTARGETVIEW> color_view(f.pfnCalcPrivateRenderTargetViewSize(d, &color_args));
    f.pfnCreateRenderTargetView(d, &color_args, color_view.value, {});
    res.BindFlags = D3D10_DDI_BIND_DEPTH_STENCIL;
    res.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    Storage<D3D10DDI_HRESOURCE> depth(f.pfnCalcPrivateResourceSize(d, &res));
    f.pfnCreateResource(d, &res, depth.value, {});
    D3D11DDIARG_CREATEDEPTHSTENCILVIEW depth_args{};
    depth_args.hDrvResource = depth.value;
    depth_args.ResourceDimension = res.ResourceDimension;
    depth_args.Format = res.Format;
    depth_args.Tex2D.ArraySize = 1;
    Storage<D3D10DDI_HDEPTHSTENCILVIEW> depth_view(f.pfnCalcPrivateDepthStencilViewSize(d, &depth_args));
    f.pfnCreateDepthStencilView(d, &depth_args, depth_view.value, {});
    res.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res.Usage = D3D10_DDI_USAGE_STAGING;
    res.BindFlags = 0;
    res.MapFlags = D3D10_DDI_CPU_ACCESS_READ;
    Storage<D3D10DDI_HRESOURCE> output(f.pfnCalcPrivateResourceSize(d, &res));
    f.pfnCreateResource(d, &res, output.value, {});
    check(runtime.error, "offscreen resources and views");

    D3D10DDIARG_INPUT_ELEMENT_DESC input[2]{};
    input[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    input[1].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    input[1].InputRegister = 1;
    input[1].AlignedByteOffset = 16;
    const D3D10DDIARG_CREATEELEMENTLAYOUT elements{input, 2};
    Storage<D3D10DDI_HELEMENTLAYOUT> layout(f.pfnCalcPrivateElementLayoutSize(d, &elements));
    f.pfnCreateElementLayout(d, &elements, layout.value, {});
    D3D11_1_DDI_RASTERIZER_DESC raster{};
    raster.FillMode = D3D10_DDI_FILL_SOLID;
    raster.CullMode = D3D10_DDI_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    Storage<D3D10DDI_HRASTERIZERSTATE> state(f.pfnCalcPrivateRasterizerStateSize(d, &raster));
    f.pfnCreateRasterizerState(d, &raster, state.value, {});
    const D3D10_DDI_VIEWPORT viewport{0, 0, width, height, 0, 1};
    const UINT stride = sizeof(RwGpuVertex), offset = 0;
    f.pfnIaSetInputLayout(d, layout.value);
    f.pfnIaSetVertexBuffers(d, 0, 1, &buffer.value, &stride, &offset);
    f.pfnIaSetTopology(d, D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    f.pfnSetRasterizerState(d, state.value);
    f.pfnSetViewports(d, 1, 0, &viewport);
    f.pfnSetRenderTargets(d, &color_view.value, 1, 0, depth_view.value, nullptr, nullptr, 0, 0, 0, 0);
    f.pfnVsSetShader(d, vertex.value);
    f.pfnPsSetShader(d, pixel.value);

    FLOAT background[]{10 / 255.f, 20 / 255.f, 30 / 255.f, 127 / 255.f};
    const auto render = [&] {
        f.pfnClearRenderTargetView(d, color_view.value, background);
        f.pfnClearDepthStencilView(d, depth_view.value, D3D10_DDI_CLEAR_DEPTH, 1.f, 0);
        f.pfnDraw(d, 3, 0);
        f.pfnDraw(d, 3, 3);
        check(runtime.error, "draw state and geometry");
    };
    render();
    require(!runtime.draws, "draws should be grouped until readback");
    f.pfnResourceCopy(d, output.value, color.value);
    check(runtime.error, "submit and copy");
    D3D10DDI_MAPPED_SUBRESOURCE mapped{};
    f.pfnStagingResourceMap(d, output.value, 0, D3D10_DDI_MAP_READ, 0, &mapped);
    check(runtime.error, "map readback");
    UINT value = 0;
    require(mapped.pData && mapped.RowPitch == width * 4 && mapped.DepthPitch == color_bytes, "readback pitch");
    std::memcpy(&value, mapped.pData, sizeof(value));
    require(value == rgba && runtime.draws == 1, "expected exactly one submission and RGBA bytes");
    f.pfnStagingResourceUnmap(d, output.value, 0);
    f.pfnResourceCopy(d, output.value, color.value);
    check(runtime.error, "reuse completed render");
    require(runtime.draws == 1, "unchanged color target resubmitted");

    runtime.supplies_ticket = false;
    render();
    f.pfnResourceCopy(d, output.value, color.value);
    require(runtime.error == DXGI_ERROR_DEVICE_REMOVED && runtime.draws == 2, "missing PCI completion accepted");
    f.pfnDestroyRasterizerState(d, state.value);
    f.pfnDestroyElementLayout(d, layout.value);
    f.pfnDestroyDepthStencilView(d, depth_view.value);
    f.pfnDestroyRenderTargetView(d, color_view.value);
    for (auto r : {output.value, depth.value, color.value, buffer.value}) f.pfnDestroyResource(d, r);
    for (auto s : {rejected.value, pixel.value, vertex.value}) f.pfnDestroyShader(d, s);
    f.pfnDestroyDevice(d);
    require(runtime.contexts == 0, "UMD context leaked");
    check(api.pfnCloseAdapter(a), "CloseAdapter");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: prime_render_umd_test path-to-umd.dll");
        const HMODULE umd = LoadLibraryA(argv[1]);
        require(umd != nullptr, "load UMD DLL");
        const auto proc = GetProcAddress(umd, "OpenAdapter10_2");
        require(proc != nullptr, "load OpenAdapter10_2");
        PFND3D10DDI_OPENADAPTER open{};
        static_assert(sizeof(open) == sizeof(proc));
        std::memcpy(&open, &proc, sizeof(open));
        exercise(open);
        FreeLibrary(umd);
        std::puts("PRIME_C08_UMD_CALLBACKS_PASS shaders=FXC submissions=2 mode=callback-test");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
