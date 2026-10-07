// AMDNR neural renderer as an OpenXR implicit API layer.
//
// The layer sits between the application and whatever OpenXR runtime is active (Pimax, SteamVR,
// Virtual Desktop...) and runs the lmxxf neural renderer over the eye buffers the application
// submits. It is deliberately runtime agnostic: it only needs the standard loader contract plus
// the D3D11/D3D12 graphics bindings.
//
// Stage A: negotiation, dispatch chain, swapchain tracking and a D3D11 <-> D3D12 round trip of the
// foveated crop. The neural renderer itself is not wired up yet.

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

// openxr_platform.h spells out ID3D11Device / ID3D12Device / LUID inline, so the D3D headers have
// to come first.
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "AmdnrDepthCapture.h"
#include "AmdnrStereoProbe.h"
#include "EyeInterop.h"
#include "FsrShaders.h"
#include "NrQualityController.h"
#include "NrSettings.h"

// The FSR headers are here for their CPU side only: the EASU and RCAS constants are computed on the
// host every frame, while the shaders themselves are compiled at runtime from the text in
// FsrShaders.h. A_CPU and A_GPU/A_HLSL describe the same macros two different ways, so keeping the
// GPU half out of this translation unit is what lets both be included at all.
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter: the CPU helpers are generic
#pragma warning(disable : 4505) // unreferenced local function: only three of them are used here
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

namespace {

    const char* const kLayerName = "XR_APILAYER_AMDNR_neural_renderer";
    const char* const kLogFile = "AmdnrXrLayer.log";

    FILE* g_log = nullptr;
    bool g_logAttempted = false;

    std::string LogPath() {
        return LayerDirectory() + "\\" + kLogFile;
    }

    // Everything the layer needs from the command chain below it.
    struct NextDispatch {
        XrInstance Instance{XR_NULL_HANDLE};
        PFN_xrGetInstanceProcAddr GetInstanceProcAddr{nullptr};
        PFN_xrCreateApiLayerInstance CreateApiLayerInstance{nullptr};

        PFN_xrCreateSession CreateSession{nullptr};
        PFN_xrDestroySession DestroySession{nullptr};
        PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews{nullptr};
        PFN_xrCreateSwapchain CreateSwapchain{nullptr};
        PFN_xrDestroySwapchain DestroySwapchain{nullptr};
        PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages{nullptr};
        PFN_xrAcquireSwapchainImage AcquireSwapchainImage{nullptr};
        PFN_xrWaitSwapchainImage WaitSwapchainImage{nullptr};
        PFN_xrReleaseSwapchainImage ReleaseSwapchainImage{nullptr};
        PFN_xrWaitFrame WaitFrame{nullptr};
        PFN_xrBeginFrame BeginFrame{nullptr};
        PFN_xrEndFrame EndFrame{nullptr};
        PFN_xrDestroyInstance DestroyInstance{nullptr};

        // Eye gaze is driven through the action system, so the layer needs those entry points too.
        PFN_xrCreateActionSet CreateActionSet{nullptr};
        PFN_xrCreateAction CreateAction{nullptr};
        PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings{nullptr};
        PFN_xrCreateActionSpace CreateActionSpace{nullptr};
        PFN_xrDestroyActionSet DestroyActionSet{nullptr};
        PFN_xrDestroySpace DestroySpace{nullptr};
        PFN_xrLocateSpace LocateSpace{nullptr};
        PFN_xrAttachSessionActionSets AttachSessionActionSets{nullptr};
        PFN_xrSyncActions SyncActions{nullptr};
        PFN_xrStringToPath StringToPath{nullptr};
        PFN_xrLocateViews LocateViews{nullptr};
    };

    NextDispatch g_next;

    // An eye swapchain the application renders into, plus the D3D11 textures behind it.
    struct TrackedSwapchain {
        XrSwapchain handle{XR_NULL_HANDLE};
        std::vector<ComPtr<ID3D11Texture2D>> textures;
        uint32_t width{0};
        uint32_t height{0};
        uint32_t arraySize{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        uint32_t lastAcquired{0};
        // Set by the acquire hook, cleared once a frame, so a view can ask whether its chain was taken
        // AT ALL during the frame being looked at. An alternate-eye renderer never touches the eye it
        // did not redraw, so a chain that was not acquired this frame is that eye -- and that is the
        // only signal for it a layer can see without reading the eye buffer back.
        bool acquiredThisFrame{false};
        bool claimed{false};

        // Layer-owned companion at the full view size, for AMDNR_XR_UPSCALE, plus a sampled view of
        // every image in this chain for the upscale pass to read. Created on the first frame that
        // needs them; `outFailed` latches a setup failure so the attempt is not repeated every frame.
        XrSwapchain outHandle{XR_NULL_HANDLE};
        std::vector<ComPtr<ID3D11RenderTargetView>> outRtvs;
        // The images behind those views. The EASU + RCAS pass only ever draws into them, but FSR 4
        // writes its answer into one with a copy, and a copy needs the texture.
        std::vector<ComPtr<ID3D11Texture2D>> outTextures;
        std::vector<ComPtr<ID3D11ShaderResourceView>> srcSrvs;
        // EASU's intermediate: the same frame writes it and reads it straight back, so one texture
        // per chain is enough and it is only reused once RCAS has consumed it.
        ComPtr<ID3D11Texture2D> midTexture;
        ComPtr<ID3D11RenderTargetView> midRtv;
        ComPtr<ID3D11ShaderResourceView> midSrv;
        uint32_t outWidth{0};
        uint32_t outHeight{0};
        bool outFailed{false};
        // Whether the source views and the output views ended up going through sRGB views. Both are
        // settled when the views are created, from what the device accepted (see
        // EnsureOutputSwapchain), and they select the shader's conversions -- so they are carried on
        // the chain instead of being guessed at draw time. They are independent: a chain can read
        // straight through and still have to decode on write, or the other way round.
        bool srcEncode{false};
        bool dstDecode{false};
    };

    std::vector<TrackedSwapchain> g_swapchains;

    // Graphics binding captured from xrCreateSession.
    ComPtr<ID3D11Device> g_appDevice;
    ComPtr<ID3D11DeviceContext> g_appContext;
    bool g_isD3D11{false};
    bool g_bindingLogged{false};

    EyeInterop g_interop;
    bool g_interopTried{false};
    // Latched when a pass fails. What has been seen to fail here is a removed device, which no retry
    // brings back, and a retry costs a run of identical log lines while the picture goes stale behind
    // them. Reset with the session, because a new one gets a new device.
    bool g_interopGaveUp{false};
    uint64_t g_frameIndex{0};
    bool g_cropLogged{false};
    bool g_depthMotionLogged{false};
    uint64_t g_processCount{0};
    uint64_t g_eyePasses[2]{};
    double g_processMs{0};
    std::chrono::steady_clock::time_point g_lastReport{std::chrono::steady_clock::now()};

    // What the report window's passes cost in total, summed once per frame, and how many frames went
    // into it. This is the same quantity the per-eye stage lines add up to -- it is where `g_processMs`
    // comes from -- accumulated at frame granularity so it can be read against the frame breakdown's
    // own `layer` term. It exists because those two disagree, and a breakdown whose `layer` share is
    // wrong is worse than no breakdown: it is the number that says whether tuning the layer is worth
    // anything at all.
    double g_windowPassMs{0.0};
    uint64_t g_windowPassFrames{0};

    // The neural window's ceiling and the fraction of it the window covers are not kept here any more.
    // Both are NrSettings::ceilingWidth/Height and ::windowWidth/windowHeight, read per frame, which is
    // what lets the control panel move them while the title runs. The ceiling is still the number
    // AMDNR_XR_CROP has always named -- it is seeded from it when the instance is created -- it simply
    // is not frozen at it any more.

    // The window size the last ComputeCrop settled on, in eye-buffer pixels. The report already prints
    // where each window is (the anchors); without this there would be nothing that says how big it is
    // after the panel moved it. It is not only a diagnostic any more: the size band's hold measures
    // every later ask against it, so it is the size actually in hand on the devices.
    uint32_t g_windowW{0};
    uint32_t g_windowH{0};
    // One line the first time an ask is held inside the band rather than moving the window, and no more
    // until a size really changes again. While a slider is being dragged the hold does its work on
    // every frame, and the point of the line is the frames where the window did *not* move.
    bool g_windowHoldLogged{false};

    // The band of the size the settings are asking for, and since when they have been asking for it.
    // A size change is the one thing on this path that makes the runtime rebuild its network, so it is
    // only taken once the ask has stood still for a moment: dragging a slider is a stream of asks, and
    // switching on each of them is the stall this whole size policy exists to keep down. The window
    // keeps the size it has while the values move, and one rebuild happens when they stop.
    constexpr double kWindowSettleMs = 400.0;
    uint32_t g_windowAskBand[2]{0, 0};
    std::chrono::steady_clock::time_point g_windowAskAt{};
    // One line while a move is waiting to settle, so the log says why the window is not following.
    bool g_windowSettleLogged{false};

    // The automatic quality tier, and the window scale it is currently asking for.
    //
    // The budget is three quarters of the frame interval rather than the whole of it: the neural pass
    // is not the only thing in the frame, and a tier that aimed at the period would be satisfied with
    // a frame that is the network plus nothing. A quarter of the period is the reserve left for the
    // title's own rendering and the layer's conversion work -- measured on this machine the layer's
    // own share outside the pass is about 2.3 ms, and the title's is larger.
    //
    // The interval is the RUNTIME'S OWN, taken from xrWaitFrame's predicted display period, and not a
    // constant. VR misses its interval as a cliff and not as a slope: a frame that overruns by a tenth
    // is not shown a tenth late, it is reprojected whole and the rate halves. So the tier has to be
    // solved against the interval the headset is really running at -- a 72 Hz session held to a 90 Hz
    // budget is trimmed harder than it needs to be, and a 120 Hz one is let off. The constant below is
    // the fallback for the frames before the runtime has reported a period.
    constexpr double kNrQualityBudgetFraction = 0.75;
    constexpr double kNrQualityBudgetFallbackMs = 1000.0 / 90.0 * kNrQualityBudgetFraction;
    amdnr::NrQualityController g_quality(kNrQualityBudgetFallbackMs);
    // The runtime's own statement of the interval it intends, in nanoseconds, refreshed every wait.
    // Declared here rather than beside the wait hook so the frame loop can read it: the loop runs
    // earlier in the translation unit, and it needs this to size the budget above.
    int64_t g_predictedPeriod = 0;
    float g_qualityScale = 1.f;
    // This frame's and the previous frame's total neural-pass cost, in milliseconds. The previous
    // one is what the tier is judged on, because the tier has to be settled before the passes run.
    double g_frameMs = 0.0;
    double g_lastFrameMs = 0.0;

    // Scales the eye buffer size the layer *recommends* to the application. This is the only place a
    // layer can touch the application's render load: by the time xrEndFrame arrives the eye buffers
    // are already shaded and their extent is fixed. Below 1 the application shades fewer pixels and
    // the compositor stretches the result back onto the panel, which shortens the wait the neural
    // pass spends on the application's own GPU work.
    float g_renderScale = 1.f;
    bool g_renderScaleLogged = false;

    // ---- Full-resolution output path (AMDNR_XR_UPSCALE) ----
    //
    // With this on the layer keeps its own swapchain at the runtime's recommended view size and
    // blits the application's (smaller) eye image into it through a bicubic + adaptive-sharpen
    // pass, then points the projection layer at that. The application keeps rendering at
    // `g_renderScale` of the view size, so its render cost falls while the compositor still
    // receives a full-resolution image.
    //
    // Off by default, and every step fails soft: if anything here cannot be set up the projection
    // layer is forwarded exactly as the application submitted it.
    //
    // Abandoned, and not for a fixable defect. With this on, the Pimax runtime accepts every frame --
    // xrEndFrame returns XR_SUCCESS, one refusal in 1500 frames, that one being the usual
    // CALL_ORDER_INVALID before the session is up -- and then never displays any of them: the headset
    // goes black while the application keeps rendering at full speed. The fault sits after the submit,
    // which is the one place a layer cannot observe or reach, so there is nothing here to repair.
    // Established by A/B on this variable alone with every other setting identical.
    bool g_upscale = false;
    // ---- FidelityFX FSR 4 for that path (AMDNR_XR_FSR4) ----
    //
    // Replaces what EASU + RCAS do in the output pass with FSR 4, which is a better upscaler and a
    // D3D12 one: it runs on the neural renderer's device and the pixels cross between the two
    // devices the same way its crops do. Only meaningful with AMDNR_XR_UPSCALE on, and it falls
    // back to EASU + RCAS for any frame it declines, so the switch is a preference and not a mode.
    bool g_fsr4 = false;
    float g_upscaleSharpen = 0.30f;
    // The runtime's recommended per-eye size, captured before g_renderScale is applied to it. The
    // output swapchain is sized from this, which is what makes it a fixed target rather than a
    // function of whatever scale happens to be configured.
    uint32_t g_viewWidth = 0;
    uint32_t g_viewHeight = 0;
    // The session the swapchains belong to; only needed to create the output one.
    XrSession g_session = XR_NULL_HANDLE;

    // ---- Sub-pixel jitter (AMDNR_XR_JITTER) ----
    //
    // An upscaler that accumulates across frames only gains resolution if each frame samples the
    // scene at a different sub-pixel position, and the only place a layer can put that offset is the
    // frustum the application renders with. So the layer shifts the field of view the application is
    // handed by a fraction of a pixel, reports the same offset to the effect, and tells the
    // compositor the frustum the application would have had without it -- the effect has already
    // removed the offset from the pixels by then, so the picture and the frustum still agree.
    //
    // The sequence is Halton(2,3) centred on zero, which is the pattern this kind of upscaler is
    // built around; the exact pattern does not matter as much as the report matching what was
    // applied, because that is the only thing the effect can check its history against.
    bool g_jitter = false;
    // Multiplies both axes. A report whose sign disagrees with the offset that was applied doubles
    // the misalignment instead of removing it, and which sign is right is not documented -- the
    // effect says only "the subpixel jitter offset applied to the camera".
    float g_jitterSign = 1.f;
    uint32_t g_jitterPhases = 8;
    uint32_t g_jitterIndex = 0;
    // This frame's offset, in render pixels, positive to the right and downwards.
    float g_jitterX = 0.f;
    float g_jitterY = 0.f;
    bool g_jitterLogged = false;
    // Whether the application's own submit has been compared against what it was handed, once. That
    // difference is the only direct evidence that the application renders with the offset at all.
    bool g_jitterDeltaLogged = false;

    // Two pixel shaders over one shared vertex shader: EASU resamples, RCAS sharpens, and each has
    // its own constant buffer.
    ComPtr<ID3D11VertexShader> g_upscaleVs;
    ComPtr<ID3D11PixelShader> g_easuPs;
    ComPtr<ID3D11PixelShader> g_rcasPs;
    ComPtr<ID3D11SamplerState> g_upscaleSampler;
    ComPtr<ID3D11Buffer> g_easuConstants;
    ComPtr<ID3D11Buffer> g_rcasConstants;
    // The output merger and rasterizer state the output pass draws under. It draws in the middle of
    // the application's frame, on the application's context, so anything it does not set it inherits
    // -- and an inherited blend state that writes no colour, a cull mode that drops the full-screen
    // triangle, or a scissor rect left over from a HUD draw all turn the result into a black image
    // with nothing for the runtime to report.
    ComPtr<ID3D11BlendState> g_upscaleBlend;
    ComPtr<ID3D11DepthStencilState> g_upscaleDepth;
    ComPtr<ID3D11RasterizerState> g_upscaleRaster;
    // RCAS sharpness is fixed for the whole run, so its constants are built once and copied into the
    // buffer on every draw.
    uint32_t g_rcasCon[4]{};
    // Latches a pipeline that cannot be built at all, so the failure is logged once instead of
    // every frame.
    bool g_upscaleFailed{false};
    // The output check: how many views the pass has filled, and how many of them have been read
    // back. The first frame is the one frame the answer cannot be trusted from -- a title that has
    // just launched is showing a loading screen, and a loading screen is black on purpose.
    uint32_t g_upscaleFrames{0};
    uint32_t g_outputChecks{0};
    // One line when the first view actually goes through the pass, so a run can be told apart from
    // one where every view fell back to the application's texture.
    bool g_upscaleDrawLogged{false};

    // The window moves on a coarse grid and only once the gaze has drifted past the dead zone. A
    // window that follows the raw eye signal moves every frame, and because the neural answer is a
    // little darker than the source, that travelling boundary reads as jitter.
    constexpr int32_t kCropQuantum = 8;
    uint32_t g_gazeDeadZone = 128;
    // The furthest the window may travel in one frame.
    //
    // Everything the window shows comes from the crop the network was handed last frame, so moving
    // further than that crop covers leaves the sampler reading past the end of its own source -- and a
    // clamped sampler answers that by repeating the edge pixel, which smears a band as wide as the
    // move along the trailing edge and un-smears it again as the head slows. Bounding the step keeps
    // every pixel inside an answer that exists. What it costs is the window trailing the gaze while
    // the gaze moves faster than the bound, and a lag the eye reads as the fovea catching up is far
    // cheaper than a band that breathes with every flick. Zero leaves the travel unbounded, which is
    // all the dead zone on its own has ever done.
    //
    // Measured with a bounded step in place, this is what the shift is: it never has to carry content
    // further than one frame of the window's own travel, so it stays a few percent of the window
    // instead of reaching a quarter of it.
    uint32_t g_gazeMaxStep = 0;

    struct CropAnchor {
        bool valid{false};
        int32_t width{0};
        int32_t height{0};
        int32_t left{0};
        int32_t top{0};
    };

    // One anchor per view: the two eyes sit at different offsets inside the eye buffer, so a single
    // shared anchor would be pushed out of its dead zone by the other eye every frame.
    constexpr uint32_t kMaxViews = 2;
    CropAnchor g_anchor[kMaxViews];
    uint64_t g_anchorMoves{0};

    // The frustum the runtime gave out before the jitter was applied to it. The submit path puts it
    // back on any view the effect has taken the offset out of.
    XrFovf g_unJitteredFov[kMaxViews]{};
    bool g_unJitteredFovValid = false;

    // Leaves the layer loaded and logging but doing no work at all, so the application's own frame
    // rate can be measured through the same loader chain. This is the only honest baseline for what
    // the pass costs.
    bool g_skipPass{false};

    // Reported once for "never", and again the first time one does turn up: whether the application
    // takes the layer up on the depth extension it now declares is the whole point of looking.
    bool g_depthAbsenceLogged{false};
    bool g_depthSeen{false};
    bool g_appExtensionsLogged{false};

    // ---- Diagnostic depth probe (AMDNR_XR_DEPTH_PROBE) ----
    //
    // The layer is the caller of xrEndFrame, so it may legally staple a synthetic
    // XrCompositionLayerDepthInfoKHR onto each projection view. That answers one narrow question:
    // does the runtime below use the depth it is handed for positional timewarp? The probe presents a
    // picture too wrong to be mistaken for real depth, and the user translates their head -- if the
    // world shears, the runtime consumed it; if nothing moves, it ignored it.
    // `Cycle` is `Far`, `Gradient` and `Near` taken in turn, one step every kDepthProbeCycleSeconds.
    // Everything the probe asks needs a reference to compare against, and a single picture supplies
    // none: "does the world shear" is a judgement the observer has to make from memory, while "does
    // the picture keep changing while I move the same way" is not. The cycle puts that reference
    // inside the same session, where the application's own motion is the only other variable.
    enum class DepthProbeMode { Off, Gradient, Near, Far, Cycle };
    DepthProbeMode g_depthProbeMode{DepthProbeMode::Off};

    constexpr int64_t kDepthProbeCycleSeconds = 3;

    // When the cycle started. The step is derived from the clock rather than kept in a counter, so it
    // cannot drift out of step with the pictures actually written to the chains.
    std::chrono::steady_clock::time_point g_depthProbeCycleStart{};

    // One depth chain per distinct final view size. Two is the ceiling because a stereo frame has at
    // most two eye rectangles; a third size is something unexpected and the probe gives up rather
    // than grow without bound.
    constexpr size_t kMaxDepthProbeChains = 2;

    // The bounded wait when taking a probe image. An unbounded wait would let a stalled compositor
    // hang the whole frame, which is never worth it for a diagnostic.
    constexpr XrDuration kDepthProbeWaitNs = 100000000; // 100 ms

    struct DepthProbeChain {
        uint32_t width{0};
        uint32_t height{0};
        XrSwapchain handle{XR_NULL_HANDLE};
        std::vector<ComPtr<ID3D11Texture2D>> textures;
        // Which of the probe's pictures the images hold right now, so a cycle step is written once
        // when it comes round rather than on every view of every frame.
        DepthProbeMode mode{DepthProbeMode::Off};
    };

    std::vector<DepthProbeChain> g_depthChains;
    // Latches a setup failure so a runtime that refuses the depth chain is not asked again every
    // frame; the layer then forwards colour exactly as it always did.
    bool g_depthProbeFailed{false};
    bool g_depthProbeAttachLogged{false};
    bool g_depthProbeYieldLogged{false};
    bool g_depthProbeAcquireLogged{false};

    // What the runtime said about the frame the layer just handed it. Everything the layer does
    // happens before this call, so a frame the compositor refuses is invisible in the timings and in
    // the frame counter -- the application keeps rendering and the picture simply never appears.
    // The first refusal is logged with its code, and the count rides along with the frame report.
    bool g_endFrameFailLogged{false};
    uint64_t g_endFrameFailures{0};
    XrResult g_endFrameLastResult{XR_SUCCESS};

