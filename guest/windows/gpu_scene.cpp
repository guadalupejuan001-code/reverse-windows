#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "gpu_transport.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr unsigned width = 640, height = 480;
constexpr std::uint32_t clear = 0xff1e140a, red = 0xff0000ff, green = 0xff00ff00, blue = 0xffff0000;
constexpr std::array<RwGpuVertex, 9> scene{{
    {-.7f,-.55f,.2f,1,red}, {.7f,-.55f,.2f,1,red}, {0,.65f,.2f,1,red},
    {-.85f,-.7f,.8f,1,green}, {.85f,-.7f,.8f,1,green}, {0,.85f,.8f,1,green},
    {-.65f,.4f,.3f,1,blue}, {-1.3f,.65f,.3f,1,blue}, {-.7f,.95f,.3f,1,blue}
}};
constexpr std::array<std::array<unsigned, 3>, 4> checks{{
    {320, 240, red}, {320, 63, green}, {34, 81, blue}, {639, 479, clear}
}};

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s failed (0x%08lx)\n", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}

ComPtr<ID3DBlob> compile(const char* program, const char* target) {
    ComPtr<ID3DBlob> shader, errors;
    const auto hr = D3DCompile(program, std::strlen(program), nullptr, nullptr, nullptr,
                               "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    check(hr, target);
    return shader;
}

std::array<std::uint32_t, checks.size()> render_d3d11(IDXGIAdapter1* adapter = nullptr) {
    ComPtr<ID3D11Device> d;
    ComPtr<ID3D11DeviceContext> c;
    const auto type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP;
    check(D3D11CreateDevice(adapter, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            &d, nullptr, &c), "D3D11 device");
    auto vs = compile("struct V {float4 p : SV_Position; float4 c : COLOR0;};"
                      "V main(float4 p : POSITION0, float4 c : COLOR0) {V v;v.p=p;v.c=c;return v;}",
                      "vs_4_0");
    auto ps = compile("struct V {float4 p : SV_Position; float4 c : COLOR0;};"
                      "float4 main(V i) : SV_Target0 {return i.c;}", "ps_4_0");
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    check(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "vertex shader");
    check(d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel), "pixel shader");
    D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    ComPtr<ID3D11InputLayout> layout;
    check(d->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout), "layout");
    D3D11_BUFFER_DESC vbo{};
    vbo.ByteWidth = sizeof(scene);
    vbo.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbo.Usage = D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = scene.data();
    ComPtr<ID3D11Buffer> positions;
    check(d->CreateBuffer(&vbo, &data, &positions), "vertices");

    D3D11_TEXTURE2D_DESC image{};
    image.Width = width; image.Height = height;
    image.MipLevels = 1; image.ArraySize = 1; image.SampleDesc.Count = 1;
    image.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    image.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, depth, output;
    check(d->CreateTexture2D(&image, nullptr, &target), "D3D render target");
    image.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    image.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    check(d->CreateTexture2D(&image, nullptr, &depth), "D3D depth target");
    image.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    image.BindFlags = 0;
    image.Usage = D3D11_USAGE_STAGING;
    image.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(d->CreateTexture2D(&image, nullptr, &output), "D3D staging");
    ComPtr<ID3D11RenderTargetView> color_view;
    ComPtr<ID3D11DepthStencilView> depth_view;
    check(d->CreateRenderTargetView(target.Get(), nullptr, &color_view), "D3D target view");
    check(d->CreateDepthStencilView(depth.Get(), nullptr, &depth_view), "D3D depth view");
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> state;
    check(d->CreateRasterizerState(&raster, &state), "D3D rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth_options{};
    depth_options.DepthEnable = TRUE;
    depth_options.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_options.DepthFunc = D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> depth_state;
    check(d->CreateDepthStencilState(&depth_options, &depth_state), "D3D depth test");
    const D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
    c->RSSetState(state.Get());
    c->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* views[]{color_view.Get()};
    c->OMSetRenderTargets(1, views, depth_view.Get());
    c->OMSetDepthStencilState(depth_state.Get(), 0);
    const float background[]{10 / 255.f, 20 / 255.f, 30 / 255.f, 1};
    c->ClearRenderTargetView(color_view.Get(), background);
    c->ClearDepthStencilView(depth_view.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    const UINT stride = sizeof(RwGpuVertex), offset = 0;
    ID3D11Buffer* buffers[]{positions.Get()};
    c->IASetInputLayout(layout.Get());
    c->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex.Get(), nullptr, 0);
    c->PSSetShader(pixel.Get(), nullptr, 0);
    c->Draw(static_cast<UINT>(scene.size()), 0);
    c->CopyResource(output.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(c->Map(output.Get(), 0, D3D11_MAP_READ, 0, &mapped), "D3D readback");
    std::array<std::uint32_t, checks.size()> pixels{};
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const auto* sample = static_cast<const std::uint8_t*>(mapped.pData) +
                             checks[i][1] * mapped.RowPitch + checks[i][0] * 4;
        std::memcpy(&pixels[i], sample, sizeof(pixels[i]));
    }
    c->Unmap(output.Get(), 0);
    return pixels;
}

