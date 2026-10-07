#include "EyeInterop.h"

#include "AmdnrDepthCapture.h"
#include "FsrShaders.h"
#include "NrSettings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

    double Ms(std::chrono::steady_clock::duration elapsed) {
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

    // d3dcompiler_47.dll is loaded by name, the same way the rest of the layer does it: a hard import
    // would stop the layer loading on a machine where the runtime ships its own copy elsewhere.
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

    // The compute stage the motion field is built on, saved before it is touched and put back after.
    // OpenXR gives a layer no context of its own, so this runs in the middle of the application's frame
    // on the application's context; compute is the one stage the layer's other passes never bind, which
    // makes it both the safest to borrow and the one whose leak nothing else here would ever clear.
    struct ComputeGuard {
        explicit ComputeGuard(ID3D11DeviceContext* context) : m_context(context) {
            if (m_context == nullptr) {
                return;
            }
            UINT instances = 0;
            m_context->CSGetShader(m_shader.GetAddressOf(), nullptr, &instances);
            m_context->CSGetConstantBuffers(0, 1, m_constants.GetAddressOf());
            m_context->CSGetShaderResources(0, 1, m_resources);
            m_context->CSGetUnorderedAccessViews(0, 1, m_views);
        }

        ~ComputeGuard() {
            if (m_context == nullptr) {
                return;
            }
            m_context->CSSetShader(m_shader.Get(), nullptr, 0);
            m_context->CSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
            m_context->CSSetShaderResources(0, 1, m_resources);
            m_context->CSSetUnorderedAccessViews(0, 1, m_views, nullptr);
            if (m_resources[0] != nullptr) {
                m_resources[0]->Release();
            }
            if (m_views[0] != nullptr) {
                m_views[0]->Release();
            }
        }

        ID3D11DeviceContext* m_context{nullptr};
        ComPtr<ID3D11ComputeShader> m_shader;
        ComPtr<ID3D11Buffer> m_constants;
        ID3D11ShaderResourceView* m_resources[1]{};
        ID3D11UnorderedAccessView* m_views[1]{};
    };

    // The shared texture the runtime hands out is TYPELESS, but a view has to name a concrete
    // format. `srgb` picks the encoded variant, which is what makes the hardware decode on read
    // and what the conversion shader encodes on write.
    DXGI_FORMAT TypedFormat(DXGI_FORMAT format, bool srgb) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return format;
        }
    }

    // A D3D12 resource opened from a shared D3D11 texture has to be put back into COMMON before the
    // other API touches it again; anything else would leak a state the D3D11 runtime cannot see.
    void Transition(ID3D12GraphicsCommandList* list,
                    ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

} // namespace

void EyeInterop::Fail(const char* what, HRESULT hr) {
    static char buffer[256];
    sprintf_s(buffer, "%s failed (0x%08lX)", what, (unsigned long)hr);
    m_lastError = buffer;
    LayerLog("EyeInterop: %s\n", buffer);
}

bool EyeInterop::Initialize(ID3D11Device* appDevice, ID3D11DeviceContext* appContext) {
    if (m_d3d12Device) {
        return true;
    }

    // The live block and the panel that writes it come up with the interop, which exists exactly when
    // NR could run. Starting the listener before the sessions come up matters: a value set from the
    // panel has to be in the block before the first frame reads it, or the first seconds of a session
    // run on the shipped picture while the browser is still opening.
    NrSettingsInitFromEnvironment();
    NrControlServerStart();

    m_appDevice = appDevice;
    m_appContext = appContext;

    if (FAILED(m_appDevice.As(&m_appDevice5)) || FAILED(m_appContext.As(&m_appContext4))) {
        m_lastError = "ID3D11Device5 / ID3D11DeviceContext4 unavailable (needs Windows 10 1703+)";
        LayerLog("EyeInterop: %s\n", m_lastError);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_appDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())))) {
        m_lastError = "IDXGIDevice unavailable";
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf()))) {
        m_lastError = "IDXGIAdapter unavailable";
        return false;
    }

    {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        LayerLog("EyeInterop: D3D12 on %ls\n", desc.Description);
    }

    // Which pass layout runs (see Process). Read here rather than per frame. The asynchronous layout is
    // opt-in, not the default: as built it hung the device (see the note in the header), so it is only
    // loaded when someone asks for it with AMDNR_XR_ASYNC=1 to measure against the synchronous one.
    {
        char setting[8]{};
        if (GetEnvironmentVariableA("AMDNR_XR_ASYNC", setting, (DWORD)sizeof(setting)) > 0) {
            m_async = setting[0] != '0';
        }
        LayerLog("EyeInterop: pass layout %s\n",
                 m_async ? "asynchronous (answer collected one frame later)" : "synchronous");
    }

    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_d3d12Device.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("D3D12CreateDevice", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_queue.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("CreateCommandQueue", hr);
        return false;
    }

    hr = m_d3d12Device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_allocator.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("CreateCommandAllocator", hr);
        return false;
    }

    hr = m_d3d12Device->CreateCommandList(0,
                                          D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          m_allocator.Get(),
                                          nullptr,
                                          IID_PPV_ARGS(m_commandList.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("CreateCommandList", hr);
        return false;
    }
    m_commandList->Close();

    // Shared fence, imported by the D3D11 side.
    hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("CreateFence", hr);
        return false;
    }

    HANDLE fenceHandle = nullptr;
    hr = m_d3d12Device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr)) {
        Fail("fence CreateSharedHandle", hr);
        return false;
    }
    hr = m_appDevice5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(m_fence11.ReleaseAndGetAddressOf()));
    CloseHandle(fenceHandle);
    if (FAILED(hr)) {
        Fail("OpenSharedFence", hr);
        return false;
    }

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) {
        m_lastError = "CreateEventW failed";
        return false;
    }

    // Read before the sessions come up: the decision has to be in hand by the time they are opened.
    if (!m_nr[0].Initialize(m_d3d12Device.Get(), m_queue.Get())) {
        m_lastError = m_nr[0].LastError();
        LayerLog("EyeInterop: neural renderer unavailable: %s\n", m_lastError);
        return false;
    }
    m_activeSessions = 1;

    // A second session earns its keep when the temporal path is on: only then does each eye need a
    // history of its own.
    //
    // The asynchronous layout needs a second session for a plainer reason, and temporal on or off: a
    // session holds one job at a time, and with the pass collected a frame after it was handed over,
    // both eyes would have one in flight at once -- on a shared session the second PrepareFrame would
    // wipe the first job. One session per eye is that constraint, not a preference.
    if (m_nr[0].TemporalEnabled() || m_async) {
        for (uint32_t eye = 1; eye < kMaxEyes; eye++) {
            if (!m_nr[eye].Initialize(m_d3d12Device.Get(), m_queue.Get())) {
                m_lastError = m_nr[eye].LastError();
                LayerLog("EyeInterop: session for eye %u unavailable: %s\n", eye, m_lastError);
                return false;
            }
            m_activeSessions++;
        }
        LayerLog("EyeInterop: %u sessions, %s\n",
                 m_activeSessions,
                 m_nr[0].TemporalEnabled() ? "one history per eye" : "one job in flight per eye");
    }

    // The look of the pass. `AMDNR_XR_GAMMA` undoes the network's tone drift (its answer comes back
    // as roughly in^1.15 in linear light, so a gamma just under 1 lifts it back) and `AMDNR_XR_GAIN`
    // trims any residual level error. The rim -- `AMDNR_XR_FEATHER` and `AMDNR_XR_ROUNDNESS` -- is not
    // read here any more: both are seeded into the settings block (NrSettingsInitFromEnvironment) and
    // read per frame, which is what lets the panel move them while the title runs.
    char setting[32]{};
    if (GetEnvironmentVariableA("AMDNR_XR_GAIN", setting, (DWORD)std::size(setting)) > 0) {
        m_gain = std::clamp((float)atof(setting), 0.25f, 4.f);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_GAMMA", setting, (DWORD)std::size(setting)) > 0) {
        m_gamma = std::clamp((float)atof(setting), 0.25f, 4.f);
    }

    char dumpFlag[8]{};
    m_dumpEnabled =
        GetEnvironmentVariableA("AMDNR_XR_DUMP", dumpFlag, (DWORD)std::size(dumpFlag)) > 0 && dumpFlag[0] != '0';

    const NrSettings rim = NrSettingsGet();
    LayerLog("EyeInterop: ready (gain %.2f gamma %.2f feather %.3f roundness %.2f, pixel dump %s)\n",
             m_gain,
             m_gamma,
             rim.feather,
             rim.roundness,
             m_dumpEnabled ? "on" : "off");
    return true;
}