    // Feeds the runtime's temporal path, where the network reprojects its previous answer instead of
    // reconstructing every frame from one image. The field comes from the two view poses, which is
    // the one part of the motion a layer genuinely knows.
    //
    // Both views get it. The history a session reprojects lives inside that session, so eye and
    // session are paired up one to one; treating only one eye would leave the frame half filtered,
    // and a difference between the two eyes is not something anyone can judge through a headset.
    bool g_temporal{false};

    // One over the near plane the application projected with, or zero when there is none to use.
    //
    // The depth the layer reads out of the application's buffer is inverse distance: it is `1 - raw`,
    // which for a standard projection is the near plane over z. Multiplying it by this turns it into
    // 1/z, which is the quantity the parallax term needs -- a point's displacement under the camera's
    // own travel is that travel over its distance. Nothing in an OpenXR layer is told the near plane,
    // so it is the one number here that has to be found by eye, and it is read live from the settings
    // block rather than once: a value that needed a restart to try would mean taking the headset off
    // for every step of the calibration.
    float MotionInvNear() {
        const float plane = NrSettingsGet().motionNear;
        return plane > 1e-4f ? 1.f / plane : 0.f;
    }

    // What a view's last pass looked like, so this frame can say where its content has gone. The
    // orientation and the position together drive the motion: the orientation alone is exact for a
    // head that only turns, and the position is what lets a depth turn the head's travel into a
    // parallax. The window's own travel is folded in separately, because the window slides on its own
    // grid and would otherwise read as the world drifting sideways.
    struct ViewTrack {
        bool valid{false};
        XrQuaternionf orientation{0.f, 0.f, 0.f, 1.f};
        XrVector3f position{0.f, 0.f, 0.f};
        int32_t left{0};
        int32_t top{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    ViewTrack g_viewTrack[kMaxViews];

    // ---- the alternate-eye probe ----
    //
    // Two per-view questions, asked of every pass and with the answers only tallied: was this view's
    // swapchain taken during the frame, and did this view's pose move since the last frame. A full-rate
    // frame takes both chains and moves both poses; an alternate-eye frame takes one and leaves the
    // other exactly as it was. The ratio is what tells the two apart from inside a layer, where the eye
    // buffers themselves cannot be read back without stalling the pipeline.
    //
    // Read-only on purpose. It changes no window, no pass and no submission -- it is a measurement, and
    // it exists so the next decision is made on a number rather than on the assumption that a renderer
    // must behave the way the reference one does.
    XrQuaternionf g_probeLastOrientation[kMaxViews]{};
    XrVector3f g_probeLastPosition[kMaxViews]{};
    bool g_probeLastPoseValid[kMaxViews]{};
    uint64_t g_probePasses[2]{};
    uint64_t g_probeAcquired[2]{};
    uint64_t g_probeMoved[2]{};

    // Whether a view whose chain was not taken this frame is left alone instead of run again. Off by
    // default: the probe is what has to show the signal first. `g_staleSkips` counts what it did, per
    // eye, over the same report interval as everything else.
    bool g_skipStaleEye = false;
    uint64_t g_staleSkips[2]{};

    // The same reprojection as above, in the shape the output pass reads: render pixels over the whole
    // view rather than network pixels over a window. FSR 4 reprojects its own history with it, so it
    // has to be built on the grid that history lives on. Filled by the neural loop -- the one place
    // both poses are in hand at once -- and read later in the same frame by the output pass.
    NrMotion g_fsr4Motion[kMaxViews];

    // What one eye's pass needs: the region of the eye buffer it reads and writes, and the size that
    // region is resampled to for the network.
    struct CropPlan {
        RECT region{0, 0, 0, 0};
        uint32_t networkWidth{0};
        uint32_t networkHeight{0};
    };

    // Eye gaze, only wired up when the runtime advertises XR_EXT_eye_gaze_interaction.
    struct EyeGaze {
        bool extensionOffered{false};
        XrActionSet actionSet{XR_NULL_HANDLE};
        XrAction action{XR_NULL_HANDLE};
        XrSpace space{XR_NULL_HANDLE};
        bool attached{false};
        XrPosef pose{};
        bool poseValid{false};
    };

    EyeGaze g_gaze;
    uint64_t g_gazeHits{0};
    // Throttles the gaze diagnostic in LocateEyeGaze, which is the only place that can say whether a
    // missing gaze is the runtime's doing or this layer's.
    uint32_t g_gazeProbeSamples{0};
    uint32_t g_lastImage{0};

    const char* DepthProbeModeName(DepthProbeMode mode) {
        switch (mode) {
        case DepthProbeMode::Gradient:
            return "gradient";
        case DepthProbeMode::Near:
            return "near";
        case DepthProbeMode::Far:
            return "far";
        case DepthProbeMode::Cycle:
            return "cycle";
        default:
            return "off";
        }
    }

    // Fills `data` row major with the synthetic depth picture for `mode`, one float per pixel.
    //
    // Gradient is the default on purpose: whether 0 or 1 means "near" depends on the depth convention
    // (plain Z or reversed-Z), and a constant fills the whole view with one end of that convention. If
    // the guess is wrong, a runtime that *does* consume depth would show no parallax, which is
    // indistinguishable from one that ignores depth -- the exact answer being sought. A horizontal
    // ramp spans 0..1 in every row, so half the view is near and half is far under *either*
    // convention and head translation always shears. The constant modes are offered only as a
    // deliberate, less reliable probe.
    void FillDepthProbeData(std::vector<float>& data, uint32_t width, uint32_t height, DepthProbeMode mode) {
        data.assign((size_t)width * height, 0.f);
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                float value = 0.f;
                switch (mode) {
                case DepthProbeMode::Near:
                    value = 0.f;
                    break;
                case DepthProbeMode::Far:
                    value = 1.f;
                    break;
                case DepthProbeMode::Gradient:
                default:
                    value = width > 1 ? (float)x / (float)(width - 1) : 0.f;
                    break;
                }
                data[(size_t)y * width + x] = value;
            }
        }
    }

    // What the probe should be showing right now. Every mode but Cycle is itself; Cycle walks
    // Far -> Gradient -> Near, one step per kDepthProbeCycleSeconds. The order runs from the least to
    // the most parallax a depth-consuming runtime would produce, so the change is hardest to miss
    // where it is easiest to doubt.
    DepthProbeMode ResolveDepthProbeMode() {
        if (g_depthProbeMode != DepthProbeMode::Cycle) {
            return g_depthProbeMode;
        }
        static const DepthProbeMode sequence[] = {
            DepthProbeMode::Far, DepthProbeMode::Gradient, DepthProbeMode::Near};
        const int64_t elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - g_depthProbeCycleStart)
                                    .count();
        const int64_t step = elapsed > 0 ? elapsed / kDepthProbeCycleSeconds : 0;
        return sequence[(size_t)(step % (int64_t)std::size(sequence))];
    }

    // Writes one of the probe's pictures into every image of a chain. All of them, not just the one
    // about to be submitted: the compositor picks whichever image is current, so a chain that is only
    // half rewritten would show the previous step on some frames and the new one on others.
    bool FillDepthProbeTextures(const std::vector<ComPtr<ID3D11Texture2D>>& textures,
                                uint32_t width,
                                uint32_t height,
                                DepthProbeMode mode) {
        if (g_appContext == nullptr || textures.empty()) {
            return false;
        }
        std::vector<float> data;
        FillDepthProbeData(data, width, height, mode);
        const UINT rowPitch = (UINT)((size_t)width * sizeof(float));
        for (const ComPtr<ID3D11Texture2D>& texture : textures) {
            if (texture == nullptr) {
                return false;
            }
            g_appContext->UpdateSubresource(
                texture.Get(), 0, nullptr, data.data(), rowPitch, (UINT)(data.size() * sizeof(float)));
        }
        return true;
    }

    // AMDNR_XR_DEPTH_PROBE_SELFTEST=1: prints the ramp's ends and middle so the fill can be checked
    // without a headset. It is not part of the probe's runtime path.
    void RunDepthProbeSelfTest() {
        std::vector<float> data;
        const uint32_t width = 9;
        FillDepthProbeData(data, width, 1, DepthProbeMode::Gradient);
        std::string line;
        char cell[32]{};
        for (uint32_t x = 0; x < width; x++) {
            std::snprintf(cell, sizeof(cell), " %.4f", data[x]);
            line += cell;
        }
        LayerLog("layer: depth probe self-test gradient %ux1 ->%s\n", width, line.c_str());

        FillDepthProbeData(data, 4, 2, DepthProbeMode::Near);
        LayerLog("layer: depth probe self-test near 4x2 -> %.4f %.4f %.4f %.4f (all rows)\n",
                 data[0], data[1], data[2], data[3]);
        FillDepthProbeData(data, 4, 2, DepthProbeMode::Far);
        LayerLog("layer: depth probe self-test far 4x2 -> %.4f %.4f %.4f %.4f (all rows)\n",
                 data[0], data[1], data[2], data[3]);
    }

    // Creates (once) the D32_FLOAT chain that matches `width` x `height` and fills every image with
    // the synthetic picture. The picture never changes, so the fill happens here and the per-frame
    // path only acquires and releases. Returns null on any failure, after latching the run's probe off.
    DepthProbeChain* EnsureDepthChain(uint32_t width, uint32_t height) {
        for (DepthProbeChain& chain : g_depthChains) {
            if (chain.width == width && chain.height == height) {
                return &chain;
            }
        }
        if (g_depthProbeFailed) {
            return nullptr;
        }
        // Not ready yet rather than broken: the session or binding may not exist on an early frame.
        if (g_appContext == nullptr || g_session == XR_NULL_HANDLE || g_next.CreateSwapchain == nullptr ||
            g_next.EnumerateSwapchainImages == nullptr) {
            return nullptr;
        }
        if (g_depthChains.size() >= kMaxDepthProbeChains) {
            g_depthProbeFailed = true;
            LayerLog("layer: depth probe met a third view size and only plans for two, standing down\n");
            return nullptr;
        }

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.arraySize = 1;
        info.mipCount = 1;
        info.faceCount = 1;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.format = (int64_t)DXGI_FORMAT_D32_FLOAT;
        // DEPTH_STENCIL_ATTACHMENT is what the depth extension demands; SAMPLED is added because a
        // runtime that reads the buffer back needs a shader-readable resource on D3D11.
        info.usageFlags =
            XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

        XrSwapchain handle = XR_NULL_HANDLE;
        if (XR_FAILED(g_next.CreateSwapchain(g_session, &info, &handle))) {
            g_depthProbeFailed = true;
            LayerLog("layer: depth probe chain %ux%u fmt=%d was refused, the probe is off for this run\n",
                     width, height, (int)DXGI_FORMAT_D32_FLOAT);
            return nullptr;
        }

        std::vector<XrSwapchainImageD3D11KHR> images;
        uint32_t count = 0;
        if (XR_SUCCEEDED(g_next.EnumerateSwapchainImages(handle, 0, &count, nullptr)) && count > 0) {
            images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(g_next.EnumerateSwapchainImages(
                    handle, count, &count, (XrSwapchainImageBaseHeader*)images.data()))) {
                images.clear();
            }
        }

        // The chain opens on whichever picture the cycle is on right now, so the first step has
        // nothing to correct when it comes round.
        const DepthProbeMode shown = ResolveDepthProbeMode();
        DepthProbeChain chain;
        chain.width = width;
        chain.height = height;
        chain.handle = handle;
        chain.mode = shown;
        for (const XrSwapchainImageD3D11KHR& image : images) {
            chain.textures.push_back(image.texture);
        }
        if (!FillDepthProbeTextures(chain.textures, width, height, shown)) {
            g_next.DestroySwapchain(handle);
            g_depthProbeFailed = true;
            LayerLog("layer: depth probe could not fill its %ux%u chain, the probe is off for this run\n",
                     width,
                     height);
            return nullptr;
        }

