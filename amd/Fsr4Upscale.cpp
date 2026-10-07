// FidelityFX FSR 4 as the layer's output upscale. See Fsr4Upscale.h for why it lives on the
// D3D12 device rather than in the D3D11 pass it replaces.

#define _WINDOWS 1

#include "Fsr4Upscale.h"

#include <d3d11_4.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "AmdnrDepthCapture.h"
#include "LayerLog.h"

// The SDK is vendored with its own layout and these headers find each other by relative path, so
// they are included the same way they include themselves.
#include "sdk/api/include/ffx_api.h"
#include "sdk/api/include/ffx_api_loader.h"
#include "sdk/api/include/ffx_api_types.h"
#include "sdk/api/include/dx12/ffx_api_dx12.h"
#include "sdk/upscalers/include/ffx_upscale.h"

using Microsoft::WRL::ComPtr;

namespace {

    const wchar_t* const kLoaderName = L"amd_fidelityfx_loader_dx12.dll";

    // The camera the layer tells the effect it is looking through. The temporal half of the effect
    // reads these to turn the depth it is handed into a distance, and nothing in an OpenXR layer is
    // told what the application projected with -- so they come from the same environment names the
    // depth capture reads for its own planes, which is the one place to set them and the only way the
    // value the reduction used and the value the effect is told are the same one.
    constexpr float kCameraNear = 0.01f;
    constexpr float kCameraFar = 1000.0f;
    constexpr float kCameraFovVertical = 1.0f;
    constexpr float kViewSpaceToMeters = 1.0f;
    // The effect wants a frame time to scale its motion by. The motion is already a displacement in
    // pixels and not a velocity, so this only has to be a plausible frame.
    constexpr float kFrameTimeMs = 16.6f;

    // Whether the depth capture was told to linearise. It writes inverse hardware depth by default and
    // a distance in [0,1] when it is, and the two need opposite ends of the effect's depth convention
    // -- so this is read rather than assumed, and a change to one side without the other would put the
    // occlusion test the wrong way round.
    bool DepthLinearised() {
        char text[32]{};
        if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_LINEAR", text, (DWORD)sizeof(text)) == 0) {
            return false;
        }
        return std::strtol(text, nullptr, 10) != 0;
    }

    float DepthPlane(const char* name, float fallback) {
        char text[32]{};
        if (GetEnvironmentVariableA(name, text, (DWORD)sizeof(text)) == 0) {
            return fallback;
        }
        const float value = (float)atof(text);
        return value > 0.f ? value : fallback;
    }

    // Whatever the runtime has to say, said into the layer's log. Without a registered callback the
    // runtime reports a rejected dispatch as a bare return code and the reason is lost.
    void MessageCallback(uint32_t type, const wchar_t* message) {
        LayerLog("Fsr4: ffx %s: %ls\n",
                 type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
                 message != nullptr ? message : L"(no message)");
    }

    // One texture on the layer's D3D12 device. DEFAULT heap, so the effect can read it as a shader
    // resource, and COMMON state, which is what the dispatch description says these are in.
    ComPtr<ID3D12Resource> MakeTexture12(ID3D12Device* device,
                                         DXGI_FORMAT format,
                                         uint32_t width,
                                         uint32_t height) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> resource;
        if (FAILED(device->CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &desc,
                                                   D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr,
                                                   IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())))) {
            return nullptr;
        }
        return resource;
    }

    void Transition(ID3D12GraphicsCommandList* cmd,
                    ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
        if (before == after || resource == nullptr) {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &barrier);
    }

} // namespace

struct Fsr4Upscale::Impl {
    // The application's device: the two copies either side of the dispatch are recorded on it,
    // because the textures they move belong to it.
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11DeviceContext4> context4;

    // The layer's neural device: the dispatch is recorded on it.
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;