bool EyeInterop::ExecuteAndWait(uint64_t queueWaitValue, double* submitMs, double* gpuWaitMs) {
    return Execute(m_commandList.Get(), queueWaitValue, true, submitMs, gpuWaitMs);
}

void EyeInterop::Abandon(ID3D12GraphicsCommandList* list) {
    // The list is closed rather than reset: nothing recorded into it is submitted, and closing is what
    // returns its allocator to a state the next frame can reset. A failure here is deliberately
    // ignored -- the caller is already on its way out of a failed pass.
    list->Close();
}

bool EyeInterop::BeginRecording(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, const char* what) {
    HRESULT result = allocator->Reset();
    if (FAILED(result)) {
        // A list a previous pass abandoned is still open, and an allocator cannot be reset while one
        // of its lists is open. Closing it here is what keeps a single failed pass from taking the
        // whole session with it.
        list->Close();
        result = allocator->Reset();
    }
    if (FAILED(result)) {
        m_lastError = what;
        LayerLog("EyeInterop: %s (allocator 0x%08lX, device 0x%08lX)\n",
                 what,
                 (unsigned long)result,
                 (unsigned long)(m_d3d12Device != nullptr ? m_d3d12Device->GetDeviceRemovedReason() : S_OK));
        return false;
    }

    result = list->Reset(allocator, nullptr);
    if (FAILED(result)) {
        m_lastError = what;
        LayerLog("EyeInterop: %s (list 0x%08lX)\n", what, (unsigned long)result);
        return false;
    }
    return true;
}

bool EyeInterop::Execute(ID3D12GraphicsCommandList* list, uint64_t queueWaitValue, bool wait,
                         double* submitMs, double* gpuWaitMs) {
    const auto submitStart = std::chrono::steady_clock::now();
    const HRESULT closed = list->Close();
    if (FAILED(closed)) {
        // Close is where a recording error finally surfaces, and the code says which kind it was: a
        // bad argument means something in the list was malformed, a device-removed reason means the
        // work itself took the adapter down.
        const HRESULT removed = m_d3d12Device != nullptr ? m_d3d12Device->GetDeviceRemovedReason() : S_OK;
        LayerLog("EyeInterop: command list Close failed hr=0x%08lx device=0x%08lx\n",
                 (unsigned long)closed,
                 (unsigned long)removed);
        m_lastError = "command list Close failed";
        return false;
    }

    if (queueWaitValue != 0) {
        m_queue->Wait(m_fence.Get(), queueWaitValue);
    }

    ID3D12CommandList* lists[]{list};
    m_queue->ExecuteCommandLists(1, lists);

    m_fenceValue++;
    const uint64_t done = m_fenceValue;
    m_queue->Signal(m_fence.Get(), done);

    if (wait && m_fence->GetCompletedValue() < done) {
        // Everything up to here is CPU side -- closing, naming the D3D11 fence the queue must wait on,
        // submitting and signalling. What follows is this thread blocked until the work is done.
        if (submitMs != nullptr) {
            *submitMs = Ms(std::chrono::steady_clock::now() - submitStart);
        }
        const auto gpuStart = std::chrono::steady_clock::now();
        m_fence->SetEventOnCompletion(done, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 5000) != WAIT_OBJECT_0) {
            m_lastError = "D3D12 fence wait timed out";
            LayerLog("EyeInterop: %s\n", m_lastError);
            return false;
        }
        if (gpuWaitMs != nullptr) {
            *gpuWaitMs = Ms(std::chrono::steady_clock::now() - gpuStart);
        }
    } else if (submitMs != nullptr) {
        *submitMs = Ms(std::chrono::steady_clock::now() - submitStart);
    }
    return true;
}

bool EyeInterop::EnsureMotionShader() {
    if (m_motionCs != nullptr) {
        return true;
    }
    if (m_motionGaveUp || m_appDevice == nullptr) {
        return false;
    }

    const auto compile = GetD3DCompile();
    if (compile == nullptr) {
        m_motionGaveUp = true;
        LayerLog("EyeInterop: d3dcompiler_47.dll is unavailable, the reprojection field stays off\n");
        return false;
    }

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> error;
    if (FAILED(compile(amdnr_fsr::kMotionShader,
                       std::strlen(amdnr_fsr::kMotionShader),
                       "AmdnrMotionCs",
                       nullptr,
                       nullptr,
                       "main",
                       "cs_5_0",
                       0,
                       0,
                       shader.ReleaseAndGetAddressOf(),
                       error.ReleaseAndGetAddressOf()))) {
        m_motionGaveUp = true;
        LayerLog("EyeInterop: the reprojection shader failed: %s\n",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "no message");
        return false;
    }

    if (FAILED(m_appDevice->CreateComputeShader(shader->GetBufferPointer(),
                                                shader->GetBufferSize(),
                                                nullptr,
                                                m_motionCs.ReleaseAndGetAddressOf()))) {
        m_motionGaveUp = true;
        LayerLog("EyeInterop: the reprojection compute shader could not be created\n");
        return false;
    }
    return true;
}

