#include "EyeInterop.h"

// ID3D12Debug, for switching the debug layer on for this layer's own device before it is created.
#include <d3d12sdklayers.h>

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

// Resets the slot's allocator and list so the next list can be recorded on them, waiting for the
// queue to let go of them first when it has to.
//
// The producer list is submitted without a wait -- that missing wait is the frame time this whole
// asynchronous path exists to save -- so "the card has had a whole frame" is an assumption. This is
// where it is checked rather than trusted: resetting an allocator the queue is still executing
// command lists from fails, and a failure here costs the frame, because the job is abandoned, the
// answer is never written back, and the runtime then refuses a frame with no picture in it.
bool EyeInterop::ResetForNextFrame(uint32_t slot, ID3D12CommandAllocator* allocator,
                                   ID3D12GraphicsCommandList* list) {
    const uint64_t producerDone = m_asyncProducerDone[slot];
    if (producerDone != 0 && m_fence != nullptr && m_fence->GetCompletedValue() < producerDone) {
        m_fence->SetEventOnCompletion(producerDone, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, 5000);
    }

    const HRESULT allocatorHr = allocator->Reset();
    if (FAILED(allocatorHr)) {
        Fail("command allocator reset", allocatorHr);
        return false;
    }

    const HRESULT listHr = list->Reset(allocator, nullptr);
    if (FAILED(listHr)) {
        Fail("command list reset", listHr);
        return false;
    }
    return true;
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
        // This build drives DLSS-NR through NGX and nothing else. The backend is fixed while the
        // layer is compiled, so there is no vendor to look up and no session to aim at one renderer
        // or the other; the adapter is read for the log line only.
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        LayerLog("EyeInterop: D3D12 on %ls (vendor 0x%04X, DLSS-NR backend)\n",
                 desc.Description,
                 (unsigned)desc.VendorId);
    }

    // The device this layer works on is its own, so the debug layer can be switched on before that
    // device exists -- and that is the only way to be told what the runtime objected to when it takes
    // the device away. The removal carries DXGI_ERROR_INVALID_CALL and nothing else, and the queue
    // that would name the offending call belongs to whichever device switched the layer on: the
    // application's device has it, this one does not, and validation messages are not shared.
    //
    // Behind AMDNR_XR_D3D12_DEBUG, because it is a diagnostic rather than a setting: it costs a
    // validation pass over every command this layer records, and it needs the Graphics Tools feature
    // to be installed on the machine.
    {
        char setting[8]{};
        const bool wanted =
            GetEnvironmentVariableA("AMDNR_XR_D3D12_DEBUG", setting, (DWORD)std::size(setting)) > 0 &&
            setting[0] != '0';
        if (wanted) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.ReleaseAndGetAddressOf())))) {
                debug->EnableDebugLayer();
                LayerLog("EyeInterop: the D3D12 debug layer is on for this layer's own device\n");
            } else {
                LayerLog("EyeInterop: no D3D12 debug layer is installed on this machine\n");
            }
        }
    }

    // Asked for at 12_0, which is what the working integrations ask for and what this backend is
    // written against. The 11_0 retry below is only there so that a card which cannot give a 12_0
    // device still gets one and fails on something the log can name, rather than leaving the layer
    // unable to start at all.
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(m_d3d12Device.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_d3d12Device.ReleaseAndGetAddressOf()));
    }
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

    // The asynchronous pass keeps a list in flight per eye, so it records from its own allocator and
    // list rather than the pair above -- which the synchronous path always leaves idle between calls.
    for (uint32_t eye = 0; eye < kMaxEyes; eye++) {
        hr = m_d3d12Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_asyncAllocator[eye].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            Fail("CreateCommandAllocator(async)", hr);
            return false;
        }
        hr = m_d3d12Device->CreateCommandList(0,
                                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              m_asyncAllocator[eye].Get(),
                                              nullptr,
                                              IID_PPV_ARGS(m_asyncList[eye].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            Fail("CreateCommandList(async)", hr);
            return false;
        }
        m_asyncList[eye]->Close();
    }

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

    // Read before the sessions come up: the asynchronous path is the other reason a second session is
    // needed, so the decision has to be in hand by the time they are opened.
    char asyncFlag[8]{};
    m_async = GetEnvironmentVariableA("AMDNR_XR_ASYNC", asyncFlag, (DWORD)std::size(asyncFlag)) > 0 &&
              asyncFlag[0] != '0';

    if (!m_nr[0].Initialize(m_d3d12Device.Get(), m_queue.Get())) {
        m_lastError = m_nr[0].LastError();
        LayerLog("EyeInterop: neural renderer unavailable: %s\n", m_lastError);
        return false;
    }
    m_activeSessions = 1;

    // A second session earns its keep when the temporal path is on -- only then does each eye need a
    // history of its own -- and whenever the asynchronous path is, because that one keeps a job in
    // flight for each eye and a session holds exactly one job.
    if (m_nr[0].TemporalEnabled() || m_async) {
        for (uint32_t eye = 1; eye < kMaxEyes; eye++) {
            if (!m_nr[eye].Initialize(m_d3d12Device.Get(), m_queue.Get())) {
                m_lastError = m_nr[eye].LastError();
                LayerLog("EyeInterop: session for eye %u unavailable: %s\n", eye, m_lastError);
                return false;
            }
            m_activeSessions++;
        }
        LayerLog("EyeInterop: %u sessions, one history per eye\n", m_activeSessions);
    }

    if (m_async) {
        LayerLog("EyeInterop: asynchronous pass on, this frame's crop is answered next frame\n");
    }

    // The look of the pass. `AMDNR_XR_GAMMA` undoes the network's tone drift (its answer comes back
    // as roughly in^1.055 in linear light, so a gamma just under 1 lifts it back) and `AMDNR_XR_GAIN`
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

