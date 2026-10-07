// Reads the application's multisampled depth-stencil through a shader.
//
// See the header for why this is the only way. The pass is written to be small and to leave the
// application's state alone as far as is practical: it runs at the end of a frame, which is where the
// layer already draws, so whatever it changes is re-established by the application on its next frame.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <wrl/client.h>

#include "AmdnrDepthMsaaRead.h"
#include "FsrShaders.h"
#include "LayerLog.h"

namespace {

    using Microsoft::WRL::ComPtr;

    // The vertex shader is the one the FSR passes already use: an oversized triangle from
    // SV_VertexID, needing no vertex buffer and no input layout. Its second output is the 0..1 uv
    // this pass reads instead of a texture coordinate.
    //
    // Two choices are made here, and both are about not inventing a surface that is not there.
    //
    // The block is reduced with the nearest sample rather than the average. An average across a
    // silhouette produces a value that belongs to no surface at all, and the whole reason to look at
    // every texel in the block is to stop the small, close geometry from being missed -- which is the
    // opposite of what averaging does with it.
    //
    // The result is published as inverse distance, `1 - raw`. For a standard projection that is
    // proportional to 1/z, which is the space a depth comparison is meaningful in, and it needs no
    // knowledge of the near and far planes -- nothing in the layer is told those, and a guessed pair
    // would silently rescale every value. It is a linear remap of the raw value, so it changes no
    // precision; what it changes is which quantity a consumer that interpolates or compares depth is
    // working with.
    //
    // The application's own sample count is not known here, so a depth carrying fewer samples than
    // the shader reads repeats sample 0 rather than reading past what exists.
    const char* const kReduceShader = R"(
cbuffer Params : register(b0)
{
    float4 Range;      // x = near plane, y = far plane, z = linearise (0 or 1), w unused
    float4 BlockStep;  // xy = the source rectangle this reads, in texels
                       // zw = how many source texels one output texel covers inside it
    float4 Origin;     // xy = where that rectangle starts in the source, in texels (0,0 when unset)
};

Texture2DMS<float4> DepthMs : register(t0);

float4 main_ps(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    uint width = 0;
    uint height = 0;
    uint samples = 0;
    DepthMs.GetDimensions(width, height, samples);

    // Everything below is in source texels, and the uv is mapped across the rectangle rather than
    // across the whole texture: the depth this reads lives in one corner of a larger allocation, and
    // where that corner starts is a measurement, not a guess -- the remaining rows and columns carry
    // nothing but the clear, and publishing them as depth is what the callers were reading.
    const float2 base = Origin.xy + uv * BlockStep.xy;

    // Centred taps, so a rectangle the size of the output degenerates to a point sample of the texel
    // under the pixel instead of drifting half a texel off it.
    float nearest = 1.0;
    [unroll] for (int ty = 0; ty < 2; ty++)
    {
        [unroll] for (int tx = 0; tx < 2; tx++)
        {
            const float2 source = base + (float2(tx, ty) - 0.5) * 0.5 * BlockStep.zw;
            int2 coord = clamp(int2(source), int2(0, 0), int2(width - 1, height - 1));
            float a = DepthMs.Load(coord, 0).x;
            float b = samples > 1 ? DepthMs.Load(coord, 1).x : a;
            float c = samples > 2 ? DepthMs.Load(coord, 2).x : a;
            float d = samples > 3 ? DepthMs.Load(coord, 3).x : a;
            nearest = min(nearest, min(min(a, b), min(c, d)));
        }
    }

    // The one transform that does need the planes, kept for when they are known. Far and near swap
    // ends here, so the value goes to 0 at the near plane and 1 at the far one.
    if (Range.z > 0.5)
    {
        float nearPlane = max(Range.x, 1e-6);
        float farPlane = max(Range.y, nearPlane + 1e-6);
        float distance = nearPlane * farPlane / (farPlane - nearest * (farPlane - nearPlane));
        return saturate((distance - nearPlane) / (farPlane - nearPlane));
    }

    return 1.0 - nearest;
}
)";

    // d3dcompiler_47.dll is loaded by name rather than linked, the same way NrCore and the layer's
    // output path do it: a hard import would stop the whole layer loading where the runtime ships its
    // own copy or none at all.
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID,
                                          SIZE_T,
                                          LPCSTR,
                                          const D3D_SHADER_MACRO*,
                                          ID3DInclude*,
                                          LPCSTR,
                                          LPCSTR,
                                          UINT,
                                          UINT,
                                          ID3DBlob**,
                                          ID3DBlob**);

    D3DCompileFn GetD3DCompile() {
        static D3DCompileFn compile = []() -> D3DCompileFn {
            const HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
            return module ? reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
        }();
        return compile;
    }

    // The format a depth is read through. A depth-stencil resource is usually typeless, and the view
    // format the layer recorded is the typed one; either way the sample is read as the single
    // channel the reduction uses.
    DXGI_FORMAT ReadFormat(DXGI_FORMAT viewFormat) {
        switch (viewFormat) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        default:
            // Everything else this layer has seen is a 32-bit float depth: D32_FLOAT, R32_FLOAT and
            // R32_TYPELESS share that layout.
            return DXGI_FORMAT_R32_FLOAT;
        }
    }

    struct ReduceConstants {
        float range[4];
        float blockStep[4];
        float origin[4];
    };

    ComPtr<ID3D11VertexShader> g_vs;
    ComPtr<ID3D11PixelShader> g_ps;
    ComPtr<ID3D11RasterizerState> g_raster;
    ComPtr<ID3D11Buffer> g_constants;

    ComPtr<ID3D11Texture2D> g_target;
    ComPtr<ID3D11RenderTargetView> g_rtv;
    ComPtr<ID3D11Texture2D> g_staging;
    uint32_t g_targetWidth = 0;
    uint32_t g_targetHeight = 0;
    // The reduce target is shared so the layer's D3D12 motion pass can read it. The handle names the
    // texture rather than a copy of it, so it is created once with the target and closed when the
    // target is rebuilt or dropped.
    HANDLE g_targetHandle = nullptr;

    // Borrowed: the capture holds the reference to the application's texture, and this only uses it
    // to notice when a different one is handed over.
    ID3D11Texture2D* g_depth = nullptr;
    DXGI_FORMAT g_depthFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11ShaderResourceView> g_srv;

    bool g_setupFailed = false;
    bool g_pending = false;

    // The mapped contents, copied out rather than left mapped: the caller keeps the pointer while it
    // summarises, and a staging texture held mapped across frames would block the next reduction.
    std::vector<uint8_t> g_readback;
    uint32_t g_readbackPitch = 0;

    bool BuildShaders(ID3D11Device* device) {
        const auto compile = GetD3DCompile();
        if (compile == nullptr) {
            LayerLog("depth msaa: d3dcompiler_47.dll is unavailable, the multisampled depth stays unread\n");
            return false;
        }

        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> pixelBlob;
        ComPtr<ID3DBlob> error;

        const auto build = [&](const char* source, const char* entry, const char* target, ID3DBlob** blob) {
            error.Reset();
            const HRESULT result = compile(source,
                                           std::strlen(source),
                                           "AmdnrMsaaDepth",
                                           nullptr,
                                           nullptr,
                                           entry,
                                           target,
                                           0,
                                           0,
                                           blob,
                                           error.ReleaseAndGetAddressOf());
            if (FAILED(result)) {
                LayerLog("depth msaa: %s failed: %s\n",
                         entry,
                         error ? static_cast<const char*>(error->GetBufferPointer()) : "no message");
                return false;
            }
            return true;
        };

        if (!build(amdnr_fsr::kVertexShader, "main_vs", "vs_5_0", vertexBlob.ReleaseAndGetAddressOf()) ||
            !build(kReduceShader, "main_ps", "ps_5_0", pixelBlob.ReleaseAndGetAddressOf())) {
            return false;
        }

        if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                              vertexBlob->GetBufferSize(),
                                              nullptr,
                                              g_vs.ReleaseAndGetAddressOf())) ||
            FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                             pixelBlob->GetBufferSize(),
                                             nullptr,
                                             g_ps.ReleaseAndGetAddressOf()))) {
            LayerLog("depth msaa: the reduce shaders were compiled but not accepted by the device\n");
            return false;
        }

        // The triangle has to survive whatever the rasterizer would otherwise do with it, so the cull
        // mode is set here rather than left to the default.
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&raster, g_raster.ReleaseAndGetAddressOf()))) {
            LayerLog("depth msaa: could not create the rasterizer state\n");
            return false;
        }

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(ReduceConstants);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&buffer, nullptr, g_constants.ReleaseAndGetAddressOf()))) {
            LayerLog("depth msaa: could not create the constant buffer\n");
            return false;
        }
        return true;
    }

    bool BuildTargets(ID3D11Device* device, uint32_t width, uint32_t height) {
        g_target.Reset();
        g_rtv.Reset();
        g_staging.Reset();
        if (g_targetHandle != nullptr) {
            CloseHandle(g_targetHandle);
            g_targetHandle = nullptr;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // The shader resource flag is not for this side -- nothing here samples the result -- but it
        // is what lets the same texture be read as a shader resource on the D3D12 side of the sharing.
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // Sharing is what lets the layer's D3D12 motion pass read this. It is not needed for the
        // reduction itself, so a driver that refuses the combination of a render target with the share
        // flags still gets its target -- only the cross-API read is lost, and that is reported below
        // the same way a failed handle creation is.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, g_target.ReleaseAndGetAddressOf()))) {
            desc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, g_target.ReleaseAndGetAddressOf()))) {
                LayerLog("depth msaa: could not create the %ux%u reduce target\n", width, height);
                return false;
            }
        }
        if (FAILED(device->CreateRenderTargetView(g_target.Get(), nullptr, g_rtv.ReleaseAndGetAddressOf()))) {
            LayerLog("depth msaa: could not create the reduce target's view\n");
            return false;
        }

        // A failure here costs only the cross-API read: the reduce and its CPU readback still work.
        ComPtr<IDXGIResource1> shared;
        if (desc.MiscFlags == 0 || FAILED(g_target.As(&shared)) ||
            FAILED(shared->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_targetHandle))) {
            g_targetHandle = nullptr;
            LayerLog("depth msaa: the %ux%u reduce target could not be made shareable, so the layer's "
                     "motion pass cannot read it\n",
                     width,
                     height);
        }

        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        // A staging texture may not be shared, so the flags the target above was given have to go: a
        // resource that keeps them is refused outright, and the failure would take the whole reduction
        // down with it rather than just the readback.
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, g_staging.ReleaseAndGetAddressOf()))) {
            LayerLog("depth msaa: could not create the %ux%u staging texture\n", width, height);
            return false;
        }

        g_targetWidth = width;
        g_targetHeight = height;
        return true;
    }

    bool BuildView(ID3D11Device* device, ID3D11Texture2D* depth, DXGI_FORMAT viewFormat) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = ReadFormat(viewFormat);
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        const HRESULT result = device->CreateShaderResourceView(depth, &desc, g_srv.ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            D3D11_TEXTURE2D_DESC texture{};
            depth->GetDesc(&texture);
            LayerLog("depth msaa: the %ux%u depth (fmt=%u, %u samples) cannot be opened as a multisampled "
                     "shader resource (0x%08lX); the reduce stays off\n",
                     texture.Width,
                     texture.Height,
                     (unsigned)texture.Format,
                     texture.SampleDesc.Count,
                     (unsigned long)result);
            return false;
        }
        return true;
    }

} // namespace

