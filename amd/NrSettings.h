// Live NR settings, shared by the OpenXR layer, the D3D12 interop and the control panel.
//
// Everything here used to be read once from the environment inside NrCore::Initialize, which made
// it impossible to change while a title was running -- and inside a headset there is no OptiScaler
// menu to open, so "while running" is the only place these can be judged.
//
// The block is process-global because the HTTP panel writes it and the per-eye NrCore sessions read
// it. Not everything lives here on purpose: the session-level choices (the network tier policy, the
// full-network kernel set, the temporal session layout) decide what gets built when a session
// starts, so changing them at runtime either rebuilds the network or breaks the session pairing.
// Those stay environment-only, where they have always been.

#pragma once

#include <cstdint>
#include <string>

// The bounds a ceiling can be moved between. The top of each is the largest input this runtime takes
// -- it reports that itself, as `input ceiling 1920x1080` in NrCore's ready line -- and a ceiling
// above it is not a slower frame but a refused one, so it is clamped rather than passed through. The
// floor is the point below which the window is a few dozen pixels across and the pass has nothing
// left to do.
constexpr uint32_t kNrCeilingMin = 256;
constexpr uint32_t kNrCeilingMaxWidth = 1920;
constexpr uint32_t kNrCeilingMaxHeight = 1080;

struct NrSettings {
    // Master switch. Off passes the eye buffer through untouched -- nothing is cropped, converted or
    // handed to a network -- which makes it the one control that costs nothing at all.
    bool enabled{true};

    // The runtime's own per-frame strength pair, 0..1 with 1 the shipped picture.
    float transferStrength{1.f};
    float colorStrength{1.f};

    // How much of the model's edit to keep (0.25..1, 1 = shipped) and how many times to run it per
    // frame (1..3). Both are per-frame values, so neither rebuilds anything.
    float modelScale{1.f};
    uint32_t passes{1};

    // The runtime's debug views: 0 normal, 1 proxy, 2 neural solo, 3 difference at 20x, 4 tint.
    uint32_t debugView{0};

    // The layer's own anti-flicker filter, which carries the network's correction from frame to frame
    // instead of letting every frame re-invent it. It is deliberately not the runtime's output
    // smoothing: no runtime this layer loads implements those ABI fields (see the note in
    // NrCore::Initialize), so a control wired to them would move nothing. What flickers on this path
    // is the difference the network adds over the untouched crop, and that difference -- not the
    // picture -- is what is accumulated, reprojected by the motion field and damped; the current
    // frame's own detail is still read fresh every frame.
    //
    // `antiFlicker` is the most of the history that may be kept for a pixel: 0 turns the filter off
    // (the shipped picture, and no history is written at all), 1 keeps as much as the gate allows.
    // `antiFlickerGate` is how far the correction may move, per channel and in linear light, before
    // the current frame wins outright -- below it the two are averaged, above it the frame is taken
    // whole so a moving edge never trails. The corrections sampled off a live session run a few
    // hundredths, so the default gate sits just above them.
    float antiFlicker{0.f};
    float antiFlickerGate{0.03f};

