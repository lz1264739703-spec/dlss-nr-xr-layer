// The live NR settings block and the local control panel that writes it.
//
// The panel exists because the settings it drives are per-frame values inside a running title, and
// inside a headset there is no menu to reach them with: OptiScaler's overlay is a desktop window and
// never reaches the eye buffers. A page served from the layer itself is reachable from the desktop,
// from the headset's desktop view, or from anything else on the machine.
//
// The listener binds to 127.0.0.1 only, serves one connection at a time, and answers three routes:
//
//   GET /                 the panel
//   GET /state            the settings the layer is actually using, after clamping
//   GET /set?<key>=<val>  write, then answer with /state
//
// It is deliberately not a general HTTP server: no files, no paths, no keep-alive, no request bodies.

// The layer build already defines both of these; the guards keep a bare cl.exe invocation -- the
// standalone panel smoke test, for one -- from tripping over the Windows min/max macros.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LayerLog.h"
#include "NrSettings.h"

// ---------------------------------------------------------------------------------------------
// The block itself

namespace {

std::mutex g_settingsMutex;
NrSettings g_settings;
bool g_settingsFromEnvironment = false;

// Written by the layer, read by the panel, in the opposite direction to the settings block. A second
// mutex rather than the settings one because a frame's report write must never queue behind the
// panel's polling.
std::mutex g_filterMutex;
NrFilterReport g_filterReport;

// Same direction and same reason as the filter report: written by the layer's window cut, read by the
// panel. Its own mutex, so a window write never waits on a filter write.
std::mutex g_windowMutex;
NrWindowReport g_windowReport;

float ClampFinite(float value, float low, float high) {
    if (!std::isfinite(value)) {
        return low;
    }
    return std::clamp(value, low, high);
}

} // namespace

NrFilterReport NrFilterReportGet() {
    std::lock_guard<std::mutex> lock(g_filterMutex);
    return g_filterReport;
}

void NrFilterReportSet(const NrFilterReport& report) {
    std::lock_guard<std::mutex> lock(g_filterMutex);
    g_filterReport = report;
}

NrWindowReport NrWindowReportGet() {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return g_windowReport;
}

void NrWindowReportSet(const NrWindowReport& report) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_windowReport = report;
}

NrSettings NrSettingsGet() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void NrSettingsSet(const NrSettings& settings) {
    NrSettings clamped;
    clamped.enabled = settings.enabled;
    clamped.intensity = ClampFinite(settings.intensity, 0.f, 1.f);
    // The anti-flicker filter's own bounds. Strength is a share of the history, so it stops at 1;
    // the gate is a distance in linear light -- an upper end of a half would smooth across whole
    // scene changes (that is not stability, it is a trail), and the lower end sits just off zero so
    // the shader's division never sees an absurd gate.
    clamped.antiFlicker = ClampFinite(settings.antiFlicker, 0.f, 1.f);
    clamped.antiFlickerGate = ClampFinite(settings.antiFlickerGate, 0.001f, 0.5f);
    // The ceiling, unlike everything around it, is clamped against the shared bound rather than a
    // taste one: it is a working resolution, and the bound is what keeps one ceiling number the same
    // input on either host (kNrCeilingMax*).
    clamped.ceilingWidth = std::clamp(settings.ceilingWidth, kNrCeilingMin, kNrCeilingMaxWidth);
    clamped.ceilingHeight = std::clamp(settings.ceilingHeight, kNrCeilingMin, kNrCeilingMaxHeight);
    // Quarter of the ceiling is already a tiny window, and past half again larger the network is
    // being asked for detail it cannot carry -- both ends are the point where the slider stops doing
    // anything an eye can see, not safety limits.
    clamped.windowWidth = ClampFinite(settings.windowWidth, 0.25f, 1.5f);
    clamped.windowHeight = ClampFinite(settings.windowHeight, 0.25f, 1.5f);
    clamped.feather = ClampFinite(settings.feather, 0.f, 0.5f);
    clamped.roundness = ClampFinite(settings.roundness, 0.f, 1.f);
    // The menu range, which is OptiScaler's own for these -- the panel offers exactly this, so the
    // guard is the menu's bound rather than the feature's own, wider acceptance range. SkinStructure
    // is -1..2 where -1 means "follow the structure amount", which is the feature's own default.
    clamped.controlTone = ClampFinite(settings.controlTone, 0.f, 2.f);
    clamped.controlStructure = ClampFinite(settings.controlStructure, 0.f, 2.f);
    clamped.controlSkin = ClampFinite(settings.controlSkin, -1.f, 2.f);
    clamped.controlMask = settings.controlMask;
    clamped.controlStyle = ClampFinite(settings.controlStyle, 0.f, 2.f);

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = clamped;
}

void NrSettingsInitFromEnvironment() {
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (g_settingsFromEnvironment) {
            return;
        }
        g_settingsFromEnvironment = true;
    }

    // The names the layer has always read. They now only seed the panel, so a session that sets none
    // of them starts from the shipped picture exactly as it always did.
    char value[32]{};
    NrSettings settings = NrSettingsGet();

    if (GetEnvironmentVariableA("AMDNR_XR_INTENSITY", value, (DWORD)sizeof(value)) > 0) {
        settings.intensity = std::clamp((float)atof(value), 0.f, 1.f);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_ANTIFLICKER", value, (DWORD)sizeof(value)) > 0) {
        settings.antiFlicker = std::clamp((float)atof(value), 0.f, 1.f);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_ANTIFLICKER_GATE", value, (DWORD)sizeof(value)) > 0) {
        settings.antiFlickerGate = (float)atof(value);
    }
    // The ceiling the layer used to take from AMDNR_XR_CROP. It still does, and it is still the
    // largest input the feature takes -- what is new is that the panel can move it while the title
    // runs, which is why it is a setting now rather than a constant read at the instance.
    if (GetEnvironmentVariableA("AMDNR_XR_CROP", value, (DWORD)sizeof(value)) > 0) {
        unsigned width = 0;
        unsigned height = 0;
        if (sscanf_s(value, "%ux%u", &width, &height) == 2 && width >= kNrCeilingMin &&
            height >= kNrCeilingMin) {
            settings.ceilingWidth = width;
            settings.ceilingHeight = height;
        }
    }
    // The window size the layer used to take from AMDNR_XR_COVER, which named one number for both
    // axes. It still does, here, and the panel can then pull the two apart.
    if (GetEnvironmentVariableA("AMDNR_XR_COVER", value, (DWORD)sizeof(value)) > 0) {
        const float cover = (float)atof(value);
        settings.windowWidth = cover;
        settings.windowHeight = cover;
    }
    if (GetEnvironmentVariableA("AMDNR_XR_FEATHER", value, (DWORD)sizeof(value)) > 0) {
        settings.feather = (float)atof(value);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_ROUNDNESS", value, (DWORD)sizeof(value)) > 0) {
        settings.roundness = (float)atof(value);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_NR_OFF", value, (DWORD)sizeof(value)) > 0 && value[0] != '0') {
        settings.enabled = false;
    }

    NrSettingsSet(settings);

    // The add-on's own config, when there is one, is the more specific answer, so it lands after
    // everything above -- see the bridge section below. One synchronous read here; the watcher that
    // keeps it current is started later, with the control server.
    NrAddonBridgeInit();

    // Read back rather than logged on the way in: the clamp belongs to the block, and what is worth a
    // line is the value frames will actually use, not the one the environment asked for.
    const NrSettings live = NrSettingsGet();
    LayerLog("NrSettings: seeded (ceiling %ux%u, window %.2fx%.2f, feather %.3f, roundness %.2f)\n",
             live.ceilingWidth,
             live.ceilingHeight,
             live.windowWidth,
             live.windowHeight,
             live.feather,
             live.roundness);
}