bool MsaaDepthReadIssue(ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11Texture2D* depth,
                        DXGI_FORMAT viewFormat,
                        uint32_t offsetX,
                        uint32_t offsetY,
                        uint32_t width,
                        uint32_t height,
                        bool linearise,
                        float nearPlane,
                        float farPlane,
                        bool readback) {
    if (g_setupFailed || device == nullptr || context == nullptr || depth == nullptr || width == 0 ||
        height == 0) {
        return false;
    }

    if (g_vs == nullptr && !BuildShaders(device)) {
        g_setupFailed = true;
        return false;
    }
    if ((g_target == nullptr || g_targetWidth != width || g_targetHeight != height) &&
        !BuildTargets(device, width, height)) {
        g_setupFailed = true;
        return false;
    }
    if (g_srv == nullptr || depth != g_depth || viewFormat != g_depthFormat) {
        g_srv.Reset();
        if (!BuildView(device, depth, viewFormat)) {
            g_setupFailed = true;
            return false;
        }
        g_depth = depth;
        g_depthFormat = viewFormat;
    }

    // The rectangle of source texels this reduction reads, and how many of them one output texel
    // covers. It is the caller's rectangle rather than the whole texture: this title draws its eye
    // buffer into one corner of a larger pooled depth, and that corner is smaller than the
    // allocation in both directions -- measured at 4781 x 1125 inside 6774 x 2718. Scaling the whole
    // allocation across the output would put a real distance under the wrong pixel and leave a band of
    // cleared depth along two edges -- which is what the picture showed before this was a crop.
    D3D11_TEXTURE2D_DESC sourceDesc{};
    depth->GetDesc(&sourceDesc);
    if (offsetX >= sourceDesc.Width || offsetY >= sourceDesc.Height) {
        return false;
    }
    const uint32_t availableWidth = sourceDesc.Width - offsetX;
    const uint32_t availableHeight = sourceDesc.Height - offsetY;
    const uint32_t regionWidth = width < availableWidth ? width : availableWidth;
    const uint32_t regionHeight = height < availableHeight ? height : availableHeight;
    if (regionWidth == 0 || regionHeight == 0) {
        return false;
    }

    ReduceConstants constants{};
    constants.range[0] = nearPlane;
    constants.range[1] = farPlane;
    constants.range[2] = linearise ? 1.f : 0.f;
    constants.blockStep[0] = (float)regionWidth;
    constants.blockStep[1] = (float)regionHeight;
    constants.blockStep[2] = (float)regionWidth / (float)width;
    constants.blockStep[3] = (float)regionHeight / (float)height;
    constants.origin[0] = (float)offsetX;
    constants.origin[1] = (float)offsetY;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(g_constants.Get(), 0);

    ID3D11RenderTargetView* renderTarget = g_rtv.Get();
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    const D3D11_VIEWPORT viewport{0.f, 0.f, (float)width, (float)height, 0.f, 1.f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_raster.Get());
    context->IASetInputLayout(nullptr);
    // The pass owns its topology: whatever the application left bound would silently emit nothing.
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_vs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* view = g_srv.Get();
    context->PSSetShaderResources(0, 1, &view);
    ID3D11Buffer* constantBuffer = g_constants.Get();
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    context->PSSetShader(g_ps.Get(), nullptr, 0);
    context->Draw(3, 0);
    // Unbind the depth again: the application binds the same texture as its own views next frame, and
    // a stale read view on it would make that a conflict rather than a bind.
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    // The result is left in the target either way. Only the copy back to the host is optional, so a
    // frame that only feeds the GPU consumer skips it.
    if (readback) {
        context->CopyResource(g_staging.Get(), g_target.Get());
        g_pending = true;
    }
    return true;
}