    // ---- The neural window itself: the ceiling it works under, how much of that it covers, and what
    // ---- its rim looks like
    //
    // The ceiling, per axis, in pixels: the largest input the runtime accepts and what the two
    // fractions below are a fraction *of*. Unlike them it is a resolution, so moving it moves the pass
    // to a different working size outright -- up to kNrCeilingMax*, and never past it, because an
    // input beyond what the runtime will take is a refused frame rather than a scaled one. Seeded from
    // AMDNR_XR_CROP, which is where it has always come from; this is only where it can now be moved
    // while the title runs. It is the one control here whose cost is not proportional: at 1920x1080 the
    // network is doing 2.25 times the work of 1280x720 per pass.
    uint32_t ceilingWidth{1920};
    uint32_t ceilingHeight{1080};
    //
    // How much of that ceiling the window covers, per axis, as a fraction. 1 is the
    // ceiling itself, which is the largest window that needs no resampling on the way in and so the
    // sharpest; below 1 the window and the network input shrink together, which is the only direction
    // that costs less; above 1 the window covers more of the eye buffer than the network can resolve
    // 1:1 and comes back softer. Sizing it against the eye buffer with a single number instead -- which
    // is what an isotropic scale against both ceiling axes does -- throws ceiling width away whenever
    // the eye buffer is not 16:9, so the two axes are separate.
    float windowWidth{1.f};
    float windowHeight{1.f};
    // The rim between the network's answer and the untouched image: `feather` is the width of the ramp
    // as a fraction of the window's shorter side, and `roundness` bends the region that ramp runs round
    // from the window's rectangle (0) to the ellipse inscribed in it (1) -- the shape an eye finds
    // least. Both are per-frame values: they change what the blend draws and nothing that gets built,
    // so they can be moved while the title runs, which is the whole reason the panel exists.
    float feather{0.15f};
    float roundness{0.f};

    // Whether the layer trims the window to hold the frame budget, on a controller that only moves
    // when a run of frames agrees (see NrQualityController.h). Off by default and deliberately so: it
    // trades the treated area for frame rate, and that is the reader's call, not a policy the layer
    // should impose. The two fractions above stay the maximum it trims down from, so raising them
    // still buys a larger tier -- the controller only ever takes, never adds.
    bool autoQuality{false};

    // The near plane the application projected its scene depth with, in metres, or 0 to leave the
    // motion field rotation-only. It is what turns the depth the layer reads into a distance: the
    // reduction publishes inverse distance, so one over this is what recovers 1/z, and the head's
    // translation is carried across a pixel as that over its distance. Wrong, it does not fail
    // loudly -- too small and the reprojection overshoots, too large and the near picture trails --
    // so it is a slider rather than a constant, and the one number here that has to be found by eye.
    float motionNear{0.f};

    // A counter the panel bumps to ask every session to drop its accumulated history. It is a counter
    // rather than a flag because there is one block and two sessions: a flag cleared by whichever read
    // it first would reach the second one already spent. Everything about the temporal path is a
    // feedback loop, so this is the only way back from a history that has been warped into nonsense --
    // which a wrong motion field can do, and which then persists, because each frame reprojects the
    // previous mistake forward instead of letting the fresh crop through.
    uint32_t historyReset{0};

    // The model-strength controls, named, ranged and defaulted the way OptiScaler and AMDNR's menu
    // hold them, so one number means one thing on either host: LocalTone and LocalStructure are 0..2
    // with 1 the shipped picture, SkinStructure is -1..2 where -1 means "follow the structure amount"
    // -- the model's own default, not a strength of minus one -- and AutoMask is the model's native
    // character/scene mask, on by default. The frame carries four feature scales rather than these
    // four menu values; NrControlWireOf below is that translation, and it is the same one the host
    // these controls came from performs, which is what makes a matching setting a matching picture.
    // All of it applies between frames with no rebuild and no restarted history: the runtime writes
    // the style into the loaded modules' dlss5_style_feature constant, which those kernels read at
    // launch, and scales the matching feature column of the first layer's 32x16 input mix -- the one
    // f32 region of the block-0 FFN weight -- in the device copy of that weight.
    float controlTone{1.f};
    float controlStructure{1.f};
    float controlSkin{-1.f};
    bool controlMask{true};
    float controlStyle{1.f};
    // Set once the panel has picked a style. The layer sends the controls flag only when a value is not
    // 1, because older runtimes refuse the 144-byte frame that carries the flag; that rule would also
    // swallow an explicit "back to the shipped style 1", which the runtime would then answer from its
    // own startup snapshot of DLSS5_STYLE instead. Pinning makes the choice stick either way.
    bool stylePinned{false};
};

