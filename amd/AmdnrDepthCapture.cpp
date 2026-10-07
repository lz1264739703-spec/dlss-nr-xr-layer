// Captures the depth-stencil buffer the application renders its scene into.
//
// This replaces ReShade as the depth source. ReShade had the depth too, but reaching it meant
// injecting a present hook plus its VR effect runtime into this title, and that runtime killed the
// game inside a minute -- while the depth it could offer was the 1920x1080 desktop mirror, at a size
// the layer does not work in anyway. The layer is already inside the process and already holds the
// application's own device and immediate context, so the one thing missing is the D3D11 entry point
// that announces a depth buffer.
//
// The layer is a guest in this process. The hook therefore only reads a view descriptor and matches
// a pointer against a small fixed table, and it never releases a resource: a release that dropped
// the last reference would destroy a texture from inside a D3D11 call on the same context. Anything
// due to be released is parked and let go in DepthCaptureDetach, which runs once the session is
// gone and no render thread can be inside the hook any more.

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "AmdnrDepthBridge.h"
#include "AmdnrDepthCapture.h"
#include "AmdnrDepthMsaaRead.h"
#include "AmdnrProjectionProbe.h"
#include "LayerLog.h"

namespace {

    using Microsoft::WRL::ComPtr;

    // ID3D11DeviceContext declares its methods in interface order, after the three IUnknown slots and
    // the five ID3D11DeviceChild ones. These are the two that take a depth-stencil view, and their
    // indices are the same on every version of the interface because the derived versions append
    // rather than reorder.
    constexpr size_t kSlotOMSetRenderTargets = 33;
    constexpr size_t kSlotOMSetRenderTargetsAndUnbindDSS = 34;
    // RSSetViewports, which is how the application says which rectangle of the target it is drawing
    // into. That rectangle is what a consumer of the depth has to be told, and it is not derivable
    // from the resource's own size: this title's scene depth lives in a 6774x2718 allocation and is
    // drawn into a much smaller part of it. The bounding box of the non-cleared texels bounds it from
    // outside and cannot tell a viewport from a union of several passes over one texture; the
    // viewport is the thing itself.
    constexpr size_t kSlotRSSetViewports = 44;