// ---------------------------------------------------------------------------------------------
// The add-on bridge
//
// The hosts this scene uses keep their look in config files: OptiScaler.ini's [DlssNr] section, and
// in AMDNR's fork three style slots -- `key=value;...` strings of the menu's own settings, stored
// under StyleSlot1..3 (see the note in that fork's changelog: sixty-odd typed settings under their
// ini keys). A tuned style therefore already exists on a machine that runs either of them, so this
// probe reads that file and writes the look controls into the same block the panel writes -- a style
// travels from the desktop to the layer as a file, and an edit to the file arrives while the title
// runs.
//
// The ReShade add-on is the other host the same scene uses (renodx-dlss5.addon64, hosted by ReShade
// with nvngx_dlssnr.dll beside it), and it keeps the same controls in ReShade.ini's [RenoDX.DLSS5]
// section under NR-prefixed names. That file is read too, after the ini above, so one machine
// running both hosts gets both, and the add-on's own section has the last word.
//
// What it deliberately does not consume: the file's other keys. Passes, WorkingScale, Preset and
// the rest describe the network rather than its look -- and on this backend a model-strength change
// rebuilds the feature, so a value nobody asked for would cost a visible hitch. The colour-axis
// strengths (ColourStrength, NRColorStrength, AmdColourStrength) are absent for the nearer reason
// that this backend has one knob and not two: folding a colour axis into it would silently change
// the look instead of leaving it alone.

namespace {

// The name each control travels under in an add-on, normalised (lowercased, separators dropped).
// Every entry is a name this layer can actually apply, because a name it cannot apply would count as
// an applied setting and do nothing. The three strength spellings the other hosts use -- Intensity
// in the NVIDIA-line hosts, TransferStrength in OptiScaler's ini, AmdDetailStrength on AMDNR's AMD
// path (its own ini says it is the runtime codec's transfer strength there) -- all name the one
// knob this backend has, DLSSNR.Intensity, so all three are mapped to it.
enum class AddonField { Tone, Structure, Skin, Mask, Style, Intensity };

struct AddonAlias {
    const char* name;
    AddonField field;
};

constexpr AddonAlias kAddonAliases[] = {
    {"localtone", AddonField::Tone},
    {"tone", AddonField::Tone},
    {"controltone", AddonField::Tone},
    {"localstructure", AddonField::Structure},
    {"structure", AddonField::Structure},
    {"controlstructure", AddonField::Structure},
    {"skinstructure", AddonField::Skin},
    {"skin", AddonField::Skin},
    {"controlskin", AddonField::Skin},
    {"automask", AddonField::Mask},
    {"mask", AddonField::Mask},
    {"controlmask", AddonField::Mask},
    {"style", AddonField::Style},
    {"nrstyle", AddonField::Style},
    {"controlstyle", AddonField::Style},
    {"dlss5style", AddonField::Style},
    // The ReShade add-on's own names for the same controls (renodx-dlss5.addon64, section
    // [RenoDX.DLSS5] in ReShade.ini): it drives the same NVIDIA feature, so the look controls are the
    // same values under an NR prefix. Deliberately absent: its NRLook* family, which is the add-on's
    // own appearance filter and has no counterpart here; its NRPass2/3/4* overrides, because this
    // layer sends one set and not one per pass; NRPreset, which is the network architecture rather
    // than a look; and the colour-axis keys, which this backend has no knob for.
    {"nrlocaltone", AddonField::Tone},
    {"nrlocalstructure", AddonField::Structure},
    {"nrskinstructure", AddonField::Skin},
    {"nrautomask", AddonField::Mask},
    {"intensity", AddonField::Intensity},
    {"nrintensity", AddonField::Intensity},
    {"transferstrength", AddonField::Intensity},
    {"transfer", AddonField::Intensity},
    {"nrtransferstrength", AddonField::Intensity},
    {"amddetailstrength", AddonField::Intensity},
};

std::string AddonNormalized(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == ' ' || c == '\t' || c == '_' || c == '-') {
            continue;
        }
        out.push_back((char)tolower((unsigned char)c));
    }
    return out;
}