HANDLE MsaaDepthReadTargetHandle(uint32_t* outWidth, uint32_t* outHeight) {
    if (outWidth != nullptr) {
        *outWidth = g_targetWidth;
    }
    if (outHeight != nullptr) {
        *outHeight = g_targetHeight;
    }
    return g_targetHandle;
}

ID3D11Texture2D* MsaaDepthReadTexture(uint32_t* outWidth, uint32_t* outHeight) {
    if (outWidth != nullptr) {
        *outWidth = g_targetWidth;
    }
    if (outHeight != nullptr) {
        *outHeight = g_targetHeight;
    }
    return g_target.Get();
}

bool MsaaDepthReadTryMap(const uint8_t** outData,
                         uint32_t* outRowPitch,
                         uint32_t* outWidth,
                         uint32_t* outHeight) {
    if (!g_pending || g_staging == nullptr) {
        return false;
    }

    ComPtr<ID3D11Device> device;
    g_staging->GetDevice(&device);
    if (device == nullptr) {
        g_pending = false;
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        g_pending = false;
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT result = context->Map(g_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return false; // not ready: the caller tries again on a later frame
    }
    if (FAILED(result)) {
        g_pending = false;
        LayerLog("depth msaa: the reduce staging map failed (0x%08lX)\n", (unsigned long)result);
        return false;
    }

    const size_t rowBytes = (size_t)g_targetWidth * sizeof(float);
    if (g_readback.size() < rowBytes * g_targetHeight) {
        g_readback.resize(rowBytes * g_targetHeight);
    }
    for (uint32_t y = 0; y < g_targetHeight; y++) {
        std::memcpy(g_readback.data() + (size_t)y * rowBytes,
                    (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
                    rowBytes);
    }
    context->Unmap(g_staging.Get(), 0);
    g_pending = false;

    g_readbackPitch = (uint32_t)rowBytes;
    *outData = g_readback.data();
    *outRowPitch = g_readbackPitch;
    *outWidth = g_targetWidth;
    *outHeight = g_targetHeight;
    return true;
}

void MsaaDepthReadRelease() {
    g_srv.Reset();
    g_depth = nullptr;
    g_depthFormat = DXGI_FORMAT_UNKNOWN;
    g_pending = false;
    // The target and its handle go with it: the layer's D3D12 device is torn down around the same
    // time, and a handle left open would keep the texture alive past the device that made it.
    g_target.Reset();
    g_rtv.Reset();
    g_staging.Reset();
    if (g_targetHandle != nullptr) {
        CloseHandle(g_targetHandle);
        g_targetHandle = nullptr;
    }
    g_targetWidth = 0;
    g_targetHeight = 0;
}
