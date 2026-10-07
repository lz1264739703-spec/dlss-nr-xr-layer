// The lmxxf neural renderer, wrapped for the OpenXR layer.
//
// Owns the runtime session, the fp16 conversion pipeline and the working textures. The pass is
// split into a producer and a consumer stage because the runtime has to see the producer command
// list executed before it is asked to run, and the host has to wait for the answer before the
// consumer command list can be recorded.

#pragma once

#include <windows.h>

#include <d3d12.h>

#include <chrono>
#include <cstdint>
#include <string>

#include <wrl/client.h>

#include "LayerLog.h"
#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;

// One view's motion field, as the network's temporal path wants it: for every pixel of the network
// input, where that pixel's content sat in the previous answer.
//
// The layer cannot ask the application for motion vectors -- an OpenXR layer sees the finished eye
// buffer and nothing else -- but it does not need to. The eye buffer is a camera image, so the
// dominant motion is the head's own rotation, and that is a pure function of the two view poses:
// rotating a pixel's view ray into the previous frame's view gives the previous pixel exactly.
//
// That is exact for rotation and wrong for translation, which is what the depth below is for. Moving
// the head sideways slides near surfaces further across the image than far ones, and without a per
// pixel distance there is no way to tell the two apart -- so the reprojection has to assume every
// pixel sits at infinity and the parallax turns into smearing on whatever is close. With a distance
// the same reprojection carries the translation as well.
struct NrMotion {
    // Takes a view-space direction in this frame to the previous frame's view space.
    float matrix[9]{};
    // Network pixel -> normalised image coordinate: s = a + (p + 0.5) * b, per axis.
    float a[2]{};
    float b[2]{};
    // Half-tangents of the view frustum, and their spans.
    float tanLeft{0.f};
    float tanWidth{0.f};
    float tanUp{0.f};
    float tanHeight{0.f};
    // Converts the normalised displacement into network pixels: the view's image extent times the
    // resample scale.
    float extentScale[2]{};
    // Carries the window's own travel into the answer's pixel grid. The window jumps on a coarse
    // grid, and without this the history would appear to slide sideways whenever it did.
    float originShift[2]{};
    // Where the camera moved to between the two frames, in the previous frame's view basis and in
    // metres. All zero leaves the reprojection exactly what it was without a depth.
    float translation[3]{};
    // Turns the inverse distance the depth carries back into 1/z: the depth is `1 - raw`, which for
    // a standard projection is the near plane over z, so this is one over the near plane. Zero is
    // what switches the parallax term off, and it is the default because the near plane is not
    // something an OpenXR layer is told -- it has to be supplied.
    float invNear{0.f};
    // Where this view's rectangle sits inside the packed eye image the depth describes, as fractions
    // of that image, so a position in the view can be turned into a position in the depth.
    float depthOrigin[2]{};
    float depthScale[2]{};
    bool valid{false};
};

// Geometry and look of one conversion pass.
//
// The source and the destination differ in size whenever the neural window covers more of the eye
// buffer than the network's input ceiling allows: the window is then squeezed into the network and
// stretched back on the way out. Everything else about the two directions is asymmetric:
//
//   producer  colour crop -> network input     no blend, no encode
//   consumer  network answer -> colour crop    blends against the untouched crop over `feather`
//                                              pixels and undoes the network's tone drift
struct NrConvert {
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t destinationWidth{0};
    uint32_t destinationHeight{0};
    bool encodeSrgb{false};
    bool blend{false};
    uint32_t feather{0};
    // How round the feathered region is: 0 leaves it the rectangle the window is, 1 turns it into the
    // ellipse inscribed in it. Four straight edges meeting at four corners is the shape a rim reads
    // as, and it reads worst at the corners, where the band has to change direction inside a few
    // pixels. Bending it towards a circle moves the transition off them.
    float roundness{0.f};
    float gain{1.f};
    float gamma{1.f};
    // Where the source is read for a given destination pixel, as a fraction of the image. Only the
    // asynchronous path sets it: the answer it writes describes last frame's crop, and without this the
    // window would sit one frame behind the rest of the frame while the head turns.
    float shiftX{0.f};
    float shiftY{0.f};
};