// A number, or nothing. `auto` -- the add-on's own "inherit" -- and anything with trailing prose
// are refused rather than read as zero: a value this bridge cannot read exactly is one it should
// not apply at all.
bool AddonNumber(const std::string& text, float& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    if (*end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    out = (float)parsed;
    return true;
}

bool AddonBool(const std::string& text, bool& out) {
    const std::string word = AddonNormalized(text);
    if (word == "true" || word == "1" || word == "yes" || word == "on") {
        out = true;
        return true;
    }
    if (word == "false" || word == "0" || word == "no" || word == "off") {
        out = false;
        return true;
    }
    return false;
}

// The style is a number in both hosts (OptiScaler's own note names 0 standard, 1 natural,
// 2 cinematic), but a person writing a style file writes the word. Both forms are taken; a value
// outside the add-on's own 0..2 is refused rather than clamped toward a style nobody named.
bool AddonStyle(const std::string& text, float& out) {
    const std::string word = AddonNormalized(text);
    if (word == "standard" || word == "nvidia" || word == "default") {
        out = 0.f;
        return true;
    }
    if (word == "natural") {
        out = 1.f;
        return true;
    }
    if (word == "cinematic") {
        out = 2.f;
        return true;
    }
    float number = 0.f;
    if (!AddonNumber(text, number) || number < 0.f || number > 2.f) {
        return false;
    }
    out = number;
    return true;
}

struct AddonCounts {
    int applied = 0;
    int ignored = 0;
};

// One `key=value`. A recognised name whose value cannot be read counts as ignored, not applied: the
// number the panel shows has to mean "values that were written".
void ApplyAddonPair(const std::string& key,
                    const std::string& value,
                    NrSettings& settings,
                    AddonCounts& counts) {
    const std::string name = AddonNormalized(key);
    if (name.empty()) {
        return;
    }
    for (const AddonAlias& alias : kAddonAliases) {
        if (name != alias.name) {
            continue;
        }
        float number = 0.f;
        bool flag = false;
        switch (alias.field) {
        case AddonField::Tone:
            if (AddonNumber(value, number)) {
                settings.controlTone = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Structure:
            if (AddonNumber(value, number)) {
                settings.controlStructure = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Skin:
            if (AddonNumber(value, number)) {
                settings.controlSkin = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Mask:
            if (AddonBool(value, flag)) {
                settings.controlMask = flag;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Style:
            if (AddonStyle(value, number)) {
                settings.controlStyle = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Intensity:
            if (AddonNumber(value, number)) {
                settings.intensity = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        }
        return;
    }
    counts.ignored++;
}

// Whether a key names one of the three style slots, and which. Checked before the line is split on
// semicolons, because a slot's value is one whole `key=value;...` string: the fragments after it
// belong to the slot, not to the file.
bool AddonSlotNumber(const std::string& key, int& slot) {
    const std::string name = AddonNormalized(key);
    if (name.size() != 10 || name.compare(0, 9, "styleslot") != 0) {
        return false;
    }
    const char digit = name[9];
    if (digit < '1' || digit > '3') {
        return false;
    }
    slot = digit - '0';
    return true;
}

// The two section names a look is stored under: OptiScaler's [DlssNr] and the ReShade add-on's
// [RenoDX.DLSS5]. Everything else in those hosts' files -- and the thousands of keys a ReShade.ini
// holds when it carries no add-on section at all -- is not addressed to this layer.
bool AddonSectionHoldsLook(const std::string& name) {
    return name == "dlssnr" || name == "renodx.dlss5";
}

// Walks one text and applies what it recognises. Two shapes are accepted because the hosts write
// two: an ini with a section, and the bare `key=value;...` string a style slot stores. A text with
// no section at all is read whole -- that is what a slot string, a `style.ini` or a hand-written
// file looks like. `slot` selects a StyleSlotN out of the file (0 = none); the slot's own string is
// then applied through this same walker.
AddonCounts ApplyAddonText(const std::string& text, int slot, NrSettings& settings) {
    AddonCounts counts;
    std::vector<std::string> slots(4);

    // Which rule the file is read under. A host's ini is read through its recognized sections only
    // -- OptiScaler.ini and ReShade.ini both carry hundreds of keys across a dozen sections, and
    // most of them are not look controls. A file with no sections at all is read whole; a file with
    // sections but none of ours reads as nothing, which is what a ReShade.ini without the add-on's
    // section is.
    bool hasAnySection = false;
    bool sectioned = false;
    {
        size_t scan = 0;
        while (scan <= text.size()) {
            const size_t next = text.find('\n', scan);
            const size_t end = next == std::string::npos ? text.size() : next;
            const std::string line = text.substr(scan, end - scan);
            const size_t begin = line.find_first_not_of(" \t\r");
            if (begin != std::string::npos && line[begin] == '[') {
                hasAnySection = true;
                const size_t close = line.find(']', begin);
                if (close != std::string::npos &&
                    AddonSectionHoldsLook(AddonNormalized(line.substr(begin + 1, close - begin - 1)))) {
                    sectioned = true;
                    break;
                }
            }
            if (next == std::string::npos) {
                break;
            }
            scan = next + 1;
        }
    }

    // A file written by an editor that marks UTF-8 carries a BOM, and the BOM is part of the first
    // line's bytes: without skipping it, a section on line 1 would not be seen as a section.
    size_t index = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        index = 3;
    }
    std::string section;
    while (index <= text.size()) {
        const size_t next = text.find('\n', index);
        const size_t end = next == std::string::npos ? text.size() : next;
        std::string line = text.substr(index, end - index);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos || line[begin] == ';' || line[begin] == '#') {
            // Empty, or a comment -- these hosts comment with ';'.
        } else if (line[begin] == '[') {
            const size_t close = line.find(']', begin);
            section = close == std::string::npos
                          ? std::string()
                          : AddonNormalized(line.substr(begin + 1, close - begin - 1));
        } else {
            const bool candidate = sectioned ? AddonSectionHoldsLook(section) : !hasAnySection;
            const size_t equals = line.find('=', begin);
            int slotNumber = 0;
            if (equals != std::string::npos &&
                AddonSlotNumber(line.substr(begin, equals - begin), slotNumber)) {
                // Slot storage, not an assignment. Only the slot the environment names is consumed
                // (below) because which slot is *applied* is menu state the file does not record;
                // applying all three would just leave the last one standing.
                if (candidate) {
                    slots[slotNumber] = line.substr(equals + 1);
                }
            } else {
                size_t fragment = begin;
                while (fragment <= line.size()) {
                    const size_t stop = line.find(';', fragment);
                    const size_t fragEnd = stop == std::string::npos ? line.size() : stop;
                    const std::string pair = line.substr(fragment, fragEnd - fragment);
                    const size_t pairBegin = pair.find_first_not_of(" \t");
                    if (pairBegin != std::string::npos) {
                        const size_t pairEquals = pair.find('=', pairBegin);
                        if (pairEquals != std::string::npos) {
                            std::string value = pair.substr(pairEquals + 1);
                            const size_t valueEnd = value.find_last_not_of(" \t");
                            value = valueEnd == std::string::npos ? std::string()
                                                                  : value.substr(0, valueEnd + 1);
                            if (candidate) {
                                ApplyAddonPair(pair.substr(pairBegin, pairEquals - pairBegin),
                                               value,
                                               settings,
                                               counts);
                            }
                            // A pair in another section is not counted either way: the readout is
                            // about this layer's controls, and OptiScaler.ini has hundreds of keys
                            // nothing here has a name for.
                        }
                    }
                    if (stop == std::string::npos) {
                        break;
                    }
                    fragment = stop + 1;
                }
            }
        }
        if (next == std::string::npos) {
            break;
        }
        index = next + 1;
    }

    if (slot >= 1 && slot <= 3 && !slots[slot].empty()) {
        const AddonCounts inner = ApplyAddonText(slots[slot], 0, settings);
        counts.applied += inner.applied;
        counts.ignored += inner.ignored;
    }
    return counts;
}

// The folder the game's executable sits in, which is where an add-on's config lives -- the layer is
// loaded into that process, so the process image is the title, not the runtime host.
std::wstring ProcessImageDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring text(path, length);
    const size_t cut = text.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : text.substr(0, cut);
}

// The layer's own folder, which is also the folder the FSR headers and the NGX caller shim sit in:
// the same module-address trick the log uses, so it is right even before any other API is called.
std::wstring LayerDirectoryWide() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LayerDirectoryWide),
                            &module)) {
        return std::wstring();
    }
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::wstring();
    }
    std::wstring directory(path, length);
    const size_t cut = directory.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : directory.substr(0, cut);
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return std::string();
    }
    std::string out((size_t)length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], length, nullptr, nullptr);
    return out;
}

bool FileIsPresent(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    out.clear();
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (ReadFile(file, &out[0], (DWORD)out.size(), &read, nullptr)) {
            out.resize(read);
        } else {
            out.clear();
        }
    }
    CloseHandle(file);
    return true;
}

std::wstring JoinPath(const std::wstring& directory, const wchar_t* name) {
    return directory.empty() ? std::wstring(name) : directory + L"\\" + name;
}

// The three names an add-on's config is found under, in the order the probe prefers them: the
// engine's own ini first, then the two names a shared style travels under.
constexpr const wchar_t* kAddonNames[] = {L"OptiScaler.ini", L"dlss5_style.ini", L"style.ini"};

std::wstring FindAddonInDirectory(const std::wstring& directory) {
    for (const wchar_t* name : kAddonNames) {
        if (FileIsPresent(JoinPath(directory, name))) {
            return JoinPath(directory, name);
        }
    }
    return std::wstring();
}

// The probe, in the order the panel documents: the environment's own path (a file, or a folder to
// look in), then the game's folder, then the layer's.
std::wstring DetectAddonFile() {
    wchar_t named[1024]{};
    if (GetEnvironmentVariableW(L"AMDNR_XR_ADDON_CONFIG", named, (DWORD)(sizeof(named) / sizeof(named[0]))) > 0) {
        std::wstring path(named);
        // A relative value is read against the game's own folder, which is where an add-on's config
        // is; anything else (a drive, a UNC path) is the path itself.
        if (!path.empty() && path.find(L':') == std::wstring::npos &&
            path.compare(0, 2, L"\\\\") != 0) {
            path = JoinPath(ProcessImageDirectory(), path.c_str());
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                const std::wstring inside = FindAddonInDirectory(path);
                if (!inside.empty()) {
                    return inside;
                }
            } else {
                return path;
            }
        }
    }
    std::wstring found = FindAddonInDirectory(ProcessImageDirectory());
    if (!found.empty()) {
        return found;
    }
    return FindAddonInDirectory(LayerDirectoryWide());
}

// Every file a look may live in, in the order they are applied. Two hosts keep two kinds of file and
// one machine can have both, so neither wins by merely existing: the OptiScaler family's ini (or a
// style file) is read first, and ReShade.ini's [RenoDX.DLSS5] section -- where the ReShade add-on
// stores the same controls under NR-prefixed names -- last, so its names land ahead.
std::vector<std::wstring> DetectAddonFiles() {
    std::vector<std::wstring> files;
    const std::wstring primary = DetectAddonFile();
    if (!primary.empty()) {
        files.push_back(primary);
    }
    const std::wstring reshade = JoinPath(ProcessImageDirectory(), L"ReShade.ini");
    if (FileIsPresent(reshade) && (primary.empty() || reshade != primary)) {
        files.push_back(reshade);
    }
    return files;
}