bool EyeInterop::BuildMotionField(uint32_t slot,
                                  const NrMotion& motion,
                                  uint32_t width,
                                  uint32_t height,
                                  HANDLE& handle,
                                  uint32_t& fieldWidth,
                                  uint32_t& fieldHeight) {
    handle = nullptr;
    fieldWidth = 0;
    fieldHeight = 0;

    // A view the layer could not reproject -- the first frame of a session, a pose it never saw -- is
    // not a failure here: the effect is handed its zero field instead, which reads as nothing moved.
    if (slot >= kMaxEyes || !motion.valid || width == 0 || height == 0 || !EnsureMotionShader()) {
        return false;
    }

    MotionField& field = m_motion[slot];
    if (field.width != width || field.height != height) {
        field.work.Reset();
        field.view.Reset();
        field.shared.Reset();
        field.handle = nullptr;
        field.width = 0;
        field.height = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // Two halves per texel. The field is a displacement as a fraction of the view, and a half keeps
        // a fraction's sub-pixel part where a pixel count in the thousands would lose it.
        desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_appDevice->CreateTexture2D(&desc, nullptr, field.work.ReleaseAndGetAddressOf())) ||
            FAILED(m_appDevice->CreateUnorderedAccessView(
                field.work.Get(), nullptr, field.view.ReleaseAndGetAddressOf()))) {
            field.work.Reset();
            field.view.Reset();
            m_motionGaveUp = true;
            LayerLog("EyeInterop: the reprojection target could not be created at %ux%u\n", width, height);
            return false;
        }

        // What the effect actually reads. A texture written through a UAV cannot be shared itself, so
        // the pass writes `work` above and copies the answer into this one, which is a shader resource
        // and nothing else and is what carries the handle.
        D3D11_TEXTURE2D_DESC sharedDesc = desc;
        sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(m_appDevice->CreateTexture2D(&sharedDesc, nullptr, field.shared.ReleaseAndGetAddressOf()))) {
            field.work.Reset();
            field.view.Reset();
            m_motionGaveUp = true;
            LayerLog("EyeInterop: the reprojection texture could not be created at %ux%u\n", width, height);
            return false;
        }

        ComPtr<IDXGIResource1> resource;
        if (FAILED(field.shared->QueryInterface(IID_PPV_ARGS(resource.ReleaseAndGetAddressOf()))) ||
            FAILED(resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &field.handle))) {
            field.work.Reset();
            field.view.Reset();
            field.shared.Reset();
            field.handle = nullptr;
            m_motionGaveUp = true;
            LayerLog("EyeInterop: the reprojection texture would not give up its handle\n");
            return false;
        }

        D3D11_BUFFER_DESC buffer{};
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        // A constant buffer has to be a multiple of 16 bytes; the transform is 31 words.
        buffer.ByteWidth = (UINT)((sizeof(amdnr_fsr::MotionConstants) + 15u) & ~15u);
        if (FAILED(m_appDevice->CreateBuffer(&buffer, nullptr, field.constants.ReleaseAndGetAddressOf()))) {
            field.work.Reset();
            field.view.Reset();
            field.shared.Reset();
            field.handle = nullptr;
            m_motionGaveUp = true;
            LayerLog("EyeInterop: the reprojection constants could not be created\n");
            return false;
        }

        field.width = width;
        field.height = height;
        LayerLog("EyeInterop: reprojection field %ux%u ready for eye %u\n", width, height, slot);
    }

    // The depth the parallax term reads, borrowed from the capture on this same device. It is rebuilt
    // whenever the reduction is, so the view is remade with it. A session with no reduction still gets
    // a field -- the rotation-only one, which is exact for a head that is being turned in place.
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    ID3D11Texture2D* depthTexture = DepthCaptureReducedTexture(&depthWidth, &depthHeight);
    if (depthTexture != m_motionDepthTexture) {
        m_motionDepthSrv.Reset();
        m_motionDepthTexture = depthTexture;
        if (depthTexture != nullptr) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MostDetailedMip = 0;
            view.Texture2D.MipLevels = 1;
            if (FAILED(m_appDevice->CreateShaderResourceView(
                    depthTexture, &view, m_motionDepthSrv.ReleaseAndGetAddressOf()))) {
                m_motionDepthSrv.Reset();
            }
        }
    }
    const bool withDepth = m_motionDepthSrv != nullptr && motion.invNear > 0.f;

    amdnr_fsr::MotionConstants constants{};
    std::memcpy(constants.matrix, motion.matrix, sizeof(constants.matrix));
    constants.a[0] = motion.a[0];
    constants.a[1] = motion.a[1];
    constants.b[0] = motion.b[0];
    constants.b[1] = motion.b[1];
    constants.tanLeft = motion.tanLeft;
    constants.tanWidth = motion.tanWidth;
    constants.tanUp = motion.tanUp;
    constants.tanHeight = motion.tanHeight;
    constants.extentScale[0] = motion.extentScale[0];
    constants.extentScale[1] = motion.extentScale[1];
    constants.originShift[0] = motion.originShift[0];
    constants.originShift[1] = motion.originShift[1];
    constants.width = width;
    constants.height = height;
    constants.translation[0] = motion.translation[0];
    constants.translation[1] = motion.translation[1];
    constants.translation[2] = motion.translation[2];
    // Zero without a depth to divide by, which is what leaves the pass rotation-only rather than
    // reading a distance that is not there.
    constants.invNear = withDepth ? motion.invNear : 0.f;
    constants.depthOrigin[0] = motion.depthOrigin[0];
    constants.depthOrigin[1] = motion.depthOrigin[1];
    constants.depthScale[0] = motion.depthScale[0];
    constants.depthScale[1] = motion.depthScale[1];

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_appContext->Map(field.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    m_appContext->Unmap(field.constants.Get(), 0);

    {
        ComputeGuard guard(m_appContext.Get());
        ID3D11Buffer* constantsBuffer = field.constants.Get();
        ID3D11ShaderResourceView* depth = withDepth ? m_motionDepthSrv.Get() : nullptr;
        ID3D11UnorderedAccessView* target = field.view.Get();

        m_appContext->CSSetConstantBuffers(0, 1, &constantsBuffer);
        m_appContext->CSSetShaderResources(0, 1, &depth);
        m_appContext->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        m_appContext->CSSetShader(m_motionCs.Get(), nullptr, 0);
        m_appContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        // The guard's restore unbinds the field's own view before the copy below, so the copy reads a
        // texture nothing is still writing through a UAV.
    }
    m_appContext->CopyResource(field.shared.Get(), field.work.Get());

    handle = field.handle;
    fieldWidth = field.width;
    fieldHeight = field.height;
    return field.handle != nullptr;
}

