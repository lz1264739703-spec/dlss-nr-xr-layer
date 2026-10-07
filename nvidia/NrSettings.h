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

// The bounds a ceiling can be moved between. The top of each is the bound the reference host clamps
// to at 1920x1080 -- carried here so one ceiling number means the same input on either host -- and
// the floor is the point below which the window is a few dozen pixels across and the pass has
// nothing left to do.
constexpr uint32_t kNrCeilingMin = 256;
constexpr uint32_t kNrCeilingMaxWidth = 1920;
constexpr uint32_t kNrCeilingMaxHeight = 1080;

struct NrSettings {
    // Master switch. Off passes the eye buffer through untouched -- nothing is cropped, converted or
    // handed to a network -- which makes it the one control that costs nothing at all.
    bool enabled{true};

    // How strong the effect is overall, 0..1 with 1 the shipped picture -- the value both working
    // integrations run the feature at. It reaches the feature as DLSSNR.Intensity, which is this
    // backend's single strength knob (the AMD build's separate transfer and colour strengths have no
    // counterpart here).
    float intensity{1.f};

    // The layer's own anti-flicker filter, which carries the network's correction from frame to frame
    // instead of letting every frame re-invent it. It is deliberately not a runtime-side output
    // smoothing: this backend resets its feature every frame (no motion vectors are handed to it), so
    // the only history the pass has is the one this layer keeps. What flickers is the difference the
    // network adds over the untouched crop, and that difference -- not the picture -- is what is
    // accumulated, reprojected by the layer's own motion field and damped; the current frame's own
    // detail is still read fresh every frame.
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
    // The ceiling, per axis, in pixels: what the two fractions below are a fraction *of*. Unlike them
    // it is a resolution, so moving it moves the pass to a different working size outright -- up to
    // kNrCeilingMax*, and never past it, because that is the bound this build carries for the
    // feature's input. Seeded from AMDNR_XR_CROP, which is where it has always come from; this is only
    // where it can now be moved while the title runs. It is the one control here whose cost is not
    // proportional: at 1920x1080 the network is doing 2.25 times the work of 1280x720 per pass.
    uint32_t ceilingWidth{1920};
    uint32_t ceilingHeight{1080};
    //
    // How much of that ceiling the window covers, per axis, as a fraction. 1 is the ceiling itself,
    // which is the largest window that needs no resampling on the way in and so the sharpest; below 1
    // the window and the network input shrink together, which is the only direction that costs less;
    // above 1 the window covers more of the eye buffer than the network can resolve 1:1 and comes back
    // softer. Sizing it against the eye buffer with a single number instead -- which is what an
    // isotropic scale against both ceiling axes does -- throws ceiling width away whenever the eye
    // buffer is not 16:9, so the two axes are separate.
    float windowWidth{1.f};
    float windowHeight{1.f};
    // The rim between the network's answer and the untouched image: `feather` is the width of the ramp
    // as a fraction of the window's shorter side, and `roundness` bends the region that ramp runs
    // round from the window's rectangle (0) to the ellipse inscribed in it (1) -- the shape an eye
    // finds least. Both are per-frame values: they change what the blend draws and nothing that gets
    // built, so they can be moved while the title runs, which is the whole reason the panel exists.
    float feather{0.15f};
    float roundness{0.f};

    // The model-strength controls, named, ranged and defaulted the way OptiScaler and AMDNR's menu hold
    // them, so one number means one thing on either host: LocalTone and LocalStructure are 0..2 with 1
    // the shipped picture, SkinStructure is -1..2 where -1 means "follow the structure amount" -- the
    // feature's own default, not a strength of minus one -- and the model's native character mask is
    // on by default. This backend drives the feature's own parameter names with them
    // (DLSSNR.LocalToneStrength, LocalStructureStrength, SkinStructureStrength and UseAutoMask), so no
    // translation sits in between. The values are written to the parameter map every frame; a change
    // also rebuilds the feature, because a feature is allowed to have baked a value at creation and a
    // rebuild is the only way to be sure the change is heard either way.
    float controlTone{1.f};
    float controlStructure{1.f};
    float controlSkin{-1.f};
    bool controlMask{true};
    float controlStyle{1.f};
};

