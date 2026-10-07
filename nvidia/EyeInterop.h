// D3D11 <-> D3D12 bridge for the eye buffers an OpenXR application hands to xrEndFrame.
//
// The application renders with D3D11 and the neural renderer needs D3D12, so a rectangle of the
// eye buffer is copied into a shareable D3D11 texture, opened on a D3D12 device, processed, and
// copied back. Both devices live on the same adapter and are synchronised with a shared fence.

#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <vector>

#include <wrl/client.h>

#include "LayerLog.h"
#include "NgxNr.h"

using Microsoft::WRL::ComPtr;

class EyeInterop {
  public:
    bool Initialize(ID3D11Device* appDevice, ID3D11DeviceContext* appContext);
    void Shutdown();

    // Processes the `box` rectangle of `eyeTexture` (array slice `arraySlice`) in place.
    //
    // `box` may be larger than the network accepts; the pass then squeezes it into
    // `networkWidth` x `networkHeight` and stretches the answer back. `srgb` tells the pass that the
    // eye buffer holds sRGB encoded colour, so the network sees linear light on the way in and the
    // answer is encoded again on the way out.
    // `eye` tags the timing totals, so the two views can be told apart, and names the view whose crop
    // is kept for the other one to be built from.
    // `motion` describes where this view's pixels were in the previous answer; it is what lets the
    // runtime's temporal path reproject its history instead of starting from nothing every frame. A
    // view whose motion is not known (`valid` false) is processed as a single frame.
    // `fromDetail` runs the view without the network: its own untouched crop plus the difference the
    // network made to the other view, moved across by the parallax between them. It needs the first
    // view to have gone through this call already, which view order guarantees.
    bool Process(uint32_t eye,
                 ID3D11Texture2D* eyeTexture,
                 uint32_t arraySlice,
                 const RECT& box,
                 uint32_t networkWidth,
                 uint32_t networkHeight,
                 bool srgb,
                 const NrMotion& motion,
                 bool fromDetail);

    bool IsInitialized() const {
        return m_d3d12Device != nullptr;
    }

    // The parallax the second view is being rebuilt with, for the frame report. Zero until the search
    // has run once, which is the first frame after the pair comes up.
    int32_t DetailShift() const {
        return m_shiftValid ? m_shift : 0;
    }

    // Per-stage totals accumulated since the last call, so the caller can report where a frame's
    // time actually goes instead of only how long the whole pass took. Resets on read. Kept per eye:
    // the first view pays for the game's frame draining, the second one should not, and only the
    // per-eye numbers show whether a fixed wait is being paid twice.
    void TakeStageTotals(uint32_t eye, double& crop, double& producer, double& network, double& consumer, double& paste);

    // Split of the network stage, to tell a runtime drain apart from waiting on the answer.
    void TakeLaunchTotals(double& enqueue, double& wait) {
        enqueue = 0;
        wait = 0;
        for (uint32_t slot = 0; slot < kMaxEyes; slot++) {
            double slotEnqueue = 0;
            double slotWait = 0;
            m_nr[slot].TakeLaunchTotals(slotEnqueue, slotWait);
            enqueue += slotEnqueue;
            wait += slotWait;
        }
    }

    const char* LastError() const {
        return m_lastError;
    }

  private:
    // Creates the shareable D3D11 texture and its D3D12 twin that every crop/result pair here is
    // made of. Returns false with `m_lastError` set.
    bool CreateSharedPair(DXGI_FORMAT format,
                          uint32_t width,
                          uint32_t height,
                          ComPtr<ID3D11Texture2D>& shared11,
                          ComPtr<ID3D12Resource>& shared12);