    using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                         UINT,
                                                         ID3D11RenderTargetView* const*,
                                                         ID3D11DepthStencilView*);
    using OMSetRenderTargetsAndUnbindDSSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                                      UINT,
                                                                      ID3D11RenderTargetView* const*,
                                                                      ID3D11DepthStencilView*);
    using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                      UINT,
                                                      const D3D11_VIEWPORT*);

    OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
    OMSetRenderTargetsAndUnbindDSSFn g_originalOMSetRenderTargetsAndUnbindDSS = nullptr;
    RSSetViewportsFn g_originalRSSetViewports = nullptr;
    void** g_patchedVtable = nullptr;
    bool g_installed = false;

    // How many distinct depth buffers are remembered. A frame touches a handful; a loading screen
    // that churns through more is not the one the scene is drawn into.
    constexpr size_t kMaxCandidates = 12;

    struct Candidate {
        ComPtr<ID3D11Texture2D> texture;
        DXGI_FORMAT viewFormat{DXGI_FORMAT_UNKNOWN};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t samples{1};
        uint64_t binds{0};
        // The rectangle the application was drawing when this target was the last one bound, as the
        // viewport it set. Zero until a viewport has been seen for it.
        float viewportX{0.f};
        float viewportY{0.f};
        float viewportWidth{0.f};
        float viewportHeight{0.f};
        uint64_t viewports{0};
        // The same thing for the other pairing: the viewport set most recently *before* this target
        // was bound. Engines differ on the order -- some set the viewport and then bind, some bind and
        // then set it -- and the pair that does not fit inside the resource is the wrong one, because
        // a pass cannot draw into a rectangle larger than its own target. This title is the
        // viewport-first kind: its `2392x960` depth pair came back paired with a `4784x1920` viewport.
        float viewportAtBindX{0.f};
        float viewportAtBindY{0.f};
        float viewportAtBindWidth{0.f};
        float viewportAtBindHeight{0.f};
        uint64_t viewportsAtBind{0};
    };

    // The last viewport the application set, held until a depth target is bound.
    float g_pendingViewportX = 0.f;
    float g_pendingViewportY = 0.f;
    float g_pendingViewportWidth = 0.f;
    float g_pendingViewportHeight = 0.f;
    uint64_t g_pendingViewports = 0;

    std::mutex g_lock;
    Candidate g_candidates[kMaxCandidates];
    size_t g_candidateCount = 0;

    // Candidates that lost their slot while the render thread was inside a D3D11 call, and so still
    // hold the reference they were stored with. Released in DepthCaptureDetach.
    std::vector<ComPtr<ID3D11Texture2D>> g_parked;

    // Almost every call repeats the view the last one bound, so one remembered pair skips the COM
    // work for the common case. Only a reading thread and the render thread ever touch these.
    ID3D11DepthStencilView* g_lastView = nullptr;
    size_t g_lastIndex = kMaxCandidates;

    uint32_t g_eyeWidth = 0;
    uint32_t g_eyeHeight = 0;

    uint64_t g_published = 0;
    uint64_t g_loggedAt = 0;

    // A match on the per-eye size is worth more than any amount of extra area: that is the size the
    // layer actually wants depth for. Failing a match, the biggest buffer is the best guess, because
    // a scene depth covers the whole view and the odds and ends (shadow maps, small effects) do not.
    //
    // Two buffers of the same size are told apart by their sample count, and the multisampled one
    // wins. At one resolution that is the buffer the scene is drawn into, while a single-sample
    // buffer of exactly that size is a resolve target or a leftover in the same pool -- this title has
    // both at 6774x2718, and the single-sample one never changes from frame to frame. The ranking used
    // to go the other way, when a multisampled depth could not be read at all; it can now, through the
    // reduction in AmdnrDepthMsaaRead.
    //
    // Samples are folded in below the area rather than scaling it, so a slightly larger buffer still
    // outranks a smaller multisampled one -- which is what keeps a scene-sized buffer above a shadow
    // map, and is what a flat sample penalty would get wrong.
    uint64_t Score(const Candidate& candidate) {
        const uint64_t area = (uint64_t)candidate.width * candidate.height;
        const uint64_t samples = candidate.samples > 8 ? 8 : candidate.samples;
        const bool matchesEye =
            g_eyeWidth != 0 && candidate.width == g_eyeWidth && candidate.height == g_eyeHeight;
        return area * 8 + samples + (matchesEye ? ((uint64_t)1 << 40) : 0);
    }

    // Which buffer holds the scene is a question about this one title, and the candidate table does
    // not answer it: several of these sizes are plausible. With AMDNR_XR_DEPTH_ROTATE set, every
    // frame hands over a different candidate in turn, so a single run measures all of them and the
    // table below can be read against the numbers that came back from each.
    bool RotateCandidates() {
        static const bool rotate = [] {
            char text[8]{};
            return GetEnvironmentVariableA("AMDNR_XR_DEPTH_ROTATE", text, (DWORD)sizeof(text)) != 0 &&
                   text[0] != '0';
        }();
        return rotate;
    }

    // Caller holds g_lock.
    size_t SelectBest() {
        size_t best = kMaxCandidates;
        uint64_t bestScore = 0;
        for (size_t i = 0; i < g_candidateCount; i++) {
            const uint64_t score = Score(g_candidates[i]);
            if (best == kMaxCandidates || score > bestScore) {
                best = i;
                bestScore = score;
            }
        }
        return best;
    }

    // Caller holds g_lock. Attaches the most recently set viewport to `index` as the at-bind pairing.
    void AttachPendingViewport(size_t index) {
        if (index >= g_candidateCount || g_pendingViewports == 0) {
            return;
        }
        Candidate& candidate = g_candidates[index];
        candidate.viewportAtBindX = g_pendingViewportX;
        candidate.viewportAtBindY = g_pendingViewportY;
        candidate.viewportAtBindWidth = g_pendingViewportWidth;
        candidate.viewportAtBindHeight = g_pendingViewportHeight;
        candidate.viewportsAtBind++;
    }

    void NoteCandidate(ID3D11DepthStencilView* view) {
        if (!g_installed) {
            return;
        }

        const std::lock_guard<std::mutex> guard(g_lock);

        if (view == nullptr) {
            // Nothing is bound, so the next viewport the application sets belongs to no target here
            // and must not be attributed to whichever target happened to be seen last.
            g_lastView = nullptr;
            g_lastIndex = kMaxCandidates;
            return;
        }

        if (view == g_lastView && g_lastIndex < g_candidateCount) {
            g_candidates[g_lastIndex].binds++;
            return;
        }

        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource); // adds a reference on success
        if (resource == nullptr) {
            return;
        }
        ID3D11Texture2D* texture = nullptr;
        const HRESULT queried = resource->QueryInterface(IID_PPV_ARGS(&texture));
        resource->Release();
        if (FAILED(queried)) {
            g_lastView = nullptr;
            g_lastIndex = kMaxCandidates; // a buffer, so not a depth target
            return;
        }

        for (size_t i = 0; i < g_candidateCount; i++) {
            if (g_candidates[i].texture.Get() == texture) {
                g_candidates[i].binds++;
                g_lastView = view;
                g_lastIndex = i;
                AttachPendingViewport(i);
                texture->Release();
                return;
            }
        }

        // The view descriptor carries the depth format even when the resource behind it is typeless,
        // which is the usual arrangement: an R32_TYPELESS texture with a D32_FLOAT view.
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);

        Candidate newcomer;
        newcomer.texture.Attach(texture); // takes over the reference taken above
        newcomer.viewFormat = viewDesc.Format;
        newcomer.width = textureDesc.Width;
        newcomer.height = textureDesc.Height;
        newcomer.samples = textureDesc.SampleDesc.Count;
        newcomer.binds = 1;

        if (g_candidateCount < kMaxCandidates) {
            g_lastIndex = g_candidateCount;
            g_candidates[g_candidateCount++] = std::move(newcomer);
            g_lastView = view;
            AttachPendingViewport(g_lastIndex);
            return;
        }

        // Full: displace the weakest entry, but only if the newcomer is worth more. Displacing rather
        // than ignoring keeps a scene depth that appears after a menu from being locked out by the
        // menu's own buffers.
        size_t weakest = 0;
        uint64_t weakestScore = Score(g_candidates[0]);
        for (size_t i = 1; i < g_candidateCount; i++) {
            const uint64_t score = Score(g_candidates[i]);
            if (score < weakestScore) {
                weakest = i;
                weakestScore = score;
            }
        }
        if (Score(newcomer) <= weakestScore) {
            g_lastView = nullptr;
            g_lastIndex = kMaxCandidates; // newcomer dropped here, and its reference with it
            return;
        }

        g_parked.push_back(std::move(g_candidates[weakest].texture));
        g_candidates[weakest] = std::move(newcomer);
        g_lastIndex = weakest;
        g_lastView = view;
        AttachPendingViewport(weakest);
    }

    void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* context,
                                                   UINT numViews,
                                                   ID3D11RenderTargetView* const* renderTargetViews,
                                                   ID3D11DepthStencilView* depthStencilView) {
        NoteCandidate(depthStencilView);
        g_originalOMSetRenderTargets(context, numViews, renderTargetViews, depthStencilView);
    }

    void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUnbindDSS(
        ID3D11DeviceContext* context,
        UINT numViews,
        ID3D11RenderTargetView* const* renderTargetViews,
        ID3D11DepthStencilView* depthStencilView) {
        NoteCandidate(depthStencilView);
        g_originalOMSetRenderTargetsAndUnbindDSS(context, numViews, renderTargetViews, depthStencilView);
    }

    // Attaches the rectangle the application is about to draw into to the depth target it bound most
    // recently. Engines bind the target and then set the viewport, so the call following the bind is
    // the one that describes it; a viewport set with nothing bound has no candidate to belong to and
    // is dropped. Holding the lock across the original call is avoided: the render thread is the only
    // writer and the recording is four stores.
    void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* context,
                                               UINT numViewports,
                                               const D3D11_VIEWPORT* viewports) {
        if (numViewports > 0 && viewports != nullptr && g_installed) {
            const std::lock_guard<std::mutex> guard(g_lock);
            g_pendingViewportX = viewports[0].TopLeftX;
            g_pendingViewportY = viewports[0].TopLeftY;
            g_pendingViewportWidth = viewports[0].Width;
            g_pendingViewportHeight = viewports[0].Height;
            g_pendingViewports++;
            if (g_lastIndex < g_candidateCount) {
                Candidate& candidate = g_candidates[g_lastIndex];
                candidate.viewportX = viewports[0].TopLeftX;
                candidate.viewportY = viewports[0].TopLeftY;
                candidate.viewportWidth = viewports[0].Width;
                candidate.viewportHeight = viewports[0].Height;
                candidate.viewports++;
            }
        }
        g_originalRSSetViewports(context, numViewports, viewports);
    }

    // Swaps one entry of a virtual table. The table lives in the read-only data of d3d11.dll, so the
    // page has to be made writable for the store and put back afterwards.
    bool PatchSlot(void** vtable, size_t slot, void* replacement, void*& original) {
        DWORD previous = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &previous)) {
            return false;
        }
        original = vtable[slot];
        vtable[slot] = replacement;
        DWORD ignored = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), previous, &ignored);
        return true;
    }

    void LogCandidates() {
        const std::lock_guard<std::mutex> guard(g_lock);
        const size_t selected = SelectBest();
        LayerLog("depth capture: %zu depth target(s) seen, eye size %ux%u\n",
                 g_candidateCount,
                 g_eyeWidth,
                 g_eyeHeight);
        for (size_t i = 0; i < g_candidateCount; i++) {
            const Candidate& candidate = g_candidates[i];
            LayerLog("depth capture:   [%zu]%s %ux%u fmt=%d samples=%u binds=%llu "
                     "atBind %.0fx%.0f at (%.0f,%.0f) x%llu | afterBind %.0fx%.0f at (%.0f,%.0f) x%llu\n",
                     i,
                     i == selected ? " *" : "  ",
                     candidate.width,
                     candidate.height,
                     (int)candidate.viewFormat,
                     candidate.samples,
                     (unsigned long long)candidate.binds,
                     candidate.viewportAtBindWidth,
                     candidate.viewportAtBindHeight,
                     candidate.viewportAtBindX,
                     candidate.viewportAtBindY,
                     (unsigned long long)candidate.viewportsAtBind,
                     candidate.viewportWidth,
                     candidate.viewportHeight,
                     candidate.viewportX,
                     candidate.viewportY,
                     (unsigned long long)candidate.viewports);
        }
    }

} // namespace