        g_depthChains.push_back(std::move(chain));
        LayerLog("layer: depth probe chain %ux%u fmt=%d (D32_FLOAT) images=%u ready\n",
                 width,
                 height,
                 (int)DXGI_FORMAT_D32_FLOAT,
                 count);
        return &g_depthChains.back();
    }

    // One acquire/wait/release so the runtime's current image for the chain is valid. The picture is
    // already there from creation, so the only work is a cycle step: while the chain is held every
    // image is rewritten, which keeps the switch inside the window where nothing else can read it.
    // The wait is bounded so a stalled compositor cannot hang the frame.
    bool AcquireDepthProbeImage(DepthProbeChain& chain, DepthProbeMode shown) {
        if (g_next.AcquireSwapchainImage == nullptr || g_next.WaitSwapchainImage == nullptr ||
            g_next.ReleaseSwapchainImage == nullptr) {
            return false;
        }

        uint32_t index = 0;
        if (XR_FAILED(g_next.AcquireSwapchainImage(chain.handle, nullptr, &index))) {
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = kDepthProbeWaitNs;
        if (XR_FAILED(g_next.WaitSwapchainImage(chain.handle, &wait)) ||
            index >= chain.textures.size()) {
            g_next.ReleaseSwapchainImage(chain.handle, nullptr);
            return false;
        }
        if (chain.mode != shown) {
            if (FillDepthProbeTextures(chain.textures, chain.width, chain.height, shown)) {
                chain.mode = shown;
                LayerLog("layer: depth probe switched to %s\n", DepthProbeModeName(shown));
            }
        }
        g_next.ReleaseSwapchainImage(chain.handle, nullptr);
        return true;
    }

    void ReadCropOverride() {
        // AMDNR_XR_CROP is not read here any more: the ceiling it named is a live setting now, seeded
        // by NrSettingsInitFromEnvironment when the instance is created. What is left here are the
        // switches that decide how the pass runs, none of which the panel touches.
        char buffer[64]{};

        if (GetEnvironmentVariableA("AMDNR_XR_SKIP", buffer, (DWORD)std::size(buffer)) > 0 && buffer[0] != '0') {
            g_skipPass = true;
            LayerLog("layer: pass disabled, measuring the application alone\n");
        }

        if (GetEnvironmentVariableA("AMDNR_XR_GAZE_DEAD_ZONE", buffer, (DWORD)std::size(buffer)) > 0) {
            g_gazeDeadZone = (uint32_t)std::max(0, atoi(buffer));
            LayerLog("layer: gaze dead zone %u px\n", g_gazeDeadZone);
        }

        if (GetEnvironmentVariableA("AMDNR_XR_GAZE_MAX_STEP", buffer, (DWORD)std::size(buffer)) > 0) {
            g_gazeMaxStep = (uint32_t)std::max(0, atoi(buffer));
            LayerLog("layer: window travel bounded to %u px per frame\n", g_gazeMaxStep);
        }

        // The depth probe is diagnostic only and off unless asked for; the values above 0 are the
        // three pictures it can present. Gradient is the recommended one -- see FillDepthProbeData
        // for why a constant can hide a runtime that does consume depth.
        if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_PROBE", buffer, (DWORD)std::size(buffer)) > 0) {
            if (_stricmp(buffer, "gradient") == 0) {
                g_depthProbeMode = DepthProbeMode::Gradient;
                LayerLog("layer: depth probe on, horizontal gradient (valid under either depth convention)\n");
            } else if (_stricmp(buffer, "near") == 0) {
                g_depthProbeMode = DepthProbeMode::Near;
                LayerLog("layer: depth probe on, constant near end -- less reliable than gradient, it "
                         "assumes conventional (non-reversed) depth\n");
            } else if (_stricmp(buffer, "far") == 0) {
                g_depthProbeMode = DepthProbeMode::Far;
                LayerLog("layer: depth probe on, constant far end -- less reliable than gradient, it "
                         "assumes conventional (non-reversed) depth\n");
            } else if (_stricmp(buffer, "cycle") == 0) {
                g_depthProbeMode = DepthProbeMode::Cycle;
                LayerLog("layer: depth probe on, cycling far -> gradient -> near every %lld s; the test "
                         "is whether the picture keeps changing while the head moves the same way\n",
                         (long long)kDepthProbeCycleSeconds);
            } else if (buffer[0] != '0' && _stricmp(buffer, "off") != 0) {
                LayerLog("layer: AMDNR_XR_DEPTH_PROBE=%s not understood, the probe stays off\n", buffer);
            }
            // The cycle is timed from here, so the first step boundary lands a whole period after the
            // probe comes up rather than at an arbitrary point in the application's startup.
            if (g_depthProbeMode != DepthProbeMode::Off) {
                g_depthProbeCycleStart = std::chrono::steady_clock::now();
            }
        }
        if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_PROBE_SELFTEST", buffer, (DWORD)std::size(buffer)) > 0 &&
            buffer[0] == '1') {
            RunDepthProbeSelfTest();
        }
    }

    void ReadRenderScale() {
        char buffer[64]{};
        if (GetEnvironmentVariableA("AMDNR_XR_RENDER_SCALE", buffer, (DWORD)std::size(buffer)) > 0) {
            g_renderScale = std::clamp((float)atof(buffer), 0.25f, 1.f);
            LayerLog("layer: recommending %.3f of the runtime's eye buffer size\n", g_renderScale);
        }

        if (GetEnvironmentVariableA("AMDNR_XR_UPSCALE", buffer, (DWORD)std::size(buffer)) > 0 && buffer[0] != '0') {
            g_upscale = true;
            LayerLog("layer: full-resolution output path on\n");
        }
        if (GetEnvironmentVariableA("AMDNR_XR_UPSCALE_SHARPEN", buffer, (DWORD)std::size(buffer)) > 0) {
            g_upscaleSharpen = std::clamp((float)atof(buffer), 0.f, 1.f);
            LayerLog("layer: output sharpen %.2f\n", g_upscaleSharpen);
        }
        if (GetEnvironmentVariableA("AMDNR_XR_FSR4", buffer, (DWORD)std::size(buffer)) > 0 && buffer[0] != '0') {
            g_fsr4 = true;
            LayerLog("layer: FSR 4 preferred for the output path, EASU + RCAS where it declines\n");
        }
    }

    void ReadTemporal() {
        char buffer[64]{};
        if (GetEnvironmentVariableA("AMDNR_XR_TEMPORAL", buffer, (DWORD)std::size(buffer)) > 0 && buffer[0] != '0') {
            g_temporal = true;
            LayerLog("layer: temporal path on for both views\n");
        }
    }

    // The alternate-eye skip, off unless asked for. It is a switch rather than a default because it
    // trusts a signal from the application, and a title that holds a chain without re-acquiring it would
    // be read as having skipped that eye. The probe's ratio is what decides whether that trust is
    // earned, so this stays off until a run has shown the ratio.
    void ReadStaleEyeSkip() {
        char buffer[64]{};
        if (GetEnvironmentVariableA("AMDNR_XR_SKIP_STALE", buffer, (DWORD)std::size(buffer)) > 0 &&
            buffer[0] != '0') {
            g_skipStaleEye = true;
            LayerLog("layer: a view whose swapchain was not taken this frame is left as it is\n");
        }
    }

    void ReadJitter() {
        char buffer[64]{};
        if (GetEnvironmentVariableA("AMDNR_XR_JITTER", buffer, (DWORD)std::size(buffer)) == 0 || buffer[0] == '0') {
            return;
        }
        g_jitter = true;
        if (GetEnvironmentVariableA("AMDNR_XR_JITTER_SIGN", buffer, (DWORD)std::size(buffer)) > 0) {
            const float sign = (float)atof(buffer);
            if (sign != 0.f) {
                g_jitterSign = sign < 0.f ? -1.f : 1.f;
            }
        }
        LayerLog("layer: the frustum handed to the application carries %u-phase sub-pixel jitter, sign %+.0f\n",
                 g_jitterPhases,
                 g_jitterSign);
    }

    // Radical inverse of `index` in `base`, the low-discrepancy sequence a multi-frame accumulation
    // wants: successive indices land as far apart as possible, so a handful of frames already covers
    // the pixel evenly instead of walking a line through it.
    float Halton(uint32_t index, uint32_t base) {
        float result = 0.f;
        float fraction = 1.f;
        for (uint32_t i = index + 1; i > 0; i /= base) {
            fraction /= (float)base;
            result += (float)(i % base) * fraction;
        }
        return result;
    }

    // This frame's offset, centred on zero and measured in render pixels. Advanced once per frame by
    // the submit path, so every call the application makes for one frame sees the same offset.
    void AdvanceJitter() {
        if (!g_jitter) {
            g_jitterX = 0.f;
            g_jitterY = 0.f;
            return;
        }
        const uint32_t phase = g_jitterIndex % g_jitterPhases;
        g_jitterIndex++;
        g_jitterX = (Halton(phase, 2) - 0.5f) * g_jitterSign;
        g_jitterY = (Halton(phase, 3) - 0.5f) * g_jitterSign;
    }

    // Row-major rotation matrix of a unit quaternion.
    void QuaternionMatrix(const XrQuaternionf& q, float m[9]) {
        const float xx = q.x * q.x;
        const float yy = q.y * q.y;
        const float zz = q.z * q.z;
        const float xy = q.x * q.y;
        const float xz = q.x * q.z;
        const float yz = q.y * q.z;
        const float wx = q.w * q.x;
        const float wy = q.w * q.y;
        const float wz = q.w * q.z;
        m[0] = 1.f - 2.f * (yy + zz);
        m[1] = 2.f * (xy - wz);
        m[2] = 2.f * (xz + wy);
        m[3] = 2.f * (xy + wz);
        m[4] = 1.f - 2.f * (xx + zz);
        m[5] = 2.f * (yz - wx);
        m[6] = 2.f * (xz - wy);
        m[7] = 2.f * (yz + wx);
        m[8] = 1.f - 2.f * (xx + yy);
    }

    void MatrixMultiply(const float a[9], const float b[9], float out[9]) {
        for (int row = 0; row < 3; row++) {
            for (int column = 0; column < 3; column++) {
                out[row * 3 + column] = a[row * 3 + 0] * b[0 + column] +
                                        a[row * 3 + 1] * b[3 + column] +
                                        a[row * 3 + 2] * b[6 + column];
            }
        }
    }

    // Rounds a recommended extent down to a multiple of 8 and keeps it usable, so the eye buffer the
    // application allocates still lands on the crop grid.
    uint32_t ScaleRecommended(uint32_t value) {
        const uint32_t scaled = ((uint32_t)((float)value * g_renderScale)) & ~7u;
        return std::max(64u, scaled);
    }

    XrVector3f Rotate(const XrVector3f& v, const XrQuaternionf& q) {
        const XrVector3f axis{q.x, q.y, q.z};
        const XrVector3f cross{axis.y * v.z - axis.z * v.y,
                               axis.z * v.x - axis.x * v.z,
                               axis.x * v.y - axis.y * v.x};
        const XrVector3f cross2{axis.y * cross.z - axis.z * cross.y,
                                axis.z * cross.x - axis.x * cross.z,
                                axis.x * cross.y - axis.y * cross.x};
        return XrVector3f{v.x + 2.f * (q.w * cross.x + cross2.x),
                          v.y + 2.f * (q.w * cross.y + cross2.y),
                          v.z + 2.f * (q.w * cross.z + cross2.z)};
    }

    // Projects the gaze ray into the view image and returns normalised [0,1] image coordinates.
    bool GazeFocusPoint(const XrCompositionLayerProjectionView& view, float& u, float& v) {
        if (!g_gaze.poseValid) {
            return false;
        }

        const XrVector3f forward{0.f, 0.f, -1.f};
        const XrVector3f gazeInSpace = Rotate(forward, g_gaze.pose.orientation);
        const XrQuaternionf viewInverse{-view.pose.orientation.x,
                                        -view.pose.orientation.y,
                                        -view.pose.orientation.z,
                                        view.pose.orientation.w};
        const XrVector3f gaze = Rotate(gazeInSpace, viewInverse);
        if (gaze.z > -1e-4f) {
            return false;
        }

        const float tanX = gaze.x / -gaze.z;
        const float tanY = gaze.y / -gaze.z;
        const float tanLeft = tanf(view.fov.angleLeft);
        const float tanRight = tanf(view.fov.angleRight);
        const float tanUp = tanf(view.fov.angleUp);
        const float tanDown = tanf(view.fov.angleDown);
        if (tanRight <= tanLeft || tanUp <= tanDown) {
            return false;
        }

        u = (tanX - tanLeft) / (tanRight - tanLeft);
        v = (tanUp - tanY) / (tanUp - tanDown);
        return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
    }

    template <typename T>
    void Resolve(const char* name, T& target) {
        if (target != nullptr || g_next.GetInstanceProcAddr == nullptr || g_next.Instance == XR_NULL_HANDLE) {
            return;
        }
        PFN_xrVoidFunction resolved = nullptr;
        if (XR_SUCCEEDED(g_next.GetInstanceProcAddr(g_next.Instance, name, &resolved))) {
            target = reinterpret_cast<T>(resolved);
        }
    }

    bool IsColorFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return true;
        default:
            return false;
        }
    }

    TrackedSwapchain* FindSwapchain(XrSwapchain handle) {
        for (auto& entry : g_swapchains) {
            if (entry.handle == handle) {
                return &entry;
            }
        }
        return nullptr;
    }

    // A view's projection, reduced to the two numbers the second eye's reconstruction needs: how many
    // pixels one unit of tangent spans, and where the optical axis -- the direction the view faces --
    // lands inside the view's own rectangle.
    //
    // The second is not the rectangle's centre, and that is the point. A wide-FOV headset hands out an
    // asymmetric frustum per eye, skewed outwards, so each eye's axis sits off centre by a different
    // amount; here the two land over four hundred pixels apart. Two pixels sharing a column number in
    // the two eyes are therefore not looking in the same direction, and a reprojection that assumes
    // they are carries the detail across by that whole difference. The vertical frustum is symmetric
    // on both eyes, so there is no vertical counterpart and none is computed.
    struct ViewAxis {
        float focal{0.f};
        float center{0.f};
    };

    bool ViewAxisOf(const XrFovf& fov, uint32_t width, ViewAxis& axis) {
        const float tanLeft = tanf(fov.angleLeft);
        const float span = tanf(fov.angleRight) - tanLeft;
        if (span <= 0.f || width == 0) {
            return false;
        }
        axis.focal = (float)width / span;
        axis.center = (float)width * (-tanLeft) / span;
        return true;
    }

    // The lattice the window's *size* is snapped to, and the band the hold in HoldInBand works in.
    // Separate from kCropQuantum, which is the lattice the window's *position* moves on: a position is
    // free to move in eight-pixel steps under a size that only moves in sixty-four.
    constexpr uint32_t kWindowBand = 64;

    // The ask snapped onto the band lattice: down to a multiple of the quantum, never past the eye
    // buffer. Zero in stays zero out -- a slider at its floor means no window, which the caller already
    // treats as "nothing to do this frame".
    uint32_t WindowBand(uint32_t asked, uint32_t limit) {
        return asked == 0 ? 0u : (std::min(asked, limit) & ~(kWindowBand - 1));
    }

    // The size in hand kept, or the band taken, once the ask has settled. `hold` says the hold did the
    // work, for the log. Zero is a slider at its floor and stays zero; the snap of an ask larger than
    // the eye buffer is bounded by that buffer.
    //
    // This is AMDNR's in-band hold -- its geometry resolve keeps a not-larger ask while it is still
    // over three quarters of the size in hand -- moved onto this layer's two axes. The caller has
    // already made sure the ask is not still moving, so what is left here is the question of whether
    // the new band is far enough from the old one to be worth the rebuild.
    uint32_t HoldInBand(uint32_t banded, uint32_t held, uint32_t limit, bool& hold) {
        hold = false;
        // Nothing to hold on to on the first frame, or a size in hand that no longer fits the buffer:
        // a buffer that shrank under the window takes the new size outright.
        if (banded == 0 || held == 0 || held > limit) {
            return banded;
        }
        // Not larger than what is in hand, and over three quarters of it: inside the band.
        if (banded < held && banded * 4u > held * 3u) {
            hold = true;
            return held;
        }
        return banded;
    }

    // The neural renderer only accepts a bounded input, so a local patch of the eye buffer is
    // processed and the rim is left untouched. `hasFocus` walks that patch onto the gaze point; it is
    // clamped so the patch always stays inside the eye buffer.
    //
    // At the default scale the patch is exactly the network's ceiling, so nothing is resampled on the
    // way in or out. Sizing it relative to the *eye buffer* instead (which is what an isotropic scale
    // against both ceiling axes does) throws away ceiling width whenever the eye buffer is taller
    // than 16:9 -- here it would use 1344 of the 1920 available columns.
    CropPlan ComputeCrop(uint32_t view,
                         const XrRect2Di& rect,
                         float focusU,
                         float focusV,
                         bool hasFocus) {
        CropAnchor& anchor = g_anchor[view < kMaxViews ? view : 0];
        CropPlan plan;
        const uint32_t width = (uint32_t)rect.extent.width;
        const uint32_t height = (uint32_t)rect.extent.height;
        if (width == 0 || height == 0) {
            return plan;
        }

        // On the band lattice so that, at full scale, the patch and the network input are the same
        // size and every conversion in the pass is exactly 1:1.
        //
        // The ceiling and the two cover factors all come from the live block rather than from constants,
        // and between them that is the whole of what the panel's size sliders do. Cover below 1 shrinks
        // the window and the network input with it, which is the only direction of the two that costs
        // less; the ceiling moves the working resolution outright, up to the runtime's own limit. The
        // axes are separate for the reason above -- a single number would size the window against the
        // eye buffer and give up ceiling width.
        const NrSettings box = NrSettingsGet();
        // The automatic tier trims the two fractions, never the ceiling: the ceiling is a working
        // resolution and moving it would rebuild the network, while the fractions are per-frame. Off,
        // `g_qualityScale` is exactly 1 and the two asks below are the ones they always were.
        const float trim = std::clamp(g_qualityScale, 0.25f, 1.f);
        const uint32_t askedWidth = (uint32_t)((float)box.ceilingWidth * box.windowWidth * trim);
        const uint32_t askedHeight = (uint32_t)((float)box.ceilingHeight * box.windowHeight * trim);
        const uint32_t bandWidth = WindowBand(askedWidth, width);
        const uint32_t bandHeight = WindowBand(askedHeight, height);

        // The settle gate. The ask above moves with every slider step, and each band it crosses is a
        // rebuild of the runtime's network -- seconds of stalled frames. So the band is only believed
        // once it has stood still for kWindowSettleMs: while a slider is being dragged the window keeps
        // the size it already has, and one rebuild happens after the drag stops. A size already in hand
        // is not affected either way -- this gates the *changing* of the size, not the pass rate.
        const auto now = std::chrono::steady_clock::now();
        if (bandWidth != g_windowAskBand[0] || bandHeight != g_windowAskBand[1]) {
            g_windowAskBand[0] = bandWidth;
            g_windowAskBand[1] = bandHeight;
            g_windowAskAt = now;
        }
        // The first frame has nothing to hold and applies outright; everything else waits out the dwell.
        const bool settled = (g_windowW == 0 || g_windowH == 0) ||
                             std::chrono::duration<double, std::milli>(now - g_windowAskAt).count() >=
                                 kWindowSettleMs;

        // An unsettled ask is not applied at all -- the size in hand runs unchanged. A settled one goes
        // through the in-band hold (see HoldInBand), which is what keeps a merely-shrunk ask from
        // paying for a rebuild it does not need.
        bool holdWidth = false;
        bool holdHeight = false;
        const uint32_t cropWidth = settled ? HoldInBand(bandWidth, g_windowW, width, holdWidth) : g_windowW;
        const uint32_t cropHeight =
            settled ? HoldInBand(bandHeight, g_windowH, height, holdHeight) : g_windowH;
        if (cropWidth == 0 || cropHeight == 0) {
            return plan;
        }
        // One line per change of the window's size, because that frame is the one the runtime rebuilds
        // its network on and the stall a run's log has to be read against; the hold and the settle both
        // get one line per episode instead of one per frame, since their whole point is the frames
        // where nothing moved -- a drag that crosses four bands is still one episode.
        if (cropWidth != g_windowW || cropHeight != g_windowH) {
            g_windowHoldLogged = false;
            g_windowSettleLogged = false;
            if (g_windowW != 0 || g_windowH != 0) {
                LayerLog("layer: window %ux%u (ask %ux%u, band %ux%u) - a new size rebuilds the network\n",
                         cropWidth,
                         cropHeight,
                         askedWidth,
                         askedHeight,
                         bandWidth,
                         bandHeight);
            }
        } else if ((holdWidth || holdHeight) && !g_windowHoldLogged) {
            g_windowHoldLogged = true;
            LayerLog("layer: window %ux%u held (ask %ux%u) - inside the band already paid for\n",
                     cropWidth,
                     cropHeight,
                     askedWidth,
                     askedHeight);
        } else if (!settled && !g_windowSettleLogged) {
            g_windowSettleLogged = true;
            LayerLog("layer: window %ux%u waiting (ask band %ux%u) - the size moves once the sliders stop\n",
                     cropWidth,
                     cropHeight,
                     bandWidth,
                     bandHeight);
        }
        g_windowW = cropWidth;
        g_windowH = cropHeight;

        const float fit = std::min(1.f,
                                   std::min((float)box.ceilingWidth / (float)cropWidth,
                                            (float)box.ceilingHeight / (float)cropHeight));
        plan.networkWidth = std::max(64u, ((uint32_t)((float)cropWidth * fit)) & ~7u);
        plan.networkHeight = std::max(64u, ((uint32_t)((float)cropHeight * fit)) & ~7u);

        // What this frame came out as, for the panel to read back. It is the only place that can say:
        // the window is banded, held against what came before and clamped against an eye buffer the
        // panel does not know, so the number that ran is not the number the sliders ask for.
        {
            NrWindowReport report;
            report.window[0] = cropWidth;
            report.window[1] = cropHeight;
            report.network[0] = plan.networkWidth;
            report.network[1] = plan.networkHeight;
            NrWindowReportSet(report);
        }

        // The window may never start outside the rectangle the application rendered this view into, and
        // both paths below need that bound, so it is computed once here.
        //
        // It is not a formality, because of the lattice snap: `& ~7` can move an origin DOWN by up to
        // seven pixels, and when the view's own rectangle does not start on a multiple of eight that
        // puts the window's leading edge outside the view -- a few columns of whatever the allocation
        // holds there, denoised and pasted back where they never belonged. The reference meets the same
        // problem from the other side and calls it the active region; the remedy is the same, snap
        // first and clamp to the view second, which is why the snap below is wrapped in the clamp.
        const int32_t maxLeft = rect.offset.x + (int32_t)(width - cropWidth);
        const int32_t maxTop = rect.offset.y + (int32_t)(height - cropHeight);

        // Centred, and on the same lattice the tracked path snaps to. Centring alone lands on half of
        // that grid whenever the eye buffer's width is not a multiple of sixteen, which leaves the
        // window half a texel off the samples it was drawn on.
        int32_t left = std::clamp(
            (rect.offset.x + (int32_t)((width - cropWidth) / 2)) & ~(kCropQuantum - 1),
            rect.offset.x,
            maxLeft);
        int32_t top = std::clamp(
            (rect.offset.y + (int32_t)((height - cropHeight) / 2)) & ~(kCropQuantum - 1),
            rect.offset.y,
            maxTop);
        if (hasFocus) {
            int32_t wantedLeft =
                rect.offset.x + (int32_t)(focusU * (float)width) - (int32_t)(cropWidth / 2);
            int32_t wantedTop =
                rect.offset.y + (int32_t)(focusV * (float)height) - (int32_t)(cropHeight / 2);

            // Snap to the grid first so sub-quantum gaze noise cannot nudge the window, then hold
            // the previous anchor until the snapped target leaves the dead zone, then clamp.
            wantedLeft = (wantedLeft & ~(kCropQuantum - 1));
            wantedTop = (wantedTop & ~(kCropQuantum - 1));
            wantedLeft = std::clamp(wantedLeft, rect.offset.x, maxLeft);
            wantedTop = std::clamp(wantedTop, rect.offset.y, maxTop);

            // A bounded step replaces the dead zone rather than joining it: the whole point of the
            // bound is that the window goes on moving, so gating the same move on a dead zone would
            // simply stop it. The clamp is to the intersection of the bound and the eye buffer, so a
            // window already against an edge cannot be walked out of it one step at a time.
            const int32_t step = (int32_t)g_gazeMaxStep;
            if (step > 0 && anchor.valid && anchor.width == (int32_t)cropWidth &&
                anchor.height == (int32_t)cropHeight) {
                wantedLeft = std::clamp(wantedLeft,
                                        std::max(rect.offset.x, anchor.left - step),
                                        std::min(maxLeft, anchor.left + step));
                wantedTop = std::clamp(wantedTop,
                                       std::max(rect.offset.y, anchor.top - step),
                                       std::min(maxTop, anchor.top + step));
            }
            const int32_t zone = step > 0 ? 0 : (int32_t)g_gazeDeadZone;

            const bool moved = !anchor.valid || anchor.width != (int32_t)cropWidth ||
                               anchor.height != (int32_t)cropHeight ||
                               std::abs(wantedLeft - anchor.left) > zone ||
                               std::abs(wantedTop - anchor.top) > zone;
            if (moved) {
                anchor = CropAnchor{true, (int32_t)cropWidth, (int32_t)cropHeight, wantedLeft, wantedTop};
                g_anchorMoves++;
            }
        }

        // A held window keeps the same pixels out of the neural pass, so the rim between filtered
        // and unfiltered content stays put instead of crawling.
        if (anchor.valid && anchor.width == (int32_t)cropWidth &&
            anchor.height == (int32_t)cropHeight) {
            left = anchor.left;
            top = anchor.top;
        }

        plan.region = RECT{left, top, left + (LONG)cropWidth, top + (LONG)cropHeight};
        return plan;
    }

    bool EnsureInterop() {
        // Checked before the initialized test, which would otherwise report the interop as ready for
        // ever and quietly turn the latch set on a failed pass into dead code -- which is what the
        // old `g_interopTried = true` there was.
        if (g_interopGaveUp) {
            return false;
        }
        if (g_interop.IsInitialized()) {
            return true;
        }
        if (g_interopTried) {
            return false;
        }
        g_interopTried = true;

        if (!g_isD3D11 || !g_appDevice || !g_appContext) {
            LayerLog("layer: no D3D11 graphics binding, the neural renderer stays off\n");
            return false;
        }
        return g_interop.Initialize(g_appDevice.Get(), g_appContext.Get());
    }

    // Diagnostic: lists what the runtime below actually offers, so an unsupported extension shows
    // up in the log instead of silently doing nothing. The question goes through the layer's own
    // downstream dispatch instead of looking for a resident openxr_loader.dll, because the loader
    // is frequently statically linked into the application and never shows up as a module.
    void LogRuntimeExtensions() {
        if (g_next.GetInstanceProcAddr == nullptr || g_next.Instance == XR_NULL_HANDLE) {
            LayerLog("layer: runtime extensions unavailable (no downstream instance dispatch)\n");
            return;
        }
        PFN_xrVoidFunction resolved = nullptr;
        if (XR_FAILED(g_next.GetInstanceProcAddr(g_next.Instance,
                                                 "xrEnumerateInstanceExtensionProperties",
                                                 &resolved)) ||
            resolved == nullptr) {
            LayerLog("layer: the runtime does not expose xrEnumerateInstanceExtensionProperties\n");
            return;
        }
        const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(resolved);

        uint32_t count = 0;
        const XrResult first = enumerate(nullptr, 0, &count, nullptr);
        if (XR_FAILED(first) || count == 0) {
            LayerLog("layer: extension enumeration failed (%d, count=%u)\n", (int)first, count);
            return;
        }
        std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
        const XrResult second = enumerate(nullptr, count, &count, properties.data());
        if (XR_FAILED(second)) {
            LayerLog("layer: extension enumeration failed (%d)\n", (int)second);
            return;
        }

        bool eyeGaze = false;
        bool layerDepth = false;
        bool reprojection = false;
        bool frameSynthesis = false;
        bool spaceWarp = false;
        bool quadViews = false;
        std::string names;
        for (uint32_t i = 0; i < count; i++) {
            const char* name = properties[i].extensionName;
            eyeGaze = eyeGaze || std::strcmp(name, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0;
            layerDepth = layerDepth || std::strcmp(name, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0;
            reprojection = reprojection || std::strcmp(name, "XR_MSFT_composition_layer_reprojection") == 0;
            frameSynthesis = frameSynthesis || std::strcmp(name, "XR_EXT_frame_synthesis") == 0;
            spaceWarp = spaceWarp || std::strcmp(name, "XR_FB_space_warp") == 0;
            quadViews = quadViews || std::strcmp(name, "XR_VARJO_quad_views") == 0;
            names += " ";
            names += name;
        }
        LayerLog("layer: runtime offers %u extensions:%s\n", count, names.c_str());
        LayerLog("layer: runtime key extensions: composition_layer_depth=%s "
                 "composition_layer_reprojection=%s frame_synthesis=%s space_warp=%s "
                 "quad_views=%s eye_gaze=%s\n",
                 layerDepth ? "present" : "absent",
                 reprojection ? "present" : "absent",
                 frameSynthesis ? "present" : "absent",
                 spaceWarp ? "present" : "absent",
                 quadViews ? "present" : "absent",
                 eyeGaze ? "present" : "absent");
    }

    // Builds a private action set bound to the eye gaze profile. It is attached together with the
    // application's own sets so the application never sees a second attach.
    void SetupEyeGaze(XrSession session) {
        if (!g_gaze.extensionOffered || g_next.Instance == XR_NULL_HANDLE || g_next.StringToPath == nullptr ||
            g_next.CreateActionSet == nullptr || g_next.CreateAction == nullptr ||
            g_next.CreateActionSpace == nullptr || g_next.SuggestInteractionProfileBindings == nullptr) {
            LayerLog("layer: eye gaze not wired up (extension offered=%d)\n", g_gaze.extensionOffered ? 1 : 0);
            return;
        }

        // A runtime may hand out more than one session in a process, so release whatever the
        // previous one left behind instead of overwriting the handles.
        if (g_gaze.space != XR_NULL_HANDLE && g_next.DestroySpace) {
            g_next.DestroySpace(g_gaze.space);
        }
        if (g_gaze.actionSet != XR_NULL_HANDLE && g_next.DestroyActionSet) {
            g_next.DestroyActionSet(g_gaze.actionSet);
        }
        g_gaze.space = XR_NULL_HANDLE;
        g_gaze.actionSet = XR_NULL_HANDLE;
        g_gaze.action = XR_NULL_HANDLE;
        g_gaze.attached = false;
        g_gaze.poseValid = false;

        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "amdnr_eye_gaze");
        strcpy_s(setInfo.localizedActionSetName, "AMDNR eye gaze");
        if (XR_FAILED(g_next.CreateActionSet(g_next.Instance, &setInfo, &g_gaze.actionSet))) {
            LayerLog("layer: eye gaze action set rejected\n");
            return;
        }

        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(actionInfo.actionName, "gaze");
        strcpy_s(actionInfo.localizedActionName, "eye gaze");
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        if (XR_FAILED(g_next.CreateAction(g_gaze.actionSet, &actionInfo, &g_gaze.action))) {
            LayerLog("layer: eye gaze action rejected\n");
            return;
        }

        XrPath gazePath = XR_NULL_PATH;
        XrPath profilePath = XR_NULL_PATH;
        if (XR_FAILED(g_next.StringToPath(g_next.Instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath)) ||
            XR_FAILED(g_next.StringToPath(
                g_next.Instance, "/interaction_profiles/ext/eye_gaze_interaction", &profilePath))) {
            LayerLog("layer: eye gaze paths rejected\n");
            return;
        }

        XrActionSuggestedBinding binding{g_gaze.action, gazePath};
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = 1;
        suggested.suggestedBindings = &binding;
        g_next.SuggestInteractionProfileBindings(g_next.Instance, &suggested);

        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = g_gaze.action;
        spaceInfo.subactionPath = XR_NULL_PATH;
        spaceInfo.poseInActionSpace.orientation = XrQuaternionf{0.f, 0.f, 0.f, 1.f};
        spaceInfo.poseInActionSpace.position = XrVector3f{0.f, 0.f, 0.f};
        if (XR_FAILED(g_next.CreateActionSpace(session, &spaceInfo, &g_gaze.space))) {
            LayerLog("layer: eye gaze space rejected\n");
            return;
        }

        LayerLog("layer: eye gaze ready, waiting for the application to attach its action sets\n");
    }

    // ---- Hooks ----

    // The application sizes its eye buffers from this extent, so it is the one lever that changes how
    // much the application itself has to shade. Everything else the layer does happens once the
    // pixels already exist.
    XrResult XRAPI_CALL Hook_xrEnumerateViewConfigurationViews(XrInstance instance,
                                                              XrSystemId systemId,
                                                              XrViewConfigurationType viewConfigurationType,
                                                              uint32_t viewCapacityInput,
                                                              uint32_t* viewCountOutput,
                                                              XrViewConfigurationView* views) {
        const XrResult result = g_next.EnumerateViewConfigurationViews(
            instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
        // A zero capacity is a count-only probe: views is null and must stay untouched.
        if (XR_FAILED(result) || views == nullptr ||
            viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            return result;
        }

        const uint32_t count = std::min(viewCapacityInput, *viewCountOutput);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t recommendedWidth = views[i].recommendedImageRectWidth;
            const uint32_t recommendedHeight = views[i].recommendedImageRectHeight;
            if (g_renderScale != 1.f) {
                views[i].recommendedImageRectWidth = ScaleRecommended(recommendedWidth);
                views[i].recommendedImageRectHeight = ScaleRecommended(recommendedHeight);
            }
            // The output swapchain is sized from the recommendation as the runtime states it, so
            // this has to be read before g_renderScale is applied below.
            if (g_viewWidth == 0 || g_viewHeight == 0) {
                g_viewWidth = recommendedWidth;
                g_viewHeight = recommendedHeight;
            }
            if (!g_renderScaleLogged) {
                LayerLog("layer: runtime recommends %ux%u per eye (max %ux%u), the layer asks for %ux%u\n",
                         recommendedWidth,
                         recommendedHeight,
                         views[i].maxImageRectWidth,
                         views[i].maxImageRectHeight,
                         views[i].recommendedImageRectWidth,
                         views[i].recommendedImageRectHeight);
            }
        }
        if (count > 0) {
            g_renderScaleLogged = true;
        }
        return result;
    }

    // The one place a layer can put a sub-pixel offset under the application: the frustum it hands
    // the application to render with.
    XrResult XRAPI_CALL Hook_xrLocateViews(XrSession session,
                                           const XrViewLocateInfo* viewLocateInfo,
                                           XrViewState* viewState,
                                           uint32_t viewCapacityInput,
                                           uint32_t* viewCountOutput,
                                           XrView* views) {
        const XrResult result =
            g_next.LocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
        if (XR_FAILED(result) || views == nullptr || viewLocateInfo == nullptr ||
            viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            return result;
        }
        const uint32_t count = std::min(viewCapacityInput, *viewCountOutput);
        if (count == 0 || count > kMaxViews) {
            return result;
        }

        // Kept whether or not an offset is in play, because the submit path needs the frustum the
        // application would have had in either case.
        g_unJitteredFovValid = true;
        for (uint32_t i = 0; i < count; i++) {
            g_unJitteredFov[i] = views[i].fov;
        }
        if (!g_jitter) {
            return result;
        }

        // The application renders at this fraction of the view size, so that is the pixel grid the
        // offset is measured on.
        const float width = (float)g_viewWidth * g_renderScale;
        const float height = (float)g_viewHeight * g_renderScale;
        if (width < 1.f || height < 1.f) {
            return result;
        }

        for (uint32_t i = 0; i < count; i++) {
            const XrFovf f = views[i].fov;
            // Moving the window the other way moves the picture: content lands `g_jitterX` pixels
            // further right when the frustum's own left edge moves left by that much of its width.
            const float dx = (g_jitterX / width) * (f.angleRight - f.angleLeft);
            const float dy = (g_jitterY / height) * (f.angleUp - f.angleDown);
            views[i].fov.angleLeft -= dx;
            views[i].fov.angleRight -= dx;
            // The image's y axis points down while the angles point up, so the same picture shift is
            // the opposite angular one.
            views[i].fov.angleUp += dy;
            views[i].fov.angleDown += dy;
        }

        if (!g_jitterLogged) {
            g_jitterLogged = true;
            LayerLog("layer: the frustum carries a jitter of (%.3f, %.3f) render px on a %ux%u grid\n",
                     g_jitterX,
                     g_jitterY,
                     (uint32_t)width,
                     (uint32_t)height);
        }
        return result;
    }

    XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance,
                                             const XrSessionCreateInfo* createInfo,
                                             XrSession* session) {
        for (const XrBaseInStructure* node = (const XrBaseInStructure*)createInfo->next; node != nullptr;
             node = node->next) {
            if (node->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const auto* binding = (const XrGraphicsBindingD3D11KHR*)node;
                g_isD3D11 = true;
                g_appDevice = binding->device;
                if (g_appDevice) {
                    g_appDevice->GetImmediateContext(g_appContext.ReleaseAndGetAddressOf());
                    // The game draws its scene through this context, so watching it here is what
                    // makes the depth buffer reachable without a present hook of our own.
                    DepthCaptureAttach(g_appDevice.Get(), g_appContext.Get());
                }
            } else if (node->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                g_isD3D11 = false;
            }
        }

        if (!g_bindingLogged) {
            g_bindingLogged = true;
            if (g_isD3D11 && g_appDevice) {
                ComPtr<IDXGIDevice> dxgiDevice;
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(g_appDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf()))) &&
                    SUCCEEDED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf()))) {
                    DXGI_ADAPTER_DESC desc{};
                    adapter->GetDesc(&desc);
                    LayerLog("layer: D3D11 binding on %ls\n", desc.Description);
                }
            } else {
                LayerLog("layer: non-D3D11 binding, the neural renderer stays off\n");
            }
        }

        const XrResult result = g_next.CreateSession(instance, createInfo, session);
        if (XR_SUCCEEDED(result)) {
            SetupEyeGaze(*session);
        }
        return result;
    }

    XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session,
                                               const XrSwapchainCreateInfo* createInfo,
                                               XrSwapchain* swapchain) {
        g_session = session;
        const XrResult result = g_next.CreateSwapchain(session, createInfo, swapchain);
        if (XR_FAILED(result)) {
            return result;
        }

        TrackedSwapchain entry;
        entry.handle = *swapchain;
        entry.width = createInfo->width;
        entry.height = createInfo->height;
        entry.arraySize = createInfo->arraySize;
        entry.format = (DXGI_FORMAT)createInfo->format;

        if (IsColorFormat(entry.format) && g_next.EnumerateSwapchainImages) {
            // A colour chain the application made is where the eyes are drawn, so its size is the
            // one a scene depth buffer should match. Only application chains reach this hook; the
            // layer's own output chains are created through the next dispatch directly.
            DepthCaptureSetEyeSize(entry.width, entry.height);
            uint32_t count = 0;
            if (XR_SUCCEEDED(g_next.EnumerateSwapchainImages(*swapchain, 0, &count, nullptr)) && count > 0) {
                std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                if (XR_SUCCEEDED(g_next.EnumerateSwapchainImages(
                        *swapchain, count, &count, (XrSwapchainImageBaseHeader*)images.data()))) {
                    for (uint32_t i = 0; i < count; i++) {
                        entry.textures.push_back(images[i].texture);
                    }
                    LayerLog("layer: eye swapchain %ux%u fmt=%d images=%u array=%u\n",
                             entry.width,
                             entry.height,
                             (int)entry.format,
                             count,
                             entry.arraySize);
                }
            }
        }

        g_swapchains.push_back(std::move(entry));
        return result;
    }

    XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                     const XrSwapchainImageAcquireInfo* acquireInfo,
                                                     uint32_t* index) {
        const XrResult result = g_next.AcquireSwapchainImage(swapchain, acquireInfo, index);
        if (XR_SUCCEEDED(result)) {
            if (TrackedSwapchain* entry = FindSwapchain(swapchain)) {
                entry->lastAcquired = *index;
                entry->acquiredThisFrame = true;
            }
        }
        return result;
    }

    XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                     const XrSwapchainImageReleaseInfo* releaseInfo) {
        return g_next.ReleaseSwapchainImage(swapchain, releaseInfo);
    }

    // The probe's own chains follow the output chains' lifetime, torn down once per session and once
    // more when the instance goes. The failure latch is cleared with them: a new session is a fresh
    // context and deserves a fresh attempt.
    void ReleaseDepthProbeChains() {
        if (g_next.DestroySwapchain != nullptr) {
            for (DepthProbeChain& chain : g_depthChains) {
                if (chain.handle != XR_NULL_HANDLE) {
                    g_next.DestroySwapchain(chain.handle);
                    chain.handle = XR_NULL_HANDLE;
                }
            }
        }
        for (DepthProbeChain& chain : g_depthChains) {
            chain.textures.clear();
        }
        g_depthChains.clear();
        g_depthProbeFailed = false;
        g_depthProbeAcquireLogged = false;
    }

    // The layer's own output chains outlive the frame they were built for, so they have to be torn
    // down wherever the application's chains are: once per chain, and once for all of them when the
    // session goes away.
    void ReleaseOutputSwapchains() {
        ReleaseDepthProbeChains();
        if (g_next.DestroySwapchain != nullptr) {
            for (TrackedSwapchain& entry : g_swapchains) {
                if (entry.outHandle != XR_NULL_HANDLE) {
                    g_next.DestroySwapchain(entry.outHandle);
                    entry.outHandle = XR_NULL_HANDLE;
                }
            }
        }
        for (TrackedSwapchain& entry : g_swapchains) {
            entry.outRtvs.clear();
            entry.srcSrvs.clear();
            entry.midTexture.Reset();
            entry.midRtv.Reset();
            entry.midSrv.Reset();
        }
    }

    XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain) {
        for (auto it = g_swapchains.begin(); it != g_swapchains.end(); ++it) {
            if (it->handle == swapchain) {
                if (it->outHandle != XR_NULL_HANDLE && g_next.DestroySwapchain != nullptr) {
                    g_next.DestroySwapchain(it->outHandle);
                }
                g_swapchains.erase(it);
                break;
            }
        }
        return g_next.DestroySwapchain(swapchain);
    }

    // The application attaches its own action sets exactly once; the gaze set rides along, which is
    // the only way a layer can get an action attached without breaking the application.
    XrResult XRAPI_CALL Hook_xrAttachSessionActionSets(XrSession session,
                                                       const XrSessionActionSetsAttachInfo* attachInfo) {
        if (g_gaze.actionSet == XR_NULL_HANDLE || g_gaze.attached) {
            return g_next.AttachSessionActionSets(session, attachInfo);
        }

        std::vector<XrActionSet> sets(attachInfo->actionSets, attachInfo->actionSets + attachInfo->countActionSets);
        sets.push_back(g_gaze.actionSet);

        XrSessionActionSetsAttachInfo merged = *attachInfo;
        merged.countActionSets = (uint32_t)sets.size();
        merged.actionSets = sets.data();

        const XrResult result = g_next.AttachSessionActionSets(session, &merged);
        g_gaze.attached = XR_SUCCEEDED(result);
        LayerLog("layer: eye gaze action set attach -> %d (attached=%d)\n", (int)result, g_gaze.attached ? 1 : 0);
        return result;
    }

    XrResult XRAPI_CALL Hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
        // A zero count means "every attached set", which already covers the gaze set.
        if (!g_gaze.attached || syncInfo->countActiveActionSets == 0 || g_next.SyncActions == nullptr) {
            return g_next.SyncActions(session, syncInfo);
        }

        std::vector<XrActiveActionSet> sets(syncInfo->activeActionSets,
                                           syncInfo->activeActionSets + syncInfo->countActiveActionSets);
        sets.push_back(XrActiveActionSet{g_gaze.actionSet, XR_NULL_PATH});

        XrActionsSyncInfo merged = *syncInfo;
        merged.countActiveActionSets = (uint32_t)sets.size();
        merged.activeActionSets = sets.data();
        return g_next.SyncActions(session, &merged);
    }

    void LocateEyeGaze(XrSpace baseSpace, XrTime displayTime) {
        g_gaze.poseValid = false;
        if (g_gaze.space == XR_NULL_HANDLE || !g_gaze.attached || g_next.LocateSpace == nullptr ||
            baseSpace == XR_NULL_HANDLE) {
            return;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult locate = g_next.LocateSpace(g_gaze.space, baseSpace, displayTime, &location);
        const XrSpaceLocationFlags required =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const bool tracked = XR_SUCCEEDED(locate) && (location.locationFlags & required) == required;

        // Throttled, and here rather than nowhere: every failure below used to return silently, which
        // made the two very different cases -- the runtime has no eye tracking to give, or this layer
        // failed to ask for it properly -- look identical from the outside. All the frame report shows
        // is gaze=0, which says nothing about which one it is.
        if ((g_gazeProbeSamples++ % 300u) == 0u) {
            if (XR_FAILED(locate)) {
                LayerLog("layer: gaze locate failed (%d)\n", (int)locate);
            } else if (!tracked) {
                LayerLog("layer: gaze not tracked (LocateSpace ok, flags=0x%x, needed 0x%x)\n",
                         (unsigned)location.locationFlags,
                         (unsigned)required);
            } else {
                const XrVector3f forward =
                    Rotate(XrVector3f{0.f, 0.f, -1.f}, location.pose.orientation);
                LayerLog("layer: gaze tracked, dir=(%.3f, %.3f, %.3f) pos=(%.3f, %.3f, %.3f)\n",
                         forward.x,
                         forward.y,
                         forward.z,
                         location.pose.position.x,
                         location.pose.position.y,
                         location.pose.position.z);
            }
        }

        if (!tracked) {
            return;
        }

        g_gaze.pose = location.pose;
        g_gaze.poseValid = true;
    }

    const XrCompositionLayerDepthInfoKHR* FindDepthInfo(const void* chain) {
        for (const XrBaseInStructure* node = (const XrBaseInStructure*)chain; node != nullptr;
             node = node->next) {
            if (node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                return (const XrCompositionLayerDepthInfoKHR*)node;
            }
        }
        return nullptr;
    }

    // Staples a synthetic depth layer onto the views the runtime will actually receive, returning how
    // many views got one. The depth infos live in `depthStorage`, one instance per view: a struct
    // shared by both eyes reads as a single submission and invites the runtime to collapse them, and
    // it also makes the log unable to tell the two apart. Views that already carry depth are left
    // alone -- the application's own depth is better than anything the probe could invent.
    uint32_t AttachDepthProbe(std::vector<XrCompositionLayerProjectionView>& views,
                              size_t base,
                              uint32_t count,
                              std::vector<XrCompositionLayerDepthInfoKHR>& depthStorage) {
        if (g_depthProbeMode == DepthProbeMode::Off || g_depthProbeFailed) {
            return 0;
        }

        // Resolved once for the whole frame, so both eyes always show the same step of the cycle and
        // a step boundary landing between them cannot leave the pair disagreeing.
        const DepthProbeMode shown = ResolveDepthProbeMode();

        uint32_t attached = 0;
        for (uint32_t i = 0; i < count; i++) {
            XrCompositionLayerProjectionView& view = views[base + i];
            if (FindDepthInfo(view.next) != nullptr) {
                if (!g_depthProbeYieldLogged) {
                    g_depthProbeYieldLogged = true;
                    LayerLog("layer: depth probe stands down on a view that already carries depth\n");
                }
                continue;
            }

            // The depth rectangle has to be the one the colour is sampled from: the runtime pairs the
            // two by position, so a size mismatch would put the probe's picture somewhere it is not.
            const XrRect2Di& rect = view.subImage.imageRect;
            const uint32_t width = (uint32_t)rect.extent.width;
            const uint32_t height = (uint32_t)rect.extent.height;
            if (width == 0 || height == 0) {
                continue;
            }

            DepthProbeChain* chain = EnsureDepthChain(width, height);
            if (chain == nullptr || !AcquireDepthProbeImage(*chain, shown)) {
                if (!g_depthProbeAcquireLogged) {
                    g_depthProbeAcquireLogged = true;
                    LayerLog("layer: depth probe could not take an image, skipping it this frame\n");
                }
                continue;
            }

            XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            // Chained in front of whatever the view already carried, so nothing downstream is lost.
            depth.next = view.next;
            depth.subImage.swapchain = chain->handle;
            depth.subImage.imageRect.offset.x = 0;
            depth.subImage.imageRect.offset.y = 0;
            depth.subImage.imageRect.extent.width = (int32_t)width;
            depth.subImage.imageRect.extent.height = (int32_t)height;
            depth.subImage.imageArrayIndex = 0;
            depth.nearZ = 0.05f;
            depth.farZ = 100.f;
            depth.minDepth = 0.f;
            depth.maxDepth = 1.f;
            depthStorage.push_back(depth);
            view.next = &depthStorage.back();
            attached++;

            if (!g_depthProbeAttachLogged) {
                g_depthProbeAttachLogged = true;
                LayerLog("layer: depth probe attached: mode=%s chain %ux%u fmt=%d (D32_FLOAT) "
                         "rect=(%d,%d %dx%d) nearZ=%.2f farZ=%.2f minDepth=%.1f maxDepth=%.1f\n",
                         DepthProbeModeName(g_depthProbeMode),
                         chain->width,
                         chain->height,
                         (int)DXGI_FORMAT_D32_FLOAT,
                         rect.offset.x,
                         rect.offset.y,
                         rect.extent.width,
                         rect.extent.height,
                         depth.nearZ,
                         depth.farZ,
                         depth.minDepth,
                         depth.maxDepth);
            }
        }
        return attached;
    }

    // Without the upscale path there is no layer-owned view array to hang depth on, and the
    // application's views are const, so the probe copies them for this one purpose. Returns false
    // when there is nothing to attach, which leaves such a frame forwarded bit for bit.
    bool RedirectForDepthProbe(const XrCompositionLayerProjection* projection,
                               std::vector<XrCompositionLayerProjectionView>& viewStorage,
                               std::vector<XrCompositionLayerProjection>& layerStorage,
                               std::vector<XrCompositionLayerDepthInfoKHR>& depthStorage,
                               std::vector<XrCompositionLayerBaseHeader*>& headerStorage) {
        if (g_depthProbeMode == DepthProbeMode::Off || g_depthProbeFailed || projection->viewCount == 0) {
            return false;
        }

        const size_t base = viewStorage.size();
        for (uint32_t view = 0; view < projection->viewCount; view++) {
            viewStorage.push_back(projection->views[view]);
        }
        if (AttachDepthProbe(viewStorage, base, projection->viewCount, depthStorage) == 0) {
            viewStorage.resize(base);
            return false;
        }

        XrCompositionLayerProjection layer = *projection;
        layer.views = &viewStorage[base];
        layerStorage.push_back(layer);
        headerStorage.push_back((XrCompositionLayerBaseHeader*)&layerStorage.back());
        return true;
    }

    // Whether the application hands the compositor a depth buffer decides how good any temporal
    // upscale built on this layer can be: without depth there is no way to tell a moving object from
    // camera parallax, and no temporal upscaler -- FSR4 included -- can be fed properly.
    //
    // The layer declares XR_KHR_composition_layer_depth in its manifest so that the extension shows
    // up in what the application is offered. Whether the application then submits a depth layer is
    // not something a layer can force, so both answers are worth a line: "never" once, and the first
    // time one does turn up.
    void ProbeDepthLayer(const XrCompositionLayerProjection* projection) {
        if (projection->viewCount == 0) {
            return;
        }

        // The extension hangs off the projection view; check the layer as well so a runtime that puts
        // it there cannot produce a false negative.
        const XrCompositionLayerDepthInfoKHR* depth = FindDepthInfo(projection->views[0].next);
        if (depth == nullptr) {
            depth = FindDepthInfo(projection->next);
        }

        if (depth == nullptr) {
            if (!g_depthAbsenceLogged) {
                g_depthAbsenceLogged = true;
                LayerLog("layer: the projection layer carries no depth, so the layer sees colour only\n");
            }
            return;
        }
        if (g_depthSeen) {
            return;
        }
        g_depthSeen = true;

        const TrackedSwapchain* chain = FindSwapchain(depth->subImage.swapchain);
        LayerLog("layer: the projection layer carries depth %ux%u fmt=%d nearZ=%.3f farZ=%.3f\n",
                 depth->subImage.imageRect.extent.width,
                 depth->subImage.imageRect.extent.height,
                 chain ? (int)chain->format : -1,
                 depth->nearZ,
                 depth->farZ);
    }

    // Applies the pass to the colour eye buffers referenced by the projection layers.
    void ProcessProjectionLayer(const XrCompositionLayerProjection* projection, XrTime displayTime) {
        // Cleared before anything can return early, and before the loop that fills it: the output pass
        // reads these later in the same frame, and a view this frame does not reach is one the effect
        // should be told nothing about rather than be handed last frame's answer for.
        for (uint32_t slot = 0; slot < kMaxViews; slot++) {
            g_fsr4Motion[slot] = NrMotion{};
        }

        if (g_skipPass) {
            return;
        }

        // Settle the automatic tier before any window is cut, so this frame is cut at the tier it just
        // chosen rather than one frame behind. Judged on the previous frame's measured pass cost; a
        // zero means no pass has completed yet, and feeding that would read as "the pass is free".
        {
            const NrSettings live = NrSettingsGet();
            if (live.autoQuality) {
                // Re-solved from the headset's real interval before the sample is taken. A refresh rate
                // the title changes mid-session moves it; so does switching headsets. Zero means the
                // runtime has not reported one yet, and the fallback the controller was built with
                // stands until it does.
                if (g_predictedPeriod > 0) {
                    g_quality.setFrameBudget((double)g_predictedPeriod / 1.0e6 * kNrQualityBudgetFraction);
                }
                if (g_lastFrameMs > 0.0) {
                    const double nowMs = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count();
                    g_quality.observe(g_lastFrameMs, nowMs);
                }
                const float previousScale = g_qualityScale;
                g_qualityScale = (float)g_quality.currentScale();
                // Every move is logged, because a move is not free: the window is cut at the new size,
                // which rebuilds the shared textures and the runtime's working set, and that lands as a
                // stall of well over a second. Without this line a session full of those stalls cannot
                // be told apart from one where the size never moved, and the fix for the first is
                // entirely different from the fix for the second.
                if (g_qualityScale != previousScale) {
                    LayerLog("layer: the automatic tier moved the window from %.2f to %.2f (budget "
                             "%.2f ms, last pass %.2f ms, %llu down / %llu up so far); the window is "
                             "rebuilt at the new size\n",
                             (double)previousScale,
                             (double)g_qualityScale,
                             g_quality.frameBudget(),
                             g_lastFrameMs,
                             (unsigned long long)g_quality.stats().downgrades,
                             (unsigned long long)g_quality.stats().upgrades);
                }
            } else if (g_qualityScale != 1.f) {
                // Turned off: hand the window back to the sliders, and put the controller back on its
                // top rung so turning it on again starts from full rather than from where it was left.
                g_quality.reset();
                g_qualityScale = 1.f;
            }
            NrRuntimeReport rt = NrRuntimeReportGet();
            if (rt.qualityScale != (double)g_qualityScale) {
                rt.qualityScale = (double)g_qualityScale;
                NrRuntimeReportSet(rt);
            }
        }
        g_frameMs = 0.0;

        LocateEyeGaze(projection->space, displayTime);

        for (uint32_t view = 0; view < projection->viewCount; view++) {
            const XrCompositionLayerProjectionView& viewInfo = projection->views[view];
            const XrSwapchainSubImage& subImage = viewInfo.subImage;
            TrackedSwapchain* entry = FindSwapchain(subImage.swapchain);
            if (entry == nullptr || entry->textures.empty()) {
                continue;
            }

            // ---- alternate-eye probe, then the skip it exists to justify ----
            //
            // Both questions asked before anything below can return early, so the ratio counts every
            // view the frame carried: was this chain taken during the frame, and did this view's pose
            // move since the last one. A full-rate frame answers yes twice on both eyes; a renderer
            // drawing one eye per frame answers no on exactly one of them.
            {
                const uint32_t slot = view < 2 ? view : 0;
                const XrQuaternionf& q = viewInfo.pose.orientation;
                const XrVector3f& p = viewInfo.pose.position;
                bool moved = true;
                if (g_probeLastPoseValid[slot]) {
                    const XrQuaternionf& a = g_probeLastOrientation[slot];
                    const XrVector3f& b = g_probeLastPosition[slot];
                    // Sum of absolute component differences rather than an angle: the question is only
                    // "did it change at all", and a threshold on an angle would have to be chosen.
                    const float dq = std::fabs(q.x - a.x) + std::fabs(q.y - a.y) +
                                     std::fabs(q.z - a.z) + std::fabs(q.w - a.w);
                    const float dp = std::fabs(p.x - b.x) + std::fabs(p.y - b.y) + std::fabs(p.z - b.z);
                    moved = dq > 1e-6f || dp > 1e-6f;
                }
                g_probeLastOrientation[slot] = q;
                g_probeLastPosition[slot] = p;
                g_probeLastPoseValid[slot] = true;
                g_probePasses[slot] += 1;
                if (entry->acquiredThisFrame) {
                    g_probeAcquired[slot] += 1;
                }
                if (moved) {
                    g_probeMoved[slot] += 1;
                }
            }

            // A view the renderer did not draw still holds the picture this layer last wrote into it --
            // the same one the network already produced, denoised and pasted back. Running the pass again
            // would spend a whole network to recompute an answer that is already sitting on screen, so
            // the view is left alone and the compositor reprojects it forward like any late eye.
            //
            // Off unless asked for, because the signal is a property of the application rather than of
            // the specification: a title that holds a chain without re-acquiring it would be misread as
            // having skipped that eye. The probe above is what says whether the signal is really there.
            if (g_skipStaleEye && !entry->acquiredThisFrame) {
                g_staleSkips[view < 2 ? view : 0] += 1;
                continue;
            }

            float focusU = 0.f;
            float focusV = 0.f;
            const bool hasFocus = GazeFocusPoint(viewInfo, focusU, focusV);
            const CropPlan plan = ComputeCrop(view,
                                              subImage.imageRect,
                                              focusU,
                                              focusV,
                                              hasFocus);
            const RECT& box = plan.region;
            const uint32_t width = (uint32_t)(box.right - box.left);
            const uint32_t height = (uint32_t)(box.bottom - box.top);
            if (width == 0 || height == 0 || plan.networkWidth == 0 || plan.networkHeight == 0) {
                continue;
            }
            // imageArrayIndex picks the array slice inside one swapchain image; which image holds
            // this frame is what the application last acquired. Conflating the two makes the pass
            // land on a stale image, so only the frames that happen to use that image are filtered
            // and the window flickers.
            if (subImage.imageArrayIndex >= entry->arraySize ||
                entry->lastAcquired >= entry->textures.size()) {
                continue;
            }

            const bool srgb = entry->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                              entry->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

            if (hasFocus) {
                g_gazeHits++;
            }

            if (!g_cropLogged) {
                g_cropLogged = true;
                LayerLog("layer: eye %ux%u -> window %ux%u at (%ld,%ld) as %ux%u srgb=%d gaze=%d image=%u "
                         "slice=%u\n",
                         entry->width,
                         entry->height,
                         width,
                         height,
                         box.left,
                         box.top,
                         plan.networkWidth,
                         plan.networkHeight,
                         srgb ? 1 : 0,
                         hasFocus ? 1 : 0,
                         entry->lastAcquired,
                         subImage.imageArrayIndex);
                // The rectangle the application renders this view into, straight off the composition
                // layer. The window above is placed inside it, and the depth the layer reduces has to
                // be mapped onto it -- which is only possible if its own rectangle is known too.
                LayerLog("layer: view rect %ux%u at (%d,%d) inside a %ux%u swapchain\n",
                         subImage.imageRect.extent.width,
                         subImage.imageRect.extent.height,
                         subImage.imageRect.offset.x,
                         subImage.imageRect.offset.y,
                         entry->width,
                         entry->height);
            }

            // Where this view's pixels sat in the previous answer. Only rotation goes in: an eye
            // buffer is a camera image, so the head's own rotation is the motion that dominates it
            // and the one a layer can compute exactly -- rotating a pixel's ray into the previous
            // frame's view lands on the previous pixel without needing depth. Something moving inside
            // the world cannot be expressed this way, and that is what driving a temporal path from
            // outside the application costs.
            // The reduced depth, taken once and used twice: the motion field maps view pixels into the
            // depth image with its rectangle, and the gate below decides from the same rectangle
            // whether the depth may be used at all.
            uint32_t depthWidth = 0;
            uint32_t depthHeight = 0;
            const HANDLE depthHandle = DepthCaptureReduceHandle(&depthWidth, &depthHeight);

            NrMotion motion{};
            ViewTrack& track = g_viewTrack[view < kMaxViews ? view : 0];
            // Filled in whenever the poses allow it, not only for the temporal path. The asynchronous
            // pass reprojects its one-frame-old window with this same transform, so it needs the field
            // whether or not the network's own history is in use; computing it costs a few multiplies.
            if (track.valid && track.width == width && track.height == height) {
                float intoPrevious[9]{};
                float fromCurrent[9]{};
                // The conjugate takes a world direction back into the previous frame's view space.
                const XrQuaternionf inverse{-track.orientation.x,
                                            -track.orientation.y,
                                            -track.orientation.z,
                                            track.orientation.w};
                QuaternionMatrix(inverse, intoPrevious);
                QuaternionMatrix(viewInfo.pose.orientation, fromCurrent);
                MatrixMultiply(intoPrevious, fromCurrent, motion.matrix);

                const XrRect2Di& rect = subImage.imageRect;
                const float scaleX = (float)plan.networkWidth / (float)width;
                const float scaleY = (float)plan.networkHeight / (float)height;
                // Network pixel -> this view image's normalised coordinate, which is the space the
                // frustum's half-tangents live in.
                motion.a[0] = (float)(box.left - rect.offset.x) / (float)rect.extent.width;
                motion.a[1] = (float)(box.top - rect.offset.y) / (float)rect.extent.height;
                motion.b[0] = (float)width / ((float)plan.networkWidth * (float)rect.extent.width);
                motion.b[1] = (float)height / ((float)plan.networkHeight * (float)rect.extent.height);
                motion.tanLeft = tanf(viewInfo.fov.angleLeft);
                motion.tanWidth = tanf(viewInfo.fov.angleRight) - motion.tanLeft;
                motion.tanUp = tanf(viewInfo.fov.angleUp);
                motion.tanHeight = motion.tanUp - tanf(viewInfo.fov.angleDown);
                motion.extentScale[0] = (float)rect.extent.width * scaleX;
                motion.extentScale[1] = (float)rect.extent.height * scaleY;
                motion.originShift[0] = (float)(box.left - track.left) * scaleX;
                motion.originShift[1] = (float)(box.top - track.top) * scaleY;

                // The head's own travel between the two frames, carried into the previous frame's view
                // basis, which is the space `matrix` and the shader work in. A point's displacement is
                // this over its distance, so it is only usable with a distance under every pixel; the
                // depth supplies that, and until it does the term stays zero and the reprojection is
                // exactly the rotation-only one it has always been.
                const XrVector3f& now = viewInfo.pose.position;
                const float delta[3] = {now.x - track.position.x,
                                        now.y - track.position.y,
                                        now.z - track.position.z};
                motion.translation[0] = intoPrevious[0] * delta[0] + intoPrevious[1] * delta[1] +
                                        intoPrevious[2] * delta[2];
                motion.translation[1] = intoPrevious[3] * delta[0] + intoPrevious[4] * delta[1] +
                                        intoPrevious[5] * delta[2];
                motion.translation[2] = intoPrevious[6] * delta[0] + intoPrevious[7] * delta[1] +
                                        intoPrevious[8] * delta[2];

                // Where this view sits inside the image the depth describes. The reduction publishes
                // the rectangle the application *draws into*, which is not the swapchain the colour is
                // cropped from: this title's scene depth is 4784x1920 inside a 6774x2718 allocation, so
                // dividing by the swapchain's size would scale every distance into the wrong place --
                // the depths would all be read from a copy of the view that does not exist. The
                // reduction is the packed eye pair, and the view's rectangle in it is the sub-image
                // rectangle it crops its colour from, so the fraction is against the depth image.
                const float depthImageWidth = depthWidth != 0 ? (float)depthWidth : (float)entry->width;
                const float depthImageHeight = depthHeight != 0 ? (float)depthHeight : (float)entry->height;
                motion.depthOrigin[0] = (float)rect.offset.x / depthImageWidth;
                motion.depthOrigin[1] = (float)rect.offset.y / depthImageHeight;
                motion.depthScale[0] = (float)rect.extent.width / depthImageWidth;
                motion.depthScale[1] = (float)rect.extent.height / depthImageHeight;
                motion.invNear = MotionInvNear();
                motion.valid = true;
            }

            if (!EnsureInterop()) {
                // Same reason as the failure below: leaving this function leaves the output target
                // unwritten while the composition layer is still pointed at it, and the picture stops
                // changing. Leaving the loop keeps the frame.
                break;
            }

            // The depth has to *cover* this view's rectangle before it is worth anything: a distance
            // read from beyond the edge of the depth belongs to no pixel of this view. What it does NOT
            // have to do is equal the swapchain the colour is cropped from, and demanding that was the
            // bug -- the reduction publishes the rectangle the application actually draws into, which
            // for this title is the whole eye pair at 4784x1920 while the swapchain is 6774x2718, so a
            // size-equality test rejects the one buffer that is right and accepts a mostly-cleared one.
            const XrRect2Di& viewRect = subImage.imageRect;
            const int64_t viewRight = (int64_t)viewRect.offset.x + (int64_t)viewRect.extent.width;
            const int64_t viewBottom = (int64_t)viewRect.offset.y + (int64_t)viewRect.extent.height;
            const bool depthCovers = depthHandle != nullptr && viewRight > 0 && viewBottom > 0 &&
                                     (int64_t)depthWidth >= viewRight &&
                                     (int64_t)depthHeight >= viewBottom;
            g_interop.SetDepthSource(depthCovers ? depthHandle : nullptr, depthWidth, depthHeight);
            if (!depthCovers) {
                motion.invNear = 0.f;
            }

            // The same reprojection once more, for the output pass. The poses, the travel and the depth
            // mapping are identical; what changes is the grid. A network pixel is not a view pixel, so
            // the field above names its window in network texels while FSR 4 wants the whole view.
            //
            // The answer is left as a fraction of the view rather than scaled into pixels: FSR 4
            // applies its own `motionVectorScale` to whatever it is handed, so the multiplication the
            // neural path does here is one the effect does instead -- and doing it there keeps the
            // field inside the range a 16 bit texture carries without losing the sub-pixel part.
            {
                NrMotion& fsr4Motion = g_fsr4Motion[view < kMaxViews ? view : 0];
                fsr4Motion = motion;
                if (motion.valid && subImage.imageRect.extent.width > 0 &&
                    subImage.imageRect.extent.height > 0) {
                    fsr4Motion.a[0] = 0.f;
                    fsr4Motion.a[1] = 0.f;
                    fsr4Motion.b[0] = 1.f / (float)subImage.imageRect.extent.width;
                    fsr4Motion.b[1] = 1.f / (float)subImage.imageRect.extent.height;
                    fsr4Motion.extentScale[0] = 1.f;
                    fsr4Motion.extentScale[1] = 1.f;
                    // The view does not slide, so there is nothing to carry the way the network's window
                    // has to be carried when it moves on its own grid.
                    fsr4Motion.originShift[0] = 0.f;
                    fsr4Motion.originShift[1] = 0.f;
                }
            }

            if (!g_depthMotionLogged && depthCovers) {
                g_depthMotionLogged = true;
                const float plane = NrSettingsGet().motionNear;
                if (plane > 0.f) {
                    LayerLog("layer: the motion field takes translation from a %ux%u depth, near=%.4f m\n",
                             depthWidth,
                             depthHeight,
                             plane);
                } else {
                    LayerLog("layer: a %ux%u depth is available, but motion_near is 0, so the motion "
                             "field stays rotation-only\n",
                             depthWidth,
                             depthHeight);
                }
            }

            ID3D11Texture2D* texture = entry->textures[entry->lastAcquired].Get();
            g_lastImage = entry->lastAcquired;

            // The near plane the title projects with is the one number in the motion path that nothing
            // tells a layer, and the depth the layer captures carries it as a scale factor rather than
            // as a value. What does carry it is the disparity between the two eyes: both views are
            // handed over here, and the solve that reads the plane back out of them runs on the second
            // one, on its own cadence. It does nothing at all unless AMDNR_XR_NEAR_PROBE is set.
            //
            // The whole eye rectangle goes in, not the window: each eye's window is placed from its
            // own gaze estimate, so the two of them sit in different parts of the scene and comparing
            // those would compare the wrong pixels.
            const XrRect2Di& eyeRect = subImage.imageRect;
            const RECT eyeBox{eyeRect.offset.x,
                              eyeRect.offset.y,
                              eyeRect.offset.x + (LONG)eyeRect.extent.width,
                              eyeRect.offset.y + (LONG)eyeRect.extent.height};
            NearProbeView(g_appDevice.Get(),
                          g_appContext.Get(),
                          view,
                          texture,
                          eyeBox,
                          motion.tanWidth,
                          tanf(viewInfo.fov.angleLeft),
                          tanf(viewInfo.fov.angleRight),
                          viewInfo.pose.position.x,
                          viewInfo.pose.position.y,
                          viewInfo.pose.position.z);

            const auto start = std::chrono::steady_clock::now();
            if (!g_interop.Process(view,
                                   texture,
                                   subImage.imageArrayIndex,
                                   box,
                                   plan.networkWidth,
                                   plan.networkHeight,
                                   srgb,
                                   motion)) {
                // The neural pass is the layer's own addition to a frame the application has already
                // drawn, so a failure in it must not cost the frame anything. Leaving the loop is what
                // keeps the rest of this function running, and the rest of this function is where the
                // output target is drawn into -- the target the composition layer is then pointed at.
                // Returning from the function here instead left that target holding the last frame it
                // was given, and a target nothing writes is a picture that never changes again: the
                // application went on rendering at 35 fps behind it the whole time.
                //
                // Giving up for the session rather than retrying next frame is deliberate. The failure
                // this was found with is a removed device, and retrying that produced 6494 identical
                // log lines over 81 seconds without one frame of progress.
                LayerLog("layer: interop failed: %s\n", g_interop.LastError());
                LayerLog("layer: the neural renderer stays off for the rest of the session; frames go "
                         "on through without it\n");
                g_interopGaveUp = true;
                break;
            }
            track.valid = true;
            track.orientation = viewInfo.pose.orientation;
            track.position = viewInfo.pose.position;
            track.left = box.left;
            track.top = box.top;
            track.width = width;
            track.height = height;
            const double passMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            g_processMs += passMs;
            g_frameMs += passMs;
            g_processCount++;
            g_eyePasses[view < 2 ? view : 0]++;
        }

        // Published for the automatic tier's next look, and for the panel. Set here rather than at the
        // end of the frame because the rest of this function -- the full-resolution output path -- is
        // not part of what the pass costs, and a tier that tried to shrink it could not.
        g_lastFrameMs = g_frameMs;
        g_windowPassMs += g_frameMs;
        g_windowPassFrames += 1;
        {
            NrRuntimeReport rt = NrRuntimeReportGet();
            rt.frameMs = g_lastFrameMs;
            NrRuntimeReportSet(rt);
        }
    }

    // ---- Full-resolution output path ---------------------------------------

    // The runtime tells the application how large it wants its eye buffers, and everything the
    // application shades below that size the compositor has to stretch. A layer cannot make the
    // application shade more pixels -- but once the pixels exist it can do the stretching itself,
    // with AMD's FidelityFX Super Resolution and hand the compositor the size it asked for.
    //
    // EASU and RCAS both expect display-encoded colour in [0,1] and are documented to work on it
    // directly, so this path does no colour conversion whenever it can avoid it: the eye image is
    // read through a plain view and the result is written through the plain view of the target, which
    // leaves the encoded values the application produced untouched end to end.
    //
    // That is the path actually taken, because the swapchain textures a runtime hands out are
    // typeless: a typeless resource accepts both the plain and the sRGB view, and the plain one is
    // tried first. Only a *typed* sRGB resource rejects the plain view, and there the read or the
    // write has to go through its sRGB view with the hardware converting; the shader then undoes it,
    // re-encoding each gathered texel on the way in or decoding the result on the way out. That
    // fallback is deliberately second choice: the encode sits in EASU's gather callback, which runs
    // once per texel per channel -- twelve times a pixel -- so on a full-resolution frame it is on
    // the order of a billion pow operations a frame.
    //
    // All of it is behind AMDNR_XR_UPSCALE and fails soft: a view whose output could not be prepared
    // is forwarded exactly as the application submitted it.

    // d3dcompiler_47.dll is loaded by name rather than linked, the same way NrCore does it: the
    // layer has to work on machines where the runtime ships its own copy, and a hard import would
    // make the whole layer fail to load if it were missing.
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

    // Reading a colour buffer without conversion means binding its plain view; letting the hardware
    // convert means binding the sRGB view, which decodes on read and encodes on write. Which of the
    // two applies is not a property of the format -- it is what the device accepts for the resource
    // in hand, so the two helpers below ask by trying.
    DXGI_FORMAT SrgbView(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return format;
        }
    }

    DXGI_FORMAT LinearView(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return format;
        }
    }

    // Creates one render-target view on `texture`, trying the pass-through view before the sRGB one
    // and reporting which took. The device's own answer decides, not an inspection of the format: the
    // same format may be a typeless resource, which accepts both views and gets the pass-through one
    // (no conversion), or a typed sRGB resource, which rejects the pass-through view with
    // E_INVALIDARG and leaves only the sRGB view (encoding on write).
    bool CreatePassthroughOrSrgbRtv(ID3D11Device* device,
                                    ID3D11Texture2D* texture,
                                    DXGI_FORMAT format,
                                    ComPtr<ID3D11RenderTargetView>& view,
                                    bool& converted) {
        const DXGI_FORMAT candidates[2] = {LinearView(format), SrgbView(format)};
        for (int attempt = 0; attempt < 2; attempt++) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = candidates[attempt];
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(device->CreateRenderTargetView(texture, &desc, view.ReleaseAndGetAddressOf()))) {
                converted = attempt == 1;
                return true;
            }
        }
        return false;
    }

    // The same choice for a source view, which is a texture array because an eye image may have more
    // than one slice.
    bool CreatePassthroughOrSrgbSrv(ID3D11Device* device,
                                    ID3D11Texture2D* texture,
                                    DXGI_FORMAT format,
                                    uint32_t arraySize,
                                    ComPtr<ID3D11ShaderResourceView>& view,
                                    bool& converted) {
        const DXGI_FORMAT candidates[2] = {LinearView(format), SrgbView(format)};
        for (int attempt = 0; attempt < 2; attempt++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = candidates[attempt];
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MostDetailedMip = 0;
            desc.Texture2DArray.MipLevels = 1;
            desc.Texture2DArray.FirstArraySlice = 0;
            desc.Texture2DArray.ArraySize = arraySize;
            if (SUCCEEDED(device->CreateShaderResourceView(
                    texture, &desc, view.ReleaseAndGetAddressOf()))) {
                converted = attempt == 1;
                return true;
            }
        }
        return false;
    }

    bool EnsureUpscalePipeline() {
        if (g_easuPs != nullptr && g_rcasPs != nullptr) {
            return true;
        }
        if (g_upscaleFailed || g_appDevice == nullptr) {
            return false;
        }

        const auto compile = GetD3DCompile();
        if (compile == nullptr) {
            g_upscaleFailed = true;
            LayerLog("layer: d3dcompiler_47.dll is unavailable, the output path stays off\n");
            return false;
        }

        // The FSR headers are read from beside the DLL, which lets the shader text include them the
        // way AMD publishes it instead of being flattened by hand. A header that cannot be read
        // simply fails the compile, which is reported like any other setup failure.
        amdnr_fsr::IncludeHandler include(LayerDirectory());

        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> easuBlob;
        ComPtr<ID3DBlob> rcasBlob;
        ComPtr<ID3DBlob> error;

        const auto build = [&](const char* source, const char* entry, const char* target, ID3DBlob** blob) {
            error.Reset();
            const HRESULT result = compile(source,
                                           std::strlen(source),
                                           "AmdnrFsr",
                                           nullptr,
                                           &include,
                                           entry,
                                           target,
                                           0,
                                           0,
                                           blob,
                                           error.ReleaseAndGetAddressOf());
            if (FAILED(result)) {
                LayerLog("layer: output %s shader failed: %s\n",
                         entry,
                         error ? static_cast<const char*>(error->GetBufferPointer()) : "no message");
                return false;
            }
            return true;
        };

        if (!build(amdnr_fsr::kVertexShader, "main_vs", "vs_5_0", vertexBlob.ReleaseAndGetAddressOf()) ||
            !build(amdnr_fsr::kEasuShader, "main_ps", "ps_5_0", easuBlob.ReleaseAndGetAddressOf()) ||
            !build(amdnr_fsr::kRcasShader, "main_ps", "ps_5_0", rcasBlob.ReleaseAndGetAddressOf())) {
            g_upscaleFailed = true;
            return false;
        }

        if (FAILED(g_appDevice->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                                   vertexBlob->GetBufferSize(),
                                                   nullptr,
                                                   g_upscaleVs.ReleaseAndGetAddressOf())) ||
            FAILED(g_appDevice->CreatePixelShader(easuBlob->GetBufferPointer(),
                                                  easuBlob->GetBufferSize(),
                                                  nullptr,
                                                  g_easuPs.ReleaseAndGetAddressOf())) ||
            FAILED(g_appDevice->CreatePixelShader(rcasBlob->GetBufferPointer(),
                                                  rcasBlob->GetBufferSize(),
                                                  nullptr,
                                                  g_rcasPs.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: output shaders could not be created\n");
            return false;
        }

        // EASU gathers, so its filter setting does not matter, but the address mode does: the kernel
        // reads outside the source rectangle and clamping there is what keeps the rim from becoming
        // a smear of whatever the allocation happens to hold. RCAS samples the intermediate through
        // the same sampler, where clamp is what stops its edge taps reading out of bounds.
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_appDevice->CreateSamplerState(&sampler, g_upscaleSampler.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: output sampler could not be created\n");
            return false;
        }

        // The pass states the draw must not inherit from the application. Opaque, no depth, no
        // stencil, no culling, no scissor: the pass covers the whole destination by construction and
        // has to be able to write every pixel of it whoever the application left the context in the
        // hands of.
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = FALSE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(g_appDevice->CreateBlendState(&blend, g_upscaleBlend.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: the output blend state could not be created\n");
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.StencilEnable = FALSE;
        if (FAILED(g_appDevice->CreateDepthStencilState(&depth, g_upscaleDepth.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: the output depth-stencil state could not be created\n");
            return false;
        }

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = FALSE;
        raster.DepthClipEnable = TRUE;
        if (FAILED(g_appDevice->CreateRasterizerState(&raster, g_upscaleRaster.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: the output rasterizer state could not be created\n");
            return false;
        }

        D3D11_BUFFER_DESC constants{};
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        constants.ByteWidth = sizeof(amdnr_fsr::EasuConstants);
        if (FAILED(g_appDevice->CreateBuffer(&constants, nullptr, g_easuConstants.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: the EASU constant buffer could not be created\n");
            return false;
        }

        constants.ByteWidth = sizeof(amdnr_fsr::RcasConstants);
        if (FAILED(g_appDevice->CreateBuffer(&constants, nullptr, g_rcasConstants.ReleaseAndGetAddressOf()))) {
            g_upscaleFailed = true;
            LayerLog("layer: the RCAS constant buffer could not be created\n");
            return false;
        }

        // Sharpness is fixed for the whole run, so the RCAS constants are built the one time.
        FsrRcasCon(g_rcasCon, g_upscaleSharpen);

        LayerLog("layer: output path pipeline ready (EASU + RCAS, sharpen %.2f)\n", g_upscaleSharpen);
        return true;
    }

    // Builds the layer's own target, exactly the size the runtime recommended, plus a sampled view
    // of every image in the application's chain for the pass to read.
    //
    // It is deliberately *not* sized from the application's texture: that texture is whatever the
    // application allocated, and it can be far larger than the region a view actually renders
    // (an engine that keeps a fixed-size scratch buffer, or one that only fills the top-left of it).
    // Scaling the whole allocation by the view ratio produced an 11302x4538 target here for two
    // 1992x1600 views -- four times the pixels of the correct target, all of it carried by the
    // compositor every frame.
    bool EnsureOutputSwapchain(TrackedSwapchain& entry, const XrRect2Di& viewRect) {
        if (entry.outHandle != XR_NULL_HANDLE) {
            return true;
        }
        if (entry.outFailed || g_session == XR_NULL_HANDLE || g_next.CreateSwapchain == nullptr ||
            g_next.EnumerateSwapchainImages == nullptr || g_viewWidth == 0 || g_viewHeight == 0) {
            entry.outFailed = true;
            return false;
        }

        // Nothing to do while the view is already at or above the size the runtime asked for; that
        // is the case whenever the application is rendering at full scale.
        if ((uint32_t)viewRect.extent.width >= g_viewWidth &&
            (uint32_t)viewRect.extent.height >= g_viewHeight) {
            entry.outFailed = true;
            LayerLog("layer: the eye buffer already meets the view size (%dx%d vs %ux%u), no output path\n",
                     viewRect.extent.width,
                     viewRect.extent.height,
                     g_viewWidth,
                     g_viewHeight);
            return false;
        }

        const uint32_t outWidth = g_viewWidth;
        const uint32_t outHeight = g_viewHeight;

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.arraySize = 1;
        info.mipCount = 1;
        info.faceCount = 1;
        info.sampleCount = 1;
        info.width = outWidth;
        info.height = outHeight;
        info.format = (int64_t)entry.format;
        // The same set the compositor asked the application for: it has to sample this chain in
        // place of the application's, and a runtime that also wants to copy out of it must find the
        // same bindings it would have found there.
        info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

        XrSwapchain handle = XR_NULL_HANDLE;
        if (XR_FAILED(g_next.CreateSwapchain(g_session, &info, &handle))) {
            entry.outFailed = true;
            LayerLog("layer: the output swapchain could not be created at %ux%u\n", outWidth, outHeight);
            return false;
        }

        uint32_t count = 0;
        std::vector<XrSwapchainImageD3D11KHR> images;
        if (XR_SUCCEEDED(g_next.EnumerateSwapchainImages(handle, 0, &count, nullptr)) && count > 0) {
            images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(g_next.EnumerateSwapchainImages(
                    handle, count, &count, (XrSwapchainImageBaseHeader*)images.data()))) {
                images.clear();
            }
        }

        // The output views, pass-through first: on the typeless swapchain images the runtime hands
        // out the plain view exists, and the encoded value the shader produces is written straight
        // through. Only a typed sRGB image refuses it, and then the sRGB view -- which encodes in
        // hardware, so RCAS has to decode its result first -- is the fallback.
        bool targetConverted = false;
        bool targetOk = !images.empty();
        for (uint32_t i = 0; i < images.size(); i++) {
            ComPtr<ID3D11RenderTargetView> view;
            bool converted = false;
            if (!CreatePassthroughOrSrgbRtv(
                    g_appDevice.Get(), images[i].texture, entry.format, view, converted)) {
                targetOk = false;
                break;
            }
            targetConverted = converted;
            entry.outRtvs.push_back(std::move(view));
            entry.outTextures.push_back(images[i].texture);
        }
        if (!targetOk) {
            entry.outRtvs.clear();
            entry.outTextures.clear();
        }

        // The same choice on the source, read through a texture array view. The pass-through view
        // hands EASU the display-encoded bytes directly; only the sRGB view decodes on read, which
        // the EASU gather callbacks then undo so the kernel still sees encoded values.
        bool sourceConverted = false;
        for (uint32_t i = 0; i < entry.textures.size(); i++) {
            ComPtr<ID3D11ShaderResourceView> view;
            bool converted = false;
            if (!CreatePassthroughOrSrgbSrv(g_appDevice.Get(),
                                            entry.textures[i].Get(),
                                            entry.format,
                                            entry.arraySize,
                                            view,
                                            converted)) {
                entry.srcSrvs.clear();
                break;
            }
            sourceConverted = converted;
            entry.srcSrvs.push_back(std::move(view));
        }

        if (entry.outRtvs.empty() || entry.srcSrvs.size() != entry.textures.size()) {
            g_next.DestroySwapchain(handle);
            entry.outRtvs.clear();
            entry.outTextures.clear();
            entry.srcSrvs.clear();
            entry.outFailed = true;
            LayerLog("layer: the output views could not be created, forwarding the eye buffer untouched\n");
            return false;
        }

        // EASU's intermediate is plain UNORM, not sRGB: the values going through it are already
        // display-encoded, so a second decode on readback is exactly what has to be avoided.
        D3D11_TEXTURE2D_DESC middle{};
        middle.Width = outWidth;
        middle.Height = outHeight;
        middle.MipLevels = 1;
        middle.ArraySize = 1;
        middle.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        middle.SampleDesc.Count = 1;
        middle.Usage = D3D11_USAGE_DEFAULT;
        middle.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_appDevice->CreateTexture2D(&middle, nullptr, entry.midTexture.ReleaseAndGetAddressOf())) ||
            FAILED(g_appDevice->CreateRenderTargetView(
                entry.midTexture.Get(), nullptr, entry.midRtv.ReleaseAndGetAddressOf())) ||
            FAILED(g_appDevice->CreateShaderResourceView(
                entry.midTexture.Get(), nullptr, entry.midSrv.ReleaseAndGetAddressOf()))) {
            g_next.DestroySwapchain(handle);
            entry.outRtvs.clear();
            entry.outTextures.clear();
            entry.srcSrvs.clear();
            entry.midTexture.Reset();
            entry.midRtv.Reset();
            entry.midSrv.Reset();
            entry.outFailed = true;
            LayerLog("layer: the EASU intermediate could not be created, forwarding the eye buffer untouched\n");
            return false;
        }

        entry.outHandle = handle;
        entry.outWidth = outWidth;
        entry.outHeight = outHeight;
        entry.srcEncode = sourceConverted;
        entry.dstDecode = targetConverted;
        LayerLog("layer: output path armed, %ux%u target per eye, sharpen %.2f, colour %s "
                 "(read %s, write %s)\n",
                 outWidth,
                 outHeight,
                 g_upscaleSharpen,
                 (sourceConverted || targetConverted) ? "converted" : "pass-through",
                 sourceConverted ? "sRGB view + encode" : "plain",
                 targetConverted ? "sRGB view + decode" : "plain");
        return true;
    }

    // The application's frame state, put back once the pass is done. The draw happens in the middle
    // of someone else's frame on someone else's context, and OpenXR gives a layer no context of its
    // own, so every stage the pass touches has to be restored rather than simply abandoned.
    struct ContextGuard {
        explicit ContextGuard(ID3D11DeviceContext* context) : m_context(context) {
            if (m_context == nullptr) {
                return;
            }

            m_context->OMGetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.GetAddressOf());
            m_context->OMGetBlendState(m_blend.GetAddressOf(), m_blendFactor, &m_sampleMask);
            m_context->OMGetDepthStencilState(m_dss.GetAddressOf(), &m_stencilRef);

            UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            m_context->RSGetViewports(&viewportCount, m_viewports);
            m_viewportCount = viewportCount;
            m_context->RSGetScissorRects(&viewportCount, m_scissors);
            m_scissorCount = viewportCount;
            m_context->RSGetState(m_raster.GetAddressOf());

            UINT instanceCount = 0;
            m_context->VSGetShader(m_vs.GetAddressOf(), nullptr, &instanceCount);
            m_context->PSGetShader(m_ps.GetAddressOf(), nullptr, &instanceCount);
            m_context->PSGetConstantBuffers(0, 1, m_psConstants.GetAddressOf());
            m_context->PSGetSamplers(0, 1, m_psSampler.GetAddressOf());
            m_context->PSGetShaderResources(0, kResourceCount, m_psResources);
            m_context->IAGetInputLayout(m_layout.GetAddressOf());
            m_context->IAGetPrimitiveTopology(&m_topology);
        }

        ~ContextGuard() {
            if (m_context == nullptr) {
                return;
            }

            m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.Get());
            m_context->OMSetBlendState(m_blend.Get(), m_blendFactor, m_sampleMask);
            m_context->OMSetDepthStencilState(m_dss.Get(), m_stencilRef);
            m_context->RSSetViewports(m_viewportCount, m_viewports);
            m_context->RSSetScissorRects(m_scissorCount, m_scissors);
            m_context->RSSetState(m_raster.Get());

            m_context->VSSetShader(m_vs.Get(), nullptr, 0);
            m_context->PSSetShader(m_ps.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_psConstants.GetAddressOf());
            m_context->PSSetSamplers(0, 1, m_psSampler.GetAddressOf());
            m_context->PSSetShaderResources(0, kResourceCount, m_psResources);
            m_context->IASetInputLayout(m_layout.Get());
            m_context->IASetPrimitiveTopology(m_topology);

            for (uint32_t i = 0; i < kResourceCount; i++) {
                if (m_psResources[i] != nullptr) {
                    m_psResources[i]->Release();
                }
            }
        }

        static constexpr uint32_t kResourceCount = 8;

        ID3D11DeviceContext* m_context{nullptr};
        ComPtr<ID3D11RenderTargetView> m_rtv;
        ComPtr<ID3D11DepthStencilView> m_dsv;
        ComPtr<ID3D11BlendState> m_blend;
        FLOAT m_blendFactor[4]{};
        UINT m_sampleMask{0};
        ComPtr<ID3D11DepthStencilState> m_dss;
        UINT m_stencilRef{0};
        D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT m_viewportCount{0};
        D3D11_RECT m_scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT m_scissorCount{0};
        ComPtr<ID3D11RasterizerState> m_raster;
        ComPtr<ID3D11VertexShader> m_vs;
        ComPtr<ID3D11PixelShader> m_ps;
        ComPtr<ID3D11Buffer> m_psConstants;
        ComPtr<ID3D11SamplerState> m_psSampler;
        ID3D11ShaderResourceView* m_psResources[kResourceCount]{};
        ComPtr<ID3D11InputLayout> m_layout;
        D3D11_PRIMITIVE_TOPOLOGY m_topology{D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    };

    // The views at which the output is read back. Spread over the first several seconds on purpose:
    // the question is not what the first frame holds but whether anything ever puts an image there,
    // and a title that has just launched spends its first seconds on a loading screen that is black
    // because it is a loading screen.
    constexpr uint32_t kOutputCheckFrames[] = {60, 240, 600, 1200, 2400, 4800, 9000, 18000};

    // What one texture holds, as a range and a mean of its luma. Every 16th row and every 16th
    // texel: enough of a twelve megapixel image to know whether anything was written into it at all.
    void DescribeTexture(const char* what, ID3D11Texture2D* texture, const D3D11_BOX* box) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (box != nullptr) {
            staging.Width = box->right - box->left;
            staging.Height = box->bottom - box->top;
        }

        ComPtr<ID3D11Texture2D> readback;
        if (FAILED(g_appDevice->CreateTexture2D(&staging, nullptr, readback.ReleaseAndGetAddressOf()))) {
            LayerLog("layer: %s: no readback texture\n", what);
            return;
        }
        if (box != nullptr) {
            g_appContext->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, texture, 0, box);
        } else {
            g_appContext->CopyResource(readback.Get(), texture);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_appContext->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            LayerLog("layer: %s: the readback would not map\n", what);
            return;
        }

        uint64_t sum = 0;
        uint64_t samples = 0;
        uint8_t lowest = 255;
        uint8_t highest = 0;
        const uint8_t* bytes = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < staging.Height; y += 16) {
            const uint8_t* row = bytes + (size_t)y * mapped.RowPitch;
            for (uint32_t x = 0; x < staging.Width; x += 16) {
                const uint8_t* pixel = row + (size_t)x * 4;
                const uint8_t luma = (uint8_t)((pixel[0] * 77 + pixel[1] * 151 + pixel[2] * 28) >> 8);
                sum += luma;
                samples++;
                lowest = luma < lowest ? luma : lowest;
                highest = luma > highest ? luma : highest;
            }
        }
        g_appContext->Unmap(readback.Get(), 0);

        LayerLog("layer: %s %ux%u, luma %u..%u mean %.1f over %llu samples\n",
                 what,
                 staging.Width,
                 staging.Height,
                 lowest,
                 highest,
                 samples != 0 ? (double)sum / (double)samples : 0.0,
                 (unsigned long long)samples);
    }

    // Reads both ends of the pass back at a handful of views across the run.
    //
    // A compositor that shows nothing is either being handed nothing or is not being handed it, and
    // those two have nothing in common as faults. This is the only place the layer can look at its
    // own output the way the compositor will, so it is the line that tells them apart. A handful of
    // times, because the read is a blocking map and this is a diagnostic rather than a pass.
    bool ReportOutput(uint32_t frames,
                      TrackedSwapchain& entry,
                      uint32_t index,
                      uint32_t sourceImage,
                      const XrRect2Di& sourceRect) {
        if (g_outputChecks >= std::size(kOutputCheckFrames) ||
            frames != kOutputCheckFrames[g_outputChecks] || index >= entry.outTextures.size() ||
            sourceImage >= entry.textures.size() || g_appDevice == nullptr || g_appContext == nullptr) {
            return false;
        }
        g_outputChecks++;

        // Both ends of the pass in the same breath. A black target is either a target nobody wrote
        // into or a target written into from a black source, and those are opposite faults: the first
        // is this layer's own copy-back, the second is upstream of it.
        const D3D11_BOX sourceBox{(UINT)sourceRect.offset.x,
                                  (UINT)sourceRect.offset.y,
                                  0,
                                  (UINT)(sourceRect.offset.x + sourceRect.extent.width),
                                  (UINT)(sourceRect.offset.y + sourceRect.extent.height),
                                  1};

        char label[64]{};
        std::snprintf(label,
                      sizeof(label),
                      "output check %u, view %u, source",
                      g_outputChecks,
                      frames);
        DescribeTexture(label, entry.textures[sourceImage].Get(), &sourceBox);

        std::snprintf(label, sizeof(label), "output check %u, view %u, target image %u", g_outputChecks, frames, index);
        DescribeTexture(label, entry.outTextures[index].Get(), nullptr);
        return true;
    }

    // Takes one image of the layer's own target for this frame. A failure leaves nothing acquired,
    // so the caller can simply forward the view untouched.
    bool AcquireOutputImage(TrackedSwapchain& entry, uint32_t& index) {
        if (entry.outHandle == XR_NULL_HANDLE || entry.outRtvs.empty() ||
            g_next.AcquireSwapchainImage == nullptr || g_next.WaitSwapchainImage == nullptr ||
            g_next.ReleaseSwapchainImage == nullptr) {
            return false;
        }
        if (XR_FAILED(g_next.AcquireSwapchainImage(entry.outHandle, nullptr, &index))) {
            return false;
        }

        // The compositor holds its own reference to whichever image it is presenting, so the one
        // this frame gets may not be ready yet. Waiting is what an application does here too.
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(g_next.WaitSwapchainImage(entry.outHandle, &wait)) || index >= entry.outRtvs.size()) {
            g_next.ReleaseSwapchainImage(entry.outHandle, nullptr);
            return false;
        }
        return true;
    }

    // Replaces one view's rectangle with a filtered, full-resolution copy: EASU resamples the eye
    // image into the intermediate, then RCAS sharpens that into the image the compositor will read.
    // Two passes because RCAS is defined as a step over EASU's finished output, not as part of it.
    //
    // With FSR 4 asked for, that whole pair is replaced by one dispatch on the neural device. It is a
    // preference and not a mode: a frame FSR 4 declines falls through to the two passes below, so the
    // worst a machine without the runtime can see is the output path it already had.
    bool DrawUpscale(TrackedSwapchain& entry,
                     uint32_t index,
                     uint32_t sourceImage,
                     const XrRect2Di& sourceRect,
                     uint32_t arraySlice,
                     uint32_t view,
                     XrRect2Di& destination) {
        if (sourceImage >= entry.srcSrvs.size() || index >= entry.outRtvs.size() ||
            entry.midRtv == nullptr || entry.midSrv == nullptr) {
            return false;
        }

        // The destination fills the whole target. Where the source rectangle sat inside the
        // application's texture is irrelevant -- the compositor is told which rectangle to sample,
        // so the filtered result can start at the origin.
        destination = XrRect2Di{};
        destination.offset.x = 0;
        destination.offset.y = 0;
        destination.extent.width = (int32_t)entry.outWidth;
        destination.extent.height = (int32_t)entry.outHeight;

        if (g_fsr4 && sourceImage < entry.textures.size() && index < entry.outTextures.size()) {
            const RECT box{sourceRect.offset.x,
                           sourceRect.offset.y,
                           sourceRect.offset.x + sourceRect.extent.width,
                           sourceRect.offset.y + sourceRect.extent.height};
            // The offset the application rendered this frame through, so the effect can line the frame
            // up with the history it kept. Zero unless the jitter is on. The motion was built for this
            // view by the neural loop earlier in this frame; a view it did not reach arrives invalid,
            // which the effect reads as nothing having moved.
            const NrMotion& motion = g_fsr4Motion[view < kMaxViews ? view : 0];
            if (g_interop.UpscaleFsr4(view,
                                      entry.textures[sourceImage].Get(),
                                      arraySlice,
                                      box,
                                      entry.outTextures[index].Get(),
                                      0,
                                      g_jitter ? g_jitterX : 0.f,
                                      g_jitter ? g_jitterY : 0.f,
                                      motion)) {
                return true;
            }
        }

        // EASU spans the source rectangle, not the whole eye texture: the viewport size drives the
        // resample and the image size only turns positions into texel coordinates, which is what
        // lets the rectangle sit anywhere inside the allocation.
        amdnr_fsr::EasuConstants easu{};
        FsrEasuConOffset(easu.con0,
                         easu.con1,
                         easu.con2,
                         easu.con3,
                         (AF1)sourceRect.extent.width,
                         (AF1)sourceRect.extent.height,
                         (AF1)entry.width,
                         (AF1)entry.height,
                         (AF1)entry.outWidth,
                         (AF1)entry.outHeight,
                         (AF1)sourceRect.offset.x,
                         (AF1)sourceRect.offset.y);
        easu.slice = (float)arraySlice;
        // The two conversions were settled when the views were created, from what the device
        // accepted, and they are independent: a chain can read straight through and still have to
        // decode on write, or the other way round. The default -- both off -- is the pass-through
        // path, where neither the gather nor the result touches the colour space.
        easu.encode = entry.srcEncode ? 1.f : 0.f;

        amdnr_fsr::RcasConstants rcas{};
        std::memcpy(rcas.con, g_rcasCon, sizeof(rcas.con));
        rcas.invSize[0] = 1.f / (float)entry.outWidth;
        rcas.invSize[1] = 1.f / (float)entry.outHeight;
        rcas.decode = entry.dstDecode ? 1.f : 0.f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_appContext->Map(g_easuConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &easu, sizeof(easu));
        g_appContext->Unmap(g_easuConstants.Get(), 0);

        mapped = D3D11_MAPPED_SUBRESOURCE{};
        if (FAILED(g_appContext->Map(g_rcasConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &rcas, sizeof(rcas));
        g_appContext->Unmap(g_rcasConstants.Get(), 0);

        ContextGuard guard(g_appContext.Get());

        const D3D11_VIEWPORT viewport{(FLOAT)destination.offset.x,
                                      (FLOAT)destination.offset.y,
                                      (FLOAT)destination.extent.width,
                                      (FLOAT)destination.extent.height,
                                      0.f,
                                      1.f};
        g_appContext->RSSetViewports(1, &viewport);
        // The pass owns everything it draws under. Inheriting the application's output merger and
        // rasterizer state is the same trap as inheriting its topology: a blend state that writes no
        // colour, a cull mode that drops this triangle, or a scissor rect left over from an earlier
        // draw would each turn the whole frame into a black image, and none of them would error.
        g_appContext->OMSetBlendState(g_upscaleBlend.Get(), nullptr, 0xFFFFFFFFu);
        g_appContext->OMSetDepthStencilState(g_upscaleDepth.Get(), 0);
        g_appContext->RSSetState(g_upscaleRaster.Get());
        // The pass owns its topology: inheriting whatever the application happened to leave bound
        // would silently emit nothing on a frame that does not end in a triangle draw.
        g_appContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_appContext->VSSetShader(g_upscaleVs.Get(), nullptr, 0);
        g_appContext->PSSetSamplers(0, 1, g_upscaleSampler.GetAddressOf());

        ID3D11ShaderResourceView* source = entry.srcSrvs[sourceImage].Get();

        // Pass 1: EASU into the intermediate, at the view size.
        ID3D11RenderTargetView* middle = entry.midRtv.Get();
        g_appContext->OMSetRenderTargets(1, &middle, nullptr);
        g_appContext->PSSetShader(g_easuPs.Get(), nullptr, 0);
        g_appContext->PSSetConstantBuffers(0, 1, g_easuConstants.GetAddressOf());
        g_appContext->PSSetShaderResources(0, 1, &source);
        g_appContext->Draw(3, 0);

        // The intermediate is about to be read, so the application's texture has to come off the slot
        // first; a resource cannot be an input and an output at once.
        ID3D11ShaderResourceView* none = nullptr;
        g_appContext->PSSetShaderResources(0, 1, &none);

        // Pass 2: RCAS from the intermediate into the image the compositor will sample.
        ID3D11RenderTargetView* target = entry.outRtvs[index].Get();
        g_appContext->OMSetRenderTargets(1, &target, nullptr);
        g_appContext->PSSetShader(g_rcasPs.Get(), nullptr, 0);
        g_appContext->PSSetConstantBuffers(0, 1, g_rcasConstants.GetAddressOf());
        ID3D11ShaderResourceView* intermediate = entry.midSrv.Get();
        g_appContext->PSSetShaderResources(0, 1, &intermediate);
        g_appContext->Draw(3, 0);

        // Unbind before the guard puts the application's bindings back: a resource that is still
        // bound as a shader input cannot be written by whatever comes next.
        g_appContext->PSSetShaderResources(0, 1, &none);

        if (!g_upscaleDrawLogged) {
            g_upscaleDrawLogged = true;
            LayerLog("layer: output path first draw, EASU + RCAS over %dx%d at (%d,%d) of a %ux%u "
                     "texture -> %dx%d at (%d,%d) of a %ux%u target (image %u)\n",
                     sourceRect.extent.width,
                     sourceRect.extent.height,
                     sourceRect.offset.x,
                     sourceRect.offset.y,
                     entry.width,
                     entry.height,
                     destination.extent.width,
                     destination.extent.height,
                     destination.offset.x,
                     destination.offset.y,
                     entry.outWidth,
                     entry.outHeight,
                     index);
        }
        return true;
    }

    // Rewrites one projection layer so its views read from the layer's own full-resolution copies.
    // Returns false when nothing was redirected, in which case the layer is forwarded untouched.
    bool UpscaleProjectionLayer(const XrCompositionLayerProjection* projection,
                                std::vector<XrCompositionLayerProjectionView>& viewStorage,
                                std::vector<XrCompositionLayerProjection>& layerStorage,
                                std::vector<XrCompositionLayerBaseHeader*>& headerStorage) {
        if (!g_upscale || projection->viewCount == 0 || projection->viewCount > kMaxViews ||
            !EnsureUpscalePipeline() || g_appContext == nullptr) {
            return false;
        }

        // Which views can be redirected, and where each one's pixels come from. Fixed storage: a
        // projection layer holds one view per eye, and this runs inside someone else's frame.
        struct Pending {
            uint32_t view{0};
            TrackedSwapchain* entry{nullptr};
            uint32_t sourceImage{0};
            XrRect2Di sourceRect{};
            uint32_t slice{0};
        };
        Pending pending[kMaxViews];
        uint32_t pendingCount = 0;
        for (uint32_t view = 0; view < projection->viewCount; view++) {
            const XrCompositionLayerProjectionView& source = projection->views[view];
            TrackedSwapchain* entry = FindSwapchain(source.subImage.swapchain);
            if (entry == nullptr || entry->lastAcquired >= entry->textures.size() ||
                source.subImage.imageArrayIndex >= entry->arraySize ||
                !EnsureOutputSwapchain(*entry, source.subImage.imageRect)) {
                continue;
            }
            // One view per target. Two views on one chain would need two images for their two
            // rectangles, and the compositor takes whichever image happens to be current, so a
            // layout that puts both eyes in one texture is left alone rather than half rewritten.
            bool shared = false;
            for (uint32_t seen = 0; seen < pendingCount; seen++) {
                if (pending[seen].entry == entry) {
                    shared = true;
                    break;
                }
            }
            if (shared) {
                continue;
            }
            pending[pendingCount].view = view;
            pending[pendingCount].entry = entry;
            pending[pendingCount].sourceImage = entry->lastAcquired;
            pending[pendingCount].sourceRect = source.subImage.imageRect;
            pending[pendingCount].slice = source.subImage.imageArrayIndex;
            pendingCount++;
        }
        if (pendingCount == 0) {
            return false;
        }

        // One image per output chain, with every view that lands in that chain drawn into it before
        // it is released. Two acquires against one chain would hand the compositor an image the
        // other view did not write, so the group has to be resolved first.
        XrSwapchainSubImage redirected[kMaxViews]{};
        bool redirectedOk[kMaxViews]{};
        bool settled[kMaxViews]{};
        for (uint32_t i = 0; i < pendingCount; i++) {
            if (settled[i]) {
                continue;
            }
            const XrSwapchain outHandle = pending[i].entry->outHandle;
            uint32_t index = 0;
            const bool acquired = AcquireOutputImage(*pending[i].entry, index);
            for (uint32_t j = i; j < pendingCount; j++) {
                if (settled[j] || pending[j].entry->outHandle != outHandle) {
                    continue;
                }
                settled[j] = true;
                if (!acquired) {
                    continue;
                }
                XrRect2Di destination{};
                if (DrawUpscale(*pending[j].entry,
                                index,
                                pending[j].sourceImage,
                                pending[j].sourceRect,
                                pending[j].slice,
                                pending[j].view,
                                destination)) {
                    redirected[pending[j].view] = XrSwapchainSubImage{outHandle, destination, 0};
                    redirectedOk[pending[j].view] = true;
                    ReportOutput(++g_upscaleFrames,
                                 *pending[j].entry,
                                 index,
                                 pending[j].sourceImage,
                                 pending[j].sourceRect);
                }
            }
            if (acquired) {
                // Flushed before the image is handed over. In D3D11 the immediate context's work is
                // submitted as it is recorded, but that "as it is recorded" is the driver's promise
                // and not the API's: what the API guarantees is what Flush has submitted. The
                // compositor is another process that reads this image the moment the release makes
                // it current, so what it has to be handed is a submission and not a recording.
                g_appContext->Flush();
                g_next.ReleaseSwapchainImage(outHandle, nullptr);
            }
        }

        bool any = false;
        const size_t base = viewStorage.size();
        for (uint32_t view = 0; view < projection->viewCount; view++) {
            XrCompositionLayerProjectionView copy = projection->views[view];
            if (redirectedOk[view]) {
                copy.subImage = redirected[view];
                // The effect has taken the sub-pixel offset back out of the pixels, so what the
                // compositor has to be told is the frustum the picture now describes -- the one the
                // application would have had without the offset. Submitting the offset frustum
                // instead would put a different fraction of a pixel under the picture every frame.
                if (g_jitter && g_unJitteredFovValid && view < kMaxViews) {
                    if (!g_jitterDeltaLogged) {
                        g_jitterDeltaLogged = true;
                        const XrFovf handed = projection->views[view].fov;
                        const XrFovf plain = g_unJitteredFov[view];
                        LayerLog("layer: the application rendered through the offset (dLeft %.6f, dRight %.6f, "
                                 "dUp %.6f, dDown %.6f rad on view %u)\n",
                                 handed.angleLeft - plain.angleLeft,
                                 handed.angleRight - plain.angleRight,
                                 handed.angleUp - plain.angleUp,
                                 handed.angleDown - plain.angleDown,
                                 view);
                    }
                    copy.fov = g_unJitteredFov[view];
                }
                any = true;
            }
            viewStorage.push_back(copy);
        }
        if (!any) {
            viewStorage.resize(base);
            return false;
        }

        // Copied wholesale so the rest of the chain -- the depth layer in particular -- is carried
        // through untouched; only the views array is the layer's own.
        XrCompositionLayerProjection layer = *projection;
        layer.views = &viewStorage[base];
        layerStorage.push_back(layer);
        headerStorage.push_back((XrCompositionLayerBaseHeader*)&layerStorage.back());
        return true;
    }

    // Where the application's own frame time goes, which the EndFrame gap alone cannot say. OpenXR
    // gives the application exactly two places to wait: `xrWaitFrame` blocks until the compositor
    // wants another frame, and the span from `xrBeginFrame` to `xrEndFrame` is the application
    // filling it. A frame interval that is mostly wait has been set by the runtime and no amount of
    // work removed from this layer will move it; one that is mostly render belongs to the
    // application. The predicted display period the runtime hands back is the runtime's own
    // statement of the interval it intends, and is logged whenever it changes.
    double g_waitMs = 0.0;
    double g_renderMs = 0.0;
    bool g_predictedLogged = false;
    // The last predicted period that was not an interval a display runs at, so a runtime that keeps
    // returning the same junk is reported once instead of on every frame.
    int64_t g_predictedRejected = 0;
    std::chrono::steady_clock::time_point g_beginAt{};
    bool g_beginAtValid = false;

    // What counts as a plausible display interval: 1 ms to 100 ms, i.e. 1000 Hz down to 10 Hz. Wide on
    // purpose -- the job is to catch a garbage value, not to second-guess an unusual refresh rate --
    // and it does catch the ones actually seen, a zero, an all-ones pattern, and a value smaller than
    // the period it was derived from.
    //
    // The check exists because the automatic quality tier now sizes its budget from this number. A
    // prediction taken at face value would solve the tier against an interval the headset never runs
    // at, and the failure would look like the tier misbehaving rather than like the runtime handing
    // back nonsense. A runtime's bad prediction is a real observed failure mode, not a hypothetical.
    constexpr int64_t kMinPredictedPeriodNs = 1000000;    // 1 ms
    constexpr int64_t kMaxPredictedPeriodNs = 100000000;  // 100 ms

    XrResult XRAPI_CALL Hook_xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo,
                                         XrFrameState* frameState) {
        const auto enter = std::chrono::steady_clock::now();
        if (g_next.WaitFrame == nullptr) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        const XrResult result = g_next.WaitFrame(session, frameWaitInfo, frameState);
        const auto exit = std::chrono::steady_clock::now();
        if (XR_SUCCEEDED(result) && frameState != nullptr) {
            const int64_t period = frameState->predictedDisplayPeriod;
            if (period >= kMinPredictedPeriodNs && period <= kMaxPredictedPeriodNs) {
                if (period != g_predictedPeriod) {
                    g_predictedPeriod = period;
                    g_predictedLogged = false;
                }
            } else if (period != g_predictedRejected) {
                // Dropped rather than adopted: g_predictedPeriod keeps the last plausible value, and
                // the tier with it, rather than being solved against this one.
                g_predictedRejected = period;
                LayerLog("layer: the runtime predicted a display period of %lld ns, which is not an "
                         "interval a display runs at; the tier keeps the last one it was given\n",
                         (long long)period);
            }
            if (!g_predictedLogged) {
                g_predictedLogged = true;
                LayerLog("layer: frame timing predicted period %.2f ms (%.1f Hz) predicted time %lld ns\n",
                         (double)g_predictedPeriod / 1.0e6,
                         g_predictedPeriod > 0 ? 1.0e9 / (double)g_predictedPeriod : 0.0,
                         (long long)frameState->predictedDisplayTime);
            }
            g_waitMs += std::chrono::duration<double, std::milli>(exit - enter).count();
        }
        return result;
    }

    XrResult XRAPI_CALL Hook_xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) {
        if (g_next.BeginFrame == nullptr) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        const XrResult result = g_next.BeginFrame(session, frameBeginInfo);
        if (XR_SUCCEEDED(result)) {
            g_beginAt = std::chrono::steady_clock::now();
            g_beginAtValid = true;
            // The frame's acquire marks start here: everything the application takes between this and
            // the matching EndFrame is its work for this frame, and what it did not take is what it did
            // not draw.
            for (TrackedSwapchain& entry : g_swapchains) {
                entry.acquiredThisFrame = false;
            }
        }
        return result;
    }

    XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
        // Where a frame's time actually goes, which nothing outside this hook can see. Three terms:
        // the gap the application itself leaves between one xrEndFrame and the next, the work this
        // layer does inside the call, and the time the runtime's own EndFrame takes to present. A
        // frame rate that does not move when the layer's work is halved is not being set by the
        // layer, and this is what says so with numbers instead of inference.
        const auto frameEnter = std::chrono::steady_clock::now();
        static std::chrono::steady_clock::time_point frameLastExit{};
        static bool frameHaveLast = false;
        static double frameGapMs = 0.0, frameLayerMs = 0.0, framePresentMs = 0.0;
        static uint64_t frameBreakdowns = 0;

        if (frameEndInfo->layerCount > 0 && g_frameIndex >= 300 && (g_frameIndex % 300) == 0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastReport).count();
            LayerLog("layer: frame %llu passes=%llu avg %.3f ms/pass (%.1f fps overall) gaze=%llu image=%u "
                     "moves=%llu crop=(%d,%d)(%d,%d) box=%ux%u endframe_refused=%llu last=%d\n",
                     (unsigned long long)g_frameIndex,
                     (unsigned long long)g_processCount,
                     g_processCount ? g_processMs / (double)g_processCount : 0.0,
                     elapsed > 0.0 ? 300.0 / elapsed : 0.0,
                     (unsigned long long)g_gazeHits,
                     g_lastImage,
                     (unsigned long long)g_anchorMoves,
                     g_anchor[0].left,
                     g_anchor[0].top,
                     g_anchor[1].left,
                     g_anchor[1].top,
                     g_windowW,
                     g_windowH,
                     (unsigned long long)g_endFrameFailures,
                     (int)g_endFrameLastResult);
            // The three terms of the frame interval, averaged over the window just closed. `gap` is
            // the application's own time (render + wait) and is the one term this layer cannot move;
            // `present` is the runtime's. Whichever of the three dominates is what the frame rate is
            // actually made of, and the layer's own share is the only part tuning here can change.
            {
                const double per = frameBreakdowns ? 1.0 / (double)frameBreakdowns : 0.0;
                LayerLog("layer: frame breakdown gap %.2f + layer %.2f + present %.2f = %.2f ms (n=%llu)\n",
                         frameGapMs * per,
                         frameLayerMs * per,
                         framePresentMs * per,
                         (frameGapMs + frameLayerMs + framePresentMs) * per,
                         (unsigned long long)frameBreakdowns);
                // The same interval split by which side of the frame budget it belongs to. `gap`
                // above is wait + render; this says which of the two it is, and the runtime's own
                // predicted period says what it intends that number to be.
                LayerLog("layer: frame split wait %.2f + render %.2f = %.2f ms (n=%llu) predicted period %.2f ms\n",
                         g_waitMs * per,
                         g_renderMs * per,
                         (g_waitMs + g_renderMs) * per,
                         (unsigned long long)frameBreakdowns,
                         (double)g_predictedPeriod / 1.0e6);
                frameGapMs = frameLayerMs = framePresentMs = 0.0;
                g_waitMs = g_renderMs = 0.0;
                frameBreakdowns = 0;
            }
            // The layer's own pass cost, taken from the pass timers rather than from the frame deltas
            // above. The two are different numbers, and this is the one that is checkable: it is the sum
            // of the per-eye stage lines printed just below, spread over the frames they cover. Read the
            // breakdown's `layer` term as a lower bound only -- the split between `gap` and `layer` there
            // does not survive the comparison, even though their sum tracks the frame interval.
            if (g_windowPassFrames > 0) {
                LayerLog("layer: passes cost %.2f ms/frame over %llu frames (%.1f ms in total)\n",
                         g_windowPassMs / (double)g_windowPassFrames,
                         (unsigned long long)g_windowPassFrames,
                         g_windowPassMs);
            }
            g_windowPassMs = 0.0;
            g_windowPassFrames = 0;
            // Where the per-pass average went, per eye. Anything large here that is not `network` is
            // this bridge stalling, and a stage that is just as large on the second eye is a fixed
            // wait being paid twice.
            for (uint32_t eye = 0; eye < 2; eye++) {
                if (g_eyePasses[eye] == 0) {
                    continue;
                }
                double cropMs = 0.0;
                double producerMs = 0.0;
                double networkMs = 0.0;
                double consumerMs = 0.0;
                double pasteMs = 0.0;
                g_interop.TakeStageTotals(eye, cropMs, producerMs, networkMs, consumerMs, pasteMs);
                const double per = 1.0 / (double)g_eyePasses[eye];
                LayerLog("layer: eye %u stages crop %.2f producer %.2f network %.2f consumer %.2f paste %.2f ms\n",
                         eye,
                         cropMs * per,
                         producerMs * per,
                         networkMs * per,
                         consumerMs * per,
                         pasteMs * per);

                // And what the producer's own wait is made of. `wait` is the thread sitting on the
                // queue; `pending` counts the passes where it found the application's D3D11 crop still
                // in flight, which is the one case the automatic tier cannot shrink. A wait that is not
                // pending is this layer's own queue, and does fall with the window.
                double recordMs = 0.0;
                double submitMs = 0.0;
                double waitStageMs = 0.0;
                uint32_t pendingPasses = 0;
                g_interop.TakeProducerSplit(eye, recordMs, submitMs, waitStageMs, pendingPasses);
                LayerLog("layer: eye %u producer split record %.2f submit %.2f wait %.2f ms, d11 pending "
                         "%u/%llu passes\n",
                         eye,
                         recordMs * per,
                         submitMs * per,
                         waitStageMs * per,
                         pendingPasses,
                         (unsigned long long)g_eyePasses[eye]);

                // And the consumer's, which is the other half of the same question. The two waits in a
                // pass are the only places this bridge blocks: the producer's is on the queue carrying
                // the colour crop, the consumer's is on the queue carrying the answer back. If the
                // second is as large as the first, both eyes are paying a fixed drain twice and the
                // frame is serial -- which is the reading the overlap question turns on. A consumer
                // wait far smaller than its network stage means the network, not the queue, is the
                // frame.
                double consumerRecordMs = 0.0;
                double consumerSubmitMs = 0.0;
                double consumerWaitMs = 0.0;
                g_interop.TakeConsumerSplit(eye, consumerRecordMs, consumerSubmitMs, consumerWaitMs);
                LayerLog("layer: eye %u consumer split record %.2f submit %.2f wait %.2f ms\n",
                         eye,
                         consumerRecordMs * per,
                         consumerSubmitMs * per,
                         consumerWaitMs * per);
            }

            // The alternate-eye probe, per view. A ratio near 100% on both eyes is a renderer drawing
            // both eyes every frame; near 50% on both is one drawing a single eye per frame -- and in
            // that case the eye whose chain was not taken is the one holding last frame's pixels, which
            // is the eye this layer would be reprojecting with the wrong pose.
            for (uint32_t eye = 0; eye < 2; eye++) {
                if (g_probePasses[eye] == 0) {
                    continue;
                }
                const double passes = (double)g_probePasses[eye];
                LayerLog("layer: eye %u probe %llu passes, chain acquired %llu (%.0f%%), pose moved %llu (%.0f%%)\n",
                         eye,
                         (unsigned long long)g_probePasses[eye],
                         (unsigned long long)g_probeAcquired[eye],
                         100.0 * (double)g_probeAcquired[eye] / passes,
                         (unsigned long long)g_probeMoved[eye],
                         100.0 * (double)g_probeMoved[eye] / passes);
                if (g_skipStaleEye) {
                    LayerLog("layer: eye %u left alone on %llu of those passes\n",
                             eye,
                             (unsigned long long)g_staleSkips[eye]);
                }
            }

            if (g_processCount > 0) {
                double enqueueMs = 0.0;
                double waitMs = 0.0;
                g_interop.TakeLaunchTotals(enqueueMs, waitMs);
                const double per = 1.0 / (double)g_processCount;
                LayerLog("layer: launch avg enqueue %.2f wait %.2f ms\n", enqueueMs * per, waitMs * per);
            }

            g_processCount = 0;
            g_eyePasses[0] = 0;
            g_eyePasses[1] = 0;
            g_probePasses[0] = g_probePasses[1] = 0;
            g_probeAcquired[0] = g_probeAcquired[1] = 0;
            g_probeMoved[0] = g_probeMoved[1] = 0;
            g_staleSkips[0] = g_staleSkips[1] = 0;
            g_processMs = 0.0;
            g_gazeHits = 0;
            g_anchorMoves = 0;
            g_lastReport = std::chrono::steady_clock::now();
        }
        g_frameIndex++;

        // End of frame is the point where this frame's scene depth is still alive and the render
        // thread is not inside a draw call, so it is safe to copy there. Nothing is published on
        // frames where no depth target has been seen yet.
        DepthCapturePublish();

        // The upscale path has to hand the runtime a layer of its own, because the projection views
        // are const in the application's frame. The copies are built here rather than inside the
        // loop so that nothing is allocated on the frames that do not use the path, and the storage
        // is reserved up front so the pointers threaded through it stay valid.
        std::vector<XrCompositionLayerProjectionView> redirectViews;
        std::vector<XrCompositionLayerProjection> redirectLayers;
        std::vector<XrCompositionLayerBaseHeader*> layers;
        layers.reserve(frameEndInfo->layerCount);
        redirectViews.reserve((size_t)frameEndInfo->layerCount * 2);
        redirectLayers.reserve(frameEndInfo->layerCount);
        // The depth infos handed to the runtime sit in a vector, so its storage is reserved up front:
        // a reallocation while the loop is running would move the pointers already written into the
        // views. Nothing is reserved while the probe is off.
        std::vector<XrCompositionLayerDepthInfoKHR> redirectDepths;
        if (g_depthProbeMode != DepthProbeMode::Off) {
            redirectDepths.reserve((size_t)frameEndInfo->layerCount * kMaxViews);
        }
        bool redirected = false;

        for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
            const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
            if (layer == nullptr) {
                layers.push_back(nullptr);
                continue;
            }
            if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* projection = (const XrCompositionLayerProjection*)layer;
                ProbeDepthLayer(projection);
                ProcessProjectionLayer(projection, frameEndInfo->displayTime);
                // Where this layer's views would land if the upscale path copies them; the probe hangs
                // its depth on that same copy rather than on anything the application owns.
                const size_t viewsBase = redirectViews.size();
                if (UpscaleProjectionLayer(projection, redirectViews, redirectLayers, layers)) {
                    redirected = true;
                    // No-op unless the probe is on, so the off path stays untouched.
                    AttachDepthProbe(redirectViews, viewsBase, projection->viewCount, redirectDepths);
                    continue;
                }
                if (RedirectForDepthProbe(projection, redirectViews, redirectLayers, redirectDepths, layers)) {
                    redirected = true;
                    continue;
                }
            }
            layers.push_back(const_cast<XrCompositionLayerBaseHeader*>(layer));
        }

        const auto presentBegin = std::chrono::steady_clock::now();
        XrResult result = XR_SUCCESS;
        if (!redirected) {
            result = g_next.EndFrame(session, frameEndInfo);
        } else {
            XrFrameEndInfo forwarded = *frameEndInfo;
            forwarded.layers = layers.data();
            result = g_next.EndFrame(session, &forwarded);
        }
        const auto presentEnd = std::chrono::steady_clock::now();

        g_endFrameLastResult = result;
        if (XR_FAILED(result)) {
            g_endFrameFailures++;
            if (!g_endFrameFailLogged) {
                g_endFrameFailLogged = true;
                LayerLog("layer: the runtime refused the frame, xrEndFrame returned %d (%s); the "
                         "application keeps rendering, so this is the only sign of it\n",
                         (int)result,
                         redirected ? "layer-built layers" : "application layers forwarded as-is");
            }
        }
        // One step of the sequence per frame, after the frame that used the current one. Every
        // xrLocateViews call inside a frame therefore sees the same offset.
        AdvanceJitter();
        // Accumulate the breakdown for the window the report above will print next time round.
        if (frameHaveLast) {
            frameGapMs += std::chrono::duration<double, std::milli>(frameEnter - frameLastExit).count();
        }
        frameHaveLast = true;
        frameLayerMs += std::chrono::duration<double, std::milli>(presentBegin - frameEnter).count();
        framePresentMs += std::chrono::duration<double, std::milli>(presentEnd - presentBegin).count();
        // The application's own share, from the frame it began to the frame it handed over. Left
        // unset for a frame whose xrBeginFrame never came back successfully, so a failed frame does
        // not drag the average down.
        if (g_beginAtValid) {
            g_renderMs += std::chrono::duration<double, std::milli>(frameEnter - g_beginAt).count();
            g_beginAtValid = false;
        }
        frameBreakdowns++;
        frameLastExit = presentEnd;
        return result;
    }

    XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
        // Before anything else: the depth hook points into this module's code, and the textures it
        // holds belong to a device that is about to go away. The near probe's staging pair goes with
        // them.
        NearProbeRelease();
        DepthCaptureDetach();
        ReleaseOutputSwapchains();
        g_swapchains.clear();
        g_session = XR_NULL_HANDLE;
        if (g_gaze.space != XR_NULL_HANDLE && g_next.DestroySpace) {
            g_next.DestroySpace(g_gaze.space);
        }
        if (g_gaze.actionSet != XR_NULL_HANDLE && g_next.DestroyActionSet) {
            g_next.DestroyActionSet(g_gaze.actionSet);
        }
        g_gaze.space = XR_NULL_HANDLE;
        g_gaze.actionSet = XR_NULL_HANDLE;
        g_gaze.action = XR_NULL_HANDLE;
        g_gaze.attached = false;
        g_gaze.poseValid = false;
        for (CropAnchor& anchor : g_anchor) {
            anchor = CropAnchor{};
        }
        g_interop.Shutdown();
        g_interopTried = false;
        g_interopGaveUp = false;
        return g_next.DestroySession(session);
    }

    XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
        NearProbeRelease();
        DepthCaptureDetach();
        ReleaseOutputSwapchains();
        g_swapchains.clear();
        g_interop.Shutdown();
        // The pipeline belongs to the device being dropped, so it goes with it rather than being
        // reused against a device that no longer exists.
        g_upscaleVs.Reset();
        g_easuPs.Reset();
        g_rcasPs.Reset();
        g_upscaleSampler.Reset();
        g_easuConstants.Reset();
        g_upscaleBlend.Reset();
        g_upscaleDepth.Reset();
        g_upscaleRaster.Reset();
        g_rcasConstants.Reset();
        g_appDevice.Reset();
        g_appContext.Reset();
        g_gaze = EyeGaze{};
        const XrResult result = g_next.DestroyInstance(instance);
        LayerLog("layer: instance destroyed (%d)\n", (int)result);
        g_next = NextDispatch{};
        return result;
    }

} // namespace