    bool EnsureTextures(DXGI_FORMAT format, uint32_t width, uint32_t height);
    // Waits until the queue is idle -- everything recorded on it, by this layer or by the runtime on
    // its behalf, is complete when this returns. Called before a rebuild replaces resources that work
    // may still be reading (see the three Ensure* families in the .cpp).
    void DrainQueue();
    // Releases the pair the last rebuild retired once the fence recorded with it has passed.
    void CollectRetired();
    // Moves the current crop/result pair into the retired slot instead of dropping it where it stands.
    void RetireCurrent();
    // The crop/result pair for one eye of the asynchronous path. Each eye has a job in flight at the
    // same time, so they cannot share the single pair the synchronous path reuses.
    bool EnsureAsyncTextures(uint32_t eye, DXGI_FORMAT format, uint32_t width, uint32_t height);
    // The asynchronous path: this frame's crop goes to the network and the previous frame's answer is
    // what gets pasted back, so no frame ever waits for its own network pass.
    bool ProcessAsync(uint32_t eye,
                      ID3D11Texture2D* eyeTexture,
                      uint32_t arraySlice,
                      const RECT& box,
                      uint32_t networkWidth,
                      uint32_t networkHeight,
                      bool srgb,
                      const NrMotion& motion);
    // What the answer being collected has to be read through to land correctly on this frame's window,
    // as a fraction of the image. Two things separate the answer from the frame it is pasted into: the
    // head has rotated since it was produced, and the window has moved since it was cropped. Both are
    // in `motion`. False when no motion is known, which is the first frame of a run.
    static bool WindowShift(const NrMotion& motion,
                            uint32_t networkWidth,
                            uint32_t networkHeight,
                            float shift[2]);
    // The first view's crop and the network's answer to it, held for the second view. Plain D3D12
    // textures: only a shader reads them, so they need neither sharing nor a CPU view.
    bool EnsureLeftTextures(DXGI_FORMAT format, uint32_t width, uint32_t height);
    // Closes, executes and signals `list`. `wait` decides whether the CPU then waits for the GPU to
    // finish it: the asynchronous path deliberately does not, because the list is this frame's
    // producer, whose result the next frame collects anyway. Leaving the wait until then is what takes
    // the application's own GPU frame off this frame's critical path.
    bool Execute(ID3D12GraphicsCommandList* list, uint64_t queueWaitValue, bool wait);
    bool ExecuteAndWait(uint64_t queueWaitValue);
    bool ResetForNextFrame(uint32_t slot, ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list);
    bool ReadBack(ID3D11Texture2D* source, uint32_t arraySlice, const RECT& box, std::vector<uint8_t>& pixels);
    // Captures one view's before/after for inspection, tagged by `eye`. Returns false for frames that
    // carry no usable content (a loading or menu frame is nearly black), so the caller can retry
    // later. Both views write their own files, so the pair can be read as a stereo pair.
    bool DumpComparison(uint32_t eye, ID3D11Texture2D* eyeTexture, uint32_t arraySlice, const RECT& box, uint32_t width, uint32_t height);
    void Fail(const char* what, HRESULT hr);

    ComPtr<ID3D11Device> m_appDevice;
    ComPtr<ID3D11Device5> m_appDevice5;
    ComPtr<ID3D11DeviceContext> m_appContext;
    ComPtr<ID3D11DeviceContext4> m_appContext4;

    ComPtr<ID3D12Device> m_d3d12Device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Shared fence used to order the two devices against each other.
    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D11Fence> m_fence11;
    HANDLE m_fenceEvent{nullptr};
    uint64_t m_fenceValue{0};

    // Shareable crop / result pair, recreated whenever the crop geometry changes.
    ComPtr<ID3D11Texture2D> m_cropShared11;
    ComPtr<ID3D12Resource> m_cropShared12;
    ComPtr<ID3D11Texture2D> m_resultShared11;
    ComPtr<ID3D12Resource> m_resultShared12;
    DXGI_FORMAT m_textureFormat{DXGI_FORMAT_UNKNOWN};
    uint32_t m_textureWidth{0};
    uint32_t m_textureHeight{0};