bool EyeInterop::ExecuteAndWait(uint64_t queueWaitValue) {
    return Execute(m_commandList.Get(), queueWaitValue, true);
}

bool EyeInterop::Execute(ID3D12GraphicsCommandList* list, uint64_t queueWaitValue, bool wait) {
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
        m_fence->SetEventOnCompletion(done, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 5000) != WAIT_OBJECT_0) {
            m_lastError = "D3D12 fence wait timed out";
            LayerLog("EyeInterop: %s\n", m_lastError);
            return false;
        }
    }
    return true;
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

bool EyeInterop::EnsureTextures(DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (m_cropShared11 && m_resultShared11 && m_textureFormat == format && m_textureWidth == width &&
        m_textureHeight == height) {
        CollectRetired();
        return true;
    }

    // A size or format change replaces resources work may still be reading. The queue is drained first
    // and the old pair goes to a retired slot rather than out of the door -- the same two things the
    // AMD tree does, and for the same reason: a release the CPU cannot see the readers of is a release
    // that has not been asked about them. (Before this it was an immediate Reset with no wait.)
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

void EyeInterop::DrainQueue() {
    if (m_queue == nullptr || m_fence == nullptr) {
        return;
    }
    // Signal and wait with nothing recorded after it: when the fence reaches this value, everything the
    // queue was handed before it has completed -- the queue's own work and the runtime's work on it.
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
    m_retiredAt = m_fence != nullptr ? (m_fenceValue > 0 ? m_fenceValue : 1) : 0;
}

bool EyeInterop::EnsureAsyncTextures(uint32_t eye, DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (eye >= kMaxEyes) {
        return false;
    }
    if (m_asyncCrop11[eye] && m_asyncResult11[eye] && m_asyncFormat == format && m_asyncWidth == width &&
        m_asyncHeight == height) {
        return true;
    }

    // Anything still in flight was cropped from the old geometry, so it no longer describes a
    // rectangle this frame could paste: the jobs are dropped rather than pasted into the wrong place.
    // Drained first, because "dropped" here means released -- and the frames that were using these
    // pairs are still on the queue.
    DrainQueue();
    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        m_asyncCrop11[slot].Reset();
        m_asyncCrop12[slot].Reset();
        m_asyncResult11[slot].Reset();
        m_asyncResult12[slot].Reset();
        m_asyncJob[slot].pending = false;
    }
    m_asyncFormat = DXGI_FORMAT_UNKNOWN;
    m_asyncWidth = 0;
    m_asyncHeight = 0;

    // Both eyes are built here, not just the one asking: the first frame processes eye 0 and eye 1 in
    // turn, and a pair built lazily would be torn down by the other eye's first call.
    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        if (!CreateSharedPair(format, width, height, m_asyncCrop11[slot], m_asyncCrop12[slot]) ||
            !CreateSharedPair(format, width, height, m_asyncResult11[slot], m_asyncResult12[slot])) {
            return false;
        }
    }

    m_asyncFormat = format;
    m_asyncWidth = width;
    m_asyncHeight = height;
    LayerLog("EyeInterop: async staging %ux%u fmt=%d, one pair per eye\n", width, height, (int)format);
    return true;
}