void verify_guest(const std::array<std::uint32_t, checks.size()>& expected) {
    const auto device = CreateFileW(RWGPU_DEVICE_PATH, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "REVERSE Prime PCI transport failed to open (Win32 %lu)\n", GetLastError());
        throw std::runtime_error("PCI transport unavailable");
    }
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{device};
    struct Packet {
        RwGpuDrawRequest request;
        std::array<RwGpuVertex, scene.size()> vertices;
    } packet{{RWGPU_ABI, width, height, clear, static_cast<std::uint32_t>(scene.size())}, scene};
    static_assert(sizeof(Packet) == sizeof(RwGpuDrawRequest) + sizeof(scene));
    std::vector<std::uint8_t> result(width * height * 4);
    DWORD written = 0;
    auto start = std::chrono::steady_clock::now();
    if (!DeviceIoControl(device, RWGPU_IOCTL_DRAW, &packet, sizeof(packet), result.data(),
                         static_cast<DWORD>(result.size()), &written, nullptr) || written != result.size()) {
        std::fprintf(stderr, "REVERSE Prime PCI draw failed (Win32 %lu)\n", GetLastError());
        throw std::runtime_error("PCI draw failed");
    }
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const auto* p = result.data() + (checks[i][1] * width + checks[i][0]) * 4;
        const auto color = 0xff000000U | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
        if (color != expected[i]) {
            std::fprintf(stderr, "PCI color (%u,%u) %08x expected %08x\n",
                         checks[i][0], checks[i][1], color, expected[i]);
            throw std::runtime_error("REgraphics differs from Direct3D oracle");
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("PRIME_C08_WINDOWS_PCI_SCENE_PASS oracle=D3D11-WARP compared_pixels=%zu roundtrip_us=%lld\n",
                checks.size(), static_cast<long long>(elapsed));
}

ComPtr<IDXGIAdapter1> find_prime_adapter() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) return {};
        check(result, "Enumerate DXGI adapter");
        DXGI_ADAPTER_DESC1 info{};
        check(adapter->GetDesc1(&info), "Read DXGI adapter identity");
        if (info.VendorId == RWGPU_VENDOR_ID && info.DeviceId == RWGPU_DEVICE_ID &&
            !(info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return adapter;
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--wddm") {
            const auto adapter = find_prime_adapter();
            if (!adapter) {
                std::fprintf(stderr, "PRIME_C08_WDDM_ADAPTER_ABSENT pci=1b36:c083\n");
                return 3;
            }
            if (render_d3d11(adapter.Get()) != render_d3d11())
                throw std::runtime_error("WDDM rendering differs from the Direct3D oracle");
            std::printf("PRIME_C08_WDDM_SCENE_PASS pci=1b36:c083 pixels=%zu\n", checks.size());
            return 0;
        }
        const auto samples = render_d3d11();
        for (std::size_t i = 0; i < checks.size(); ++i) {
            if (samples[i] != checks[i][2]) {
                std::fprintf(stderr, "D3D11 color (%u,%u) %08x expected %08x\n",
                             checks[i][0], checks[i][1], samples[i], checks[i][2]);
                throw std::runtime_error("Direct3D fixture differs from pixel oracle");
            }
        }
        std::printf("PRIME_C08_DIRECT3D_REFERENCE_PASS pixels=%zu device=WARP\n", checks.size());
        if (argc == 2 && std::string_view(argv[1]) == "--reference") return 0;
        if (argc != 1) throw std::runtime_error("Usage: prime_gpu_scene [--reference | --wddm]");
        verify_guest(samples);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