    // The pair the last rebuild replaced, kept until the fence value recorded at its retirement has
    // completed (see DrainQueue/CollectRetired/RetireCurrent). Same mechanism as the AMD tree's.
    ComPtr<ID3D11Texture2D> m_retiredCrop11;
    ComPtr<ID3D12Resource> m_retiredCrop12;
    ComPtr<ID3D11Texture2D> m_retiredResult11;
    ComPtr<ID3D12Resource> m_retiredResult12;
    uint64_t m_retiredAt{0};

    // The first view's crop and the network's answer to it, kept while the second view is built. The
    // working pair above is reused by both views, so the second view's own crop overwrites the first
    // one's the moment it is taken; without this copy there would be nothing left to carry across.
    ComPtr<ID3D12Resource> m_leftCrop;
    ComPtr<ID3D12Resource> m_leftOutput;
    DXGI_FORMAT m_leftFormat{DXGI_FORMAT_UNKNOWN};
    uint32_t m_leftWidth{0};
    uint32_t m_leftHeight{0};
    // Cleared whenever those textures are rebuilt, so a view can never be built from a stale pair.
    bool m_leftValid{false};

    // The parallax between the two views, as found by the search shader. It is measured on the frame
    // the second view is rebuilt for and spent on the next one: the shift has to be in hand before the
    // transfer is recorded, and the search can only report once the list carrying it has run. The two
    // eyes sit a fixed distance apart looking at the same room, so one frame's answer is the next
    // frame's answer as long as nobody walks into the shot.
    int32_t m_shift{0};
    bool m_shiftValid{false};
    bool m_transferLogged{false};

    static constexpr uint32_t kMaxEyes = 2;

    // One runtime session per eye. The history the temporal path reprojects lives inside the session
    // that owns it, and a stereo frame is two separate runs, so two eyes sharing one session would
    // each reproject the other eye's previous answer onto their own picture. Two sessions is what
    // makes the effect uniform across the frame -- and uniformity is the only way it can be judged,
    // because nobody can compare a treated eye against an untreated one inside a headset.
    NgxNr m_nr[kMaxEyes];
    // How many of them came up. With the temporal path off there is no history to keep apart, so a
    // single session still serves both eyes the way it always has.
    uint32_t m_activeSessions{0};

    // The network's answer lands darker than its input, so the consumer bends it back. `m_gamma` is
    // 1/1.055 because the dumps show the answer comes back as roughly in^1.055 in linear light; that
    // puts the window's brightness back in step with the rim to within about a percent.
    float m_gain{1.f};
    float m_gamma{0.95f};
    // The rim -- how much of the window's shorter side is a ramp between filtered and unfiltered
    // rather than a step, and how far that ramp is bent from the window's rectangle towards the
    // ellipse inscribed in it -- is not cached here. Both are per-frame blend parameters, so they live
    // in the shared settings block and are read at the point of use, which is what lets the panel move
    // them while the title runs (see NrSettings::feather / ::roundness). These two are only the values
    // last written to the log, so a change shows up there and not just in the panel's own echo.
    float m_loggedFeather{-1.f};
    float m_loggedRoundness{-1.f};
    // Re-armed whenever the crop textures are rebuilt, so a new window size says so in the log too.
    bool m_featherLogged{false};

    double m_stageCrop[kMaxEyes]{};
    double m_stageProducer[kMaxEyes]{};
    double m_stageNetwork[kMaxEyes]{};
    double m_stageConsumer[kMaxEyes]{};
    double m_stagePaste[kMaxEyes]{};

    // Counted per eye. Both views pass through here once a frame, and a single shared counter made
    // every trigger land on the same view -- the counter advances by two per frame, so an even
    // trigger point is always the second view and the first was never captured at all. Counting each
    // view separately makes the trigger fall on the same frame for both, which is what turns the two
    // captures into a stereo pair. A frame that fails does so before either counter moves, so the two
    // can only drift apart if one view fails alone, which aborts the frame anyway.
    uint32_t m_processCounter[kMaxEyes]{};
    // How many pairs this view has written. The first frame with readable content is not the frame
    // worth measuring: a bright, almost featureless view -- a loading fade, a wall in soft light --
    // passes the darkness gate and then yields two crops that differ by half a grey level, which is
    // no disparity signal at all. Gating on detail and keeping a few pairs means the captures land on
    // frames that actually carry a scene.
    uint32_t m_dumpWrites[kMaxEyes]{};
    // The capture maps staging textures synchronously, which flushes the GPU pipeline, so it stays
    // off unless AMDNR_XR_DUMP is set. On by default it would stall every frame spent on a dark
    // screen, and a level load is exactly that.
    bool m_dumpEnabled{false};