// What the anti-flicker filter runs with on this frame, as the consumer's shader reads it. The
// history itself is a pair of the layer's own textures (see NrCore); this is only the knobs and
// whether there is anything in them to read yet.
struct NrFilter {
    bool on{false};
    float strength{0.f};
    // How far the correction may move, per channel, before the history is dropped entirely for that
    // pixel and the current frame is taken instead.
    float gate{0.f};
    bool historyValid{false};
    // Which view's history this run reads and writes. A stereo frame runs the consumer once per eye,
    // and each eye's history has to hold that eye's own corrections: a shared pair hands one eye the
    // other's picture, which reads as a ghost offset by the stereo parallax.
    uint32_t eye{0};
};

class NrCore {
  public:
    // Looks for LmxxfNrRuntime.dll / .pak next to the layer and in the staged build folder.
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();
    bool IsReady() const {
        return m_context != nullptr;
    }

    // Whether this core was asked to drive the runtime's temporal path. The host uses it to decide
    // how many sessions to open, since a history belongs to the session that owns it.
    bool TemporalEnabled() const {
        return m_temporal;
    }

    uint32_t MaxInputWidth() const {
        return m_maxWidth;
    }
    uint32_t MaxInputHeight() const {
        return m_maxHeight;
    }
    const char* LastError() const {
        return m_lastError;
    }

    // Split of the launch stage, so a cost that does not move with the input size can be pinned on
    // the enqueue (the runtime draining our queue) or on the wait for the answer. Resets on read.
    void TakeLaunchTotals(double& enqueue, double& wait);

    // Producer stage: converts `source` into the fp16 network input and records the runtime's input
    // reads. `sourceViewFormat` is the format the conversion samples through, so the caller decides
    // whether the hardware decodes sRGB. The command list is expected to be executed before
    // Launch() is called.
    bool RecordProducer(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* source,
                        DXGI_FORMAT sourceViewFormat,
                        const NrConvert& convert,
                        const NrMotion& motion);
    ID3D12Resource* InputFp16() const {
        return m_inputFp16.Get();
    }

    // Runs the network and waits for its answer, mirroring the sequence the probe validated.
    //
    // The two halves are also callable on their own, which is what the asynchronous path needs: this
    // frame enqueues the job and the next frame collects it, so the wait no longer sits in the same
    // frame as the enqueue. Split apart, the answer has a whole frame to arrive instead of the few
    // milliseconds a synchronous frame leaves for it.
    bool Launch();
    // Puts this frame's job on the queue. The producer's command list has to have been executed
    // first, because the job's input reads are recorded into it.
    bool Enqueue();
    // Polls until the answer is ready, then leaves the job open for RecordConsumer. Returns false and
    // abandons the job if the network never answers.
    bool WaitAnswer();

    // Consumer stage: copies the answer locally, releases the job and writes the result into
    // `destination`. `original` is the same region before the pass, used to feather the rim; it may
    // be null when `convert.blend` is off. `encodeSrgb` makes the conversion undo the decode done on
    // the way in.
    bool RecordConsumer(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* destination,
                        DXGI_FORMAT destinationFormat,
                        ID3D12Resource* original,
                        DXGI_FORMAT originalViewFormat,
                        const NrConvert& convert,
                        uint32_t eye);

    // Opens the layer's depth reduction on this device, so the motion pass can put a real distance
    // under each pixel instead of treating every one as infinitely far. `handle` is a D3D11 shared
    // handle on the texture the reduction renders into; it has to be offered again whenever that
    // texture is rebuilt. A null handle drops whatever was held and leaves the motion rotation-only.
    bool SetDepthSharedHandle(HANDLE handle, uint32_t width, uint32_t height);