bool EyeInterop::UpscaleFsr4(uint32_t eye,
                             ID3D11Texture2D* source,
                             uint32_t sourceSlice,
                             const RECT& sourceRect,
                             ID3D11Texture2D* dest,
                             uint32_t destSlice,
                             float jitterX,
                             float jitterY,
                             const NrMotion& motion) {
    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    if (m_fsr4GaveUp[slot] || source == nullptr || dest == nullptr || m_d3d12Device == nullptr) {
        return false;
    }

    if (!m_fsr4Tried[slot]) {
        m_fsr4Tried[slot] = true;
        // Brought up on the devices this class already has rather than on new ones: the effect reads
        // and writes textures that belong to the application, and those cross between devices
        // through shared handles, which only works between devices on the same adapter.
        if (!m_fsr4[slot].Initialize(m_appDevice.Get(), m_appContext.Get(), m_d3d12Device.Get(), m_queue.Get())) {
            LayerLog("EyeInterop: FSR 4 unavailable for eye %u (%s); the output path keeps EASU + RCAS "
                     "for the rest of the session\n",
                     slot,
                     m_fsr4[slot].LastError());
            m_fsr4GaveUp[slot] = true;
            return false;
        }
    }

    // The field is built after the effect is known to be up, so a machine the effect cannot run on
    // never pays for it. The render size is the source rectangle: that is the grid the effect reads
    // the frame at, and the field has to be on it.
    HANDLE field = nullptr;
    uint32_t fieldWidth = 0;
    uint32_t fieldHeight = 0;
    BuildMotionField(slot,
                     motion,
                     (uint32_t)(sourceRect.right - sourceRect.left),
                     (uint32_t)(sourceRect.bottom - sourceRect.top),
                     field,
                     fieldWidth,
                     fieldHeight);

    return m_fsr4[slot].Run(
        source, sourceSlice, sourceRect, dest, destSlice, jitterX, jitterY, field, fieldWidth, fieldHeight);
}

bool EyeInterop::CreateSharedPair(DXGI_FORMAT format,
                                  uint32_t width,
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
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    ComPtr<ID3D11Texture2D> created;
    HRESULT hr = m_appDevice->CreateTexture2D(&desc, nullptr, created.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Fail("shared CreateTexture2D", hr);
        return false;
    }

    ComPtr<IDXGIResource1> dxgiResource;
    hr = created->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        Fail("IDXGIResource1", hr);
        return false;
    }

    HANDLE handle = nullptr;
    hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr)) {
        Fail("texture CreateSharedHandle", hr);
        return false;
    }

    hr = m_d3d12Device->OpenSharedHandle(handle, IID_PPV_ARGS(shared12.ReleaseAndGetAddressOf()));
    CloseHandle(handle);
    if (FAILED(hr)) {
        Fail("OpenSharedHandle", hr);
        return false;
    }

    shared11 = created;
    return true;
}

void EyeInterop::DrainQueue() {
    if (m_queue == nullptr || m_fence == nullptr) {
        return;
    }
    // The same signal-and-wait Execute uses, run with nothing recorded after it: when the fence reaches
    // this value, everything the queue was handed before it has completed. The D3D11 side needs no part
    // of this -- what it has queued into the crop texture is deferred by D3D11 itself until the copy
    // completes -- so what is being waited on here is the queue's own work and the runtime's work
    // handed to it, which is exactly the reader a swap of the shared pair could tear.
    m_fenceValue++;
    const uint64_t done = m_fenceValue;
    m_queue->Signal(m_fence.Get(), done);
    if (m_fence->GetCompletedValue() >= done) {
        return;
    }
    m_fence->SetEventOnCompletion(done, m_fenceEvent);
    if (WaitForSingleObject(m_fenceEvent, 5000) != WAIT_OBJECT_0) {
        LayerLog("EyeInterop: the queue did not drain within 5 s; the rebuild goes ahead anyway\n");
    }
}

void EyeInterop::CollectRetired() {
    if (m_retiredAt == 0) {
        return;
    }
    if (m_fence == nullptr || m_fence->GetCompletedValue() < m_retiredAt) {
        return;
    }
    m_retiredAt = 0;
    m_retiredCrop11.Reset();
    m_retiredCrop12.Reset();
    m_retiredResult11.Reset();
    m_retiredResult12.Reset();
}

void EyeInterop::RetireCurrent() {
    m_retiredCrop11.Swap(m_cropShared11);
    m_retiredCrop12.Swap(m_cropShared12);
    m_retiredResult11.Swap(m_resultShared11);
    m_retiredResult12.Swap(m_resultShared12);
    // Everything the queue was handed before this point is covered by the value the fence was last
    // asked to signal, so that is what the slot has to wait for.
    m_retiredAt = m_fence != nullptr ? (m_fenceValue > 0 ? m_fenceValue : 1) : 0;
}

void EyeInterop::SetDepthSource(HANDLE handle, uint32_t width, uint32_t height) {
    // Every session gets it: the two eyes run in separate sessions when the temporal path is on, and
    // each builds its own motion field. A session that was never opened reports that with a null
    // device and simply declines.
    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        m_nr[slot].SetDepthSharedHandle(handle, width, height);
    }
}

bool EyeInterop::EnsureTextures(DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (m_cropShared11 && m_resultShared11 && m_textureFormat == format && m_textureWidth == width &&
        m_textureHeight == height) {
        // The one frame in hand, which with the window on a size band is now almost every frame: a
        // band the window already covers keeps its textures, its network and its history.
        CollectRetired();
        return true;
    }

    // A size or format change lands here, and it is the frame the runtime builds a new network on. Two
    // things happen before a single resource is replaced, and both are about the same reader -- work
    // that is still using what is about to be swapped out from under it:
    //
    //   - the queue is drained, so everything recorded on it (including the runtime's own work handed
    //     to it for the old geometry) is complete;
    //   - the old pair goes to the retired slot rather than out of the door, released only once the
    //     fence recorded with it has passed.
    //
    // This used to be an immediate Reset with no wait at all. AMDNR 0.3.2 and 0.3.3.2 both drain before
    // reallocating and reuse their imported buffers afterwards; the drain is the part that matters
    // here, and one fence round trip per band crossing is nothing beside the rebuild it goes with.
    DrainQueue();
    CollectRetired();
    RetireCurrent();

    if (!CreateSharedPair(format, width, height, m_cropShared11, m_cropShared12) ||
        !CreateSharedPair(format, width, height, m_resultShared11, m_resultShared12)) {
        return false;
    }

    m_textureFormat = format;
    m_textureWidth = width;
    m_textureHeight = height;
    m_featherLogged = false;

    LayerLog("EyeInterop: crop textures %ux%u fmt=%d\n", width, height, (int)format);
    return true;
}