void DepthCaptureAttach(ID3D11Device* device, ID3D11DeviceContext* context) {
    (void)device;
    // Started before this function's own early returns: the projection probe patches different
    // entries of the same virtual table and answers a different question, so it does not depend on
    // the depth watch coming up.
    ProjectionProbeAttach(context);

    if (g_installed || context == nullptr) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr) {
        LayerLog("depth capture: the immediate context has no virtual table, staying off\n");
        return;
    }
    if (g_originalOMSetRenderTargets != nullptr) {
        // A table already patched once. Patching the same table again with the same replacement is
        // harmless, but a second context of the same class shares it, and its own "original" would
        // then be our hook, which would call itself forever.
        g_installed = true;
        return;
    }

    void* originalA = nullptr;
    void* originalB = nullptr;
    if (!PatchSlot(vtable, kSlotOMSetRenderTargets, (void*)&Hook_OMSetRenderTargets, originalA)) {
        LayerLog("depth capture: could not make the virtual table writable, staying off\n");
        return;
    }
    if (!PatchSlot(vtable,
                   kSlotOMSetRenderTargetsAndUnbindDSS,
                   (void*)&Hook_OMSetRenderTargetsAndUnbindDSS,
                   originalB)) {
        // The first slot is already patched; put it back rather than run half a hook.
        void* ignored = nullptr;
        PatchSlot(vtable, kSlotOMSetRenderTargets, originalA, ignored);
        LayerLog("depth capture: could not hook the second entry point, staying off\n");
        return;
    }

    // The viewport hook is not required for the watch itself, so a failure here costs the drawn
    // rectangle and nothing else.
    void* originalC = nullptr;
    if (!PatchSlot(vtable, kSlotRSSetViewports, (void*)&Hook_RSSetViewports, originalC)) {
        LayerLog("depth capture: could not hook RSSetViewports, the drawn rectangle stays unknown\n");
    } else {
        g_originalRSSetViewports = (RSSetViewportsFn)originalC;
    }

    g_originalOMSetRenderTargets = (OMSetRenderTargetsFn)originalA;
    g_originalOMSetRenderTargetsAndUnbindDSS = (OMSetRenderTargetsAndUnbindDSSFn)originalB;
    g_patchedVtable = vtable;
    g_installed = true;
    LayerLog("depth capture: watching depth targets on the application's immediate context\n");
}