// The four scales the frame's pre-block controls carry, from the four menu values above. The runtime
// multiplies one column of its 32x16 input mix by each, so 1 is the shipped picture; the wiring is
// OptiScaler's -- the host these controls were named after -- and matching it is what makes the same
// menu setting the same picture on either host. With the native mask on, the structure amount rides
// the two features the model's own mask reads (skin is its character half, other its scene half) and
// the plain structure feature stays neutral; with the mask off the amount goes through the plain
// structure feature instead and the mask pair is negated. The defaults -- 1, 1, -1, mask on -- come
// out as (1, 1, 1, 1), the shipped picture, which is what AMDNR's own reference sends before any of
// these is touched.
struct NrControlWire {
    float tone;
    float structure;
    float skin;
    float other;
};

inline NrControlWire NrControlWireOf(const NrSettings& settings) {
    const float structure = settings.controlStructure;
    const float skin = settings.controlSkin < 0.f ? structure : settings.controlSkin;
    if (settings.controlMask) {
        return {settings.controlTone, 1.f, skin, structure};
    }
    return {settings.controlTone, structure, -1.f, -1.f};
}

// A copy of the current block. Every reader takes a copy and then works from it, so no frame can see
// the panel's write land halfway through one.
NrSettings NrSettingsGet();

// What the panel writes. Values are clamped here rather than at the point of use, so nothing the
// runtime would reject ever reaches it.
void NrSettingsSet(const NrSettings& settings);

// Reads AMDNR_XR_STRENGTH / _DEBUG_VIEW / _ANTIFLICKER / _ANTIFLICKER_GATE / _CROP / _COVER /
// _FEATHER / _ROUNDNESS and the near plane into the block, so an environment that still names them
// gets exactly the values it used to. Called once and idempotent, from the point the instance is
// created rather than the first session: the ceiling decides the shape of the very first frame, so it
// cannot wait for the interop to come up. Anything the panel has already written is kept, because the
// read is a read-modify-write of the live block. It also runs the add-on bridge's first read (see
// below), so a config file named by the environment is in place before the first frame is cut.
void NrSettingsInitFromEnvironment();

// What the layer resolved the window to on the last frame it cut one, so the panel can show a
// resolution rather than a multiplier.
//
// A reader cannot judge a fraction: the pixels it buys depend on rounding to a multiple of eight and
// on a clamp against an eye buffer the panel is never shown. So the layer reports what it actually
// used. Reported, never set -- the panel writes NrSettings and reads this, and nothing goes the other
// way. The ceiling is not here: it is a setting, and after clamping it is the same number the panel
// wrote.
struct NrWindowReport {
    // The window actually cut out of the eye buffer. Equal to `network` at a cover of 1 and below;
    // above 1 the window is larger than the ceiling and has to be squeezed into it.
    uint32_t window[2]{};
    // What the network was handed, which is what the pass actually costs.
    uint32_t network[2]{};
};

void NrWindowReportSet(const NrWindowReport& report);
NrWindowReport NrWindowReportGet();

// What the runtime says it actually built, read back from its own status line. The pass count is the
// one setting the panel cannot apply on the next frame: it is written to a file the runtime reads
// once, when it builds the network, so between that write and the next launch the file and the live
// network disagree -- and "this needs a restart" is only true while they do. Reporting it lets the
// panel say that from evidence rather than always. Reported, never set, like NrWindowReport.
struct NrRuntimeReport {
    // The pass count the running network was built with, 0 until the first status line arrives.
    uint32_t passes{0};
    // What the neural passes cost the frame, in milliseconds: the layer's own measurement of the two
    // eye passes, summed. Not the runtime's `net_gpu_ms` -- that number is only printed when the
    // runtime's own GPU timing is switched on, and the deployment this was written against reports it
    // as `off`. A controller fed that would be fed zeros and would never move. The layer's stopwatch
    // is always running, and it measures exactly the cost the tier can do something about.
    double frameMs{0.0};
    // The scale the automatic tier is currently applying to the window, 1.0 when it is off or has not
    // moved. Written by the layer's frame loop, so the panel can show the tier it actually settled on
    // rather than the one it asked for.
    double qualityScale{1.0};
    // The anti-flicker probe: what a box of the history texture held the last time it was read back.
    // `filterValidShare` is the fraction of sampled pixels the filter had written a correction for,
    // `filterMagnitude` the mean absolute correction over those pixels, and `filterSampleFrame` the
    // frame it was sampled on (0 = never). `filterAccumulation` is the mean count of frames of
    // corrections each pixel's history stands for: a fresh write is 1.0, and only a frame that
    // actually blended against the previous history can move it above that -- so it is the one number
    // here that cannot be produced by a filter whose history is written but never read back. They
    // exist to be checkable rather than assumed: a filter whose history is never written, never read
    // or holding nothing reads as zero (or as a flat 1.0) here.
    double filterValidShare{0.0};
    double filterMagnitude{0.0};
    double filterAccumulation{0.0};
    uint64_t filterSampleFrame{0};
    // Frames the filter has run, and how many times its history was dropped since the process started.
    uint64_t filterFrames{0};
    uint64_t filterResets{0};
    // Whether the filter was on when the last import was taken, so the panel can say "off" rather
    // than showing a stale sample as if it were live.
    bool filterOn{false};
};