bool EyeInterop::EnsureLeftTextures(DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (m_leftCrop != nullptr && m_leftFormat == format && m_leftWidth == width && m_leftHeight == height) {
        return true;
    }

    // A new size or format means the pair on hand no longer describes this frame, and a view built
    // from it would be built from the wrong window. Drained first for the same reason as the others:
    // these are released here, and the queue may still be reading them.
    DrainQueue();
    m_leftCrop.Reset();
    m_leftOutput.Reset();
    m_leftValid = false;
    m_leftFormat = DXGI_FORMAT_UNKNOWN;
    m_leftWidth = 0;
    m_leftHeight = 0;

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(m_d3d12Device->CreateCommittedResource(&heapProperties,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(m_leftCrop.ReleaseAndGetAddressOf()))) ||
        FAILED(m_d3d12Device->CreateCommittedResource(&heapProperties,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(m_leftOutput.ReleaseAndGetAddressOf())))) {
        m_leftCrop.Reset();
        m_leftOutput.Reset();
        return false;
    }
    m_leftCrop->SetName(L"first view crop");
    m_leftOutput->SetName(L"first view answer");

    m_leftFormat = format;
    m_leftWidth = width;
    m_leftHeight = height;
    LayerLog("EyeInterop: first-view textures %ux%u fmt=%d\n", width, height, (int)format);
    return true;
}