void DepthCaptureDetach() {
    // Stopped first, so no further copy is issued on a context that is about to go away.
    ProjectionProbeDetach();

    if (g_installed && g_patchedVtable != nullptr) {
        DWORD previous = 0;
        // The two slots are adjacent, so one protection change covers both.
        if (VirtualProtect(&g_patchedVtable[kSlotOMSetRenderTargets],
                           sizeof(void*) * 2,
                           PAGE_READWRITE,
                           &previous)) {
            g_patchedVtable[kSlotOMSetRenderTargets] = (void*)g_originalOMSetRenderTargets;
            g_patchedVtable[kSlotOMSetRenderTargetsAndUnbindDSS] =
                (void*)g_originalOMSetRenderTargetsAndUnbindDSS;
            DWORD ignored = 0;
            VirtualProtect(&g_patchedVtable[kSlotOMSetRenderTargets],
                           sizeof(void*) * 2,
                           previous,
                           &ignored);
        }
    }
    if (g_installed && g_patchedVtable != nullptr && g_originalRSSetViewports != nullptr) {
        DWORD previous = 0;
        if (VirtualProtect(&g_patchedVtable[kSlotRSSetViewports],
                           sizeof(void*),
                           PAGE_READWRITE,
                           &previous)) {
            g_patchedVtable[kSlotRSSetViewports] = (void*)g_originalRSSetViewports;
            DWORD ignored = 0;
            VirtualProtect(&g_patchedVtable[kSlotRSSetViewports], sizeof(void*), previous, &ignored);
        }
    }
    g_originalRSSetViewports = nullptr;
    g_installed = false;
    g_patchedVtable = nullptr;

    // Everything the hook was holding can be let go now that no render thread can be inside it.
    const std::lock_guard<std::mutex> guard(g_lock);
    for (size_t i = 0; i < g_candidateCount; i++) {
        g_candidates[i].texture.Reset();
        g_candidates[i] = Candidate{};
    }
    g_candidateCount = 0;
    g_parked.clear();
    // The reduce pass holds views and a target of its own, on the same device, so it goes with the
    // session rather than outliving it.
    MsaaDepthReadRelease();
    g_lastView = nullptr;
    g_lastIndex = kMaxCandidates;
    if (g_originalOMSetRenderTargets != nullptr) {
        LayerLog("depth capture: detached after %llu published frames\n",
                 (unsigned long long)g_published);
    }
    g_originalOMSetRenderTargets = nullptr;
    g_originalOMSetRenderTargetsAndUnbindDSS = nullptr;
    g_published = 0;
    g_loggedAt = 0;
}