bool EyeInterop::Process(uint32_t eye,
                         ID3D11Texture2D* eyeTexture,
                         uint32_t arraySlice,
                         const RECT& box,
                         uint32_t networkWidth,
                         uint32_t networkHeight,
                         bool srgb,
                         const NrMotion& motion) {
    if (!m_d3d12Device) {
        m_lastError = "not initialized";
        return false;
    }

    // The master switch is tested here rather than inside the network so that "off" really is off: the
    // eye buffer is never cropped, converted or handed over, and the frame costs nothing beyond this
    // test. It is the one control that leaves no trace on the picture at all.
    if (!NrSettingsGet().enabled) {
        return true;
    }

    // With one session up both eyes run against it, which is how this worked before the temporal path
    // existed and is still correct when there is no history to keep apart.
    NrCore& nr = m_nr[m_activeSessions > 0 ? (eye % m_activeSessions) : 0];

    D3D11_TEXTURE2D_DESC eyeDesc{};
    eyeTexture->GetDesc(&eyeDesc);
    const uint32_t width = (uint32_t)(box.right - box.left);
    const uint32_t height = (uint32_t)(box.bottom - box.top);
    if (width == 0 || height == 0 || networkWidth == 0 || networkHeight == 0) {
        m_lastError = "empty crop";
        return false;
    }

    if (!EnsureTextures(eyeDesc.Format, width, height)) {
        return false;
    }

    // The rim mix is expressed as a fraction of the window so it looks the same at any window size,
    // and it is read from the live block rather than cached: the panel moves it while the title runs.
    const NrSettings rim = NrSettingsGet();
    const uint32_t feather = (uint32_t)(rim.feather * (float)std::min(width, height));
    const float roundness = rim.roundness;
    if (!m_featherLogged || rim.feather != m_loggedFeather || rim.roundness != m_loggedRoundness) {
        m_featherLogged = true;
        m_loggedFeather = rim.feather;
        m_loggedRoundness = rim.roundness;
        LayerLog("EyeInterop: window %ux%u -> network %ux%u, feather %.3f (%u px) roundness %.2f\n",
                 width,
                 height,
                 networkWidth,
                 networkHeight,
                 rim.feather,
                 feather,
                 roundness);
    }

    const auto start = std::chrono::steady_clock::now();

    // ---- D3D11: crop out of the eye buffer ----
    uint64_t cropDone = 0;
    {
        // Waits for whatever the last collect left in flight, because two readers of the shared pairs
        // are ordered behind this one line: the consumer that reads this same crop texture, and the
        // paste that reads the result texture. In the synchronous layout those were already finished
        // and this is a formality; in the asynchronous one it is what stops the next eye's crop (and
        // the next frame's) from landing on top of a pass those textures still belong to.
        m_appContext4->Wait(m_fence11.Get(), m_fenceValue);

        const D3D11_BOX source{(UINT)box.left, (UINT)box.top, 0u, (UINT)box.right, (UINT)box.bottom, 1u};
        m_appContext->CopySubresourceRegion(m_cropShared11.Get(), 0, 0, 0, 0, eyeTexture, arraySlice, &source);

        m_fenceValue++;
        cropDone = m_fenceValue;
        m_appContext4->Signal(m_fence11.Get(), cropDone);
        m_appContext->Flush();
    }
    const auto cropEnd = std::chrono::steady_clock::now();

    const uint32_t slot = eye < kMaxEyes ? eye : 0;

    // ---- the previous pass, if one is still in flight (asynchronous layout) ----
    //
    // Collected before anything of this frame is recorded, for two reasons: its consumer wants this
    // frame's crop as the untouched side of its blend -- the answer describes last frame's crop, and
    // the window may have moved since -- and the runtime's input has to be free again before the new
    // job is handed over. In the synchronous layout nothing is ever in flight here; the pass was
    // collected in the frame that handed it over, at the tail of this function.
    FinishTimes previous{};
    const auto previousStart = std::chrono::steady_clock::now();
    if (m_async && m_pending[slot].pending &&
        !FinishPending(slot, eyeTexture, arraySlice, box, width, height, feather, roundness, srgb,
                       cropDone, false, &previous)) {
        return false;
    }
    const auto previousEnd = std::chrono::steady_clock::now();

    const DXGI_FORMAT sharedFormat = m_cropShared12->GetDesc().Format;

    auto producerEnd = std::chrono::steady_clock::now();
    auto networkEnd = producerEnd;

    // The producer stage is where the two devices meet, so a slow pass is broken down further: how long
    // the view took to record, how long the submit took, and how long the thread then sat waiting on
    // the queue. producerD11Pending records whether the application's own crop had landed by the time
    // this view was handed over -- if it had not, the wait belongs to D3D11, not to this layer's work.
    double producerRecordMs = 0.0;
    double producerSubmitMs = 0.0;
    double producerGpuWaitMs = 0.0;
    int producerD11Pending = -1;

    // ---- D3D12, producer: colour crop -> fp16, then the runtime's input reads ----
    {
        const auto recordStart = std::chrono::steady_clock::now();
        if (!BeginRecording(m_commandList.Get(), m_allocator.Get(), "command list reset failed")) {
            return false;
        }

        NrConvert toNetwork{};
        toNetwork.sourceWidth = width;
        toNetwork.sourceHeight = height;
        toNetwork.destinationWidth = networkWidth;
        toNetwork.destinationHeight = networkHeight;

        Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool recorded = nr.RecordProducer(
            m_commandList.Get(), m_cropShared12.Get(), TypedFormat(sharedFormat, srgb), toNetwork, motion);
        Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!recorded) {
            Abandon(m_commandList.Get());
            m_lastError = nr.LastError();
            return false;
        }
        producerRecordMs = Ms(std::chrono::steady_clock::now() - recordStart);

        // Read before the submit: this answers what the queue was handed, not what it left behind.
        producerD11Pending = (m_fence11->GetCompletedValue() < cropDone) ? 1 : 0;

        // The submit waits on the application's crop in both layouts, but only the synchronous one makes
        // this thread sit through it: the asynchronous layout leaves the wait to the queue and collects
        // the pass a frame later, which is the whole of what "asynchronous" means here.
        if (!Execute(m_commandList.Get(), cropDone, !m_async, &producerSubmitMs, &producerGpuWaitMs)) {
            return false;
        }
    }
    producerEnd = std::chrono::steady_clock::now();

    // ---- D3D12, the network itself ----
    //
    // The job is handed over here; its answer is wanted at the collection -- the same frame in the
    // synchronous layout, the next one in the asynchronous. Enqueue waits for nothing, and the poll
    // that used to sit beside it now sits where the answer is read (see FinishPending).
    if (!nr.Enqueue()) {
        m_lastError = nr.LastError();
        return false;
    }
    networkEnd = std::chrono::steady_clock::now();

    m_pending[slot].pending = true;
    m_pending[slot].region = box;
    m_pending[slot].networkWidth = networkWidth;
    m_pending[slot].networkHeight = networkHeight;

    // ---- the pass just handed over, collected in the same frame (synchronous layout) ----
    FinishTimes collected{};
    if (!m_async &&
        !FinishPending(slot, eyeTexture, arraySlice, box, width, height, feather, roundness, srgb,
                       cropDone, true, &collected)) {
        return false;
    }
    const auto callEnd = m_async ? networkEnd : std::chrono::steady_clock::now();

    m_stageCrop[slot] += Ms(cropEnd - start);
    m_stageProducer[slot] += Ms(producerEnd - cropEnd);
    // The answer's own tail is wherever it was read: beside the enqueue in the synchronous layout, at
    // the collection in the asynchronous one. Both are the network's time, so both land in its bucket.
    m_stageNetwork[slot] += Ms(networkEnd - producerEnd) + previous.answer + collected.answer;
    const double consumerMs = (previous.answered ? previous.record + previous.submit + previous.gpu : 0.0) +
                              (collected.answered ? collected.record + collected.submit + collected.gpu : 0.0);
    m_stageConsumer[slot] += consumerMs;
    m_stagePaste[slot] += previous.paste + collected.paste;
    m_producerRecord[slot] += producerRecordMs;
    m_producerSubmit[slot] += producerSubmitMs;
    m_producerWait[slot] += producerGpuWaitMs;
    m_consumerRecord[slot] += previous.record + collected.record;
    m_consumerSubmit[slot] += previous.submit + collected.submit;
    m_consumerWait[slot] += previous.gpu + collected.gpu;
    if (producerD11Pending == 1) {
        m_producerPending[slot] += 1;
    }

    // The per-pass average hides where a stall comes from, so a slow pass is broken down by stage. One
    // line covers both layouts: what this call handed over (crop, producer, enqueue) and what it
    // collected (answer, consumer, paste) -- in the asynchronous layout those are two different frames'
    // passes, and the `finish` group is simply absent on the call that hands one over.
    const double totalMs = Ms(callEnd - start);
    if (totalMs > 60.0) {
        LayerLog("EyeInterop: slow pass %.0f ms (layout %s, crop %.0f, producer %.0f [record %.0f "
                 "submit %.0f gpu %.0f d11pending %d], enqueue %.0f, finish %.0f [answer %.0f "
                 "consumer record %.0f submit %.0f gpu %.0f paste %.0f])\n",
                 totalMs,
                 m_async ? "async" : "sync",
                 Ms(cropEnd - start),
                 Ms(producerEnd - cropEnd),
                 producerRecordMs,
                 producerSubmitMs,
                 producerGpuWaitMs,
                 producerD11Pending,
                 Ms(networkEnd - producerEnd),
                 Ms(previousEnd - previousStart),
                 previous.answer + collected.answer,
                 previous.record + collected.record,
                 previous.submit + collected.submit,
                 previous.gpu + collected.gpu,
                 previous.paste + collected.paste);
    }

    // Sampled repeatedly rather than captured once. The numbers only mean something next to another
    // run's, and a single frame of a scene someone is moving through cannot be compared with a single
    // frame of another. It stays opt-in (AMDNR_XR_DUMP) and rate limited, because reading back
    // flushes the pipeline; the pixel dumps themselves are still written once per view.
    const uint32_t dumpSlot = slot;
    m_processCounter[dumpSlot]++;
    if (m_dumpEnabled && m_processCounter[dumpSlot] >= 240 && (m_processCounter[dumpSlot] % 120) == 0) {
        m_appContext->Flush();
        DumpComparison(dumpSlot, eyeTexture, arraySlice, box, width, height);
    }

    return true;
}