    // A shared fence, one handle on each device. The copy in and the copy out are recorded on the
    // application's context and the dispatch is recorded on the layer's queue, so the two have to be
    // ordered against each other or the dispatch reads a texture the copy has not finished writing.
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};

    HMODULE module{nullptr};
    ffxFunctions ffx{};
    ffxContext context{nullptr};

    // The pair the effect reads and writes: the view's pixels go in exactly as the application left
    // them, and the upscaled answer comes back out.
    ComPtr<ID3D11Texture2D> colour11;
    ComPtr<ID3D12Resource> colour12;
    ComPtr<ID3D11Texture2D> output11;
    ComPtr<ID3D12Resource> output12;

    // The zero field the effect is handed on a view the layer could not reproject: displacements as a
    // fraction of the view, so all zero reads as "nothing moved".
    ComPtr<ID3D12Resource> motion12;
    // The layer's own field for this view, opened across from the handle its build published. Held
    // rather than reopened: the handle names one texture and only changes when its size does.
    ComPtr<ID3D12Resource> fieldSource;
    HANDLE fieldHandle{nullptr};
    uint32_t fieldWidth{0};
    uint32_t fieldHeight{0};

    // The layer's reduced depth, opened across from the application's device on the handle the depth
    // capture publishes. Held rather than reopened: the handle names one texture and only changes when
    // the capture rebuilds it, and this pass reads it through a shader resource view that has to be
    // remade whenever it does.
    ComPtr<ID3D12Resource> depthSource;
    HANDLE depthHandle{nullptr};
    uint32_t depthSourceWidth{0};
    uint32_t depthSourceHeight{0};

    // The depth this effect is handed: the view's rectangle of that reduction, at render size. Zero
    // everywhere until a reduction exists, which reads as "everything is at the far plane" -- the
    // honest answer for a pass that has not been given a distance.
    ComPtr<ID3D12Resource> depth12;
    ComPtr<ID3D12Resource> exposure12;

    uint32_t renderWidth{0};
    uint32_t renderHeight{0};
    uint32_t upscaleWidth{0};
    uint32_t upscaleHeight{0};

    // The plane pair the effect linearises the depth with, read once when the geometry is built.
    float cameraNear{kCameraNear};
    float cameraFar{kCameraFar};

    uint32_t frames{0};
    bool loggedFirst{false};
    // The motion field is not there on the very first frame -- the layer has no previous pose to
    // reproject against yet -- so the summary above is logged on a frame that cannot show it. This
    // reports the first frame that can, once.
    bool loggedMotion{false};
    // Whether the next dispatch has to tell the effect the camera moved discontinuously. Raised by a
    // fresh geometry -- a size the effect has never seen has no history to keep -- and cleared once
    // it has been said, because every frame after that is a continuation of the same camera.
    bool resetNext{true};
    const char* lastError{""};

    bool Fail(const char* what) {
        lastError = what;
        LayerLog("Fsr4: %s\n", what);
        return false;
    }

    bool FailHr(const char* what, HRESULT hr) {
        LayerLog("Fsr4: %s (hr 0x%08lX)\n", what, (unsigned long)hr);
        lastError = what;
        return false;
    }

    // Closes and submits the recording, and signals the queue. No CPU wait: the per-frame path is
    // ordered against the application's context through the shared fence instead, and waiting here
    // would put the whole dispatch back on the application's render thread.
    bool Submit() {
        const HRESULT closed = list->Close();
        if (FAILED(closed)) {
            return FailHr("the command list would not close", closed);
        }

        ID3D12CommandList* lists[]{list.Get()};
        queue12->ExecuteCommandLists(1, lists);

        const HRESULT signalled = queue12->Signal(fence12.Get(), ++fenceValue);
        if (FAILED(signalled)) {
            return FailHr("the queue would not signal", signalled);
        }
        return true;
    }

    // Waits on the CPU for everything submitted so far. Only the one-off setup work uses this; it is
    // what makes the constant textures readable by the time the first frame is dispatched.
    bool Drain() {
        if (fence12->GetCompletedValue() < fenceValue) {
            const HRESULT armed = fence12->SetEventOnCompletion(fenceValue, fenceEvent);
            if (FAILED(armed)) {
                return FailHr("the fence would not report completion", armed);
            }
            if (WaitForSingleObject(fenceEvent, 5000) != WAIT_OBJECT_0) {
                return Fail("waiting for the layer's queue timed out");
            }
        }
        return true;
    }

    bool BeginRecording() {
        const HRESULT resetAllocator = allocator->Reset();
        if (FAILED(resetAllocator)) {
            return FailHr("the command allocator would not reset", resetAllocator);
        }
        const HRESULT resetList = list->Reset(allocator.Get(), nullptr);
        if (FAILED(resetList)) {
            return FailHr("the command list would not reset", resetList);
        }
        return true;
    }

    // Writes `rowBytes` worth of `row` into the start of every row of `resource`, once, at setup.
    // Deterministic content matters here: a texture that was never written reads as whatever the
    // allocation held, and the effect reads all three of these on every frame.
    bool Fill(ID3D12Resource* resource, const void* row, uint32_t rowBytes) {
        D3D12_RESOURCE_DESC desc = resource->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 total = 0;
        device12->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowSize, &total);
        if (rows == 0 || rowSize < rowBytes) {
            return Fail("a constant texture has no room for its row");
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.Format = DXGI_FORMAT_UNKNOWN;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> staging;
        HRESULT hr = device12->CreateCommittedResource(&heap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &buffer,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(staging.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return FailHr("the staging buffer for a constant texture", hr);
        }

        uint8_t* mapped = nullptr;
        hr = staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) {
            return FailHr("the staging buffer would not map", hr);
        }
        for (UINT64 y = 0; y < rows; y++) {
            std::memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch, row, rowBytes);
        }
        staging->Unmap(0, nullptr);

        if (!BeginRecording()) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = staging.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = resource;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = 0;

        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);

        if (!Submit()) {
            return false;
        }
        // The staging buffer goes out of scope here, so the copy has to have run before this returns.
        return Drain();
    }

    // The shareable pair every cross-device texture here is made of: created on the application's
    // device, opened on the layer's, and written by neither until a copy says so.
    bool CreateShared(uint32_t width,
                      uint32_t height,
                      ComPtr<ID3D11Texture2D>& shared11,
                      ComPtr<ID3D12Resource>& shared12) {
        shared11.Reset();
        shared12.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // Plain UNORM, not sRGB. The application's pixels arrive display-encoded and the effect is
        // told so explicitly; letting a typed view decode them here as well would decode twice.
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // Both bindings are declared because the same description serves both directions: the one
        // the effect reads has to be a shader resource and the one it writes has to be a UAV, and
        // D3D12 will not make a view the D3D11 side never declared.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        HRESULT hr = device11->CreateTexture2D(&desc, nullptr, shared11.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            return FailHr("a shared texture could not be created", hr);
        }

        ComPtr<IDXGIResource1> dxgiResource;
        hr = shared11->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            return FailHr("a shared texture has no IDXGIResource1", hr);
        }

        HANDLE handle = nullptr;
        hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            return FailHr("a shared texture would not give up its handle", hr);
        }

        hr = device12->OpenSharedHandle(handle, IID_PPV_ARGS(shared12.ReleaseAndGetAddressOf()));
        CloseHandle(handle);
        if (FAILED(hr)) {
            return FailHr("the layer's device could not open a shared texture", hr);
        }
        return true;
    }

    // What the runtime offers on this device, and what the request was for. Asked before a context
    // exists, which is the only moment the answer describes the machine rather than a choice already
    // made. A runtime that offers 4.1.1 and settles on 3.1.5 has decided something about the device,
    // and that is a different fact from a runtime that never offered it.
    void ReportVersions() {
        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        query.device = device12.Get();

        uint64_t count = 0;
        query.outputCount = &count;
        query.versionIds = nullptr;
        query.versionNames = nullptr;
        if (ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
            LayerLog("Fsr4: the runtime would not say what versions it offers\n");
            return;
        }
        if (count == 0) {
            LayerLog("Fsr4: the runtime offers the upscale effect no versions\n");
            return;
        }

        std::vector<uint64_t> ids(count, 0);
        std::vector<const char*> names(count, nullptr);
        uint64_t filled = count;
        query.outputCount = &filled;
        query.versionIds = ids.data();
        query.versionNames = names.data();
        if (ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
            LayerLog("Fsr4: %llu versions offered but not readable\n", (unsigned long long)count);
            return;
        }
        for (uint64_t i = 0; i < filled; i++) {
            LayerLog("Fsr4: offered [%llu] id 0x%llX  %s\n",
                     (unsigned long long)i,
                     (unsigned long long)ids[i],
                     names[i] != nullptr ? names[i] : "(unnamed)");
        }
        LayerLog("Fsr4: the request is version 0x%X\n", (unsigned)FFX_UPSCALER_VERSION);
    }

    // What the adapter under this device calls itself. FSR 4 is a machine-learning upscaler that
    // only runs on one vendor's hardware, so a runtime that decides which version to hand out has to
    // be asking the adapter what it is -- and this process has a graphics proxy in it that is known
    // to rewrite exactly that answer. Both forms of the query are asked, because they are separate
    // entry points and a proxy that hooks one of them leaves the other telling the truth.
    void ReportAdapter() {
        ComPtr<IDXGIDevice> dxgiDevice;
        // The D3D12 device first, and the application's D3D11 device after it. A device this process
        // cannot get an IDXGIDevice out of is not a plain D3D12 device, and knowing which of the two
        // answers says whether the graphics proxy in this process wraps one runtime or both.
        if (FAILED(device12->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())))) {
            LayerLog("Fsr4: the D3D12 device has no IDXGIDevice\n");
            if (FAILED(device11->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())))) {
                LayerLog("Fsr4: the D3D11 device has no IDXGIDevice either\n");
                return;
            }
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf()))) {
            LayerLog("Fsr4: the device has no adapter\n");
            return;
        }

        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            LayerLog("Fsr4: adapter  vendor 0x%04X device 0x%04X  %ls\n",
                     desc.VendorId,
                     desc.DeviceId,
                     desc.Description);
        }
        ComPtr<IDXGIAdapter1> adapter1;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(adapter1.ReleaseAndGetAddressOf())))) {
            DXGI_ADAPTER_DESC1 desc1{};
            if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
                LayerLog("Fsr4: adapter1 vendor 0x%04X device 0x%04X  %ls\n",
                         desc1.VendorId,
                         desc1.DeviceId,
                         desc1.Description);
            }
        }
    }

    // What the device says it can do, alongside the versions above. FSR 4's kernels are built for a
    // shader model the earlier upscalers do not need, so a device reporting less than that is the
    // first thing to look at when a request for 4.1.1 comes back as 3.1.5.
    void ReportDevice() {
        D3D12_FEATURE_DATA_SHADER_MODEL model{};
        model.HighestShaderModel = D3D_SHADER_MODEL_6_8;
        const HRESULT asked = device12->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL, &model, (UINT)sizeof(model));
        if (SUCCEEDED(asked)) {
            LayerLog("Fsr4: the device reports shader model 0x%X\n", (unsigned)model.HighestShaderModel);
        } else {
            LayerLog("Fsr4: the device would not report a shader model (hr 0x%08lX)\n", (unsigned long)asked);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        if (SUCCEEDED(device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, (UINT)sizeof(options)))) {
            LayerLog("Fsr4: wave ops %s, wave lane count %u..%u\n",
                     options.WaveOps ? "yes" : "no",
                     (unsigned)options.WaveLaneCountMin,
                     (unsigned)options.WaveLaneCountMax);
        }
    }

    void ReleaseGeometry() {
        if (context != nullptr) {
            ffx.DestroyContext(&context, nullptr);
            context = nullptr;
        }
        colour11.Reset();
        colour12.Reset();
        output11.Reset();
        output12.Reset();
        depth12.Reset();
        motion12.Reset();
        exposure12.Reset();
        // The reduction is reopened on the next frame: a rebuilt geometry is a new size, and the
        // reduction that was open described the old one.
        depthSource.Reset();
        depthHandle = nullptr;
        depthSourceWidth = 0;
        depthSourceHeight = 0;
        // The layer's field is reopened on the next frame too: a rebuilt geometry is a new size, and
        // the field that was open was built for the old one.
        fieldSource.Reset();
        fieldHandle = nullptr;
        fieldWidth = 0;
        fieldHeight = 0;
        loggedMotion = false;
        renderWidth = 0;
        renderHeight = 0;
        upscaleWidth = 0;
        upscaleHeight = 0;
    }

    // Everything sized from the frame, rebuilt when the frame changes size. The effect's maximum
    // sizes are fixed when its context is created, so a change of size means a new context as well.
    bool EnsureGeometry(uint32_t renderW, uint32_t renderH, uint32_t upscaleW, uint32_t upscaleH) {
        if (context != nullptr && renderW == renderWidth && renderH == renderHeight &&
            upscaleW == upscaleWidth && upscaleH == upscaleHeight) {
            return true;
        }

        ReleaseGeometry();
        // A new context has no history, so the first frame dispatched through it has to say so.
        resetNext = true;
        renderWidth = renderW;
        renderHeight = renderH;
        upscaleWidth = upscaleW;
        upscaleHeight = upscaleH;

        if (!CreateShared(renderW, renderH, colour11, colour12) ||
            !CreateShared(upscaleW, upscaleH, output11, output12)) {
            return false;
        }

        depth12 = MakeTexture12(device12.Get(), DXGI_FORMAT_R32_FLOAT, renderW, renderH);
        // The zero field the effect is handed when the layer has none for this view: two halves per
        // texel, not two floats. The layer builds its own field at this same width, so the precision
        // is the layer's to spend, and the reason it is a fraction of the view rather than pixels is
        // that a half holds about three decimal digits -- a whole pixel once the value is a few
        // thousand of them, and comfortably sub-pixel when it is a fraction of the view.
        motion12 = MakeTexture12(device12.Get(), DXGI_FORMAT_R16G16_FLOAT, renderW, renderH);
        exposure12 = MakeTexture12(device12.Get(), DXGI_FORMAT_R32_FLOAT, 1, 1);
        if (depth12 == nullptr || motion12 == nullptr || exposure12 == nullptr) {
            return Fail("the frame's inputs could not be created");
        }

        // The far plane everywhere, in the inverted convention the depth capture writes: zero is a
        // surface at infinity, which is the honest answer for a frame whose depth has not been
        // captured yet. The copy below overwrites this the moment a reduction exists.
        std::vector<float> depthRow(renderW, 0.0f);
        // Nothing moved in a frame the layer has no motion for, so every texel points at itself.
        std::vector<uint16_t> motionRow((size_t)renderW * 2, 0);
        const float exposure = 1.0f;

        if (!Fill(depth12.Get(), depthRow.data(), renderW * (uint32_t)sizeof(float)) ||
            !Fill(motion12.Get(), motionRow.data(), renderW * 2u * (uint32_t)sizeof(uint16_t)) ||
            !Fill(exposure12.Get(), &exposure, (uint32_t)sizeof(exposure))) {
            return false;
        }

        cameraNear = DepthPlane("AMDNR_XR_DEPTH_NEAR", kCameraNear);
        cameraFar = DepthPlane("AMDNR_XR_DEPTH_FAR", kCameraFar);

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device12.Get();

        ffxCreateContextDescUpscaleVersion apiVersion{};
        apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        apiVersion.header.pNext = &backend.header;
        apiVersion.version = FFX_UPSCALER_VERSION;

        ffxCreateContextDescUpscale upscale{};
        upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        upscale.header.pNext = &apiVersion.header;
        // Display-encoded colour, which is what an OpenXR eye buffer holds. The per-frame dispatch
        // says the same thing again through its own flag.
        //
        // The depth the layer captures is inverse distance -- one at the near plane, zero at the far
        // one -- which is the effect's inverted convention. A capture told to linearise writes the
        // other way round instead, so the flag follows it rather than being assumed.
        upscale.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        if (!DepthLinearised()) {
            upscale.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
        }
        upscale.maxRenderSize = FfxApiDimensions2D{renderW, renderH};
        upscale.maxUpscaleSize = FfxApiDimensions2D{upscaleW, upscaleH};
        upscale.fpMessage = MessageCallback;

        ReportAdapter();
        ReportDevice();
        ReportVersions();

        const ffxReturnCode_t created = ffx.CreateContext(&context, &upscale.header, nullptr);
        if (created != FFX_API_RETURN_OK || context == nullptr) {
            LayerLog("Fsr4: ffxCreateContext returned %u\n", created);
            lastError = "the effect has no context";
            return false;
        }

        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx.Query(&context, &provider.header) == FFX_API_RETURN_OK) {
            LayerLog("Fsr4: provider %s (id 0x%llX), %ux%u -> %ux%u\n",
                     provider.versionName != nullptr ? provider.versionName : "(unnamed)",
                     (unsigned long long)provider.versionId,
                     renderW,
                     renderH,
                     upscaleW,
                     upscaleH);
        } else {
            LayerLog("Fsr4: provider version unknown, %ux%u -> %ux%u\n", renderW, renderH, upscaleW, upscaleH);
        }
        return true;
    }
};