HANDLE DepthCaptureReduceHandle(uint32_t* width, uint32_t* height) {
    return MsaaDepthReadTargetHandle(width, height);
}

ID3D11Texture2D* DepthCaptureReducedTexture(uint32_t* width, uint32_t* height) {
    return MsaaDepthReadTexture(width, height);
}

void DepthCaptureSetEyeSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    const std::lock_guard<std::mutex> guard(g_lock);
    if ((uint64_t)width * height > (uint64_t)g_eyeWidth * g_eyeHeight) {
        g_eyeWidth = width;
        g_eyeHeight = height;
    }
}

bool DepthCaptureSelectedViewport(uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height) {
    const std::lock_guard<std::mutex> guard(g_lock);
    const size_t selected = SelectBest();
    if (selected == kMaxCandidates) {
        return false;
    }

    const Candidate& candidate = g_candidates[selected];
    const float limitWidth = (float)candidate.width;
    const float limitHeight = (float)candidate.height;
    // Whichever pairing fits inside the resource. A pass cannot draw into more than its own target, so
    // a paired viewport larger than the buffer belongs to a different pass and is dropped; that is what
    // tells the two engine orderings apart without having to assume which one this title uses.
    const bool atBindFits = candidate.viewportsAtBind > 0 && candidate.viewportAtBindWidth >= 1.f &&
                            candidate.viewportAtBindHeight >= 1.f &&
                            candidate.viewportAtBindWidth <= limitWidth &&
                            candidate.viewportAtBindHeight <= limitHeight;
    const bool afterFits = candidate.viewports > 0 && candidate.viewportWidth >= 1.f &&
                           candidate.viewportHeight >= 1.f && candidate.viewportWidth <= limitWidth &&
                           candidate.viewportHeight <= limitHeight;
    if (!atBindFits && !afterFits) {
        return false;
    }

    const float chosenX = atBindFits ? candidate.viewportAtBindX : candidate.viewportX;
    const float chosenY = atBindFits ? candidate.viewportAtBindY : candidate.viewportY;
    const float chosenWidth = atBindFits ? candidate.viewportAtBindWidth : candidate.viewportWidth;
    const float chosenHeight = atBindFits ? candidate.viewportAtBindHeight : candidate.viewportHeight;

    x = (uint32_t)(chosenX > 0.f ? chosenX + 0.5f : 0.f);
    y = (uint32_t)(chosenY > 0.f ? chosenY + 0.5f : 0.f);
    width = (uint32_t)(chosenWidth + 0.5f);
    height = (uint32_t)(chosenHeight + 0.5f);
    return width > 0 && height > 0;
}