void NrRuntimeReportSet(const NrRuntimeReport& report);
NrRuntimeReport NrRuntimeReportGet();

// ---- The add-on bridge
//
// The hosts this scene uses keep their look in config files: OptiScaler.ini's [DlssNr] section (and
// in AMDNR's fork three style slots -- `key=value;...` strings of the menu's own settings, stored
// under StyleSlot1..3), and ReShade.ini's [RenoDX.DLSS5] section, where the ReShade add-on
// (renodx-dlss5.addon64) stores the same model-look controls under NR-prefixed names. A tuned style
// therefore already exists on a machine that runs any of them, so the layer probes for those files
// and consumes the look controls out of them: the same style arrives in the headset without being
// re-dialled by eye, and an edit to a file arrives while the title runs.
//
// Probe order: AMDNR_XR_ADDON_CONFIG (a file, or a folder to look in), then OptiScaler.ini /
// dlss5_style.ini / style.ini beside the game executable, then the same three beside the layer DLL
// -- and ReShade.ini beside the game executable is read after those, so both hosts can be consumed
// on one machine. AMDNR_XR_ADDON_SLOT (1..3) picks a StyleSlotN out of the ini; without it the
// slots are left alone, because which slot is in use is menu state the file does not record.
//
// Only the per-frame look controls are consumed (the alias table in NrControlServer.cpp is the
// list). Keys the runtime reads once, when it builds its network, are counted and left alone: a
// bridge that showed a value it cannot apply would be the pass-count lie again.
struct NrAddonReport {
    std::string path;       // the file the last read came from, empty when none has been found
    int applied = 0;        // pairs whose value was written into the block by that read
    int ignored = 0;        // pairs seen but not written: unknown name, or a value left on auto
    bool fromPaste = false; // the last apply came from the panel's box rather than from the file
};

// The bridge's first read, synchronous: run from NrSettingsInitFromEnvironment, so a config file
// named by the environment is in place before the first frame is cut. Idempotent.
void NrAddonBridgeInit();

// The watcher, one stat per second: started with the control server -- before its socket, so a busy
// port does not cost the file watch -- and stopped with it.
void NrAddonBridgeStart();
void NrAddonBridgeStop();

// Applies one text exactly as the file's own would be applied: an ini, an ini snippet, or a bare
// `key=value;...` slot string. What the panel's paste box sends.
void NrAddonBridgeApplyText(const std::string& text);

// What the panel shows: the file being watched and what the last read did with it.
NrAddonReport NrAddonBridgeReport();

// The control panel, on 127.0.0.1 only. Started with the interop (so it exists whenever NR could
// run) and stopped with it; both calls are idempotent, and a port already in use is reported, not
// fatal. The port comes from AMDNR_XR_PANEL_PORT, default 8787.
void NrControlServerStart();
void NrControlServerStop();
bool NrControlServerRunning();