std::string LayerDirectory() {
    const HMODULE self = GetModuleHandleW(L"AmdnrXrLayer.dll");
    if (self == nullptr) {
        return ".";
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, buffer, (DWORD)std::size(buffer));
    if (length == 0 || length >= std::size(buffer)) {
        return ".";
    }

    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return ".";
    }
    path.resize(slash);

    char narrow[MAX_PATH * 4]{};
    if (WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow, (int)std::size(narrow), nullptr, nullptr) <= 0) {
        return ".";
    }
    return narrow;
}

void LayerLog(const char* fmt, ...) {
    if (!g_logAttempted) {
        g_logAttempted = true;
        // Share the file so the log can be tailed while an application is running.
        g_log = _fsopen(LogPath().c_str(), "a", _SH_DENYNO);
    }
    if (!g_log) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fprintf(g_log,
            "%04u-%02u-%02u %02u:%02u:%02u.%03u: ",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);

    fflush(g_log);
}

extern "C" {

__declspec(dllexport) XrResult XRAPI_CALL
AmdnrXrLayer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

__declspec(dllexport) XrResult XRAPI_CALL AmdnrXrLayer_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance);

__declspec(dllexport) XrResult XRAPI_CALL
AmdnrXrLayer_xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                const char* layerName,
                                                XrNegotiateApiLayerRequest* apiLayerRequest) {
    if (loaderInfo == nullptr || apiLayerRequest == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerName == nullptr || std::strcmp(layerName, kLayerName) != 0) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    apiLayerRequest->structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    apiLayerRequest->structSize = sizeof(XrNegotiateApiLayerRequest);
    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
    apiLayerRequest->getInstanceProcAddr =
        (PFN_xrGetInstanceProcAddr)&AmdnrXrLayer_xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance =
        (PFN_xrCreateApiLayerInstance)&AmdnrXrLayer_xrCreateApiLayerInstance;

    LayerLog("layer: negotiated with loader (interface v%u)\n", XR_CURRENT_LOADER_API_LAYER_VERSION);
    return XR_SUCCESS;
}