void DepthCapturePublish() {
    // Ahead of this function's own early returns: the probe answers a different question and must not
    // be skipped on a frame where no depth candidate happened to be selected.
    ProjectionProbePublish();

    if (!g_installed) {
        return;
    }

    AmdnrDepthFrame frame{};
    {
        const std::lock_guard<std::mutex> guard(g_lock);
        const size_t selected = RotateCandidates() && g_candidateCount > 0
                                    ? (size_t)(g_published % g_candidateCount)
                                    : SelectBest();
        if (selected == kMaxCandidates) {
            return;
        }
        const Candidate& candidate = g_candidates[selected];
        frame.size = sizeof(AmdnrDepthFrame);
        frame.version = AMDNR_DEPTH_BRIDGE_VERSION;
        frame.width = candidate.width;
        frame.height = candidate.height;
        frame.format = (uint32_t)candidate.viewFormat;
        // Raw hardware depth: the producer knows nothing about the projection, so it does not claim
        // to have linearised anything.
        frame.flags = 0;
        frame.texture = (uint64_t)(uintptr_t)candidate.texture.Get();
        frame.frame_index = ++g_published;
        frame.near_z = 0.f;
        frame.far_z = 0.f;
        frame.eye_width = g_eyeWidth;
        frame.eye_height = g_eyeHeight;
    }

    AmdnrXrSupplyDepth(&frame);

    // Every 300 frames, the same cadence the downstream sampler reports on, so the table and the
    // numbers that came out of it can be read as one picture.
    if (g_published == 1 || g_published - g_loggedAt >= 300) {
        g_loggedAt = g_published;
        LogCandidates();
    }
}

// Detach runs from the layer's instance teardown in the normal case. This is the net beneath it: a
// table left pointing into this module after it is unloaded would turn the next draw call into a jump
// into freed memory.
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        DepthCaptureDetach();
    }
    return TRUE;
}