Fsr4Upscale::~Fsr4Upscale() {
    Shutdown();
}

void Fsr4Upscale::Shutdown() {
    if (m_impl == nullptr) {
        return;
    }
    if (m_impl->context != nullptr) {
        m_impl->ffx.DestroyContext(&m_impl->context, nullptr);
    }
    if (m_impl->fenceEvent != nullptr) {
        CloseHandle(m_impl->fenceEvent);
        m_impl->fenceEvent = nullptr;
    }
    delete m_impl;
    m_impl = nullptr;
    m_ready = false;
}

bool Fsr4Upscale::Initialize(ID3D11Device* device11,
                             ID3D11DeviceContext* context11,
                             ID3D12Device* device12,
                             ID3D12CommandQueue* queue12) {
    if (m_impl != nullptr) {
        return m_ready;
    }

    Impl* impl = new Impl();
    impl->device11 = device11;
    impl->context11 = context11;
    impl->device12 = device12;
    impl->queue12 = queue12;
    m_impl = impl;

    // The one failure path this function has: say why, leave nothing half built, and let the caller
    // keep its own upscale. A machine without the signed runtime is a normal machine.
    const auto stop = [this, impl](const char* what) {
        impl->Fail(what);
        m_lastError = what;
        Shutdown();
        return false;
    };

    if (FAILED(context11->QueryInterface(IID_PPV_ARGS(impl->context4.ReleaseAndGetAddressOf())))) {
        return stop("the application's context is not an ID3D11DeviceContext4");
    }

    if (FAILED(device11->QueryInterface(IID_PPV_ARGS(impl->device5.ReleaseAndGetAddressOf())))) {
        return stop("the application's device is not an ID3D11Device5");
    }

    if (FAILED(device12->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator.ReleaseAndGetAddressOf()))) ||
        FAILED(device12->CreateCommandList(0,
                                           D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           impl->allocator.Get(),
                                           nullptr,
                                           IID_PPV_ARGS(impl->list.ReleaseAndGetAddressOf())))) {
        return stop("the layer's device would not make a command list");
    }
    impl->list->Close();

    if (FAILED(device12->CreateFence(
            0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence12.ReleaseAndGetAddressOf())))) {
        return stop("the ordering fence could not be created");
    }

    HANDLE fenceHandle = nullptr;
    if (FAILED(device12->CreateSharedHandle(impl->fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle))) {
        return stop("the ordering fence would not give up its handle");
    }
    const HRESULT opened =
        impl->device5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(impl->fence11.ReleaseAndGetAddressOf()));
    CloseHandle(fenceHandle);
    if (FAILED(opened)) {
        return stop("the application's device could not open the ordering fence");
    }

    impl->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl->fenceEvent == nullptr) {
        return stop("the fence event could not be created");
    }

    // The runtime sits beside the layer's own DLLs, so the search path is set to the folder the
    // layer was loaded from before anything is loaded by name -- the loader and the upscaler beside
    // it then find each other the same way.
    const std::string directory = LayerDirectory();
    const std::wstring wide(directory.begin(), directory.end());
    if (!wide.empty()) {
        SetDllDirectoryW(wide.c_str());
    }
    const std::wstring path = wide + L"\\" + kLoaderName;
    impl->module = LoadLibraryW(path.c_str());
    if (impl->module == nullptr) {
        return stop("amd_fidelityfx_loader_dx12.dll is not installed next to the layer");
    }

    ffxLoadFunctions(&impl->ffx, impl->module);
    if (impl->ffx.CreateContext == nullptr || impl->ffx.Query == nullptr ||
        impl->ffx.Dispatch == nullptr || impl->ffx.DestroyContext == nullptr) {
        return stop("the runtime does not export the ffx api");
    }

    ffxConfigureDescGlobalDebug debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    debug.fpMessage = MessageCallback;
    // Errors only. The runtime is verbose enough at this level to explain a rejected dispatch, and
    // anything above it would put a line in the log for every frame.
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    impl->ffx.Configure(nullptr, &debug.header);

    m_ready = true;
    LayerLog("Fsr4: the runtime is loaded from %s\n", directory.c_str());
    return true;
}