int AddonSlotFromEnvironment() {
    char value[32]{};
    if (GetEnvironmentVariableA("AMDNR_XR_ADDON_SLOT", value, (DWORD)sizeof(value)) > 0) {
        const int slot = atoi(value);
        if (slot >= 1 && slot <= 3) {
            return slot;
        }
    }
    return 0;
}

std::mutex g_addonMutex;
// One file's identity: where it is, and the write time + size its last read was taken at, so an
// unchanged file is not re-applied.
struct AddonSource {
    std::wstring path;
    unsigned long long stamp = 0;
};
struct AddonState {
    std::vector<AddonSource> sources;
    int applied = 0;
    int ignored = 0;
    bool fromPaste = false;
};
AddonState g_addon;
std::thread g_addonThread;
std::atomic<bool> g_addonStop{false};

bool AddonFileStamp(const std::wstring& path, unsigned long long& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    stamp = ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) |
            data.ftLastWriteTime.dwLowDateTime;
    stamp ^= ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return true;
}

// Reads one file and adds its counts to `counts`, so two files leave one number for the panel while
// the log still says which file each setting came from.
void ConsumeAddonFile(const std::wstring& path, NrSettings& settings, AddonCounts& counts) {
    std::string text;
    if (!ReadTextFile(path, text)) {
        LayerLog("addon: %s could not be read; its settings are left as they are\n",
                 Utf8(path).c_str());
        return;
    }
    const AddonCounts one = ApplyAddonText(text, AddonSlotFromEnvironment(), settings);
    counts.applied += one.applied;
    counts.ignored += one.ignored;
    LayerLog("addon: %d settings applied, %d ignored, from %s\n",
             one.applied,
             one.ignored,
             Utf8(path).c_str());
}

void RefreshAddonFiles() {
    std::vector<AddonSource> sources;
    for (const std::wstring& path : DetectAddonFiles()) {
        unsigned long long stamp = 0;
        if (AddonFileStamp(path, stamp)) {
            sources.push_back(AddonSource{path, stamp});
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_addonMutex);
        bool same = g_addon.sources.size() == sources.size();
        for (size_t i = 0; same && i < sources.size(); ++i) {
            same = g_addon.sources[i].path == sources[i].path &&
                   g_addon.sources[i].stamp == sources[i].stamp;
        }
        if (same) {
            return;
        }
        if (sources.empty()) {
            const bool had = !g_addon.sources.empty();
            g_addon.sources.clear();
            if (had) {
                LayerLog("addon: no config file left; the settings already applied stay\n");
            }
            return;
        }
    }
    // Read them outside the lock: a small file each, and the layer's frame path must never wait on a
    // write to a file it is not reading.
    NrSettings settings = NrSettingsGet();
    AddonCounts counts;
    for (const AddonSource& source : sources) {
        ConsumeAddonFile(source.path, settings, counts);
    }
    NrSettingsSet(settings);
    {
        std::lock_guard<std::mutex> lock(g_addonMutex);
        g_addon.sources = sources;
        g_addon.applied = counts.applied;
        g_addon.ignored = counts.ignored;
        g_addon.fromPaste = false;
    }
}

void AddonWatchLoop() {
    while (!g_addonStop.load()) {
        // Five short waits rather than one long one, so a stopped bridge joins promptly -- the
        // layer's own teardown runs inside the game's exit path.
        for (int i = 0; i < 5 && !g_addonStop.load(); ++i) {
            Sleep(200);
        }
        if (!g_addonStop.load()) {
            RefreshAddonFiles();
        }
    }
}

} // namespace

void NrAddonBridgeInit() {
    RefreshAddonFiles();
}

void NrAddonBridgeStart() {
    if (g_addonThread.joinable()) {
        return;
    }
    g_addonStop.store(false);
    g_addonThread = std::thread(AddonWatchLoop);
}

void NrAddonBridgeStop() {
    g_addonStop.store(true);
    if (g_addonThread.joinable()) {
        g_addonThread.join();
    }
}

void NrAddonBridgeApplyText(const std::string& text) {
    if (text.empty()) {
        return;
    }
    NrSettings settings = NrSettingsGet();
    const AddonCounts counts = ApplyAddonText(text, 0, settings);
    NrSettingsSet(settings);
    {
        std::lock_guard<std::mutex> lock(g_addonMutex);
        g_addon.applied = counts.applied;
        g_addon.ignored = counts.ignored;
        // The sources stay: the files are still the ones being watched, and saying so is what keeps
        // the readout from claiming no file was found just because the last write came from the box.
        g_addon.fromPaste = true;
    }
    LayerLog("addon: panel applied %d settings, %d ignored, from the pasted text\n",
             counts.applied,
             counts.ignored);
}

NrAddonReport NrAddonBridgeReport() {
    std::lock_guard<std::mutex> lock(g_addonMutex);
    NrAddonReport report;
    for (size_t i = 0; i < g_addon.sources.size(); ++i) {
        if (i != 0) {
            report.path += " + ";
        }
        report.path += Utf8(g_addon.sources[i].path);
    }
    report.applied = g_addon.applied;
    report.ignored = g_addon.ignored;
    report.fromPaste = g_addon.fromPaste;
    return report;
}

// ---------------------------------------------------------------------------------------------
// Serialisation

namespace {

// A JSON string body for the one field here that is a path: escapes what JSON requires, drops
// control characters, and stops at 512 bytes on a UTF-8 character boundary. A path longer than that
// is in the log in full, and half a sequence would make the whole document unparseable.
std::string JsonEscaped(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    size_t index = 0;
    while (index < text.size() && out.size() < 512) {
        const unsigned char c = (unsigned char)text[index];
        if (c < 0x80) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
                out.push_back((char)c);
            } else if (c < 0x20) {
                out.push_back('?');
            } else {
                out.push_back((char)c);
            }
            index += 1;
            continue;
        }
        // A multibyte sequence goes in whole or not at all.
        size_t width = 2;
        if (c >= 0xF0) {
            width = 4;
        } else if (c >= 0xE0) {
            width = 3;
        }
        if (index + width > text.size()) {
            break;
        }
        out.append(text, index, width);
        index += width;
    }
    return out;
}

} // namespace

std::string SettingsToJson() {
    const NrSettings s = NrSettingsGet();
    const NrFilterReport f = NrFilterReportGet();
    const NrWindowReport r = NrWindowReportGet();
    const NrAddonReport addon = NrAddonBridgeReport();
    const std::string addonPath = JsonEscaped(addon.path);
    // 2560 rather than the ~1200 the numbers need: the watched config file's path is the one field
    // here whose length is not bounded by the layer, and a truncated JSON document would take the
    // whole panel down rather than one reading.
    char json[2560]{};
    snprintf(json,
             sizeof(json),
             "{\"enabled\":%d,\"intensity\":%.4f,\"antiflicker\":%.4f,\"afgate\":%.4f,"
             "\"tone\":%.4f,\"structure\":%.4f,\"skin\":%.4f,\"mask\":%d,\"style\":%.4f,"
             "\"ceilw\":%u,\"ceilh\":%u,\"winw\":%.4f,\"winh\":%.4f,"
             "\"feather\":%.4f,\"roundness\":%.4f,"
             // The anti-flicker probe, read back out of the filter's own history: what the layer
             // measured rather than what it was asked for, which is the only way the panel can show
             // that the history is written and consumed instead of quietly doing nothing.
             "\"afon\":%d,\"afframes\":%llu,\"afresets\":%llu,"
             "\"afvalid\":%.4f,\"afmag\":%.5f,\"afdepth\":%.2f,\"afframe\":%llu,"
             // What the layer resolved the window to on its last cut, so the sliders above can read
             // out in pixels rather than in a fraction a reader cannot judge.
             "\"boxw\":%u,\"boxh\":%u,\"netw\":%u,\"neth\":%u,"
             // The add-on bridge: which config file is being watched, and what the last read of it
             // -- or the last paste -- actually wrote. `addonpaste` says which of the two it was,
             // because "came from the box" and "came from the file" need different things done.
             "\"addonpath\":\"%s\",\"addonapplied\":%d,\"addonignored\":%d,\"addonpaste\":%d}",
             s.enabled ? 1 : 0,
             s.intensity,
             s.antiFlicker,
             s.antiFlickerGate,
             s.controlTone,
             s.controlStructure,
             s.controlSkin,
             s.controlMask ? 1 : 0,
             s.controlStyle,
             s.ceilingWidth,
             s.ceilingHeight,
             s.windowWidth,
             s.windowHeight,
             s.feather,
             s.roundness,
             f.on ? 1 : 0,
             f.frames,
             f.resets,
             f.validShare,
             f.magnitude,
             f.accumulation,
             f.sampleFrame,
             r.window[0],
             r.window[1],
             r.network[0],
             r.network[1],
             addonPath.c_str(),
             addon.applied,
             addon.ignored,
             addon.fromPaste ? 1 : 0);
    return json;
}