bool EyeInterop::Process(uint32_t eye,
                         ID3D11Texture2D* eyeTexture,
                         uint32_t arraySlice,
                         const RECT& box,
                         uint32_t networkWidth,
                         uint32_t networkHeight,
                         bool srgb,
                         const NrMotion& motion,
                         bool fromDetail) {
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

    // The asynchronous pass is a different enough sequence -- the answer it collects belongs to last
    // frame's crop -- that it is kept beside the synchronous one rather than threaded through it.
    if (m_async) {
        return ProcessAsync(eye, eyeTexture, arraySlice, box, networkWidth, networkHeight, srgb, motion);
    }

    // With one session up both eyes run against it, which is how this worked before the temporal path
    // existed and is still correct when there is no history to keep apart.
    NgxNr& nr = m_nr[m_activeSessions > 0 ? (eye % m_activeSessions) : 0];
    nr.SelectEye(eye);

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
        const D3D11_BOX source{(UINT)box.left, (UINT)box.top, 0u, (UINT)box.right, (UINT)box.bottom, 1u};
        m_appContext->CopySubresourceRegion(m_cropShared11.Get(), 0, 0, 0, 0, eyeTexture, arraySlice, &source);

        m_fenceValue++;
        cropDone = m_fenceValue;
        m_appContext4->Signal(m_fence11.Get(), cropDone);
        m_appContext->Flush();
    }
    const auto cropEnd = std::chrono::steady_clock::now();

    const DXGI_FORMAT sharedFormat = m_cropShared12->GetDesc().Format;

    auto producerEnd = std::chrono::steady_clock::now();
    auto networkEnd = producerEnd;
    auto consumerEnd = producerEnd;

    // The second view skips the network when the first one's crop and answer are still around. A view
    // that never went through -- a single-view frame, or one whose pass failed -- falls back to the
    // network rather than being dropped, so the picture is always treated, just not always cheaply.
    if (fromDetail && m_leftValid) {
        const DXGI_FORMAT typedFormat = TypedFormat(sharedFormat, srgb);
        NrShiftSearch search{};
        if (m_shiftValid) {
            // The two views sit a fixed distance apart, so this frame's answer is next to last
            // frame's. Saying so lets the search keep its footing where the texture repeats.
            search.prefer = m_shift;
        }
        int32_t found = 0;

        // ---- D3D12: where the other view's copy of this content sits ----
        //
        // Two lists rather than one because the shift has to be known before the transfer can name it,
        // and the search can only report after it has run. One extra round trip against a whole
        // network pass is the trade.
        {
            if (FAILED(m_allocator->Reset()) || FAILED(m_commandList->Reset(m_allocator.Get(), nullptr))) {
                m_lastError = "command list reset failed";
                return false;
            }

            Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(m_commandList.Get(), m_leftCrop.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (!nr.RecordShiftSearch(m_commandList.Get(),
                                      m_leftCrop.Get(),
                                      typedFormat,
                                      m_cropShared12.Get(),
                                      typedFormat,
                                      width,
                                      height,
                                      search)) {
                m_lastError = nr.LastError();
                return false;
            }
            if (!m_transferLogged) {
                LayerLog("EyeInterop: searching for the parallax between the views\n");
            }
        }

        if (!ExecuteAndWait(cropDone)) {
            return false;
        }
        consumerEnd = std::chrono::steady_clock::now();
        networkEnd = consumerEnd;
        producerEnd = consumerEnd;

        if (nr.TakeShiftSearch(search, found)) {
            m_shift = found;
            m_shiftValid = true;
        }

        // ---- D3D12: this view's crop plus what the network did to the other one ----
        {
            if (FAILED(m_allocator->Reset()) || FAILED(m_commandList->Reset(m_allocator.Get(), nullptr))) {
                m_lastError = "command list reset failed";
                return false;
            }

            Transition(m_commandList.Get(), m_leftOutput.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // The consumer's conversion, with the answer replaced by the other view's difference.
            NrConvert toEye{};
            toEye.sourceWidth = width;
            toEye.sourceHeight = height;
            toEye.destinationWidth = width;
            toEye.destinationHeight = height;
            toEye.encodeSrgb = srgb;
            toEye.blend = true;
            toEye.feather = feather;
            toEye.gain = m_gain;
            toEye.gamma = m_gamma;

            NrTransfer transfer{};
            transfer.shift = m_shiftValid ? m_shift : 0;
            if (!nr.RecordTransfer(m_commandList.Get(),
                                   m_resultShared12.Get(),
                                   sharedFormat,
                                   m_cropShared12.Get(),
                                   typedFormat,
                                   m_leftOutput.Get(),
                                   typedFormat,
                                   m_leftCrop.Get(),
                                   typedFormat,
                                   transfer,
                                   toEye)) {
                m_lastError = nr.LastError();
                return false;
            }

            Transition(m_commandList.Get(), m_leftOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(m_commandList.Get(), m_leftCrop.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            if (!m_transferLogged) {
                m_transferLogged = true;
                LayerLog("EyeInterop: this view comes from the other one's detail, parallax %d px, shift known %d\n",
                         m_shift,
                         m_shiftValid ? 1 : 0);
            }
        }

        if (!ExecuteAndWait(0)) {
            return false;
        }
    } else {
        // ---- D3D12, producer: colour crop -> fp16, then the runtime's input reads ----
        {
            if (FAILED(m_allocator->Reset()) || FAILED(m_commandList->Reset(m_allocator.Get(), nullptr))) {
                m_lastError = "command list reset failed";
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
                m_lastError = nr.LastError();
                return false;
            }

            if (!ExecuteAndWait(cropDone)) {
                return false;
            }
        }
        producerEnd = std::chrono::steady_clock::now();

        // ---- D3D12, the network itself ----
        if (!nr.Launch()) {
            m_lastError = nr.LastError();
            return false;
        }
        networkEnd = std::chrono::steady_clock::now();

        // ---- D3D12, consumer: answer -> 8 bit, then back into the shared texture ----
        {
            if (FAILED(m_allocator->Reset()) || FAILED(m_commandList->Reset(m_allocator.Get(), nullptr))) {
                m_lastError = "command list reset failed";
                return false;
            }

            // Back to the window size, mixed with the untouched crop over the rim and bent back towards
            // the crop's tone, so the pass reads as a clean-up rather than as a rectangle.
            NrConvert toEye{};
            toEye.sourceWidth = networkWidth;
            toEye.sourceHeight = networkHeight;
            toEye.destinationWidth = width;
            toEye.destinationHeight = height;
            toEye.encodeSrgb = srgb;
            toEye.blend = true;
            toEye.feather = feather;
            toEye.roundness = roundness;
            toEye.gain = m_gain;
            toEye.gamma = m_gamma;

            const DXGI_FORMAT typedFormat = TypedFormat(sharedFormat, srgb);
            if (!nr.RecordConsumer(m_commandList.Get(),
                                   m_resultShared12.Get(),
                                   sharedFormat,
                                   m_cropShared12.Get(),
                                   typedFormat,
                                   toEye)) {
                m_lastError = nr.LastError();
                return false;
            }

            // Hold on to this view's crop and answer while the other one is built. Both are back in
            // COMMON by the time the consumer returns, and the working pair is reused by every view,
            // so without this copy the first view's pixels would be gone by the time they were wanted.
            if (eye == 0 && EnsureLeftTextures(sharedFormat, width, height)) {
                Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(m_commandList.Get(), m_resultShared12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(m_commandList.Get(), m_leftCrop.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                Transition(m_commandList.Get(), m_leftOutput.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                m_commandList->CopyResource(m_leftCrop.Get(), m_cropShared12.Get());
                m_commandList->CopyResource(m_leftOutput.Get(), m_resultShared12.Get());
                Transition(m_commandList.Get(), m_leftCrop.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Transition(m_commandList.Get(), m_leftOutput.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Transition(m_commandList.Get(), m_cropShared12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                Transition(m_commandList.Get(), m_resultShared12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                m_leftValid = true;
            }

            if (!ExecuteAndWait(0)) {
                return false;
            }
        }
        consumerEnd = std::chrono::steady_clock::now();
    }

    // ---- D3D11: paste the result back ----
    {
        m_appContext4->Wait(m_fence11.Get(), m_fenceValue);
        m_appContext->CopySubresourceRegion(
            eyeTexture, arraySlice, box.left, box.top, 0, m_resultShared11.Get(), 0, nullptr);
    }
    const auto pasteEnd = std::chrono::steady_clock::now();

    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    m_stageCrop[slot] += Ms(cropEnd - start);
    m_stageProducer[slot] += Ms(producerEnd - cropEnd);
    m_stageNetwork[slot] += Ms(networkEnd - producerEnd);
    m_stageConsumer[slot] += Ms(consumerEnd - networkEnd);
    m_stagePaste[slot] += Ms(pasteEnd - consumerEnd);

    // The per-pass average hides where a stall comes from, so a slow pass is broken down by stage.
    const double totalMs = Ms(pasteEnd - start);
    if (totalMs > 60.0) {
        LayerLog("EyeInterop: slow pass %.0f ms (crop %.0f, producer %.0f, network %.0f, consumer %.0f, paste %.0f)\n",
                 totalMs,
                 Ms(cropEnd - start),
                 Ms(producerEnd - cropEnd),
                 Ms(networkEnd - producerEnd),
                 Ms(consumerEnd - networkEnd),
                 Ms(pasteEnd - consumerEnd));
    }

    // Sampled repeatedly rather than captured once. The numbers only mean something next to another
    // run's, and a single frame of a scene someone is moving through cannot be compared with a single
    // frame of another. It stays opt-in (AMDNR_XR_DUMP) and rate limited, because reading back
    // flushes the pipeline; the pixel dumps themselves are still written once per view.
    const uint32_t dumpSlot = eye < kMaxEyes ? eye : 0;
    m_processCounter[dumpSlot]++;
    if (m_dumpEnabled && m_processCounter[dumpSlot] >= 240 && (m_processCounter[dumpSlot] % 120) == 0) {
        m_appContext->Flush();
        DumpComparison(dumpSlot, eyeTexture, arraySlice, box, width, height);
    }

    return true;
}

// How the answer being collected has to be read for its content to land under this frame's pose.
//
// This is the motion shader's arithmetic, transcribed to the CPU, and it has to stay that arithmetic:
// that shader answers the identical question for the network's temporal path -- given a pixel of this
// frame's crop, where was this content in the previous one. The window's own travel belongs inside
// that answer rather than on top of it, because the answer was cropped at one origin and is written
// at another. The term is only correct while the result is pasted onto this frame's window, which is
// what the caller does; landing it on the origin it was cropped from would apply that travel twice.
bool EyeInterop::WindowShift(const NrMotion& motion,
                             uint32_t networkWidth,
                             uint32_t networkHeight,
                             float shift[2]) {
    if (!motion.valid || networkWidth == 0 || networkHeight == 0 || motion.tanWidth == 0.f ||
        motion.tanHeight == 0.f) {
        return false;
    }

    // The centre of the window, as the motion shader addresses it: pixel centres, so the middle of the
    // image is half its size and not half its size minus one.
    const float px = (float)networkWidth * 0.5f;
    const float py = (float)networkHeight * 0.5f;
    const float sx = motion.a[0] + px * motion.b[0];
    const float sy = motion.a[1] + py * motion.b[1];

    // The viewing ray this pixel stands for, and the same ray in the previous frame's view space.
    const float ray[3] = {motion.tanLeft + sx * motion.tanWidth, motion.tanUp - sy * motion.tanHeight, -1.f};
    const float before[3] = {
        motion.matrix[0] * ray[0] + motion.matrix[1] * ray[1] + motion.matrix[2] * ray[2],
        motion.matrix[3] * ray[0] + motion.matrix[4] * ray[1] + motion.matrix[5] * ray[2],
        motion.matrix[6] * ray[0] + motion.matrix[7] * ray[1] + motion.matrix[8] * ray[2]};

    // A ray that came out behind the camera has no previous pixel, the same answer the shader gives.
    const float depth = -before[2];
    if (depth < 1e-4f) {
        return false;
    }

    const float previousX = (before[0] / depth - motion.tanLeft) / motion.tanWidth;
    const float previousY = (motion.tanUp - before[1] / depth) / motion.tanHeight;

    // Network pixels first -- which is what `extentScale` and `originShift` are in -- and then a
    // fraction of the image, which is the space the consumer samples its source in.
    const float x = ((previousX - sx) * motion.extentScale[0] + motion.originShift[0]) / (float)networkWidth;
    const float y = ((previousY - sy) * motion.extentScale[1] + motion.originShift[1]) / (float)networkHeight;

    // A pose pair the runtime produced while it was still coming up can be anything at all, and a
    // non-finite shift is a request to read the whole answer from off the end of itself. Declining is
    // safe in a way that clamping would not be: no shift simply leaves the window one frame stale,
    // which is exactly what the synchronous path shows every frame. Half the window is the guard, and
    // it is unreachable by a head -- it stands for some thousands of degrees per second.
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 0.5f || std::fabs(y) > 0.5f) {
        return false;
    }

    shift[0] = x;
    shift[1] = y;
    return true;
}

// The asynchronous pass: the same work as the synchronous one, split so that no frame waits for its
// own network. The crop taken here is handed to the network and the answer collected here belongs to
// the crop taken last frame, so the window ends up holding the previous frame's picture of itself.
// That one frame of staleness inside the window is what buys the removal of the wait.
bool EyeInterop::ProcessAsync(uint32_t eye,
                              ID3D11Texture2D* eyeTexture,
                              uint32_t arraySlice,
                              const RECT& box,
                              uint32_t networkWidth,
                              uint32_t networkHeight,
                              bool srgb,
                              const NrMotion& motion) {
    const uint32_t slot = eye < kMaxEyes ? eye : 0;
    // One job per eye is in flight, so the session has to be this eye's own; the second one is opened
    // for exactly that reason when this path is on.
    NgxNr& nr = m_nr[m_activeSessions > 1 ? slot : 0];
    nr.SelectEye(eye);

    D3D11_TEXTURE2D_DESC eyeDesc{};
    eyeTexture->GetDesc(&eyeDesc);
    const uint32_t width = (uint32_t)(box.right - box.left);
    const uint32_t height = (uint32_t)(box.bottom - box.top);
    if (width == 0 || height == 0 || networkWidth == 0 || networkHeight == 0) {
        m_lastError = "empty crop";
        return false;
    }

    if (!EnsureAsyncTextures(slot, eyeDesc.Format, width, height)) {
        return false;
    }

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

    ID3D11Texture2D* crop11 = m_asyncCrop11[slot].Get();
    ID3D12Resource* crop12 = m_asyncCrop12[slot].Get();
    ID3D11Texture2D* result11 = m_asyncResult11[slot].Get();
    ID3D12Resource* result12 = m_asyncResult12[slot].Get();
    ID3D12GraphicsCommandList* list = m_asyncList[slot].Get();
    ID3D12CommandAllocator* allocator = m_asyncAllocator[slot].Get();

    const DXGI_FORMAT sharedFormat = crop12->GetDesc().Format;
    const DXGI_FORMAT typedFormat = TypedFormat(sharedFormat, srgb);

    const auto start = std::chrono::steady_clock::now();

    // ---- Collect the answer to last frame's crop, carried onto this frame ----
    //
    // It was enqueued a whole frame ago, so the wait here is normally already satisfied and what is
    // left is the consumer's own fraction of a millisecond. It runs before this frame's crop because
    // the consumer mixes against the crop the network actually saw, and cropping first would overwrite
    // it -- and the wait for that crop, put in front of the consumer, is a wait for the application's
    // own frame, which is the stall this path exists to remove.
    //
    // The answer describes last frame's window, cropped where that window then was, and it is about to
    // be written into this frame's window, which is somewhere else. The shift is what carries its
    // content across the two: the head's rotation since it was produced, and the window's own travel.
    // Read with neither, the answer lands with its content in the wrong place, and the window shows a
    // picture of a scene that has moved on -- most of it when the window itself has just moved.
    const AsyncJob job = m_asyncJob[slot];
    bool collected = false;
    if (job.pending) {
        float shift[2]{0.f, 0.f};
        const bool shifted = WindowShift(motion, job.networkWidth, job.networkHeight, shift);
        if (!nr.WaitAnswer()) {
            m_lastError = nr.LastError();
            LayerLog("EyeInterop: %s\n", m_lastError);
            m_asyncJob[slot].pending = false;
        } else if (!ResetForNextFrame(slot, allocator, list)) {
            m_asyncJob[slot].pending = false;
            return false;
        } else {
            NrConvert toEye{};
            toEye.sourceWidth = job.networkWidth;
            toEye.sourceHeight = job.networkHeight;
            toEye.destinationWidth = width;
            toEye.destinationHeight = height;
            toEye.encodeSrgb = srgb;
            toEye.blend = true;
            toEye.feather = feather;
            toEye.roundness = roundness;
            toEye.gain = m_gain;
            toEye.gamma = m_gamma;
            if (shifted) {
                toEye.shiftX = shift[0];
                toEye.shiftY = shift[1];
                m_asyncShiftPeak[slot] =
                    std::max(m_asyncShiftPeak[slot], std::max(std::fabs(shift[0]), std::fabs(shift[1])));
            }

            // Reported as the worst frame in the window, not as a sample of it. The window is held
            // still within its dead zone and then moved in one go, so nearly every frame carries
            // nothing and the frame that carries something carries all of it; a sample lands on the
            // first kind almost every time, which is how a run can look perfectly stable while one
            // frame in twenty jumps by a seventh of the window.
            if (++m_asyncShiftFrames[slot] >= 300) {
                LayerLog("EyeInterop: eye %u answer carried by (%.4f, %.4f) of the window, worst %.4f "
                         "over %u frames, motion %s\n",
                         slot,
                         toEye.shiftX,
                         toEye.shiftY,
                         m_asyncShiftPeak[slot],
                         m_asyncShiftFrames[slot],
                         shifted ? "known" : "unavailable");
                m_asyncShiftFrames[slot] = 0;
                m_asyncShiftPeak[slot] = 0.f;
            }

            if (!nr.RecordConsumer(list, result12, sharedFormat, crop12, typedFormat, toEye)) {
                m_lastError = nr.LastError();
                m_asyncJob[slot].pending = false;
                // Closed even though the work is abandoned. A list left open is one whose allocator
                // cannot be reset, and the reset at the top of the next frame would then be reported
                // as an illegal operation instead of the failure that actually happened here.
                list->Close();
                return false;
            }
            if (!Execute(list, 0, true)) {
                m_asyncJob[slot].pending = false;
                return false;
            }
            m_asyncConsumerDone[slot] = m_fenceValue;
            collected = true;
        }
    }
    const auto networkEnd = std::chrono::steady_clock::now();

    // ---- Crop this frame ----
    //
    // Recorded on the application's context before the paste below, and that order is the whole point:
    // the copy has to read the frame the application just rendered, not the strip of last frame's
    // answer that is about to be written back over part of it.
    uint64_t cropDone = 0;
    {
        const D3D11_BOX source{(UINT)box.left, (UINT)box.top, 0u, (UINT)box.right, (UINT)box.bottom, 1u};
        m_appContext->CopySubresourceRegion(crop11, 0, 0, 0, 0, eyeTexture, arraySlice, &source);

        m_fenceValue++;
        cropDone = m_fenceValue;
        m_appContext4->Signal(m_fence11.Get(), cropDone);
        m_appContext->Flush();
    }
    const auto cropEnd = std::chrono::steady_clock::now();

    // ---- Paste it back, over this frame's window ----
    //
    // This frame's rectangle, not the one the answer was cropped from. The switch is what makes the
    // shift above correct rather than double-counted: the consumer is asked where a pixel of *this*
    // frame's window has to read the old answer, so it is this frame's rectangle that the result
    // belongs to. Landing it on last frame's rectangle instead would put the whole answer down one
    // window's travel from where it was read for, which is a band of wrong content exactly as wide as
    // the window moved -- and the window moves most when the head does.
    if (collected) {
        m_appContext4->Wait(m_fence11.Get(), m_asyncConsumerDone[slot]);
        m_appContext->CopySubresourceRegion(
            eyeTexture, arraySlice, box.left, box.top, 0, result11, 0, nullptr);
    }
    const auto pasteEnd = std::chrono::steady_clock::now();

    // ---- Hand this frame's crop to the network ----
    //
    // Every failure below abandons the job rather than leaving the flag set: a job whose producer
    // never ran would otherwise be collected next frame, and there would be no answer to collect.
    {
        if (!ResetForNextFrame(slot, allocator, list)) {
            m_asyncJob[slot].pending = false;
            return false;
        }

        NrConvert toNetwork{};
        toNetwork.sourceWidth = width;
        toNetwork.sourceHeight = height;
        toNetwork.destinationWidth = networkWidth;
        toNetwork.destinationHeight = networkHeight;

        Transition(list, crop12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool recorded = nr.RecordProducer(list, crop12, typedFormat, toNetwork, motion);
        Transition(list, crop12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!recorded) {
            m_lastError = nr.LastError();
            m_asyncJob[slot].pending = false;
            // See the consumer's own copy of this: the list is closed so that the next frame's reset
            // of its allocator is legal rather than being the thing that gets reported.
            list->Close();
            return false;
        }

        // The wait for the crop is a queue-side wait and costs the CPU nothing. What is deliberately
        // left out is the wait for this list itself: nothing in this frame reads its result, and by the
        // time the next frame does the GPU has had a whole frame to finish it. That missing wait is the
        // 10-17 ms the application used to spend here on its own frame coming back.
        if (!Execute(list, cropDone, false)) {
            m_asyncJob[slot].pending = false;
            return false;
        }
        // The only record of when the queue is done with this list. Nothing waits on it here, by
        // design; ResetForNextFrame is where it is checked.
        m_asyncProducerDone[slot] = m_fenceValue;
    }
    const auto producerEnd = std::chrono::steady_clock::now();

    if (!nr.Enqueue()) {
        m_lastError = nr.LastError();
        m_asyncJob[slot].pending = false;
        return false;
    }

    m_asyncJob[slot].pending = true;
    m_asyncJob[slot].networkWidth = networkWidth;
    m_asyncJob[slot].networkHeight = networkHeight;

    if (!m_asyncLogged) {
        m_asyncLogged = true;
        LayerLog("EyeInterop: asynchronous staging in use, window %ux%u, answer carried onto the next "
                 "frame by the head's own motion\n",
                 width,
                 height);
    }

    m_stageCrop[slot] += Ms(cropEnd - networkEnd);
    m_stageProducer[slot] += Ms(producerEnd - pasteEnd);
    // The collection is what the synchronous path calls the network stage, and in this one it is the
    // whole point: it is the number that should now be near zero.
    m_stageNetwork[slot] += Ms(networkEnd - start);
    m_stagePaste[slot] += Ms(pasteEnd - cropEnd);
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

void EyeInterop::Shutdown() {
    // No session, no per-frame values to set: the panel goes away with the interop rather than
    // listening to a block nothing reads.
    NrControlServerStop();

    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        m_nr[slot].Shutdown();
    }
    m_activeSessions = 0;

    // Anything the asynchronous path still had in flight is dropped: the sessions that own those jobs
    // go away with this call, and an answer nobody will collect has nothing left to answer to.
    for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
        m_asyncJob[slot] = AsyncJob{};
        m_asyncCrop11[slot].Reset();
        m_asyncCrop12[slot].Reset();
        m_asyncResult11[slot].Reset();
        m_asyncResult12[slot].Reset();
        m_asyncList[slot].Reset();
        m_asyncAllocator[slot].Reset();
        m_asyncProducerDone[slot] = 0;
    }
    m_asyncFormat = DXGI_FORMAT_UNKNOWN;
    m_asyncWidth = 0;
    m_asyncHeight = 0;

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