  private:
    bool LoadRuntime();
    bool EnsureConvertPipeline();
    bool EnsureMotionPipeline();
    bool RecordMotion(ID3D12GraphicsCommandList* cmd, const NrMotion& motion, uint32_t width, uint32_t height);
    bool EnsureTextures(uint32_t width, uint32_t height);
    bool EnsureResultTexture(uint32_t width, uint32_t height, DXGI_FORMAT format);
    // The history pairs, one per eye, at the size the consumer writes (the colour crop). All four
    // are allocated together and all four are dropped together: a pair that disagreed about the
    // image would be worse than no history at all.
    bool EnsureFilterTextures(uint32_t width, uint32_t height);
    // Copies a box of the history that was just written into a readback buffer, on the sampling
    // frames only, and reads the previous sample back on the frame after. Reading one frame late is
    // what makes it free: the frame that recorded the copy was submitted and waited on before the
    // next one runs, so the map cannot stall the GPU.
    void ProbeFilterHistory(ID3D12GraphicsCommandList* cmd, uint32_t written, uint32_t eye);
    void ReadFilterProbe();
    // Everything that invalidates one history invalidates the other, because they describe the same
    // image: a rebuild, a control change, a near-plane change, a resolution change, a gap, the
    // panel's reset. Written once here so no caller can forget the second one.
    void InvalidateHistory();
    // Never called directly: the producer and the consumer both go through it.
    void RecordConvert(ID3D12GraphicsCommandList* cmd,
                       ID3D12Resource* source,
                       DXGI_FORMAT sourceViewFormat,
                       ID3D12Resource* original,
                       DXGI_FORMAT originalViewFormat,
                       ID3D12Resource* destination,
                       DXGI_FORMAT destinationFormat,
                       const NrConvert& convert,
                       const NrFilter& filter);
    void Fail(const char* message);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;

    HMODULE m_module{nullptr};
    LmxxfNrApi* m_api{nullptr};
    // The runtime's import-pool export (0.3.3.2 and later), resolved with GetProcAddress like the API
    // itself: it answers how well the buffers its HIP bridge imports are being reused, which is the
    // number that says whether a rebuild re-imports them or is served from the pool. Null on an older
    // runtime, and the status line then leaves the numbers out rather than reporting zeros.
    int32_t (*m_poolStats)(LmxxfNrImportPoolStats*){nullptr};
    void* m_context{nullptr};
    LmxxfNrJob m_job{};
    std::wstring m_assetsPath;
    uint32_t m_maxWidth{1920};
    uint32_t m_maxHeight{1080};
    uint32_t m_frameIndex{0};
    bool m_warmedUp{false};

