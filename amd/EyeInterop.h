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

#include "Fsr4Upscale.h"
#include "LayerLog.h"
#include "NrCore.h"

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
    // `eye` tags the timing totals, so the two views can be told apart.
    // `motion` describes where this view's pixels were in the previous answer; it is what lets the
    // runtime's temporal path reproject its history instead of starting from nothing every frame. A
    // view whose motion is not known (`valid` false) is processed as a single frame.
    bool Process(uint32_t eye,
                 ID3D11Texture2D* eyeTexture,
                 uint32_t arraySlice,
                 const RECT& box,
                 uint32_t networkWidth,
                 uint32_t networkHeight,
                 bool srgb,
                 const NrMotion& motion);

    bool IsInitialized() const {
        return m_d3d12Device != nullptr;
    }

    // Upscales `sourceRect` of `source` (array slice `sourceSlice`) into the whole of `dest` with
    // FidelityFX FSR 4, on the D3D12 device the neural renderer already owns: the view's pixels are
    // copied into a shared texture, upscaled there, and copied back out. Both textures belong to the
    // application's device.
    //
    // False means the effect is not available here or declined this frame, and the caller is
    // expected to run its own pass instead. It never leaves `dest` partly written.
    // `jitterX` / `jitterY` are the sub-pixel offset the caller put under the application's frustum
    // for this frame, in render pixels. `motion` is where this view's pixels were in the previous
    // frame; the reprojection field the effect reprojects its history with is built from it here, on
    // the application's own device, and handed across as a shared texture -- see BuildMotionField for
    // why it is not built on the effect's device.
    //
    // `eye` picks the effect: each view has its own, for the same reason the network has one session
    // per eye. The history a temporal upscaler reprojects lives inside the context that owns it, so
    // two views sharing one would each warp the other's previous frame onto their own picture.
    bool UpscaleFsr4(uint32_t eye,
                     ID3D11Texture2D* source,
                     uint32_t sourceSlice,
                     const RECT& sourceRect,
                     ID3D11Texture2D* dest,
                     uint32_t destSlice,
                     float jitterX,
                     float jitterY,
                     const NrMotion& motion);

    // Offers the reduced depth from the application's own buffer to the motion pass, so the
    // reprojection can carry the head's translation as well as its rotation. A null handle leaves the
    // field rotation-only, which is how it ran before there was a depth at all, and so does a handle
    // that cannot be opened. Cheap to call every frame: each session opens the texture once and keeps
    // it until the handle changes.
    void SetDepthSource(HANDLE handle, uint32_t width, uint32_t height);

    // Per-stage totals accumulated since the last call, so the caller can report where a frame's
    // time actually goes instead of only how long the whole pass took. Resets on read. Kept per eye:
    // the first view pays for the game's frame draining, the second one should not, and only the
    // per-eye numbers show whether a fixed wait is being paid twice.
    void TakeStageTotals(uint32_t eye, double& crop, double& producer, double& network, double& consumer, double& paste);
    // The producer's own split over the same window: recording the view, submitting it, and then
    // waiting on the queue -- the last of which is the one that says whose stall it is. `pending` is
    // how many of those passes found the D3D11 crop still in flight.
    void TakeProducerSplit(uint32_t eye, double& record, double& submit, double& wait, uint32_t& pending);
    // The consumer's own split over the same window: recording the answer, submitting it, and the
    // queue drain that follows. The producer split above says whose stall the first wait is; this one
    // says whether the second wait is the network still running or a bubble in this layer's own queue.
    // It is the other half of the reading that decides whether the two eyes could ever overlap.
    void TakeConsumerSplit(uint32_t eye, double& record, double& submit, double& wait);

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
    // Waits until the queue is idle: everything recorded on it, by this layer or by the runtime on the
    // layer's behalf, is complete when this returns. Called before a rebuild replaces resources that
    // work may still be reading.
    void DrainQueue();
    // Releases the pair the last rebuild retired once the fence recorded with it has passed. A slot
    // still covered by un-finished work is left alone; the next call after the fence passes collects it.
    void CollectRetired();
    // Moves the current crop/result pair into the retired slot instead of dropping it where it stands.
    void RetireCurrent();

    // One eye's pass that has been handed to the runtime but not collected yet, for the asynchronous
    // layout: what the answer being collected describes, and where it has to be carried to.
    struct PendingPass {
        bool pending{false};
        // The crop rectangle that pass read. The collection below blends the answer against *this*
        // frame's crop, so the difference between the two rectangles is the shift the consumer shader
        // reads both sides through.
        RECT region{};
        uint32_t networkWidth{0};
        uint32_t networkHeight{0};
    };

    // What collecting one in-flight pass cost, for the log and the per-window totals.
    struct FinishTimes {
        double answer{0};
        double record{0};
        double submit{0};
        double gpu{0};
        double paste{0};
        bool ran{false};
        bool answered{false};
    };

    // Waits out an in-flight pass's answer, turns it back into the eye image and pastes it into the
    // application's eye texture. `wait` true is the synchronous layout: the CPU sits through the
    // consumer's GPU work. False leaves that wait to the application's own queue -- nothing on this
    // thread blocks on the consumer, and the copy is ordered by the fence instead.
    bool FinishPending(uint32_t slot,
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
                       FinishTimes* times);
    // Closes, executes and signals `list`. `wait` decides whether the CPU then waits for the GPU to
    // finish it.
    // The two timings are optional: the producer stage splits them out so a slow pass says how much
    // of the wait is CPU work and how much is the queue stalling on the GPU.
    bool Execute(ID3D12GraphicsCommandList* list, uint64_t queueWaitValue, bool wait,
                 double* submitMs = nullptr, double* gpuWaitMs = nullptr);
    bool ExecuteAndWait(uint64_t queueWaitValue, double* submitMs = nullptr, double* gpuWaitMs = nullptr);
    // Resets the allocator/list pair for a new recording, recovering from a list a previous pass left
    // open. D3D12 will not reset an allocator while a list recorded from it is still open, so without
    // that recovery one failed pass would fail every later frame on the allocator rather than on its
    // own cause, and the layer would stop running for the rest of the session. False with
    // `m_lastError` set.
    bool BeginRecording(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, const char* what);
    // Closes a list whose recording is being abandoned. Nothing about it is submitted, so closing it
    // is all that is needed to let its allocator be reset again.
    static void Abandon(ID3D12GraphicsCommandList* list);
    bool ReadBack(ID3D11Texture2D* source, uint32_t arraySlice, const RECT& box, std::vector<uint8_t>& pixels);
    // Captures one view's before/after for inspection, tagged by `eye`. Returns false for frames that
    // carry no usable content (a loading or menu frame is nearly black), so the caller can retry
    // later. Both views write their own files, so the pair can be read as a stereo pair.
    bool DumpComparison(uint32_t eye, ID3D11Texture2D* eyeTexture, uint32_t arraySlice, const RECT& box, uint32_t width, uint32_t height);
    // Builds this view's reprojection field, on the application's own device, into `motion[slot]`, and
    // leaves its shared handle in `handle`. Returns false when no field could be built, which is a
    // frame the effect is handed the zero field for rather than a frame that fails.
    //
    // It is built here, on D3D11, rather than on the effect's own D3D12 device deliberately. The
    // effect's dispatch runs on that device, and a recording made on the same command list that writes
    // to the effect's own shader-visible descriptor heap corrupts the dispatch before it is even
    // called -- a fault with no runtime message and a device removal. Keeping every input the layer
    // produces on the device that produces it, and crossing to the effect only through a shared
    // texture, is the same path the colour and the depth already take.
    bool BuildMotionField(uint32_t slot, const NrMotion& motion, uint32_t width, uint32_t height, HANDLE& handle, uint32_t& fieldWidth, uint32_t& fieldHeight);
    // The field's compute shader, compiled on first use. False latches the failure, so a machine
    // without d3dcompiler_47.dll is asked once rather than every frame.
    bool EnsureMotionShader();
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

    // The crop/result pair the last rebuild replaced. It is not released where it stands: it waits here
    // until the fence value recorded at retirement has completed, which is the one thing a CPU-side
    // release cannot ask about the two readers it cannot see -- the queue and the runtime's own work.
    // AMDNR 0.3.3.2 rebuilt its import pool for the same reason (AMD HIP never frees a mapped import
    // once the host drops one mid-flight, so old buffers are reused rather than re-imported).
    ComPtr<ID3D11Texture2D> m_retiredCrop11;
    ComPtr<ID3D12Resource> m_retiredCrop12;
    ComPtr<ID3D11Texture2D> m_retiredResult11;
    ComPtr<ID3D12Resource> m_retiredResult12;
    // The fence value at which that pair was retired, or 0 when the slot is empty.
    uint64_t m_retiredAt{0};

    // One in-flight pass per eye (see PendingPass). In the asynchronous layout the pass handed over at
    // the end of one frame is collected at the start of the next, which is what gives the answer a
    // whole frame to arrive in and keeps this thread off the GPU's critical path; the synchronous
    // layout collects it in the frame that handed it over, and these slots are then always empty
    // between frames.
    static constexpr uint32_t kMaxEyes = 2;
    PendingPass m_pending[kMaxEyes];
    // Which of the two layouts runs (see Process). Off, and AMDNR_XR_ASYNC=1 turns the asynchronous one
    // on for a measurement: as built it hung the device -- two sessions, the second after a geometry
    // change, with the runtime's own drain reporting a failed GPU drain and D3D11 coming back
    // 0x887A0005/0x887A0006. Whatever the ordering mistake is, it is not one to leave loaded by
    // default; the synchronous layout is the one that has run clean.
    bool m_async{false};

    // FSR 4 for the output pass: one effect per eye on the D3D12 device, each brought up the first
    // time a frame asks for it. It holds a history -- the frames it accumulates are the ones the
    // jitter and the motion field are aimed at -- so the two views cannot share one any more than the
    // network's two sessions can. `m_fsr4GaveUp` latches an eye the effect cannot serve here, so the
    // attempt is made once rather than every frame.
    Fsr4Upscale m_fsr4[kMaxEyes];
    bool m_fsr4Tried[kMaxEyes]{};
    bool m_fsr4GaveUp[kMaxEyes]{};

    // The reprojection field each effect reprojects its history with, built on the application's own
    // device and carried across to the effect through a shared handle. One per eye: the two effects
    // keep separate histories, so each needs its own field -- and a field reused between them could be
    // overwritten before the first dispatch had read it, because nothing on this side waits for that
    // dispatch to run.
    //
    // A texture written through a UAV cannot itself be shared, so the pass writes `work` and copies it
    // into `shared`, which is what holds the handle the effect opens.
    struct MotionField {
        ComPtr<ID3D11Texture2D> work;                // the UAV target, not shared
        ComPtr<ID3D11UnorderedAccessView> view;      // its view
        ComPtr<ID3D11Texture2D> shared;              // the copy the handle names, shader-resource only
        ComPtr<ID3D11Buffer> constants;              // the transform, rewritten every frame
        HANDLE handle{nullptr};
        uint32_t width{0};
        uint32_t height{0};
    };
    MotionField m_motion[kMaxEyes];
    ComPtr<ID3D11ComputeShader> m_motionCs;
    bool m_motionGaveUp{false};
    // The depth the field measures its parallax against, viewed on the application's device. Borrowed
    // from the capture rather than held: the reduction is rebuilt whenever its size changes, and
    // `m_depthTexture` is what says whether the view still describes the texture in hand.
    ComPtr<ID3D11ShaderResourceView> m_motionDepthSrv;
    ID3D11Texture2D* m_motionDepthTexture{nullptr};

    // One runtime session per eye. The history the temporal path reprojects lives inside the session
    // that owns it, and a stereo frame is two separate runs, so two eyes sharing one session would
    // each reproject the other eye's previous answer onto their own picture. Two sessions is what
    // makes the effect uniform across the frame -- and uniformity is the only way it can be judged,
    // because nobody can compare a treated eye against an untreated one inside a headset.
    NrCore m_nr[kMaxEyes];
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

    // The producer stage split three ways, summed over the same window. The producer is the one stage
    // that waits, and where the wait belongs decides whether the automatic tier can do anything about
    // it: a wait that is this layer's own queue shrinks when the window shrinks, and one that is the
    // application's does not.
    double m_producerRecord[kMaxEyes]{};
    double m_producerSubmit[kMaxEyes]{};
    double m_producerWait[kMaxEyes]{};
    // Passes in the window where the D3D11 crop had not landed yet when the view was handed over.
    uint32_t m_producerPending[kMaxEyes]{};

    // The consumer stage split the same three ways. Its drain is the second and last CPU block of a
    // pass, and the only one whose size nothing else in the log attributes: it is spent waiting for
    // this layer's own queue to finish the answer's conversion back to the eye buffer.
    double m_consumerRecord[kMaxEyes]{};
    double m_consumerSubmit[kMaxEyes]{};
    double m_consumerWait[kMaxEyes]{};

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

    const char* m_lastError{""};
};