namespace {

// Percent decoding for the one value that is text rather than a number: the paste box sends a whole
// snippet, and `;`, `=` and newlines only survive a query string encoded. `+` is left as itself --
// encodeURIComponent writes a space as %20, and reading a literal `+` as a space is how a value
// that never had one gets mangled quietly.
std::string PercentDecode(const std::string& text) {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const int high = nibble(text[index + 1]);
            const int low = nibble(text[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back((char)(high * 16 + low));
                index += 2;
                continue;
            }
        }
        out.push_back(text[index]);
    }
    return out;
}

// Applies one `key=value` pair from the query string. Unknown keys are ignored rather than refused:
// the panel is edited by hand as often as it is used, and a typo should not cost the whole write.
void ApplyPair(const std::string& key, const std::string& value) {
    if (key.empty() || value.empty()) {
        return;
    }
    NrSettings s = NrSettingsGet();
    const float number = (float)atof(value.c_str());
    const long integer = atol(value.c_str());

    if (key == "enabled") {
        s.enabled = integer != 0;
    } else if (key == "intensity") {
        s.intensity = number;
    } else if (key == "antiflicker") {
        s.antiFlicker = number;
    } else if (key == "afgate") {
        s.antiFlickerGate = number;
    } else if (key == "ceilw") {
        s.ceilingWidth = (uint32_t)std::max(0L, integer);
    } else if (key == "ceilh") {
        s.ceilingHeight = (uint32_t)std::max(0L, integer);
    } else if (key == "winw") {
        s.windowWidth = number;
    } else if (key == "winh") {
        s.windowHeight = number;
    } else if (key == "feather") {
        s.feather = number;
    } else if (key == "roundness") {
        s.roundness = number;
    } else if (key == "tone") {
        s.controlTone = number;
    } else if (key == "structure") {
        s.controlStructure = number;
    } else if (key == "skin") {
        s.controlSkin = number;
    } else if (key == "mask") {
        // The feature's own character/scene mask (DLSSNR.UseAutoMask), not a scale of its own: on, the
        // structure amount rides the model's own mask; off, the mask is not consulted at all.
        s.controlMask = integer != 0;
    } else if (key == "style") {
        s.controlStyle = number;
    } else if (key == "addontext") {
        // The paste box, and the one branch here that must not fall through to the write at the
        // bottom: the bridge reads the block itself, and writing back the copy this function read
        // at the top would undo everything the bridge just applied.
        NrAddonBridgeApplyText(PercentDecode(value));
        return;
    } else {
        return;
    }
    NrSettingsSet(s);
}