// Collects one in-flight pass: waits out its answer, turns it back into the eye image and pastes it.
// Called either in the frame that handed the pass over (`wait` true, the synchronous layout) or at the
// start of the next one (`wait` false, the asynchronous), and that is the only difference between the
// two -- the work is the same, only the moment moves.
//
// The asynchronous layout is why this exists as its own step. Every wait in the old inline version sat
// in the frame that had just submitted the work it was waiting for, so this thread was on the GPU's
// critical path twice a frame: once on the application's own crop copy (measured at over 100 ms on a
// slow frame) and once on the answer. Collected a frame later, both waits are behind a frame of work
// that has already happened.
bool EyeInterop::FinishPending(uint32_t slot,
                               ID3D11Texture2D* eyeTexture,
                               uint32_t arraySlice,
                               const RECT& box,
                               uint32_t width,
                               uint32_t height,
                               uint32_t feather,
                               float roundness,
                               bool srgb,
                               uint64_t cropDone,
                               bool wait,
                               FinishTimes* times) {
    PendingPass& pass = m_pending[slot];
    if (!pass.pending) {
        return true;
    }
    pass.pending = false;
    times->ran = true;

    // The same session the pass was handed to: the eye-to-session mapping is fixed for a run (see
    // Process), so the slot's core is the core that holds the job.
    NrCore& nr = m_nr[m_activeSessions > 0 ? (slot % m_activeSessions) : 0];

    // A job that never answers drops the pass rather than the frame: the eye texture keeps what the
    // application drew, and the next frame hands over a fresh job (WaitAnswer has retired this one).
    const auto answerStart = std::chrono::steady_clock::now();
    const bool answered = nr.WaitAnswer();
    times->answer = Ms(std::chrono::steady_clock::now() - answerStart);
    if (!answered) {
        LayerLog("EyeInterop: the answer never came; this frame keeps the application's own pixels\n");
        return true;
    }
    times->answered = true;

    const DXGI_FORMAT sharedFormat = m_cropShared12->GetDesc().Format;
    const DXGI_FORMAT typedFormat = TypedFormat(sharedFormat, srgb);

    const auto recordStart = std::chrono::steady_clock::now();
    if (!BeginRecording(m_commandList.Get(), m_allocator.Get(), "command list reset failed")) {
        return false;
    }

    // Back to the window size, mixed with the untouched crop over the rim and bent back towards the
    // crop's tone, so the pass reads as a clean-up rather than as a rectangle.
    NrConvert toEye{};
    toEye.sourceWidth = pass.networkWidth;
    toEye.sourceHeight = pass.networkHeight;
    toEye.destinationWidth = width;
    toEye.destinationHeight = height;
    toEye.encodeSrgb = srgb;
    toEye.blend = true;
    toEye.feather = feather;
    toEye.roundness = roundness;
    toEye.gain = m_gain;
    toEye.gamma = m_gamma;
    // The window's own travel between the pass that made the answer and this frame, as a fraction of
    // this frame's crop. Both sides of the blend are read through it, so the answer lands where its
    // content now is instead of one window's worth of movement behind; zero on the synchronous path,
    // where the two rectangles are the same one.
    toEye.shiftX = (float)(box.left - pass.region.left) / (float)width;
    toEye.shiftY = (float)(box.top - pass.region.top) / (float)height;

    if (!nr.RecordConsumer(m_commandList.Get(),
                           m_resultShared12.Get(),
                           sharedFormat,
                           m_cropShared12.Get(),
                           typedFormat,
                           toEye,
                           slot)) {
        Abandon(m_commandList.Get());
        m_lastError = nr.LastError();
        return false;
    }
    times->record = Ms(std::chrono::steady_clock::now() - recordStart);

    // The queue waits for the crop this frame wrote, because both sides of the blend are this frame's
    // pixels. `wait` then decides whether this thread does too -- the synchronous layout does, the
    // asynchronous one leaves that to the queue and gets on with the frame.
    if (!Execute(m_commandList.Get(), cropDone, wait, &times->submit, &times->gpu)) {
        return false;
    }

    // ---- D3D11: paste the result back ----
    //
    // Ordered by the fence, never by a CPU wait: the copy is queued behind a Wait on the consumer's
    // completion value, and the signal at the end of this is what the next crop's copy waits on -- so
    // neither the crop texture nor the result texture can be overwritten mid-use by the frame that
    // follows this one. (The one deliberate exception sits inside RecordConsumer above: the filter
    // probe's readback maps a buffer the GPU may still hold, a few milliseconds every hundredth frame.)
    const auto pasteStart = std::chrono::steady_clock::now();
    m_appContext4->Wait(m_fence11.Get(), m_fenceValue);
    m_appContext->CopySubresourceRegion(
        eyeTexture, arraySlice, box.left, box.top, 0, m_resultShared11.Get(), 0, nullptr);
    m_fenceValue++;
    m_appContext4->Signal(m_fence11.Get(), m_fenceValue);
    times->paste = Ms(std::chrono::steady_clock::now() - pasteStart);
    return true;
}