    // Tuning knobs, read once from the environment. They exist so the effect can be made obvious
    // while judging it: one pass at full strength is a subtle denoise, several passes are not.
    float m_strength{1.f};
    // ---- Anti-flicker ----
    //
    // The layer's own temporal filter on the network's correction, because this path has no other.
    // The runtime is loaded without its temporal history here, so every frame's answer is a fresh
    // guess and the detail it invents moves from frame to frame; the frame's output_smooth fields are
    // read by no runtime implementation this layer loads, and the smoothing upstream does ship is not
    // compiled into it. A pair of textures per eye holds that eye's previous frame's filtered
    // correction, ping-ponged so this frame reads one while writing the other; `m_filterRead` says
    // which is which. The histories are per eye because the corrections carry each view's own
    // picture: one shared pair blends the left eye's history into the right eye's run, and with the
    // stereo parallax between them that reads as a ghost.
    ComPtr<ID3D12Resource> m_filterHistory[2][2];
    uint32_t m_filterWidth{0};
    uint32_t m_filterHeight{0};
    uint32_t m_filterRead[2]{};
    // False until a history that describes the current image exists, per eye: the first frame with
    // the filter on, and after anything that invalidates one -- see InvalidateHistory, and the gap
    // rule below.
    bool m_filterValid[2]{};
    // What the filter last ran with, so a live change from the panel starts from a clean history
    // instead of blending a correction built with one setting into one built with another.
    float m_filterStrength{-1.f};
    float m_filterGate{-1.f};
    // Frames the filter ran and how many times its history was dropped, both since the process
    // started. Reported rather than assumed: a filter that never runs and one that runs are the same
    // two zeros on a panel that does not ask.
    uint64_t m_filterFrames{0};
    uint64_t m_filterResets{0};
    // The probe: a box of the most recently written history copied into a readback buffer on a
    // sampling frame and read on the next one, because by then the frame's own wait has already
    // covered the copy. It answers one question -- is the filter's state real -- and the panel and
    // the log show what it found.
    ComPtr<ID3D12Resource> m_filterProbe;
    uint32_t m_filterProbeWidth{0};
    uint32_t m_filterProbeHeight{0};
    bool m_filterProbePending{false};
    uint64_t m_filterProbeFrame{0};
    // The controls the network currently in use was built with. The runtime bakes these into the
    // weights, so a change makes it rebuild inside PrepareFrame and leaves the history describing a
    // network that no longer exists; noticing the change one frame earlier is what lets that frame
    // start clean instead of reprojecting an answer from the old build.
    float m_controls[5]{1.f, 1.f, 1.f, 1.f, 1.f};
    bool m_controlsValid{false};
    // The near plane the history currently in hand was built with, and the last history-reset request
    // seen. Both only exist to notice a change: a history warped with one near plane cannot be warped
    // correctly with another, and the request is a counter so that both sessions see it exactly once.
    float m_motionNear{-1.f};
    uint32_t m_historyResetSeen{0};
    uint32_t m_statusSamples{0};

    // When the previous frame was handed over, and whether there has been one. The history describes
    // the scene as it was then; a long gap means the scene had time to become a different one -- a
    // loading screen, a menu, a stall -- and no motion field can carry the old answer across that.
    // Borrowed from the reference whose rule is `firstFrame || gap > 500 ms`.
    std::chrono::steady_clock::time_point m_lastFrameAt{};
    bool m_lastFrameAtValid{false};
    double m_enqueueMs{0};
    double m_waitMs{0};

    // Feeds the runtime's temporal path. Off by default: with no motion field to hand it, the network
    // has nothing to reproject its history with, and the session it reported was "hist=off".
    bool m_temporal{false};
    // Set whenever the working set or the network itself was rebuilt, so the first frame after it
    // starts from a clean history instead of warping an answer that no longer describes this image.
    bool m_historyInvalid{true};
    uint64_t m_historyInvalidFrames{0};

    ComPtr<ID3D12RootSignature> m_convertRootSignature;
    ComPtr<ID3D12PipelineState> m_convertPipeline;
    ComPtr<ID3D12DescriptorHeap> m_convertHeap;

    ComPtr<ID3D12RootSignature> m_motionRootSignature;
    ComPtr<ID3D12PipelineState> m_motionPipeline;
    ComPtr<ID3D12DescriptorHeap> m_motionHeap;

    // fp16 mirrors used as the network input and as a staging area for its answer.
    ComPtr<ID3D12Resource> m_inputFp16;
    ComPtr<ID3D12Resource> m_outputFp16;
    ComPtr<ID3D12Resource> m_result8;
    // The motion field handed to the runtime: two float channels at the network's input size.
    ComPtr<ID3D12Resource> m_motion;
    // The layer's depth reduction, opened across from D3D11. Only the motion pass reads it, and only
    // when the caller has supplied a near plane; without one the resource may still be here and
    // simply goes unbound.
    ComPtr<ID3D12Resource> m_depth;
    uint32_t m_depthWidth{0};
    uint32_t m_depthHeight{0};
    // The fp16 pair runs at the network's input size; `m_result8` runs at the conversion's output
    // size, which is larger once the window covers more than the network ceiling.
    uint32_t m_textureWidth{0};
    uint32_t m_textureHeight{0};
    uint32_t m_resultWidth{0};
    uint32_t m_resultHeight{0};
    DXGI_FORMAT m_resultFormat{DXGI_FORMAT_UNKNOWN};

    const char* m_lastError{""};
};