// A copy of the current block. Every reader takes a copy and then works from it, so no frame can see
// the panel's write land halfway through one.
NrSettings NrSettingsGet();

// What the panel writes. Values are clamped here rather than at the point of use, so nothing the
// runtime would reject ever reaches it.
void NrSettingsSet(const NrSettings& settings);

// Reads AMDNR_XR_INTENSITY / _ANTIFLICKER / _ANTIFLICKER_GATE / _CROP / _COVER / _FEATHER /
// _ROUNDNESS into the block, so an environment that still names them gets exactly the values it used
// to. Called once and idempotent, from the point the instance is created rather than the first
// session: the ceiling decides the shape of the very first frame, so it cannot wait for the interop
// to come up. Anything the panel has already written is kept, because the read is a
// read-modify-write of the live block. It also runs the add-on bridge's first read (see
// below), so a config file named by the environment is in place before the first frame is cut.
void NrSettingsInitFromEnvironment();

// What the anti-flicker filter actually did, read back out of its own history rather than assumed.
// The probe copies a box of the history into a readback buffer and decodes it on the CPU, so the
// numbers are measured; a filter whose history is never written, never read or holding nothing reads
// as zero (or as a flat 1.0) here. Written by the backend, read by the panel.
struct NrFilterReport {
    // The fraction of the sampled box the filter had written a correction for, and the mean absolute
    // correction over those pixels.
    double validShare{0.0};
    double magnitude{0.0};
    // The mean count of frames of corrections each pixel's history stands for: a fresh write is 1.0,
    // and only a frame that actually blended against the previous history can move it above that --
    // so it is the one number here that cannot be produced by a filter whose history is written but
    // never read back.
    double accumulation{0.0};
    // The filter's own frame count when the sample was taken (0 = never sampled).
    uint64_t sampleFrame{0};
    // Frames the filter has run, and how many times its history was dropped since the process started.
    uint64_t frames{0};
    uint64_t resets{0};
    // Whether the filter was on when the last import was taken, so the panel can say "off" rather
    // than showing a stale sample as if it were live.
    bool on{false};
};

void NrFilterReportSet(const NrFilterReport& report);
NrFilterReport NrFilterReportGet();

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

// ---- The add-on bridge
//
// The hosts this scene uses keep their look in config files: OptiScaler.ini's [DlssNr] section (and
// in AMDNR's fork three style slots -- `key=value;...` strings of the menu's own settings, stored
// under StyleSlot1..3), and ReShade.ini's [RenoDX.DLSS5] section, where the ReShade add-on
// (renodx-dlss5.addon64) stores the same model-look controls under NR-prefixed names. A tuned style
// therefore already exists on a machine that runs any of them, so the layer probes for those files
// and consumes the look controls out of them -- on this build the look controls are the same six
// the AMD sibling consumes, with the two separate strengths folded onto this backend's one knob (see
// the alias table in NrControlServer.cpp).
//
// Probe order: AMDNR_XR_ADDON_CONFIG (a file, or a folder to look in), then OptiScaler.ini /
// dlss5_style.ini / style.ini beside the game executable, then the same three beside the layer DLL
// -- and ReShade.ini beside the game executable is read after those, so both hosts can be consumed
// on one machine. AMDNR_XR_ADDON_SLOT (1..3) picks a StyleSlotN out of the ini; without it the
// slots are left alone, because which slot is in use is menu state the file does not record.
//
// Only the per-frame look controls are consumed. Keys that describe the network rather than its look
// (Passes, Preset, HookPoint...) are counted and left alone: a bridge that showed a value it cannot
// apply would be the lie the pass-count control already taught the sibling build to avoid.
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