namespace {

    std::string DumpDirectory() {
        std::string path = LayerDirectory() + "\\dump";
        CreateDirectoryA(path.c_str(), nullptr);
        return path;
    }

    // P6, because it needs no encoder and any tool can open it.
    void WritePpm(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
            return;
        }
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            fputc(rgba[i + 0], file);
            fputc(rgba[i + 1], file);
            fputc(rgba[i + 2], file);
        }
        fclose(file);
    }

    void Report(const char* label, const std::vector<uint8_t>& pixels) {
        if (pixels.empty()) {
            LayerLog("dump: %s is empty\n", label);
            return;
        }
        // The alpha byte is skipped: the eye buffers do not carry anything meaningful in it.
        uint64_t sums[3]{};
        uint8_t lows[3]{255, 255, 255};
        uint8_t highs[3]{0, 0, 0};
        size_t count = 0;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            for (int c = 0; c < 3; c++) {
                const uint8_t value = pixels[i + c];
                sums[c] += value;
                lows[c] = value < lows[c] ? value : lows[c];
                highs[c] = value > highs[c] ? value : highs[c];
            }
            count++;
        }
        LayerLog("dump: %s mean %.2f/%.2f/%.2f min %u/%u/%u max %u/%u/%u\n",
                 label,
                 count ? (double)sums[0] / count : 0.0,
                 count ? (double)sums[1] / count : 0.0,
                 count ? (double)sums[2] / count : 0.0,
                 lows[0],
                 lows[1],
                 lows[2],
                 highs[0],
                 highs[1],
                 highs[2]);
    }

    double MeanAbsoluteDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size() || a.empty()) {
            return -1.0;
        }
        uint64_t total = 0;
        size_t count = 0;
        for (size_t i = 0; i + 3 < a.size(); i += 4) {
            for (int c = 0; c < 3; c++) {
                const int d = (int)a[i + c] - (int)b[i + c];
                total += (uint64_t)(d < 0 ? -d : d);
            }
            count += 3;
        }
        return count ? (double)total / count : -1.0;
    }

} // namespace

bool EyeInterop::ReadBack(ID3D11Texture2D* source, uint32_t arraySlice, const RECT& box, std::vector<uint8_t>& pixels) {
    const uint32_t width = (uint32_t)(box.right - box.left);
    const uint32_t height = (uint32_t)(box.bottom - box.top);
    if (width == 0 || height == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_appDevice->CreateTexture2D(&desc, nullptr, staging.ReleaseAndGetAddressOf()))) {
        return false;
    }

    const D3D11_BOX sourceBox{(UINT)box.left, (UINT)box.top, 0u, (UINT)box.right, (UINT)box.bottom, 1u};
    m_appContext->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, arraySlice, &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_appContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    pixels.resize((size_t)width * height * 4);
    for (uint32_t y = 0; y < height; y++) {
        memcpy(pixels.data() + (size_t)y * width * 4,
               (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
               (size_t)width * 4);
    }
    m_appContext->Unmap(staging.Get(), 0);
    return true;
}

// Mean squared Laplacian residual of the luminance, over the middle of the window where the feather
// is not blending anything back. This is the number that says whether the network is removing grain:
// an answer that is merely the input with its contrast bent comes back carrying the same amount or
// more of it, while a real denoise drops it. It is a ratio between two runs that matters, not the
// absolute value, because the scene differs from run to run.
static double HighFrequencyEnergy(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    if (width < 8 || height < 8) {
        return 0.0;
    }
    const uint32_t x0 = width / 4;
    const uint32_t y0 = height / 4;
    const uint32_t x1 = width - x0;
    const uint32_t y1 = height - y0;

    const auto luminance = [&](uint32_t x, uint32_t y) {
        const size_t at = ((size_t)y * (size_t)width + (size_t)x) * 4;
        return 0.2126 * (double)pixels[at] + 0.7152 * (double)pixels[at + 1] + 0.0722 * (double)pixels[at + 2];
    };

    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            const double residual = luminance(x, y) - 0.25 * (luminance(x - 1, y) + luminance(x + 1, y) +
                                                              luminance(x, y - 1) + luminance(x, y + 1));
            sum += residual * residual;
            count++;
        }
    }
    return count ? sum / (double)count : 0.0;
}