bool Fsr4Upscale::Run(ID3D11Texture2D* source,
                      uint32_t sourceSlice,
                      const RECT& sourceRect,
                      ID3D11Texture2D* dest,
                      uint32_t destSlice,
                      float jitterX,
                      float jitterY,
                      HANDLE field,
                      uint32_t fieldWidth,
                      uint32_t fieldHeight) {
    // Only a context means the pass can run. A missing context is a geometry that has not been
    // built yet, and building it happens below.
    if (!m_ready || m_impl == nullptr) {
        return false;
    }
    Impl& impl = *m_impl;

    const uint32_t renderW = (uint32_t)(sourceRect.right - sourceRect.left);
    const uint32_t renderH = (uint32_t)(sourceRect.bottom - sourceRect.top);

    D3D11_TEXTURE2D_DESC destDesc{};
    dest->GetDesc(&destDesc);

    if (renderW == 0 || renderH == 0 || destDesc.Width == 0 || destDesc.Height == 0) {
        m_lastError = "the frame has no size";
        return impl.Fail(m_lastError);
    }

    if (!impl.EnsureGeometry(renderW, renderH, destDesc.Width, destDesc.Height)) {
        m_lastError = impl.lastError;
        return false;
    }

    // The view's pixels, out of wherever the application put them inside its own texture and into a
    // texture of exactly the render size. The effect takes whole textures -- it has no notion of
    // reading a rectangle -- and the eye image is two views packed side by side.
    const D3D11_BOX box{(UINT)sourceRect.left,
                        (UINT)sourceRect.top,
                        0,
                        (UINT)sourceRect.right,
                        (UINT)sourceRect.bottom,
                        1};
    impl.context11->CopySubresourceRegion(
        impl.colour11.Get(), 0, 0, 0, 0, source, sourceSlice, &box);

    // The field the layer built for this view on the application's own device, if it has one. Like
    // the depth below it is held rather than reopened: the handle names one texture and only changes
    // when its size does. A size that is not the render size describes a different grid, so it is
    // dropped rather than read as if it covered this view.
    if (field != impl.fieldHandle || fieldWidth != impl.fieldWidth || fieldHeight != impl.fieldHeight) {
        impl.fieldSource.Reset();
        impl.fieldHandle = field;
        impl.fieldWidth = fieldWidth;
        impl.fieldHeight = fieldHeight;
        if (field != nullptr && fieldWidth == renderW && fieldHeight == renderH &&
            FAILED(impl.device12->OpenSharedHandle(
                field, IID_PPV_ARGS(impl.fieldSource.ReleaseAndGetAddressOf())))) {
            impl.fieldSource.Reset();
        }
    }

    // The depth the layer captured this frame, if it has one. The handle names one texture and only
    // changes when the capture rebuilds it, so it is reopened then and held otherwise. Its size is the
    // packed eye image's -- the same texture the view was cropped from -- and a reduction of any other
    // size describes a different layout, so it is dropped rather than read under the wrong pixels.
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    const HANDLE depthHandle = DepthCaptureReduceHandle(&depthWidth, &depthHeight);
    if (depthHandle != impl.depthHandle) {
        impl.depthSource.Reset();
        impl.depthHandle = depthHandle;
        impl.depthSourceWidth = depthWidth;
        impl.depthSourceHeight = depthHeight;
        if (depthHandle != nullptr &&
            FAILED(impl.device12->OpenSharedHandle(
                depthHandle, IID_PPV_ARGS(impl.depthSource.ReleaseAndGetAddressOf())))) {
            impl.depthSource.Reset();
        }
    }
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    const bool depthUsable = impl.depthSource != nullptr &&
                             impl.depthSourceWidth == sourceDesc.Width &&
                             impl.depthSourceHeight == sourceDesc.Height;

    // Everything the application has recorded so far, that copy included, has to land before the
    // dispatch reads it. The depth reduction and the motion field are both recorded on the same
    // context, so they are covered by the same signal.
    const uint64_t copied = ++impl.fenceValue;
    if (FAILED(impl.context4->Signal(impl.fence11.Get(), copied))) {
        m_lastError = "the application's context would not signal";
        return impl.Fail(m_lastError);
    }
    if (FAILED(impl.queue12->Wait(impl.fence12.Get(), copied))) {
        m_lastError = "the layer's queue would not wait for the copy";
        return impl.Fail(m_lastError);
    }

    if (!impl.BeginRecording()) {
        m_lastError = impl.lastError;
        return false;
    }

    // The view's rectangle of that depth, into the render-sized texture the dispatch reads. It is the
    // same rectangle the colour came out of: the reduction is the eye image's own layout, so a view's
    // position in that layout is the position it crops its colour from. The effect is handed one view
    // and not the packed pair, which is why the copy is here rather than the whole texture.
    if (depthUsable) {
        D3D12_TEXTURE_COPY_LOCATION fromDepth{};
        fromDepth.pResource = impl.depthSource.Get();
        fromDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        fromDepth.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION toDepth{};
        toDepth.pResource = impl.depth12.Get();
        toDepth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        toDepth.SubresourceIndex = 0;

        const D3D12_BOX depthBox{(UINT)sourceRect.left,
                                 (UINT)sourceRect.top,
                                 0,
                                 (UINT)sourceRect.right,
                                 (UINT)sourceRect.bottom,
                                 1};

        Transition(impl.list.Get(), impl.depthSource.Get(),
                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(impl.list.Get(), impl.depth12.Get(),
                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        impl.list->CopyTextureRegion(&toDepth, 0, 0, 0, &fromDepth, &depthBox);
        Transition(impl.list.Get(), impl.depth12.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Transition(impl.list.Get(), impl.depthSource.Get(),
                   D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // The field the dispatch reprojects with. The layer's own field when it has one for this view, and
    // the zero field otherwise -- which reads as "nothing moved" and is the honest answer for a view
    // the layer could not reproject, and for the first frame, which has no previous pose to use.
    ID3D12Resource* const fieldResource =
        impl.fieldSource != nullptr ? impl.fieldSource.Get() : impl.motion12.Get();

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = impl.list.Get();

    // Every resource is described as it actually is. The state is the one the texture really sits
    // in -- everything here is written by a copy and left in COMMON -- because the backend inserts
    // the barriers it believes are missing from what it is told, and a state that is wrong is a
    // barrier that is never inserted.
    dispatch.color = ffxApiGetResourceDX12(impl.colour12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    dispatch.depth = ffxApiGetResourceDX12(impl.depth12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    // The field crossed devices rather than being written here. It arrives in COMMON, which is where a
    // resource opened from a shared handle starts and where the backend reads its inputs.
    dispatch.motionVectors = ffxApiGetResourceDX12(fieldResource, FFX_API_RESOURCE_STATE_COMMON);
    dispatch.exposure = ffxApiGetResourceDX12(impl.exposure12.Get(), FFX_API_RESOURCE_STATE_COMMON);
    // The one resource written through a UAV, and the usage has to say so.
    dispatch.output = ffxApiGetResourceDX12(
        impl.output12.Get(), FFX_API_RESOURCE_STATE_COMMON, FFX_API_RESOURCE_USAGE_UAV);

    dispatch.renderSize = FfxApiDimensions2D{renderW, renderH};
    dispatch.upscaleSize = FfxApiDimensions2D{destDesc.Width, destDesc.Height};
    // The offset under the frustum the application rendered with, in render pixels. The effect is the
    // only thing in the frame that knows how to take it back out of the pixels again.
    dispatch.jitterOffset = FfxApiFloatCoords2D{jitterX, jitterY};
    // The field carries a displacement as a fraction of the view, and this turns it into the render
    // pixels the effect documents. It is the render size and not the upscale size: the motion is a
    // property of the eye buffer the application drew, not of the image this writes out.
    dispatch.motionVectorScale =
        FfxApiFloatCoords2D{(float)renderW, (float)renderH};
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = kFrameTimeMs;
    dispatch.preExposure = 1.0f;
    // Raised once, on the first frame of a geometry, and not on every frame: this is the effect being
    // told that the camera moved discontinuously, and a camera that does that every frame is a camera
    // the effect keeps no history for. The history is what the jitter above exists to fill.
    dispatch.reset = impl.resetNext;
    impl.resetNext = false;
    dispatch.cameraNear = impl.cameraNear;
    dispatch.cameraFar = impl.cameraFar;
    dispatch.cameraFovAngleVertical = kCameraFovVertical;
    dispatch.viewSpaceToMetersFactor = kViewSpaceToMeters;
    // The eye buffer holds perceptual sRGB, and the effect is told so rather than being handed the
    // linear values it would rather have.
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    const ffxReturnCode_t dispatched = impl.ffx.Dispatch(&impl.context, &dispatch.header);
    if (dispatched != FFX_API_RETURN_OK) {
        LayerLog("Fsr4: ffxDispatch returned %u\n", dispatched);
        // Closed but not submitted. It has to be closed again before its allocator can be reset,
        // and nothing about a rejected recording is worth executing.
        impl.list->Close();
        m_lastError = "the effect rejected the frame";
        return false;
    }

    if (!impl.Submit()) {
        m_lastError = impl.lastError;
        return false;
    }

    // Back on the application's device. The wait is on the application's own stream, so the copy
    // that follows is ordered behind the dispatch without the CPU ever waiting for it.
    if (FAILED(impl.context4->Wait(impl.fence11.Get(), impl.fenceValue))) {
        m_lastError = "the application's context would not wait for the dispatch";
        return impl.Fail(m_lastError);
    }

    impl.context11->CopySubresourceRegion(
        dest, destSlice, 0, 0, 0, impl.output11.Get(), 0, nullptr);

    if (impl.fieldSource != nullptr && !impl.loggedMotion) {
        impl.loggedMotion = true;
        LayerLog("Fsr4: the reprojection is live on %ux%u, depth %s\n",
                 renderW,
                 renderH,
                 depthUsable ? "captured" : "not captured");
    }

    impl.frames++;
    if (!impl.loggedFirst) {
        impl.loggedFirst = true;
        LayerLog("Fsr4: first frame, %ux%u at (%ld,%ld) -> %ux%u\n",
                 renderW,
                 renderH,
                 (long)sourceRect.left,
                 (long)sourceRect.top,
                 destDesc.Width,
                 destDesc.Height);
        D3D11_TEXTURE2D_DESC colourDesc{};
        impl.colour11->GetDesc(&colourDesc);
        LayerLog("Fsr4: dispatch colour %ux%u fmt %u, depth %s %ux%u, motion %s, "
                 "exposure 1x1 1.0, jitter (%.2f,%.2f), mv scale (%.2f,%.2f), reset %d, flags 0x%X, "
                 "near %.3f far %.1f fov %.3f, frame %.1f ms, preExposure %.2f\n",
                 dispatch.renderSize.width,
                 dispatch.renderSize.height,
                 (unsigned)colourDesc.Format,
                 depthUsable ? "captured" : "far/none",
                 dispatch.renderSize.width,
                 dispatch.renderSize.height,
                 impl.fieldSource != nullptr ? "from the layer" : "zero, none supplied",
                 dispatch.jitterOffset.x,
                 dispatch.jitterOffset.y,
                 dispatch.motionVectorScale.x,
                 dispatch.motionVectorScale.y,
                 dispatch.reset ? 1 : 0,
                 (unsigned)dispatch.flags,
                 dispatch.cameraNear,
                 dispatch.cameraFar,
                 dispatch.cameraFovAngleVertical,
                 dispatch.frameTimeDelta,
                 dispatch.preExposure);
    }
    return true;
}