void ApplyQuery(const std::string& query) {
    size_t index = 0;
    while (index < query.size()) {
        const size_t next = query.find('&', index);
        const std::string pair = query.substr(index, next == std::string::npos ? std::string::npos : next - index);
        const size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            ApplyPair(pair.substr(0, equals), pair.substr(equals + 1));
        }
        if (next == std::string::npos) {
            break;
        }
        index = next + 1;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The panel

namespace {

const char kPanelHtml[] = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8"><title>AMDNR 画面降噪 · 实时调节</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{color-scheme:dark}
body{margin:0;padding:18px 20px 40px;background:#14161a;color:#e6e8ec;
     font:14px/1.5 "Segoe UI",system-ui,sans-serif}
h1{font-size:17px;margin:0 0 2px}
h2{font-size:13px;margin:22px 0 8px;padding-bottom:6px;border-bottom:1px solid #2b2f36;
   color:#9aa3af;letter-spacing:.06em;text-transform:uppercase}
.sub{color:#6f7784;font-size:12px;margin-bottom:14px}
.row{display:grid;grid-template-columns:250px 1fr 84px;gap:10px;align-items:center;
     padding:7px 0}
.row label{color:#c3c9d2}
.row label .k{display:block;color:#596170;font-size:11px;line-height:1.35;
     font-family:Consolas,monospace;margin-top:2px}
.help{color:#7b838f;font-size:12px;margin:2px 0 10px}
input[type=range]{width:100%;accent-color:#4c8bf5}
input[type=number],select{background:#1d2026;border:1px solid #333842;color:#e6e8ec;
     border-radius:5px;padding:3px 6px;width:100%}
.big{display:flex;align-items:center;gap:12px;margin:10px 0 4px}
.big button{font-size:15px;padding:9px 18px;border-radius:7px;border:1px solid #333842;
     background:#22262d;color:#e6e8ec;cursor:pointer}
.big.on button{background:#1f5c33;border-color:#2f7f49}
.big .state{color:#9aa3af}
button.mini{background:#22262d;border:1px solid #333842;color:#c3c9d2;border-radius:5px;
     padding:5px 10px;cursor:pointer;margin-right:8px}
.warn{background:#2a2113;border:1px solid #4a3a17;color:#e0c68a;border-radius:6px;
     padding:8px 11px;font-size:12px;margin:8px 0 2px}
.resline{font:13px Consolas,monospace;color:#c3c9d2;background:#1d2026;border:1px solid #333842;
     border-radius:6px;padding:8px 11px;margin:8px 0 2px}
.resline b{color:#8ab4ff;font-weight:600}
.px{font:12px Consolas,monospace;color:#8ab4ff;text-align:right}
#foot{margin-top:26px;color:#5f6773;font-size:12px}
#foot code{color:#8d96a3}
</style></head><body>
<h1>AMDNR 画面降噪 · 实时调节</h1>
<div class="sub">面板由游戏里的层自己提供 · <span id="link">.</span> · 改完下一帧就生效，不用重启游戏</div>

<h2>一、总开关</h2>
<div class="big" id="master"><button id="mbtn">…</button>
  <span class="state" id="mstate"></span></div>
<div class="help">开着 = 画面会被降噪处理（会吃性能）。关掉 = 画面原样输出，一点性能都不花。</div>

<h2>二、模型强度 · 和 A 卡（OptiScaler / AMDNR 菜单）同名、同范围、同默认</h2>
<div class="warn">这一组直接写进 DLSS-NR 自己的参数：<code>DLSSNR.Intensity / LocalToneStrength / LocalStructureStrength / SkinStructureStrength / UseAutoMask / Style</code>。
<strong>和 OptiScaler / AMDNR 菜单里的「模型强度」是同一套名字、同一套范围、同一套默认值</strong>，同一个数字在两张卡上是同一个画面。<br>
<strong>改动会重建一次特征（顿一下）</strong>，所以尽量在菜单或静止场景里调；重建只发生一次，不是每帧。</div>
<div class="row"><label>整体强度<span class="k">DLSSNR.Intensity<br>整个效果用多少力。1.0 = 出厂（两个参考实现都跑在这个值）。A 卡上分开的「细节强度 / 色彩强度」在这个后端合并成这一个</span></label>
  <input type="range" id="intensity" min="0" max="1" step="0.01">
  <input type="number" id="intensity_n" min="0" max="1" step="0.01"></div>
<div class="row"><label>色调强度<span class="k">LocalTone → DLSSNR.LocalToneStrength<br>0–2，默认 1</span></label>
  <input type="range" id="tone" min="0" max="2" step="0.05">
  <input type="number" id="tone_n" min="0" max="2" step="0.05"></div>
<div class="row"><label>结构强度<span class="k">LocalStructure → DLSSNR.LocalStructureStrength<br>0–2，默认 1</span></label>
  <input type="range" id="structure" min="0" max="2" step="0.05">
  <input type="number" id="structure_n" min="0" max="2" step="0.05"></div>
<div class="row"><label>角色结构<span class="k">SkinStructure → DLSSNR.SkinStructureStrength<br>−1–2，默认 −1；−1 = 跟随结构强度（模型自己的默认行为，不是「强度为零」）</span></label>
  <input type="range" id="skin" min="-1" max="2" step="0.05">
  <input type="number" id="skin_n" min="-1" max="2" step="0.05"></div>
<div class="row"><label>原生角色掩码<span class="k">AutoMask → DLSSNR.UseAutoMask<br>模型自己的人物 / 场景区分，默认开</span></label>
  <select id="mask"><option value="1">1 · 开（默认，出厂画面）</option><option value="0">0 · 关</option></select><span></span></div>
<div class="row"><label>风格档位<span class="k">Style → DLSSNR.Style<br>改完重建一次特征</span></label>
  <select id="style">
    <option value="1">1 · 默认风格（出厂设定）</option>
    <option value="0">0 · NVIDIA 默认，最接近参考输出</option>
    <option value="2">2 · 第三种风格</option>
  </select><span></span></div>
<div style="margin-top:10px">
  <button class="mini" id="reset">全部复位到出厂（模型强度 / 输入框尺寸 / 边缘参数）</button>
</div>

)HTML"
R"HTML(<h2>三、输入框（窗口）· 改完下一帧生效</h2>
<div class="help">这一组管的就是<strong>喂给神经网络的那块区域</strong>（会跟着你的眼睛走的那个 box）。<br>
「上限」是网络能吃的最大输入（原来的环境变量 <code>AMDNR_XR_CROP</code>，本机配置里是 1280×720）；「横向/纵向」是<strong>相对上限的比例</strong>，<strong>1.00 = 正好等于上限</strong>，不需要缩放，最清晰。</div>
<div class="row"><label>上限宽度<span class="k">ceiling_width<br>网络输入的最大宽度，单位像素。这就是神经网络的工作分辨率</span></label>
  <input type="range" id="ceilw" min="256" max="1920" step="8">
  <input type="number" id="ceilw_n" min="256" max="1920" step="8"></div>
<div class="row"><label>上限高度<span class="k">ceiling_height<br>网络输入的最大高度，单位像素</span></label>
  <input type="range" id="ceilh" min="256" max="1080" step="8">
  <input type="number" id="ceilh_n" min="256" max="1080" step="8"></div>
<div class="warn">上限是<strong>硬边界</strong>：两张卡共用 256–1920×1080 这一组边界，超出去会被服务端夹回来（服务端夹，不是提示你）。<br>
它还是<strong>非线性</strong>的：1920×1080 每遍要跑的像素是 1280×720 的 <strong>2.25 倍</strong>，所以动它比动下面两个滑块贵得多——省性能优先缩下面，提清晰度优先抬这里再配 1.00。</div>
<div class="row"><label>横向宽度<span class="k">window_width<br>输入框宽度 = 上限宽度 × 这个比例。右边是它实际切出来的像素</span></label>
  <input type="range" id="winw" min="0.25" max="1.5" step="0.01">
  <span class="px" id="winw_px">—</span></div>
<div class="row"><label>纵向高度<span class="k">window_height<br>输入框高度 = 上限高度 × 这个比例。右边是它实际切出来的像素</span></label>
  <input type="range" id="winh" min="0.25" max="1.5" step="0.01">
  <span class="px" id="winh_px">—</span></div>
<div class="resline" id="winres">读一次实际分辨率…</div>
<div class="help">往<strong>小</strong>调 → 处理范围变小、网络输入跟着变小、省性能，但没被处理的边缘离视线中心更近，更容易被看到；往<strong>大</strong>调 → 覆盖更多画面，但超过 1.00 之后网络输入不再变大，只是硬撑，整体发软。</div>
<div class="row"><label>圆角<span class="k">roundness<br>把输入框从矩形（0）变成内切椭圆（1）。过渡带最不容易被眼睛抓到的形状</span></label>
  <input type="range" id="roundness" min="0" max="1" step="0.01">
  <input type="number" id="roundness_n" min="0" max="1" step="0.01"></div>
<div class="row"><label>过渡<span class="k">feather<br>边缘用多宽的一段从「处理过」淡到「原图」，按短边的比例算。太小会看到一圈硬边，太大会糊一圈</span></label>
  <input type="range" id="feather" min="0" max="0.5" step="0.005">
  <input type="number" id="feather_n" min="0" max="0.5" step="0.005"></div>
<div class="warn">上限 / 横向 / 纵向动的是<strong>输入框本身的大小</strong>，DLSS-NR 的特征要按新尺寸重建一次，所以<strong>每次会顿一下</strong>。圆角/过渡只是画面混合参数，随便拧，不重建、不卡。</div>

<h2>四、抗闪烁 · 改完立刻生效，不用重启</h2>
<div class="row"><label>抗闪烁强度<span class="k">anti_flicker<br>把模型每帧新生成的「修正量」按时间累积起来，专治静止画面上的噪点闪动。0 = 关；越大越稳，也越容易在快速移动的物体上留下拖影</span></label>
  <input type="range" id="antiflicker" min="0" max="1" step="0.05">
  <input type="number" id="antiflicker_n" min="0" max="1" step="0.05"></div>
<div class="row"><label>闪烁判定门槛<span class="k">anti_flicker_gate<br>修正量的变化超过这个值，就判定画面在动，直接用当前帧、不掺历史。调小 = 更不容易拖影但闪动更明显；调大 = 更稳但拖影更长</span></label>
  <input type="range" id="afgate" min="0.002" max="0.2" step="0.002">
  <input type="number" id="afgate_n" min="0.002" max="0.5" step="0.002"></div>
<div class="resline" id="afline">抗闪烁：读一次状态…</div>
<div class="help">「抗闪烁强度」是<strong>层自己</strong>的滤波器：把窗口里模型每帧新生成的那份「修正量」按时间累积，再用层自己的运动场对齐到当前帧，专压静止画面上的闪动。DLSS-NR 每一帧都是重置后重跑的（没有运动矢量喂给它），所以这就是这个后端唯一的时域稳定手段。代价是强度越高、门槛越大，快速移动的物体越容易出现拖影。下面那行读数来自层<strong>从历史纹理里真采样回来的数据</strong>，「历史平均叠了 N 帧」是最关键的一个数：刚打开时的第一帧是 1.00，之后必须涨到好几帧（上限 8）。只有真把历史读回来、真混合过，它才可能大于 1；若它一直停在 1.00，说明历史只写没读——那就是坏数据，别信这个滤波器。</div>

)HTML"
R"HTML(<h2>六、addon 配置桥 · 直接吃别人调好的风格</h2>
<div class="help">层按这个顺序找配置，把里面的画面控制读进来：<strong>启动时读一次，之后文件一改自动重读</strong>（约 1 秒）。<br>
探测顺序：<code>AMDNR_XR_ADDON_CONFIG</code>（文件或文件夹）→ 游戏 exe 旁的 <code>OptiScaler.ini</code> / <code>dlss5_style.ini</code> / <code>style.ini</code> → 本层 DLL 旁的同名文件 → 最后再读游戏 exe 旁的 <code>ReShade.ini</code>（只取其 <code>[RenoDX.DLSS5]</code> 段，ReShade 插件把同一套控制存在那里，后读所以它的话最大）。<br>
认得的键：OptiScaler/AMDNR 一边是 <code>LocalTone</code>、<code>LocalStructure</code>、<code>SkinStructure</code>、<code>AutoMask</code>、<code>Style</code>，强度认 <code>Intensity</code> / <code>TransferStrength</code> / <code>AmdDetailStrength</code>（本后端只有 <code>DLSSNR.Intensity</code> 一个强度旋钮，这三种写法都归它）；ReShade 插件一边是 <code>NRLocalTone</code>、<code>NRLocalStructure</code>、<code>NRSkinStructure</code>、<code>NRAutoMask</code>、<code>NRStyle</code>、<code>NRIntensity</code>；短名 <code>tone/structure/skin/mask/style/intensity</code> 也认。值是 <code>auto</code> 的算「没调」，不动。<br>
<strong>忽略</strong> = 本后端没有同义控制的键：色彩轴的强度（<code>ColourStrength</code>、<code>NRColorStrength</code>、<code>AmdColourStrength</code> —— 本后端只有一个强度旋钮，把色彩轴折进去会静默改画面，所以不吃）、ReShade 的 <code>NRLook*</code> 外观滤镜与 <code>NRPass2/3/4*</code> 逐遍项、以及 Passes / WorkingScale / Preset 这类建网络时才读的键。<br>
模型强度类的值读进来和手动拖滑块一样，会让特征重建一次（顿一下）。<br>
<code>[DlssNr] StyleSlot1..3</code> 是 AMDNR 菜单存的整套外观（<code>LocalTone=1.2;LocalStructure=1.05;…</code> 这种串）：文件里没记「当前用哪个槽」，所以要用 <code>AMDNR_XR_ADDON_SLOT=1/2/3</code> 点名，不点名就不动它们。</div>
<div class="resline" id="addonline">addon 配置：读一次状态…</div>
<div class="help">别人分享的风格串可以直接粘在这里应用（<strong>下一帧生效，不写任何文件</strong>）：</div>
<textarea id="addontext" spellcheck="false" style="background:#1d2026;border:1px solid #333842;color:#e6e8ec;border-radius:5px;padding:6px 8px;width:100%;box-sizing:border-box;height:66px;resize:vertical;font:12px Consolas,monospace" placeholder="NRLocalTone=1.2;NRLocalStructure=1.05;NRSkinStructure=-1;NRAutoMask=true"></textarea>
<div style="margin-top:8px"><button class="mini" id="addonapply">应用这段风格</button></div>
)HTML"
R"HTML(<div id="foot">面板把值直接写进游戏进程里的参数块，下一帧就用新值。
服务只绑在 <code>127.0.0.1</code>，外面连不进来。<br>
原来的「明暗 / 结构 / 皮肤 / 其他 / 风格」五根滑块在本后端上没有接线，已经撤掉；其中能对上 DLSS-NR 参数的三个已并入上面的第二组。
<span id="err"></span></div>

)HTML"
R"HTML(<script>
const ids = ["intensity","tone","structure","skin","antiflicker","afgate",
             "ceilw","ceilh","winw","winh","roundness","feather"];
let dragging = false;
let enabled = true;

// Escapes one value for innerHTML. The add-on path is a file path from a machine this page knows
// nothing about; a `<` in it would otherwise be read as markup.
function esc(t){
  return String(t).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
}

function show(s){
  enabled = !!s.enabled;
  const mb = document.getElementById("mbtn"), ms = document.getElementById("mstate");
  mb.textContent = enabled ? "降噪：开着（点一下关掉）" : "降噪：关着（点一下打开）";
  document.getElementById("master").className = "big" + (enabled ? " on" : "");
  ms.textContent = enabled ? "画面正在被降噪处理" : "画面原样输出，不花性能";
  for (const id of ids){
    const el = document.getElementById(id);
    if (!el || el === document.activeElement) continue;
    const v = s[id];
    el.value = v;
    const n = document.getElementById(id + "_n");
    if (n && n !== document.activeElement) n.value = v;
  }
  // The two selects are not sliders and are not in `ids`: a value with no matching option would leave
  // the element blank, so each is set from the value the layer reports instead.
  const styleSel = document.getElementById("style");
  if (styleSel && styleSel !== document.activeElement) styleSel.value = String(Math.round(s.style));
  const maskSel = document.getElementById("mask");
  if (maskSel && maskSel !== document.activeElement) maskSel.value = s.mask ? "1" : "0";
  // The anti-flicker filter's own state. `afframes`/`afresets` are the layer's counters, and
  // `afvalid`/`afmag`/`afdepth` come from a box of the history texture read back and decoded on the
  // CPU -- the only claim about the history that is measured rather than assumed: a filter whose
  // history is never written, never read or holding nothing reads as zero here.
  const afl = document.getElementById("afline");
  if (afl){
    if (!s.antiflicker){
      afl.innerHTML = "抗闪烁：<b>关</b> —— 模型的修正量每帧重算，静止画面上的闪动不会被压制。";
    } else {
      const sample = s.afframe
        ? ("第 " + s.afframe + " 帧（滤波器自己的帧数）采样的历史里，<b>" + (s.afvalid * 100).toFixed(1) +
           "%</b> 的像素带着修正量，平均 |修正| <b>" + s.afmag.toFixed(5) +
           "</b>，历史平均叠了 <b>" + (s.afdepth || 0).toFixed(2) + "</b> 帧")
        : "还没采到样本（开启后跑几帧会自动采一次）";
      afl.innerHTML = "抗闪烁：<b>开</b>（强度 " + s.antiflicker.toFixed(2) + "，门槛 " +
                      s.afgate.toFixed(3) + "） · 已累积 <b>" + (s.afframes || 0) +
                      "</b> 帧 · 历史重置 <b>" + (s.afresets || 0) + "</b> 次 · " + sample;
    }
  }
  // The window sliders are dragged as a fraction but read out in pixels, because the fraction is not
  // something a reader can judge and the pixels are. These come from the layer, which is the only
  // place that knows the ceiling and the eye buffer the window is snapped and clamped against.
  const dim = (w, h) => (w && h) ? (w + "×" + h) : "—";
  const pw = document.getElementById("winw_px"), ph = document.getElementById("winh_px");
  if (pw) pw.textContent = s.boxw ? (s.boxw + " px") : "—";
  if (ph) ph.textContent = s.boxh ? (s.boxh + " px") : "—";
  const res = document.getElementById("winres");
  if (res){
    if (!s.boxw || !s.boxh){
      // "The layer has never cut a window" and "the window came out empty" are the same two dashes
      // otherwise, and the two need different things done about them.
      res.innerHTML = "层还没切过输入框 —— 游戏还没走到 3D 画面，或者降噪这一帧没跑。" +
                      "上限现在设的是 <b>" + dim(s.ceilw, s.ceilh) + "</b> px。";
    } else {
      res.innerHTML = "输入框 <b>" + dim(s.boxw, s.boxh) + "</b> px" +
                      " · 网络输入 <b>" + dim(s.netw, s.neth) + "</b> px" +
                      " · 上限 " + dim(s.ceilw, s.ceilh) + " px";
    }
  }
  // The add-on bridge's readout. `addonpath` is the file the layer picked up, empty when none was
  // found; the counts are from the last read, and `addonpaste` says whether that read was the file
  // or the box below. A file path is the one string on this page the layer did not write, so it is
  // escaped rather than trusted.
  const al = document.getElementById("addonline");
  if (al){
    if (s.addonpaste){
      al.innerHTML = "addon 配置桥：最近一次来自<b>粘贴框</b> —— 应用 <b>" + s.addonapplied +
                     "</b> 项，忽略 " + s.addonignored + " 项" +
                     (s.addonpath ? (" · 正在盯的文件：<code>" + esc(s.addonpath) + "</code>") : "") + "。";
    } else if (s.addonpath){
      al.innerHTML = "addon 配置桥：<code>" + esc(s.addonpath) + "</code> —— 应用 <b>" +
                     s.addonapplied + "</b> 项，忽略 " + s.addonignored +
                     " 项。文件一改（约 1 秒）自动重读。";
    } else {
      al.innerHTML = "addon 配置桥：<b>没找到配置文件</b> —— 把带 <code>[DlssNr]</code> 的 OptiScaler.ini 放到游戏 exe 旁，" +
                     "或用 <code>AMDNR_XR_ADDON_CONFIG</code> 指定路径；也可以直接把风格串粘在下面。";
    }
  }
}

async function pull(){
  try{
    const r = await fetch("/state", {cache:"no-store"});
    document.getElementById("err").textContent = "";
    if (!dragging) show(await r.json());
  }catch(e){
    document.getElementById("err").textContent = " · 连不上面板了（游戏还开着吗？）";
  }
}

async function push(key, value){
  try{ await fetch("/set?" + key + "=" + encodeURIComponent(value), {cache:"no-store"}); }
  catch(e){}
}

function wire(id){
  const el = document.getElementById(id), n = document.getElementById(id + "_n");
  if (el){
    el.addEventListener("pointerdown", () => dragging = true);
    el.addEventListener("pointerup", () => dragging = false);
    el.addEventListener("change", () => { dragging = false; push(id, el.value); if (n) n.value = el.value; });
  }
  if (n){
    n.addEventListener("change", () => { push(id, n.value); if (el) el.value = n.value; });
  }
}
ids.forEach(wire);

// The two selects: a change pushes the option's own value, and the read-back waits a little longer
// than a slider's -- a model-strength change rebuilds the feature, and a report read while that hitch
// is still running would show the old numbers.
["style","mask"].forEach(id => {
  const el = document.getElementById(id);
  if (el) el.addEventListener("change", () => { push(id, el.value); setTimeout(pull, 400); });
});

document.getElementById("mbtn").addEventListener("click", () => {
  push("enabled", enabled ? 0 : 1);
  setTimeout(pull, 120);
});
// Back to what both working integrations ship: intensity 1, tone 1, structure 1, skin -1 (follow the
// structure amount), the native mask on, style 1 -- and the window block back to the layer's own
// defaults, 1920x1080 at a cover of 1, the shipped rim.
document.getElementById("reset").addEventListener("click", async () => {
  for (const [k, v] of [["intensity",1],["tone",1],["structure",1],["skin",-1],["mask",1],["style",1],
                        ["ceilw",1920],["ceilh",1080],["winw",1],["winh",1],["roundness",0],["feather",0.15]]) {
    await push(k, v);
  }
  setTimeout(pull, 400);
});
// The add-on bridge's paste box. The whole snippet travels as one value, so a style lands in one
// write rather than one request per setting; the counts come back in the same /state the sliders use.
document.getElementById("addonapply").addEventListener("click", async () => {
  const box = document.getElementById("addontext");
  if (!box || !box.value.trim()) return;
  await push("addontext", box.value);
  setTimeout(pull, 400);
});

document.getElementById("link").textContent = location.origin;
pull();
setInterval(pull, 700);
</script></body></html>
)HTML";

// ---------------------------------------------------------------------------------------------
// The listener

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};
SOCKET g_listener = INVALID_SOCKET;
bool g_wsaStarted = false;

void SendAll(SOCKET client, const std::string& body, const char* status, const char* type) {
    char header[256]{};
    const int headerLength = snprintf(header,
                                      sizeof(header),
                                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                      status,
                                      type,
                                      body.size());
    if (send(client, header, headerLength, 0) <= 0) {
        return;
    }
    size_t sent = 0;
    while (sent < body.size()) {
        const int written = send(client, body.data() + sent, (int)(body.size() - sent), 0);
        if (written <= 0) {
            return;
        }
        sent += (size_t)written;
    }
}

void SendJson(SOCKET client) {
    SendAll(client, SettingsToJson(), "200 OK", "application/json");
}

void HandleConnection(SOCKET client) {
    // The panel only ever sends a GET line with a short query, so one read is enough and a slow
    // client cannot pin the thread. The paste box is the long end of that: a whole style string is a
    // few kilobytes once percent-encoded, so the buffer is sized for it rather than for the sliders.
    char request[8192]{};
    const int received = recv(client, request, sizeof(request) - 1, 0);
    if (received <= 0) {
        return;
    }

    std::string line(request, (size_t)received);
    const size_t lineEnd = line.find("\r\n");
    if (lineEnd != std::string::npos) {
        line.resize(lineEnd);
    }

    if (line.compare(0, 4, "GET ") != 0) {
        SendAll(client, "only GET", "405 Method Not Allowed", "text/plain; charset=utf-8");
        return;
    }
    const size_t pathEnd = line.find(' ', 4);
    std::string target = line.substr(4, pathEnd == std::string::npos ? std::string::npos : pathEnd - 4);

    std::string query;
    const size_t question = target.find('?');
    if (question != std::string::npos) {
        query = target.substr(question + 1);
        target.resize(question);
    }

    if (target == "/" || target == "/index.html") {
        SendAll(client, std::string(kPanelHtml), "200 OK", "text/html; charset=utf-8");
        return;
    }
    if (target == "/state") {
        SendJson(client);
        return;
    }
    if (target == "/set") {
        ApplyQuery(query);
        SendJson(client);
        return;
    }

    SendAll(client, "not found", "404 Not Found", "text/plain; charset=utf-8");
}

void ServeLoop(SOCKET listener) {
    while (!g_stop.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
            continue;
        }

        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        HandleConnection(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    g_running.store(false);
}

} // namespace

void NrControlServerStart() {
    if (g_running.load() || g_listener != INVALID_SOCKET) {
        return;
    }

    // Before the socket on purpose: the config watcher belongs to the settings, not to the panel,
    // and a port already in use must not cost the hot reload.
    NrAddonBridgeStart();

    int port = 8787;
    char value[32]{};
    if (GetEnvironmentVariableA("AMDNR_XR_PANEL_PORT", value, (DWORD)sizeof(value)) > 0) {
        const int requested = atoi(value);
        if (requested > 0 && requested < 65536) {
            port = requested;
        }
    }

    if (!g_wsaStarted) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            LayerLog("panel: WSAStartup failed, the control panel stays off\n");
            return;
        }
        g_wsaStarted = true;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        LayerLog("panel: socket() failed (%d)\n", WSAGetLastError());
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    // Loopback only: the panel has no authentication and must not be reachable from anywhere else.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0) {
        LayerLog("panel: port %d busy (%d), the control panel stays off\n", port, WSAGetLastError());
        closesocket(listener);
        return;
    }
    if (listen(listener, 4) != 0) {
        LayerLog("panel: listen() failed (%d)\n", WSAGetLastError());
        closesocket(listener);
        return;
    }

    g_listener = listener;
    g_stop.store(false);
    g_running.store(true);
    g_thread = std::thread(ServeLoop, listener);
    LayerLog("panel: control panel on http://127.0.0.1:%d\n", port);
}

void NrControlServerStop() {
    NrAddonBridgeStop();
    g_stop.store(true);
    if (g_listener != INVALID_SOCKET) {
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running.store(false);
}

bool NrControlServerRunning() {
    return g_running.load();
}