// How much detail a frame has to carry before its crops are worth keeping, as the same mean squared
// Laplacian residual HighFrequencyEnergy reports. A bright but flat view -- a soft wall, an
// out-of-focus area -- sits near 0.2, and its two crops differ by about half a grey level, which is
// no disparity signal at all. A scene with surfaces and edges is well above 2.
static constexpr double kDumpMinDetail = 2.0;
// A few pairs per view, so a frame reached after the first usable one can still be kept.
static constexpr uint32_t kDumpMaxWrites = 4;

bool EyeInterop::DumpComparison(uint32_t eye,
                                ID3D11Texture2D* eyeTexture,
                                uint32_t arraySlice,
                                const RECT& box,
                                uint32_t width,
                                uint32_t height) {
    // Files are named by side rather than numbered, so a disparity pass can read the two views as a
    // pair instead of having to work out which pass overwrote the other.
    const char* side = eye == 0 ? "l" : "r";
    const RECT full{0, 0, (LONG)width, (LONG)height};

    std::vector<uint8_t> input;
    std::vector<uint8_t> output;
    std::vector<uint8_t> pasted;
    if (!ReadBack(m_cropShared11.Get(), 0, full, input) || !ReadBack(m_resultShared11.Get(), 0, full, output) ||
        !ReadBack(eyeTexture, arraySlice, box, pasted)) {
        LayerLog("dump: readback failed\n");
        return false;
    }

    // Menus and loading screens are almost black, and a near-black crop says nothing about colour
    // handling. A bright but flat view is no better: its two crops differ by fractions of a grey
    // level, so there is nothing in the pair to measure. Both gates run before a write slot is spent,
    // so a run of unusable frames costs stalls but never a capture.
    double sum = 0.0;
    size_t samples = 0;
    for (size_t i = 0; i + 3 < input.size(); i += 4) {
        sum += (double)input[i] + (double)input[i + 1] + (double)input[i + 2];
        samples += 3;
    }
    if (!samples || sum / samples < 24.0) {
        return false;
    }

    const double highFrequencyInput = HighFrequencyEnergy(input, width, height);
    const double highFrequencyOutput = HighFrequencyEnergy(output, width, height);
    if (highFrequencyInput < kDumpMinDetail) {
        return false;
    }

    const uint32_t index = m_dumpWrites[eye];
    if (index >= kDumpMaxWrites) {
        return true;
    }

    LayerLog("dump[%s]: box (%ld,%ld)-(%ld,%ld) mean abs difference input->output %.3f, output->pasted %.3f, "
             "high frequency %.4f -> %.4f (%+.1f%%)\n",
             side,
             (long)box.left,
             (long)box.top,
             (long)box.right,
             (long)box.bottom,
             MeanAbsoluteDifference(input, output),
             MeanAbsoluteDifference(output, pasted),
             highFrequencyInput,
             highFrequencyOutput,
             highFrequencyInput > 0.0 ? 100.0 * (highFrequencyOutput / highFrequencyInput - 1.0) : 0.0);

    if (index == 0) {
        char label[64];
        snprintf(label, sizeof(label), "%s input crop", side);
        Report(label, input);
        snprintf(label, sizeof(label), "%s neural output", side);
        Report(label, output);
        snprintf(label, sizeof(label), "%s eye buffer after paste", side);
        Report(label, pasted);
    }

    m_dumpWrites[eye] = index + 1;

    const std::string directory = DumpDirectory();
    const std::string stem = directory + "\\" + side + "_" + std::to_string(index);
    WritePpm(stem + "_input.ppm", input, width, height);
    WritePpm(stem + "_output.ppm", output, width, height);
    WritePpm(stem + "_pasted.ppm", pasted, width, height);
    LayerLog("dump: wrote %s_%u_{input,output,pasted}.ppm (%ux%u) to %s\n",
             side,
             index,
             width,
             height,
             directory.c_str());
    return true;
}

void EyeInterop::TakeStageTotals(uint32_t eye,
                                double& crop,
                                double& producer,
                                double& network,
                                double& consumer,
                                double& paste) {
    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    crop = m_stageCrop[slot];
    producer = m_stageProducer[slot];
    network = m_stageNetwork[slot];
    consumer = m_stageConsumer[slot];
    paste = m_stagePaste[slot];
    m_stageCrop[slot] = 0;
    m_stageProducer[slot] = 0;
    m_stageNetwork[slot] = 0;
    m_stageConsumer[slot] = 0;
    m_stagePaste[slot] = 0;
}

void EyeInterop::TakeProducerSplit(uint32_t eye,
                                   double& record,
                                   double& submit,
                                   double& wait,
                                   uint32_t& pending) {
    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    record = m_producerRecord[slot];
    submit = m_producerSubmit[slot];
    wait = m_producerWait[slot];
    pending = m_producerPending[slot];
    m_producerRecord[slot] = 0;
    m_producerSubmit[slot] = 0;
    m_producerWait[slot] = 0;
    m_producerPending[slot] = 0;
}

void EyeInterop::TakeConsumerSplit(uint32_t eye, double& record, double& submit, double& wait) {
    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    record = m_consumerRecord[slot];
    submit = m_consumerSubmit[slot];
    wait = m_consumerWait[slot];
    m_consumerRecord[slot] = 0;
    m_consumerSubmit[slot] = 0;
    m_consumerWait[slot] = 0;
}

void EyeInterop::Shutdown() {
    // No session, no per-frame values to set: the panel goes away with the interop rather than
    // listening to a block nothing reads.
    NrControlServerStop();

    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        m_nr[slot].Shutdown();
    }
    m_activeSessions = 0;

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_resultShared12.Reset();
    m_cropShared12.Reset();
    m_resultShared11.Reset();
    m_cropShared11.Reset();
    m_fence11.Reset();
    m_fence.Reset();
    m_commandList.Reset();
    m_allocator.Reset();
    m_queue.Reset();
    m_d3d12Device.Reset();
    m_appContext4.Reset();
    m_appContext.Reset();
    m_appDevice5.Reset();
    m_appDevice.Reset();
}