__declspec(dllexport) XrResult XRAPI_CALL AmdnrXrLayer_xrCreateApiLayerInstance(
    const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
    if (layerInfo == nullptr || layerInfo->nextInfo == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (layerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    const XrApiLayerNextInfo* next = layerInfo->nextInfo;
    g_next.GetInstanceProcAddr = next->nextGetInstanceProcAddr;
    g_next.CreateApiLayerInstance = next->nextCreateApiLayerInstance;

    LayerLog("layer: creating instance: app=%s\n",
             info && info->applicationInfo.applicationName ? info->applicationInfo.applicationName : "(unnamed)");

    // Every environment value the live settings block is seeded from, read here rather than at the
    // first session: the ceiling decides the shape of the very first frame, so it cannot wait for the
    // interop to come up. Idempotent, and anything the panel has already written is left alone.
    NrSettingsInitFromEnvironment();
    ReadCropOverride();
    ReadRenderScale();
    ReadTemporal();
    ReadStaleEyeSkip();
    ReadJitter();

    // Applications never ask for eye tracking themselves, so the layer asks for the extension on
    // their behalf and retries without it when the runtime below does not know it.
    std::vector<const char*> baseExtensions;
    if (info != nullptr) {
        baseExtensions.assign(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
    }
    std::vector<const char*> gazeExtensions = baseExtensions;
    if (std::find(gazeExtensions.begin(), gazeExtensions.end(), XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) ==
        gazeExtensions.end()) {
        gazeExtensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    }

    // What the application itself asked for. With the layer declaring the depth extension in its
    // manifest, this line is where it shows whether the application took it up -- and that decides
    // whether the depth probe will ever have anything to report.
    if (info != nullptr && !g_appExtensionsLogged) {
        g_appExtensionsLogged = true;
        std::string names;
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
            if (i != 0) {
                names += ' ';
            }
            names += info->enabledExtensionNames[i] != nullptr ? info->enabledExtensionNames[i] : "(null)";
        }
        LayerLog("layer: application enabled %u extensions: %s\n",
                 info->enabledExtensionCount,
                 names.c_str());
    }

    const auto createInstance = [&](const std::vector<const char*>& names) -> XrResult {
        XrInstanceCreateInfo request = *info;
        request.enabledExtensionCount = (uint32_t)names.size();
        request.enabledExtensionNames = names.empty() ? nullptr : names.data();
        XrApiLayerCreateInfo chained = *layerInfo;
        chained.nextInfo = next->next;
        return next->nextCreateApiLayerInstance(&request, &chained, instance);
    };

    g_gaze.extensionOffered = true;
    XrResult result = createInstance(gazeExtensions);
    // Not just XR_ERROR_EXTENSION_NOT_PRESENT: runtimes are not consistent about how they report an
    // extension they cannot deliver. Pimax answers -9, but SteamVR answers XR_ERROR_RUNTIME_FAILURE
    // (-2) for the same situation, which used to escape this retry and kill the whole instance - and
    // with it the application, since OpenComposite treats a failed create as fatal. Retry on any
    // failure so the only difference eye tracking can make is whether the layer gets gaze data.
    if (result != XR_SUCCESS) {
        g_gaze.extensionOffered = false;
        LayerLog("layer: runtime rejects %s (result %d), retrying without eye tracking\n",
                 XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
                 (int)result);
        result = createInstance(baseExtensions);
    }

    if (XR_SUCCEEDED(result)) {
        g_next.Instance = *instance;
        LogRuntimeExtensions();
    } else {
        LayerLog("layer: instance creation failed: %d\n", (int)result);
    }
    return result;
}

__declspec(dllexport) XrResult XRAPI_CALL AmdnrXrLayer_xrGetInstanceProcAddr(XrInstance instance,
                                                                             const char* name,
                                                                             PFN_xrVoidFunction* function) {
    if (function == nullptr || name == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;

    if (g_next.GetInstanceProcAddr == nullptr) {
        // The loader probes the layer during negotiation, before any instance exists.
        return XR_ERROR_HANDLE_INVALID;
    }

    Resolve("xrCreateSession", g_next.CreateSession);
    Resolve("xrDestroySession", g_next.DestroySession);
    Resolve("xrEnumerateViewConfigurationViews", g_next.EnumerateViewConfigurationViews);
    Resolve("xrCreateSwapchain", g_next.CreateSwapchain);
    Resolve("xrDestroySwapchain", g_next.DestroySwapchain);
    Resolve("xrEnumerateSwapchainImages", g_next.EnumerateSwapchainImages);
    Resolve("xrAcquireSwapchainImage", g_next.AcquireSwapchainImage);
    Resolve("xrWaitSwapchainImage", g_next.WaitSwapchainImage);
    Resolve("xrReleaseSwapchainImage", g_next.ReleaseSwapchainImage);
    Resolve("xrWaitFrame", g_next.WaitFrame);
    Resolve("xrBeginFrame", g_next.BeginFrame);
    Resolve("xrEndFrame", g_next.EndFrame);
    Resolve("xrDestroyInstance", g_next.DestroyInstance);
    Resolve("xrCreateActionSet", g_next.CreateActionSet);
    Resolve("xrCreateAction", g_next.CreateAction);
    Resolve("xrSuggestInteractionProfileBindings", g_next.SuggestInteractionProfileBindings);
    Resolve("xrCreateActionSpace", g_next.CreateActionSpace);
    Resolve("xrDestroyActionSet", g_next.DestroyActionSet);
    Resolve("xrDestroySpace", g_next.DestroySpace);
    Resolve("xrLocateSpace", g_next.LocateSpace);
    Resolve("xrAttachSessionActionSets", g_next.AttachSessionActionSets);
    Resolve("xrSyncActions", g_next.SyncActions);
    Resolve("xrStringToPath", g_next.StringToPath);
    Resolve("xrLocateViews", g_next.LocateViews);

    if (std::strcmp(name, "xrLocateViews") == 0 && g_next.LocateViews) {
        *function = (PFN_xrVoidFunction)&Hook_xrLocateViews;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrEnumerateViewConfigurationViews") == 0 &&
        g_next.EnumerateViewConfigurationViews) {
        *function = (PFN_xrVoidFunction)&Hook_xrEnumerateViewConfigurationViews;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrCreateSession") == 0 && g_next.CreateSession) {
        *function = (PFN_xrVoidFunction)&Hook_xrCreateSession;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrCreateSwapchain") == 0 && g_next.CreateSwapchain) {
        *function = (PFN_xrVoidFunction)&Hook_xrCreateSwapchain;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrDestroySwapchain") == 0 && g_next.DestroySwapchain) {
        *function = (PFN_xrVoidFunction)&Hook_xrDestroySwapchain;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrAcquireSwapchainImage") == 0 && g_next.AcquireSwapchainImage) {
        *function = (PFN_xrVoidFunction)&Hook_xrAcquireSwapchainImage;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrReleaseSwapchainImage") == 0 && g_next.ReleaseSwapchainImage) {
        *function = (PFN_xrVoidFunction)&Hook_xrReleaseSwapchainImage;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrWaitFrame") == 0 && g_next.WaitFrame) {
        *function = (PFN_xrVoidFunction)&Hook_xrWaitFrame;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrBeginFrame") == 0 && g_next.BeginFrame) {
        *function = (PFN_xrVoidFunction)&Hook_xrBeginFrame;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrEndFrame") == 0 && g_next.EndFrame) {
        *function = (PFN_xrVoidFunction)&Hook_xrEndFrame;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrDestroySession") == 0 && g_next.DestroySession) {
        *function = (PFN_xrVoidFunction)&Hook_xrDestroySession;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrDestroyInstance") == 0 && g_next.DestroyInstance) {
        *function = (PFN_xrVoidFunction)&Hook_xrDestroyInstance;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrAttachSessionActionSets") == 0 && g_next.AttachSessionActionSets) {
        *function = (PFN_xrVoidFunction)&Hook_xrAttachSessionActionSets;
        return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrSyncActions") == 0 && g_next.SyncActions) {
        *function = (PFN_xrVoidFunction)&Hook_xrSyncActions;
        return XR_SUCCESS;
    }

    return g_next.GetInstanceProcAddr(instance, name, function);
}

} // extern "C"