    // ---- Asynchronous path (AMDNR_XR_ASYNC) ----
    //
    // Every frame used to run its own crop, network pass and paste before it could be submitted, so
    // the application's render thread sat inside this file for the whole 18-30 ms. With this on, the
    // frame's crop is handed to the network and the *previous* frame's answer is what gets pasted
    // back: the same work, one frame later, with nothing on this frame's critical path but the crop
    // and the paste. The price is one frame of staleness inside the neural window.
    //
    // The window is the region the crop covers, so what is pasted is the previous frame's picture of
    // that region, read through the head's own motion so that it lands under this frame's pose. What
    // is left of the trade is the age of the content, not its alignment: an object that moved inside
    // the world between the two frames is one frame behind inside the window, because a layer can see
    // the eye buffer and nothing else. That, and not the window's placement, is why this is a switch
    // and not the default.
    bool m_async{false};
    bool m_asyncLogged{false};

    // One crop/result pair per eye: both eyes have a job in flight at once, so the single pair the
    // synchronous path reuses would be overwritten by the other eye's crop.
    ComPtr<ID3D11Texture2D> m_asyncCrop11[kMaxEyes];
    ComPtr<ID3D12Resource> m_asyncCrop12[kMaxEyes];
    ComPtr<ID3D11Texture2D> m_asyncResult11[kMaxEyes];
    ComPtr<ID3D12Resource> m_asyncResult12[kMaxEyes];
    DXGI_FORMAT m_asyncFormat{DXGI_FORMAT_UNKNOWN};
    uint32_t m_asyncWidth{0};
    uint32_t m_asyncHeight{0};

    // What the job in flight for an eye still needs when it is collected next frame.
    struct AsyncJob {
        bool pending{false};
        // The network input size the job was cropped for, because the consumer resamples the answer
        // back to the window with the size the job was cropped for and not this frame's.
        uint32_t networkWidth{0};
        uint32_t networkHeight{0};
    };
    AsyncJob m_asyncJob[kMaxEyes];
    // Frames since this eye last reported what its answers are being carried by, and the largest shift
    // seen in that window. The shift is the one number this path can get wrong while still producing a
    // picture, so it goes out as a range rather than as a sample: the window travels on a coarse grid,
    // so most frames carry nothing at all and the few that carry something are the whole story. A
    // sample of it, or an average, hides exactly those frames.
    uint32_t m_asyncShiftFrames[kMaxEyes]{};
    float m_asyncShiftPeak[kMaxEyes]{};
    // The fence value that says the consumer's list has finished, so the paste can be ordered after
    // it without waiting on the CPU for the whole queue.
    uint64_t m_asyncConsumerDone[kMaxEyes]{};
    // When the queue finished with the last producer list. That list is submitted without a wait on
    // purpose, so this is the only thing that says whether its allocator may be reset yet.
    uint64_t m_asyncProducerDone[kMaxEyes]{};

    // One allocator and one list per eye. The producer's list is executed without waiting, so the two
    // eyes cannot share the allocator the synchronous path uses: a command allocator may not be reset
    // while a list recorded from it is still executing, and waiting for the other eye's producer here
    // would put back the very stall this path exists to remove.
    ComPtr<ID3D12CommandAllocator> m_asyncAllocator[kMaxEyes];
    ComPtr<ID3D12GraphicsCommandList> m_asyncList[kMaxEyes];

    const char* m_lastError{""};
};
