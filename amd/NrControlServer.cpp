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

// Written by the layer, read by the panel. A second mutex rather than the settings one because the
// two travel in opposite directions and there is no read of one that should wait on a write of the
// other: the layer takes this every frame, and the panel's own writes must never queue behind it.
std::mutex g_reportMutex;
NrWindowReport g_report;

// Same direction and same reason as the window report: written by the layer's status sample, read by
// the panel. Its own mutex, so a status write never waits on a window write.
std::mutex g_runtimeMutex;
NrRuntimeReport g_runtime;

// Whether the runtime will pad the network to the input's own size, as the config files stood when the
// session started (or when the panel last wrote the key). Read once rather than on every /state: the
// files include a 45 kB default-config.txt, and the panel polls twice a second.
unsigned g_freeResCached = 0;

// The style the runtime will use at the next launch, as the config files stood when the session
// started (or when the panel last wrote the key). Cached for the same reason `g_freeResCached` is:
// the merge walks a 45 kB default-config.txt, and the panel polls twice a second. -1 means no file
// sets it, which is a different answer from any of the three values it can hold.
int g_styleCached = -1;

// The runtime's own hot-reloaded keys, as the three config layers leave them when the session starts
// (or when the panel last wrote one). Cached for the same reason `g_freeResCached` is -- the merge
// walks a 45 kB default-config.txt and the panel polls twice a second. -1, or the empty string, means
// no layer sets the key, which is a different answer from any value it can hold.
//
// These live here rather than in NrSettings because the layer never applies them: they go from the
// file straight into the runtime, which re-reads them by itself (native_hot_flags.h, 0.38+). The panel
// reads them back from this cache so it can show what the runtime is about to use -- the same reason
// the style is cached, and the same reason the frame's own control fields are not enough: the runtime
// ignores those on this build.
struct RuntimeHotKeys {
    std::string strength; // "auto", or "transfer,colour"
    int notice = -1;      // 0/1/2
    int showFps = -1;     // 0/1
    int predict = -1;     // 0/1
    int skin = -1;        // 0/1
};
RuntimeHotKeys g_hotKeys;

float ClampFinite(float value, float low, float high) {
    if (!std::isfinite(value)) {
        return low;
    }
    return std::clamp(value, low, high);
}

} // namespace

NrSettings NrSettingsGet() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void NrSettingsSet(const NrSettings& settings) {
    NrSettings clamped;
    clamped.enabled = settings.enabled;
    clamped.transferStrength = ClampFinite(settings.transferStrength, 0.f, 1.f);
    clamped.colorStrength = ClampFinite(settings.colorStrength, 0.f, 1.f);
    clamped.modelScale = ClampFinite(settings.modelScale, 0.25f, 1.f);
    clamped.passes = std::clamp(settings.passes, 1u, 3u);
    clamped.debugView = std::min(settings.debugView, 4u);
    // The anti-flicker filter's own bounds. Strength is a share of the history, so it stops at 1;
    // the gate is a distance in linear light -- an upper end of a half would smooth across whole
    // scene changes (that is not stability, it is a trail), and the lower end sits just off zero so
    // the shader's division never sees an absurd gate.
    clamped.antiFlicker = ClampFinite(settings.antiFlicker, 0.f, 1.f);
    clamped.antiFlickerGate = ClampFinite(settings.antiFlickerGate, 0.001f, 0.5f);
    // The ceiling, unlike everything around it, is clamped against a hard limit rather than a taste
    // one: the runtime refuses an input past its own maximum instead of scaling it, so a value above
    // kNrCeilingMax* would not be a slow frame but no frame at all.
    clamped.ceilingWidth = std::clamp(settings.ceilingWidth, kNrCeilingMin, kNrCeilingMaxWidth);
    clamped.ceilingHeight = std::clamp(settings.ceilingHeight, kNrCeilingMin, kNrCeilingMaxHeight);
    // Quarter of the ceiling is already a tiny window, and past half again larger the network is
    // being asked for detail it cannot carry -- both ends are the point where the slider stops doing
    // anything an eye can see, not safety limits.
    clamped.windowWidth = ClampFinite(settings.windowWidth, 0.25f, 1.5f);
    clamped.windowHeight = ClampFinite(settings.windowHeight, 0.25f, 1.5f);
    clamped.feather = ClampFinite(settings.feather, 0.f, 0.5f);
    clamped.roundness = ClampFinite(settings.roundness, 0.f, 1.f);
    // A metre is already a room away, so anything above it can only be a mistake: larger than that
    // and the parallax is too small to see on anything, which reads as "the slider does nothing".
    //
    // The lower end is clamped further than the slider suggests for the same reason in the other
    // direction: the parallax is one over this, so a millimetre-scale near plane multiplies the head's
    // travel by hundreds, and a motion field that large does not merely look wrong -- it writes the
    // temporal history into a state it cannot warp its way out of. Below a centimetre is not a near
    // plane a title would project with.
    clamped.motionNear =
        settings.motionNear <= 0.f ? 0.f : ClampFinite(settings.motionNear, 0.01f, 1.f);
    clamped.historyReset = settings.historyReset;
    clamped.autoQuality = settings.autoQuality;
    // The menu range, which is OptiScaler's own for these three -- the panel offers exactly this, so
    // the guard is the menu's bound rather than the runtime's wider [-1, 4] acceptance range.
    clamped.controlTone = ClampFinite(settings.controlTone, 0.f, 2.f);
    clamped.controlStructure = ClampFinite(settings.controlStructure, 0.f, 2.f);
    clamped.controlSkin = ClampFinite(settings.controlSkin, -1.f, 2.f);
    clamped.controlMask = settings.controlMask;
    clamped.controlStyle = ClampFinite(settings.controlStyle, -1.f, 4.f);
    clamped.stylePinned = settings.stylePinned;

    std::lock_guard<std::mutex> lock(g_settingsMutex);
    g_settings = clamped;
}

void NrWindowReportSet(const NrWindowReport& report) {
    std::lock_guard<std::mutex> lock(g_reportMutex);
    g_report = report;
}

NrWindowReport NrWindowReportGet() {
    std::lock_guard<std::mutex> lock(g_reportMutex);
    return g_report;
}

void NrRuntimeReportSet(const NrRuntimeReport& report) {
    std::lock_guard<std::mutex> lock(g_runtimeMutex);
    g_runtime = report;
}

NrRuntimeReport NrRuntimeReportGet() {
    std::lock_guard<std::mutex> lock(g_runtimeMutex);
    return g_runtime;
}

// ---------------------------------------------------------------------------------------------
// The pass count is the one setting here that is not a frame value
//
// Everything else in the block is read again on the frame after it is written. The number of passes is
// not: the runtime reads DLSS5_MULTI_PASS once, when it builds its network, and it reads it from the
// same three config files the add-on uses -- default-config.txt, then custom-config.txt, then
// native-game-flags.txt, with a real environment variable above all three. So the panel edits that
// file rather than a frame field nothing looks at, and says so in the page: a control that only takes
// effect after a relaunch, with nothing on screen saying which case the reader is in, is worse than
// one that plainly needs the restart. The add-on's own F9 hotkey does the same thing from its side.
//
// The write keeps whatever is already in the file -- its BOM, its line endings, its comments and every
// line that is not this key. A file without the key gets the line appended, which is what creates
// custom-config.txt on a machine that has never had one.

namespace {

// The layer's own folder, which is also the runtime's: the two travel as a pair, and it is the folder
// the runtime resolves DLSS5-AMD against.
std::wstring LayerDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LayerDirectory),
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

bool WriteTextFile(const std::wstring& path, const std::string& data) {
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_WRITE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = data.empty() ||
                    (WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr) &&
                     written == data.size());
    CloseHandle(file);
    return ok;
}

// The folder the runtime reads: DLSS5-AMD next to the DLL, else the DLL's own folder, else up to four
// parents of it -- the same order, and the same "first folder holding any of the three files wins"
// rule, so the panel and the runtime cannot disagree about which file is live.
std::wstring FindConfigDirectory() {
    std::wstring directory = LayerDirectory();
    if (directory.empty()) {
        return directory;
    }
    const std::wstring start = directory;
    std::vector<std::wstring> candidates{start + L"\\DLSS5-AMD"};
    for (int up = 0; up < 5; ++up) {
        candidates.push_back(directory);
        const size_t cut = directory.find_last_of(L"\\/");
        if (cut == std::wstring::npos) {
            break;
        }
        directory.resize(cut);
    }
    for (const std::wstring& candidate : candidates) {
        if (FileIsPresent(candidate + L"\\default-config.txt") ||
            FileIsPresent(candidate + L"\\custom-config.txt") ||
            FileIsPresent(candidate + L"\\native-game-flags.txt")) {
            return candidate;
        }
    }
    return start + L"\\DLSS5-AMD";
}

// The last assignment of `key` in one file's text. The documented merge rule is "the last line wins",
// so this deliberately does not stop at the first match.
bool ConfigValue(const std::string& text, const char* key, std::string& value) {
    const std::string wanted = std::string(key) + "=";
    bool found = false;
    size_t index = 0;
    // A file written by an editor that marks UTF-8 carries a BOM, and the BOM is part of the first
    // line's bytes: without skipping it, a key that happens to sit on line 1 would never match, and a
    // miss here is silent -- the panel would simply show the wrong number.
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        index = 3;
    }
    while (index <= text.size()) {
        const size_t next = text.find('\n', index);
        const size_t end = next == std::string::npos ? text.size() : next;
        std::string line = text.substr(index, end - index);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t begin = line.find_first_not_of(" \t");
        if (begin != std::string::npos && line.compare(begin, wanted.size(), wanted) == 0) {
            value = line.substr(begin + wanted.size());
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            found = true;
        }
        if (next == std::string::npos) {
            break;
        }
        index = next + 1;
    }
    return found;
}

// The value of one key as the three config layers leave it: default-config.txt, then
// custom-config.txt, then native-game-flags.txt, a later file winning. Empty when no layer sets it.
//
// This is the same merge, over the same three files, that the runtime performs -- which is the whole
// point: ever since the runtime grew its own hot reload (native_hot_flags.h, 0.38) the panel has to
// show what the runtime is about to apply rather than what the layer would have picked, and those two
// are no longer the same source. A real environment variable outranks all three files and is
// deliberately not part of this; the callers that can be overridden say so.
std::string LayerConfigValue(const char* key) {
    const std::wstring directory = FindConfigDirectory();
    if (directory.empty()) {
        return std::string();
    }
    static const wchar_t* const kLayers[] = {
        L"default-config.txt", L"custom-config.txt", L"native-game-flags.txt"};
    std::string value;
    for (const wchar_t* layer : kLayers) {
        std::string text;
        std::string found;
        if (ReadTextFile(directory + L"\\" + layer, text) && ConfigValue(text, key, found)) {
            value = found;
        }
    }
    return value;
}

// One of those values as an integer, or -1 when no layer sets the key or the text is not a number in
// range. -1 rather than 0 because 0 is a real setting for several of these keys, and "unset" has to
// stay tellable apart from it.
int IntFromConfigFiles(const char* key, int low, int high) {
    const std::string text = LayerConfigValue(key);
    if (text.empty()) {
        return -1;
    }
    const int parsed = atoi(text.c_str());
    return (parsed >= low && parsed <= high) ? parsed : -1;
}

// Which pass count the runtime will use. 0 means no file sets it.
unsigned PassesFromConfigFiles() {
    const int passes = IntFromConfigFiles("DLSS5_MULTI_PASS", 1, 3);
    return passes < 0 ? 0u : (unsigned)passes;
}

// Which NVIDIA style the runtime will use at the next launch: 0, 1 or 2, or -1 when no file sets it.
//
// 0 is a value of its own here -- NVIDIA's default -- so unlike the pass count this cannot spell
// "unset" as 0, and the caller has to be able to tell the two apart.
int StyleFromConfigFiles() {
    return IntFromConfigFiles("DLSS5_STYLE", 0, 2);
}

// Whether the runtime will run the network at the input's own size instead of snapping it up to the
// 720/900/1080 tiers. Merged across the three files the same way the pass count is, so a later file
// wins. Only the automatic tier depends on it, and it depends on it completely: with snapping on,
// shrinking the window below 720 rows buys nothing at all, because the runtime pads it straight back
// up to the tier -- which would make the panel's toggle a control that does nothing.
unsigned FreeResFromConfigFiles() {
    return IntFromConfigFiles("DLSS5_NETWORK_FREE_RES", 0, 1) > 0 ? 1u : 0u;
}

// Rewrites `key=value` in place. Comments, the BOM and the file's own line endings survive; a repeated
// key collapses into the single rewritten line, because only the last of them would have counted.
bool RewriteConfigKey(const std::wstring& path,
                      const char* key,
                      const std::string& value,
                      bool create) {
    std::string text;
    const bool existed = ReadTextFile(path, text);
    if (!existed && !create) {
        return false;
    }

    size_t offset = 0;
    std::string prefix;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        offset = 3;
        prefix.assign(text, 0, 3);
    }
    const std::string body = text.substr(offset);
    const std::string eol = body.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const bool trailingNewline = !body.empty() && body.back() == '\n';

    const std::string wanted = std::string(key) + "=";
    const std::string replacement = wanted + value;
    std::vector<std::string> lines;
    bool replaced = false;
    size_t index = 0;
    while (index < body.size()) {
        const size_t next = body.find('\n', index);
        const size_t end = next == std::string::npos ? body.size() : next;
        std::string line = body.substr(index, end - index);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t begin = line.find_first_not_of(" \t");
        if (begin != std::string::npos && line.compare(begin, wanted.size(), wanted) == 0) {
            if (!replaced) {
                lines.push_back(replacement);
                replaced = true;
            }
        } else {
            lines.push_back(line);
        }
        if (next == std::string::npos) {
            break;
        }
        index = next + 1;
    }
    if (!replaced) {
        lines.push_back(replacement);
    }

    std::string out = prefix;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out += eol;
        }
        out += lines[i];
    }
    if (trailingNewline || !existed) {
        out += eol;
    }
    return WriteTextFile(path, out);
}

// The write behind every control that lives in a config layer, plus the two cases that would
// otherwise leave the control looking broken: a native-game-flags.txt line that overrides the file
// just written, and a real environment variable, which outranks all three files.
//
// `when` is what the log says about the moment the value takes effect, because the keys behind this
// panel do not all arrive together. The runtime hot-reloads the multi-pass group within a second
// (native_hot_flags.h: it polls the three files' write times at most once a second, and the network
// applies the new value on its next frame); the style and the free-resolution flag are read once,
// when it builds the network, and do need the restart the page says they need. One log phrase cannot
// describe both, so the caller supplies it.
constexpr const char* kHotReloadWhen =
    "hot-reloaded by the runtime within a second, so it applies without a restart";
constexpr const char* kNextLaunchWhen =
    "read when the network is built, so it applies at the next launch";

bool PublishConfigKey(const char* key, const std::string& value, const char* when) {
    const std::wstring directory = FindConfigDirectory();
    if (directory.empty()) {
        LayerLog("panel: %s not written, the layer's own folder is unknown\n", key);
        return false;
    }

    const std::wstring custom = directory + L"\\custom-config.txt";
    if (!RewriteConfigKey(custom, key, value, true)) {
        LayerLog("panel: %s=%s could not be written to %s\n",
                 key,
                 value.c_str(),
                 Utf8(custom).c_str());
        return false;
    }
    LayerLog("panel: %s=%s written to %s (%s)\n", key, value.c_str(), Utf8(custom).c_str(), when);

    const std::wstring nativeGame = directory + L"\\native-game-flags.txt";
    std::string nativeText;
    std::string ignored;
    if (ReadTextFile(nativeGame, nativeText) && ConfigValue(nativeText, key, ignored)) {
        // It wins over custom-config.txt, so a line left there would silently undo the write above.
        RewriteConfigKey(nativeGame, key, value, false);
        LayerLog("panel: native-game-flags.txt also sets %s and overrides custom-config.txt, so that "
                 "line was rewritten too\n",
                 key);
    }

    char environment[64]{};
    if (GetEnvironmentVariableA(key, environment, (DWORD)sizeof(environment)) > 0) {
        // The runtime copies these files into its process environment once, when it builds its network,
        // so a variable found here is usually its own snapshot of an earlier file rather than something
        // the user set. Whether it decides the outcome depends on the key, and saying "overrides the
        // config files" for both named the wrong cause for the group that was actually broken: the pass
        // count never moved because this runtime read the reloaded value and dropped it, not because of
        // an environment variable. Report the two cases apart.
        const bool hotReloaded = std::strcmp(when, kHotReloadWhen) == 0;
        LayerLog("panel: %s is also an environment variable (%s); the runtime snapshots these files into its "
                 "own environment when it builds the network, so %s\n",
                 key,
                 environment,
                 hotReloaded ? "this key is still re-read from the file within a second and that value wins"
                             : "this key keeps the value from startup until the game restarts");
    }
    return true;
}

void PublishPasses(unsigned passes) {
    // The one key here the runtime re-reads while it runs, so no restart is involved. It is also the
    // reason the page's group heading changed: this used to be filed under "needs a restart" and it
    // never did.
    PublishConfigKey("DLSS5_MULTI_PASS", std::to_string(passes), kHotReloadWhen);
}

// The write behind the panel's style control. Unlike the pass count this one really is read once,
// when the network is built, so the page's "needs a restart" is the truth here.
void PublishStyle(int style) {
    if (PublishConfigKey("DLSS5_STYLE", std::to_string(style), kNextLaunchWhen)) {
        g_styleCached = style;
    }
}

// The automatic tier's dependency, written for the same reason and through the same file as the
// style: the runtime reads it when it builds the network, so it applies at the next launch. Turning
// the tier on without this would leave a toggle that trims a window the runtime immediately pads
// back up.
void PublishFreeRes(unsigned enabled) {
    if (PublishConfigKey("DLSS5_NETWORK_FREE_RES", std::to_string(enabled), kNextLaunchWhen)) {
        g_freeResCached = enabled;
    }
}

} // namespace

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

    if (GetEnvironmentVariableA("AMDNR_XR_STRENGTH", value, (DWORD)sizeof(value)) > 0) {
        const float strength = std::clamp((float)atof(value), 0.f, 1.f);
        settings.transferStrength = strength;
        settings.colorStrength = strength;
    }
    // The pass count has exactly one source: the config file the runtime reads. AMDNR_XR_PASSES used to
    // seed this, and it named a frame field the runtime never looks at, so it was removed rather than
    // left as a second way to ask for the same thing and get nothing. A real DLSS5_MULTI_PASS
    // environment variable is the one thing that outranks the files, so it is read last.
    if (const unsigned configured = PassesFromConfigFiles()) {
        settings.passes = configured;
    }
    if (GetEnvironmentVariableA("DLSS5_MULTI_PASS", value, (DWORD)sizeof(value)) > 0) {
        const int configured = atoi(value);
        if (configured >= 1 && configured <= 3) {
            settings.passes = (uint32_t)configured;
        }
    }
    // Read here rather than on demand: the panel asks for this twice a second, and the merge walks a
    // 45 kB default-config.txt each time it is asked.
    g_freeResCached = FreeResFromConfigFiles();
    // The style control, read the same way and for the same reason. The runtime's own value is what
    // the panel displays, because the layer never sees it otherwise: the style is read by the runtime
    // out of these files, and it is not one of the fields the frame carries.
    g_styleCached = StyleFromConfigFiles();
    if (GetEnvironmentVariableA("DLSS5_STYLE", value, (DWORD)sizeof(value)) > 0) {
        const int configured = atoi(value);
        if (configured >= 0 && configured <= 2) {
            g_styleCached = configured;
        }
    }
    // The multi-pass group, which the runtime re-reads by itself while the title runs. Read through
    // the same merge so the panel shows the value the runtime is about to apply, and through the
    // environment first, because a real variable outranks all three files for the runtime too --
    // showing the file's value there would be showing a value nothing is using.
    auto hotInt = [](const char* key, int low, int high) {
        char text[32]{};
        if (GetEnvironmentVariableA(key, text, (DWORD)sizeof(text)) > 0) {
            const int parsed = atoi(text);
            if (parsed >= low && parsed <= high) {
                return parsed;
            }
        }
        return IntFromConfigFiles(key, low, high);
    };
    g_hotKeys.strength = LayerConfigValue("DLSS5_STRENGTH");
    if (GetEnvironmentVariableA("DLSS5_STRENGTH", value, (DWORD)sizeof(value)) > 0) {
        g_hotKeys.strength = value;
    }
    g_hotKeys.notice = hotInt("DLSS5_NOTICE", 0, 2);
    g_hotKeys.showFps = hotInt("DLSS5_SHOW_FPS", 0, 1);
    g_hotKeys.predict = hotInt("DLSS5_MULTI_PASS_PREDICT", 0, 1);
    g_hotKeys.skin = hotInt("DLSS5_MULTI_PASS_SKIN_PROTECT", 0, 1);
    if (GetEnvironmentVariableA("AMDNR_XR_DEBUG_VIEW", value, (DWORD)sizeof(value)) > 0) {
        settings.debugView = (uint32_t)std::clamp(atoi(value), 0, 4);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_ANTIFLICKER", value, (DWORD)sizeof(value)) > 0) {
        settings.antiFlicker = std::clamp((float)atof(value), 0.f, 1.f);
    }
    if (GetEnvironmentVariableA("AMDNR_XR_ANTIFLICKER_GATE", value, (DWORD)sizeof(value)) > 0) {
        settings.antiFlickerGate = (float)atof(value);
    }
    // The ceiling the layer used to take from AMDNR_XR_CROP. It still does, and it is still the
    // largest input the runtime takes -- what is new is that the panel can move it while the title
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
    if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_NEAR", value, (DWORD)sizeof(value)) > 0) {
        // The name the depth reduction linearises with, and the near plane this layer's own parallax
        // needs. Read first so AMDNR_XR_MOTION_NEAR, if it is also set, still has the last word.
        settings.motionNear = std::max(0.f, (float)atof(value));
    }
    if (GetEnvironmentVariableA("AMDNR_XR_MOTION_NEAR", value, (DWORD)sizeof(value)) > 0) {
        settings.motionNear = std::max(0.f, (float)atof(value));
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
// The host these controls were named after keeps its look in a config file: OptiScaler.ini's
// [DlssNr] section, and in AMDNR's fork three style slots -- `key=value;...` strings of the menu's
// own settings, stored under StyleSlot1..3 (see the note in that fork's changelog: sixty-odd typed
// settings under their ini keys). A tuned style therefore already exists on a machine that runs
// either of them, so this probe reads that file and writes the look controls into the same block the
// panel writes -- a style travels from the desktop to the layer as a file, and an edit to the file
// arrives while the title runs.
//
// The ReShade add-on is the other host the same scene uses (renodx-dlss5.addon64, hosted by ReShade
// with nvngx_dlssnr.dll beside it), and it keeps the same six controls in ReShade.ini's
// [RenoDX.DLSS5] section under NR-prefixed names. That file is read too, after the ini above, so one
// machine running both hosts gets both, and the add-on's own section has the last word.
//
// What it deliberately does not consume: the file's other keys. Passes, WorkingScale, Preset and
// the rest are read by the runtime once, when it builds its network; showing them on the panel
// while the running network keeps the old value is the lie the pass-count control already taught us
// to avoid. Intensity, AmdModelScale (an NR resolution, not this layer's model scale) and the RenoDX
// colour-composition keys (SkinDetail, SkinColour, EnvironmentDetail, EnvironmentColour, and the
// AmdCompose* pair) are absent for the nearer reason: this layer has no control with that meaning,
// and mapping one onto the closest thing would silently apply a value from a different scale.

namespace {

// The name each control travels under in an add-on, normalised (lowercased, separators dropped).
// The left column is what a file or a pasted slot string says; every entry is a name this layer can
// actually apply between frames, because a name it cannot apply would count as an applied setting
// and do nothing.
enum class AddonField { Tone, Structure, Skin, Mask, Style, Transfer, Colour };

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
    {"transferstrength", AddonField::Transfer},
    {"transfer", AddonField::Transfer},
    // AMDNR's AMD path keeps its own names for the same two runtime values: the fork's ini says it
    // outright -- "lmxxf: they are the runtime codec's own transfer / colour strengths", 0..1,
    // 1 = full model -- which is the pair this layer sends as transfer_strength / color_strength.
    // They are the keys an AMDNR user's Save Settings actually writes, so without them the file of
    // the host this fork's menu belongs to would carry its tone and structure and drop its
    // strength. The RenoDX-mode pair (AmdComposeDetail / AmdComposeColour) is deliberately absent:
    // it drives the host's own composition, runs to 2 and 4, and is only live with AmdComposition=1
    // -- a different mechanism on a different scale.
    {"amddetailstrength", AddonField::Transfer},
    {"colourstrength", AddonField::Colour},
    {"colorstrength", AddonField::Colour},
    {"amdcolourstrength", AddonField::Colour},
    {"amdcolorstrength", AddonField::Colour},
    {"colour", AddonField::Colour},
    {"color", AddonField::Colour},
    // The ReShade add-on's own names for the same six controls (renodx-dlss5.addon64, section
    // [RenoDX.DLSS5] in ReShade.ini): it drives the same NVIDIA runtime, so the model-look controls
    // are the same values under an NR prefix. Deliberately absent: its NRLook* family, which is the
    // add-on's own appearance filter and has no counterpart here; its NRPass2/3/4* overrides,
    // because this layer sends one control set and not one per pass; and NRPreset, which is the
    // network architecture rather than a look.
    {"nrlocaltone", AddonField::Tone},
    {"nrlocalstructure", AddonField::Structure},
    {"nrskinstructure", AddonField::Skin},
    {"nrautomask", AddonField::Mask},
    {"nrtransferstrength", AddonField::Transfer},
    {"nrcolorstrength", AddonField::Colour},
    {"nrcolourstrength", AddonField::Colour},
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
        case AddonField::Transfer:
            if (AddonNumber(value, number)) {
                settings.transferStrength = number;
                counts.applied++;
            } else {
                counts.ignored++;
            }
            return;
        case AddonField::Colour:
            if (AddonNumber(value, number)) {
                settings.colorStrength = number;
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
            // Pinned, like the panel's own style control: the pin is what keeps an explicit
            // "back to the shipped 1" on the wire instead of letting the runtime answer with its
            // own startup snapshot of DLSS5_STYLE.
            if (AddonStyle(value, number)) {
                settings.controlStyle = number;
                settings.stylePinned = true;
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

// Walks one text and applies what it recognises. Two shapes are accepted because the add-on writes
// two: an ini with a [DlssNr] section, and the bare `key=value;...` string its style slots store. A
// text with no [DlssNr] section at all is read whole -- that is what a slot string, a `style.ini` or
// a hand-written file looks like. `slot` selects a StyleSlotN out of the file (0 = none); the slot's
// own string is then applied through this same walker.
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
    // line's bytes: without skipping it, a [DlssNr] on line 1 would not be seen as a section.
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
            // Empty, or a comment -- both hosts comment with ';'.
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
    return FindAddonInDirectory(LayerDirectory());
}

// Every file a look may live in, in the order they are applied. Two hosts keep two kinds of file and
// one machine can have both -- this one does -- so neither wins by merely existing: the OptiScaler
// family's ini (or a style file) is read first, and ReShade.ini's [RenoDX.DLSS5] section -- where the
// ReShade add-on stores the same controls under NR-prefixed names -- last, so its names land ahead.
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
// unchanged file is not re-applied. The runtime's own hot reload watches the same pair.
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
        // The path stays: the file is still the one being watched, and saying so is what keeps the
        // readout from claiming no file was found just because the last write came from the box.
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
    const NrWindowReport r = NrWindowReportGet();
    const NrRuntimeReport rt = NrRuntimeReportGet();
    const NrAddonReport addon = NrAddonBridgeReport();
    const std::string addonPath = JsonEscaped(addon.path);
    // The four scales the frame carries, computed by the same translation the frame uses, so the page
    // can show what the four menu values below actually send -- the mapping is the part a reader
    // cannot guess, and an unshown mapping is one they would have to take on faith.
    const NrControlWire wire = NrControlWireOf(s);
    // The one field here that is a string, and it comes out of a file a person can edit, so it is
    // narrowed to the characters the key can legitimately hold before it goes into the JSON: a quote
    // or a backslash left in an edited config file would otherwise take the whole panel down, and the
    // runtime would reject the value anyway.
    std::string strength = g_hotKeys.strength;
    for (char& c : strength) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == ',' || c == '.' || c == '-' || c == '_';
        if (!plain) {
            c = '?';
        }
    }
    // 2560 rather than the ~1200 the numbers need: the watched config file's path is the one field
    // here whose length is not bounded by the layer, and a truncated JSON document would take the
    // whole panel down rather than one reading.
    char json[2560]{};
    snprintf(json,
             sizeof(json),
             "{\"enabled\":%d,\"transfer\":%.4f,\"color\":%.4f,\"scale\":%.4f,\"passes\":%u,"
             "\"debug\":%u,\"antiflicker\":%.4f,\"afgate\":%.4f,\"motionnear\":%.4f,"
             "\"ceilw\":%u,\"ceilh\":%u,"
             "\"winw\":%.4f,\"winh\":%.4f,\"feather\":%.4f,\"roundness\":%.4f,"
             "\"tone\":%.4f,\"structure\":%.4f,\"skin\":%.4f,\"mask\":%d,\"style\":%.4f,"
             "\"wtone\":%.4f,\"wstructure\":%.4f,\"wskin\":%.4f,\"wother\":%.4f,"
             "\"rtpasses\":%u,\"framems\":%.2f,\"quality\":%.4f,\"autoq\":%d,\"freeres\":%u,"
             // The anti-flicker probe, read back out of the filter's own history: what the layer
             // measured rather than what it was asked for, which is the only way the panel can show
             // that the history is written and consumed instead of quietly doing nothing.
             "\"afon\":%d,\"afframes\":%llu,\"afresets\":%llu,"
             "\"afvalid\":%.4f,\"afmag\":%.5f,\"afdepth\":%.2f,\"afframe\":%llu,"
             "\"rtstyle\":%d,\"rtstrength\":\"%s\",\"rtnotice\":%d,\"rtshowfps\":%d,"
             "\"rtpredict\":%d,\"rtskin\":%d,"
             // The add-on bridge: which config file is being watched, and what the last read of it
             // -- or the last paste -- actually wrote. `addonpaste` says which of the two it was,
             // because "came from the box" and "came from the file" need different things done.
             "\"addonpath\":\"%s\",\"addonapplied\":%d,\"addonignored\":%d,\"addonpaste\":%d,"
             "\"boxw\":%u,\"boxh\":%u,\"netw\":%u,\"neth\":%u}",
             s.enabled ? 1 : 0,
             s.transferStrength,
             s.colorStrength,
             s.modelScale,
             s.passes,
             s.debugView,
             s.antiFlicker,
             s.antiFlickerGate,
             s.motionNear,
             s.ceilingWidth,
             s.ceilingHeight,
             s.windowWidth,
             s.windowHeight,
             s.feather,
             s.roundness,
             s.controlTone,
             s.controlStructure,
             s.controlSkin,
             s.controlMask ? 1 : 0,
             s.controlStyle,
             wire.tone,
             wire.structure,
             wire.skin,
             wire.other,
             rt.passes,
             rt.frameMs,
             rt.qualityScale,
             s.autoQuality ? 1 : 0,
             g_freeResCached,
             rt.filterOn ? 1 : 0,
             rt.filterFrames,
             rt.filterResets,
             rt.filterValidShare,
             rt.filterMagnitude,
             rt.filterAccumulation,
             rt.filterSampleFrame,
             g_styleCached,
             strength.c_str(),
             g_hotKeys.notice,
             g_hotKeys.showFps,
             g_hotKeys.predict,
             g_hotKeys.skin,
             addonPath.c_str(),
             addon.applied,
             addon.ignored,
             addon.fromPaste ? 1 : 0,
             r.window[0],
             r.window[1],
             r.network[0],
             r.network[1]);
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
    } else if (key == "transfer") {
        s.transferStrength = number;
    } else if (key == "color") {
        s.colorStrength = number;
    } else if (key == "scale") {
        s.modelScale = number;
    } else if (key == "passes") {
        // The one pair here that is a file write rather than a frame write: the runtime reads the pass
        // count when it builds its network, so this lands in DLSS5-AMD\custom-config.txt and applies at
        // the next launch. The value is kept as well, because the page reads it back from /state.
        s.passes = (uint32_t)std::max(0L, integer);
        PublishPasses(std::clamp(s.passes, 1u, 3u));
    } else if (key == "debug") {
        s.debugView = (uint32_t)std::max(0L, integer);
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
    } else if (key == "autoq") {
        s.autoQuality = integer != 0;
        // The tier and the free-resolution padding are one feature: trimming the window below the 720
        // tier only costs less if the runtime stops snapping the input back up to it. Turning the tier
        // on therefore sets the key too, so the toggle does what its own help text says.
        if (s.autoQuality && g_freeResCached != 1u) {
            PublishFreeRes(1u);
        }
    } else if (key == "motionnear") {
        s.motionNear = number;
    } else if (key == "motionreset") {
        // Bumped rather than assigned: the panel always sends 1, and each session compares it with the
        // value it last saw, so one press reaches both of them exactly once.
        s.historyReset++;
    } else if (key == "tone") {
        s.controlTone = number;
    } else if (key == "structure") {
        s.controlStructure = number;
    } else if (key == "skin") {
        s.controlSkin = number;
    } else if (key == "mask") {
        // The switch the other three are read through, not a scale of its own: on, the structure
        // amount rides the model's own mask pair; off, the pair is negated and the amount goes
        // through the plain structure feature. See NrControlWireOf.
        s.controlMask = integer != 0;
    } else if (key == "style") {
        s.controlStyle = number;
    } else if (key == "rtstyle") {
        // Both halves of one style change, because the runtime has two ways to hear it. Writing the file is what a
        // fresh network reads when it builds (so the choice survives a restart); setting the per-frame control_style
        // is what retunes the network that is already running -- the runtime maps it onto the modules'
        // dlss5_style_feature constant between frames, which those kernels read at launch, so there is no restart and
        // no rebuild. The pin keeps the flag on the wire even for the shipped 1, so "back to 1" is heard too instead
        // of falling through to the runtime's own startup snapshot of DLSS5_STYLE.
        const int style = (int)std::clamp(integer, 0L, 2L);
        PublishStyle(style);
        s.controlStyle = (float)style;
        s.stylePinned = true;
    } else if (key == "rtstrength") {
        // Two forms, and both are written through as sent. "auto" is a value of its own here: the
        // runtime parses anything that is not a pair of numbers as "keep the strength the frame
        // carried", which is what the panel's own transfer/color sliders hand it -- so auto is not a
        // rejection, it is the setting that lets those two sliders be heard. Anything else has to be
        // the pair its sscanf expects, in the same 0..3 range, or it would be silently ignored by the
        // runtime, and a control that is silently ignored is worse than one that refuses.
        std::string text = value;
        if (text != "auto") {
            float transfer = 0.f;
            float colour = 0.f;
            if (sscanf_s(text.c_str(), "%f,%f", &transfer, &colour) != 2 || transfer < 0.f ||
                transfer > 3.f || colour < 0.f || colour > 3.f) {
                return;
            }
            char pair[32]{};
            snprintf(pair, sizeof(pair), "%g,%g", transfer, colour);
            text = pair;
        }
        PublishConfigKey("DLSS5_STRENGTH", text, kHotReloadWhen);
        g_hotKeys.strength = text;
    } else if (key == "rtnotice") {
        const int notice = (int)std::clamp(integer, 0L, 2L);
        PublishConfigKey("DLSS5_NOTICE", std::to_string(notice), kHotReloadWhen);
        g_hotKeys.notice = notice;
    } else if (key == "rtshowfps") {
        const int showFps = (int)std::clamp(integer, 0L, 1L);
        PublishConfigKey("DLSS5_SHOW_FPS", std::to_string(showFps), kHotReloadWhen);
        g_hotKeys.showFps = showFps;
    } else if (key == "rtpredict") {
        const int predict = (int)std::clamp(integer, 0L, 1L);
        PublishConfigKey("DLSS5_MULTI_PASS_PREDICT", std::to_string(predict), kHotReloadWhen);
        g_hotKeys.predict = predict;
    } else if (key == "rtskin") {
        const int skin = (int)std::clamp(integer, 0L, 1L);
        PublishConfigKey("DLSS5_MULTI_PASS_SKIN_PROTECT", std::to_string(skin), kHotReloadWhen);
        g_hotKeys.skin = skin;
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
textarea{background:#1d2026;border:1px solid #333842;color:#e6e8ec;border-radius:5px;
     padding:6px 8px;width:100%;box-sizing:border-box;height:66px;resize:vertical;
     font:12px Consolas,monospace}
.big{display:flex;align-items:center;gap:12px;margin:10px 0 4px}
.big button{font-size:15px;padding:9px 18px;border-radius:7px;border:1px solid #333842;
     background:#22262d;color:#e6e8ec;cursor:pointer}
.big.on button{background:#1f5c33;border-color:#2f7f49}
.big .state{color:#9aa3af}
button.mini{background:#22262d;border:1px solid #333842;color:#c3c9d2;border-radius:5px;
     padding:5px 10px;cursor:pointer;margin-right:8px}
.warn{background:#2a2113;border:1px solid #4a3a17;color:#e0c68a;border-radius:6px;
     padding:8px 11px;font-size:12px;margin:8px 0 2px}
#foot{margin-top:26px;color:#5f6773;font-size:12px}
#foot code{color:#8d96a3}
/* The two window sliders read out in pixels, so the third column is a plain number rather than an
   editable box: the fraction is what is dragged, the resolution is what it came out as. */
.px{font:12px Consolas,monospace;color:#9aa3af;text-align:right}
.resline{font:13px Consolas,monospace;color:#c3c9d2;background:#1d2026;border:1px solid #333842;
     border-radius:6px;padding:8px 11px;margin:8px 0 2px}
.resline b{color:#8ab4ff;font-weight:600}
</style></head><body>
)HTML"
// MSVC truncates a single string literal at 16380 bytes, and the panel is longer than that once the
// Chinese help text is counted at three bytes a character. Adjacent literals concatenate, so the page
// is written as one literal per section and each stays well inside the limit.
R"HTML(<h1>AMDNR 画面降噪 · 实时调节</h1>
<div class="sub">面板由游戏里的层自己提供 · <span id="link">.</span> · 标了「需重启」的要重启游戏，其余改完下一帧生效</div>

<h2>一、总开关</h2>
<div class="big" id="master"><button id="mbtn">…</button>
  <span class="state" id="mstate"></span></div>
<div class="help">开着 = 画面会被降噪处理（会吃性能）。关掉 = 画面原样输出，一点性能都不花。</div>

<h2>二、强度 · 改完立刻生效，不会卡</h2>
<div class="help">这一组每帧都会重新读一遍，随便拧。它们走的是<strong>层每帧传给运行时的字段</strong>，不经过配置文件。</div>
<div class="row"><label>细节强度<span class="k">transfer_strength<br>模型改动了画面上多少「细节」。1.0 是原厂，往小调效果更淡</span></label>
  <input type="range" id="transfer" min="0" max="1" step="0.01">
  <input type="number" id="transfer_n" min="0" max="1" step="0.01"></div>
<div class="row"><label>色彩强度<span class="k">color_strength<br>「颜色」那部分改动了多少。1.0 是原厂</span></label>
  <input type="range" id="color" min="0" max="1" step="0.01">
  <input type="number" id="color_n" min="0" max="1" step="0.01"></div>
<div class="row"><label>模型用量<span class="k">model_scale<br>模型整体用多少力。0.25 最轻，1.0 用满</span></label>
  <input type="range" id="scale" min="0.25" max="1" step="0.01">
  <input type="number" id="scale_n" min="0.25" max="1" step="0.01"></div>
<div class="row"><label>显示模式<span class="k">debug_view<br>平时用「正常画面」，其他几项是排查问题用的</span></label>
  <select id="debug">
    <option value="0">正常画面</option><option value="1">只看代理图</option>
    <option value="2">只看模型输出</option><option value="3">改动前后差异（放大 20 倍）</option>
    <option value="4">染色</option></select><span></span></div>
<div class="row"><label>抗闪烁强度<span class="k">anti_flicker<br>把模型每帧新生成的「修正量」按时间累积起来，专治静止画面上的噪点闪动。0 = 关；越大越稳，也越容易在快速移动的物体上留下拖影</span></label>
  <input type="range" id="antiflicker" min="0" max="1" step="0.05">
  <input type="number" id="antiflicker_n" min="0" max="1" step="0.05"></div>
<div class="row"><label>闪烁判定门槛<span class="k">anti_flicker_gate<br>修正量的变化超过这个值，就判定画面在动，直接用当前帧、不掺历史。调小 = 更不容易拖影但闪动更明显；调大 = 更稳但拖影更长</span></label>
  <input type="range" id="afgate" min="0.002" max="0.2" step="0.002">
  <input type="number" id="afgate_n" min="0.002" max="0.5" step="0.002"></div>
<div class="resline" id="afline">抗闪烁：读一次状态…</div>
<div class="row"><label>近处修正（运动矢量）<span class="k">motion_near<br>把深度换算成距离用的近平面，单位米。<strong>数值越小，近处的移动修正越强</strong>：近处拖影就调小，发飘、边缘被推开就调大</span></label>
  <input type="range" id="motionnear" min="0" max="0.2" step="0.01">
  <input type="number" id="motionnear_n" min="0" max="1" step="0.001"></div>
<div class="help"><code>motion_near</code>：0 = 关掉平移视差，只按头部旋转重投影（和加深度之前完全一样）。它只在运动矢量被真正使用时才有意义：时间路径开着（<code>AMDNR_XR_TEMPORAL</code>），或者上面「抗闪烁强度」不为 0 —— 运动矢量就是防闪烁滤波器把历史对齐到当前帧用的那张场。<br>
<strong>改这个值会自动清空时间历史</strong>，所以你拖完会看到窗口干净一下再重新积累，这是正常的。允许范围是 0 或 0.01–1.0，比 0.01 还小的值会被拒绝：视差是它的倒数，太小的近平面会把运动矢量放大几百倍，把历史写成回不来的样子。</div>
<div style="margin-top:10px">
  <button class="mini" id="mreset">重置时间历史（窗口卡住/错乱时按一下）</button>
</div>
<div class="help">「抗闪烁强度」是<strong>层自己</strong>的滤波器：把窗口里模型每帧新生成的那份「修正量」按时间累积，再用运动场对齐到当前帧，专压静止画面上的闪动。它<strong>和运行时的 output_smooth 无关</strong>（那个实现不在本机运行时里，面板上不再摆它），也不依赖时间路径 —— 运动场是层自己记的。代价是强度越高、门槛越大，快速移动的物体越容易出现拖影。下面那行读数来自层<strong>从历史纹理里真采样回来的数据</strong>，「历史平均叠了 N 帧」是最关键的一个数：刚打开时的第一帧是 1.00，之后必须涨到好几帧（上限 8）。只有真把历史读回来、真混合过，它才可能大于 1；若它一直停在 1.00，说明历史只写没读——那就是坏数据，别信这个滤波器。</div>
<div class="help" style="margin-top:10px">
  <label><input type="checkbox" id="autoq"> <strong>自动档位</strong>：开启后，层会在上面设的窗口大小<strong>之内</strong>自动降档，
  把网络耗时压回一帧的预算里（画质换帧率）。窗口滑块因此变成「上限」，层只往下取、不往上加。
  默认关 —— 关着的时候窗口就是你设的值，不做任何自动调整。<br>
  开启时层会同时把 <code>DLSS5_NETWORK_FREE_RES=1</code> 写进 <code>custom-config.txt</code>：不写的话，窗口缩到 720 档以下会被运行时<strong>吸回 720 档</strong>，降档白降。
  这一项运行时只在建网络时读一次，所以<strong>第一次开启要重启游戏</strong>；窗口档位本身下一帧就生效。</label>
</div>
<div class="resline" id="qline">…</div>

)HTML"
R"HTML(<h2>三、DLSS5 输入框 · 改完下一帧生效</h2>
<div class="help">这一组管的就是<strong>喂给神经网络的那块区域</strong>（会跟着你的眼睛走的那个 box）。<br>
上面「上限」是网络能吃的最大输入（原来的环境变量 <code>AMDNR_XR_CROP</code>）；下面「横向/纵向」是<strong>相对上限的比例</strong>，<strong>1.00 = 正好等于上限</strong>，不需要缩放，最清晰。</div>
<div class="row"><label>上限宽度<span class="k">ceiling_width<br>网络输入的最大宽度，单位像素。这就是 DLSS5 的工作分辨率</span></label>
  <input type="range" id="ceilw" min="256" max="1920" step="8">
  <input type="number" id="ceilw_n" min="256" max="1920" step="8"></div>
<div class="row"><label>上限高度<span class="k">ceiling_height<br>网络输入的最大高度，单位像素</span></label>
  <input type="range" id="ceilh" min="256" max="1080" step="8">
  <input type="number" id="ceilh_n" min="256" max="1080" step="8"></div>
<div class="warn">上限是<strong>硬上限</strong>：本机运行时最多接受 1920×1080，超过会被夹回来（服务端夹，不是提示你）。<br>
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
<div class="warn">横向/纵向是「重建网络输入纹理」的量，所以<strong>松开滑块才生效，而且每次会顿一下</strong>。调的时候尽量在静止画面里拧。<br>
圆角/过渡只是画面混合参数，随便拧，不重建、不卡。</div>

<h2>四、模型强度四项 + 风格 · 改完立刻生效，不用重启</h2>
<div class="warn"><strong>这一组和 OptiScaler / AMDNR 菜单里的「模型强度」是同一套名字、同一套范围、同一套默认值</strong>，
同一个数字在两边是同一个意思、同一个画面。它们走运行时的 <code>control_tone..control_style</code>，层每帧送过去：<br>
风格写进内核模块的 <code>dlss5_style_feature</code> 常量（内核每次启动读它）；另外四项各自缩放第一层
<strong>32×16 输入混合矩阵</strong>里对应特征的那一列 —— 那份矩阵是权重缓冲里唯一还保持 f32 的一段，
所以运行时在两帧之间改写设备上的那 2 KB 就行，<strong>不重建网络、不清历史、不卡顿、不用重启</strong>。<br>
<strong>1 = 出厂画面。</strong>四个菜单值送出去之前要换成四个特征缩放，换法和 OptiScaler 一致：<br>
· <strong>原生角色掩码开</strong>（默认）：结构强度作用在遮罩的「场景」那一半、角色结构作用在「皮肤」那一半，普通结构特征保持 1；<br>
· <strong>掩码关</strong>：结构强度改走普通结构特征，遮罩那一对取 -1（这就是两份 reference 里「mask off = (1, 结构, -1, -1)」那一组）。<br>
下面那行读数就是实际送出去的四个值。如果某一项拧了完全没反应，说明它对应的特征列号猜错了 —— 用环境变量
<code>DLSS5_PREFIX_COLUMNS=a,b,c,d</code> 换一组列号（0..15）再试，不用改代码。</div>
<div class="row"><label>色调强度<span class="k">LocalTone → control_tone<br>0–2，默认 1</span></label>
  <input type="range" id="tone" min="0" max="2" step="0.05">
  <input type="number" id="tone_n" min="0" max="2" step="0.05"></div>
<div class="row"><label>结构强度<span class="k">LocalStructure → control_other（掩码开）/ control_structure（掩码关）<br>0–2，默认 1</span></label>
  <input type="range" id="structure" min="0" max="2" step="0.05">
  <input type="number" id="structure_n" min="0" max="2" step="0.05"></div>
<div class="row"><label>角色结构<span class="k">SkinStructure → control_skin<br>−1–2，默认 −1；−1 = 跟随结构强度（模型自己的默认行为，不是「强度为零」）</span></label>
  <input type="range" id="skin" min="-1" max="2" step="0.05">
  <input type="number" id="skin_n" min="-1" max="2" step="0.05"></div>
<div class="row"><label>原生角色掩码<span class="k">AutoMask<br>模型自己的人物/场景区分。默认开；关掉就按「掩码关」那一组值送</span></label>
  <select id="mask"><option value="1">1 · 开（默认，出厂画面）</option><option value="0">0 · 关</option></select><span></span></div>
<div class="resline" id="wires">实际送给运行时的四个特征缩放：—</div>
<div class="row"><label>风格档位<span class="k">style → DLSS5_STYLE<br>改完立刻生效，同时写进配置文件</span></label>
  <select id="rtstyle">
    <option value="1">1 · 剑星原版风格（默认）</option>
    <option value="0">0 · NVIDIA 默认，最接近参考输出</option>
    <option value="2">2 · 第三种风格</option>
  </select><span class="px" id="rtstyle_px">—</span></div>
<div class="warn">风格这一项做两件事：把值写进 <code>DLSS5-AMD\custom-config.txt</code>（下次启动仍然是这个风格），
并<strong>每帧</strong>交给运行时（当前这一局立刻就能看到变化）。环境变量 <code>DLSS5_STYLE</code> 的优先级高于配置文件，
层会在日志里说明；但它盖不住每帧传过去的值。</div>

)HTML"
R"HTML(<h2>五、运行时实时档 · 改完 1 秒左右生效，不用重启</h2>
<div class="help">第二组是<strong>层每帧传给运行时</strong>的值；这一组是<strong>运行时自己从配置文件读</strong>的键，两回事。<br>
运行时自带热重载（0.38 版起）：它最多每秒扫一次那三个配置文件（<code>default-config.txt</code> → <code>custom-config.txt</code> → <code>native-game-flags.txt</code>），
发现文件的写时间变了，就<strong>只重读下面这几个键</strong>，并在<strong>下一帧网络之前</strong>换上 —— 所以这里改完不用重启，一秒左右就生效。
面板做的是把新值写进 <code>custom-config.txt</code>。</div>
<div class="row"><label>叠加遍数<span class="k">DLSS5_MULTI_PASS<br>每帧跑几遍模型。遍数越多越干净，也越吃性能</span></label>
  <input type="range" id="passes" min="1" max="3" step="1">
  <input type="number" id="passes_n" min="1" max="3" step="1"></div>
<div class="row"><label>第三遍用预测<span class="k">DLSS5_MULTI_PASS_PREDICT<br>1 = 3 遍实际只跑 2 次真实网络，第 3 遍由前一遍预测出来；0 = 老老实实跑满 3 次</span></label>
  <select id="rtpredict"><option value="1">1 · 开（默认，快）</option><option value="0">0 · 关（跑满 3 次）</option></select><span></span></div>
<div class="row"><label>肤色保护<span class="k">DLSS5_MULTI_PASS_SKIN_PROTECT<br>遍数大于 1 时对肤色区域做启发式混合，人物场景更稳</span></label>
  <select id="rtskin"><option value="1">1 · 开</option><option value="0">0 · 关（默认）</option></select><span></span></div>
<div class="row"><label>解码强度<span class="k">DLSS5_STRENGTH<br>运行时自己的强度开关，格式「细节,色彩」，各自 0–3。<br>auto（默认）时它不插手，第二组那两个滑块说了算；<br>设成具体数值后它会<strong>盖过</strong>那两个滑块</span></label>
  <select id="rtstrength">
    <option value="auto">auto · 不插手（听第二组滑块的）</option>
    <option value="0.5,0.5">0.5,0.5 · 比原厂淡</option>
    <option value="1,1">1,1 · 原厂强度</option>
    <option value="1.5,1.5">1.5,1.5 · 加强</option>
    <option value="2,2">2,2 · 更强</option>
    <option value="3,3">3,3 · 最强（上限）</option>
  </select><span class="px" id="rtstrength_px">—</span></div>
<div class="row"><label>屏幕状态行<span class="k">DLSS5_NOTICE<br>运行时自己画在画面上的那行字</span></label>
  <select id="rtnotice"><option value="2">2 · 显示（默认）</option><option value="1">1 · 只在换档时显示</option><option value="0">0 · 关掉</option></select><span></span></div>
<div class="row"><label>显示帧率<span class="k">DLSS5_SHOW_FPS<br>上面那行字里带不带 FPS</span></label>
  <select id="rtshowfps"><option value="1">1 · 显示（默认）</option><option value="0">0 · 不显示</option></select><span></span></div>
<div class="warn"><strong id="passstate">正在读运行时的实际遍数…</strong><br>
两种会让这一档失效的情况：一、<strong>同名环境变量</strong>（如 <code>DLSS5_MULTI_PASS</code>）的优先级高于所有配置文件，设了的话面板写文件不会生效，运行时用的是环境变量；二、<code>DLSS5_HOT_RELOAD=0</code> 会整个关掉热重载（默认 1），它本身只在启动时读一次，改它要重启。<br>
其余所有键（<code>DLSS5_NETWORK_FREE_RES</code>、跳过块、HIP 内核、网络高度……）都不在这一档，要重启；<code>DLSS5_STYLE</code> 是例外，走第四组那条实时通道。</div>

)HTML"
R"HTML(<h2>六、addon 配置桥 · 直接吃别人调好的风格</h2>
<div class="help">层按这个顺序找配置，把里面的画面控制读进来：<strong>启动时读一次，之后文件一改自动重读</strong>（约 1 秒）。<br>
探测顺序：<code>AMDNR_XR_ADDON_CONFIG</code>（文件或文件夹）→ 游戏 exe 旁的 <code>OptiScaler.ini</code> / <code>dlss5_style.ini</code> / <code>style.ini</code> → 本层 DLL 旁的同名文件 → 最后再读游戏 exe 旁的 <code>ReShade.ini</code>（只取其 <code>[RenoDX.DLSS5]</code> 段，ReShade 插件把同一套画面控制存在那里，后读所以它的话最大）。<br>
认得的键：OptiScaler/AMDNR 一边是 <code>LocalTone</code>、<code>LocalStructure</code>、<code>SkinStructure</code>、<code>AutoMask</code>、<code>Style</code>、<code>TransferStrength</code>、<code>ColourStrength</code>（AMD 路径的 <code>AmdDetailStrength</code>/<code>AmdColourStrength</code> 也算），ReShade 插件一边是 <code>NRLocalTone</code>、<code>NRLocalStructure</code>、<code>NRSkinStructure</code>、<code>NRAutoMask</code>、<code>NRStyle</code>、<code>NRTransferStrength</code>、<code>NRColorStrength</code>；两边的短名 <code>tone/structure/skin/mask/style/transfer/color</code> 也认。值是 <code>auto</code> 的算「没调」，不动。<br>
<strong>忽略</strong> = 这些键本层没有同义的帧内控制（Passes、WorkingScale、Preset、Intensity、<code>AmdModelScale</code>、ReShade 那边的 <code>NRLook*</code> 外观滤镜与 <code>NRPass2/3/4*</code> 逐遍项……）—— 摆出来只会骗人；数进忽略里是给你看「文件读到了，只是这些键不归这层管」。<br>
<code>[DlssNr] StyleSlot1..3</code> 是 AMDNR 菜单存的整套外观（<code>LocalTone=1.2;LocalStructure=1.05;…</code> 这种串）：文件里没记「当前用哪个槽」，所以要用 <code>AMDNR_XR_ADDON_SLOT=1/2/3</code> 点名，不点名就不动它们。</div>
<div class="resline" id="addonline">addon 配置：读一次状态…</div>
<div class="help">别人分享的风格串可以直接粘在这里应用（<strong>下一帧生效，不写任何文件</strong>）：</div>
<textarea id="addontext" spellcheck="false" placeholder="LocalTone=1.2;LocalStructure=1.05;SkinStructure=-1;AutoMask=true"></textarea>
<div style="margin-top:8px"><button class="mini" id="addonapply">应用这段风格</button></div>

<div id="foot">面板把值直接写进游戏进程里的参数块，下一帧就用新值；第四组只有风格两样都做（每帧送去，同时记进配置文件），第五组本就是配置文件（运行时每秒重读一次）。
服务只绑在 <code>127.0.0.1</code>，外面连不进来。<span id="err"></span></div>
)HTML"
R"HTML(<script>
const ids = ["transfer","color","scale","passes","debug","antiflicker","afgate","motionnear",
             "ceilw","ceilh","winw","winh","roundness","feather",
             "tone","structure","skin","mask"];
let dragging = false;
let enabled = true;

// Sets a <select> to a value, adding that value as an option when the panel does not already offer it.
// That only happens for the strength, whose file may hold a pair no option names: without the extra
// option the select would show a value the file does not hold, and the next change would write that
// wrong value back.
function setSelect(el, value){
  if (!el || el === document.activeElement) return;
  const text = String(value);
  if (!Array.prototype.some.call(el.options, o => o.value === text)){
    const live = el.querySelector("option[data-live]");
    if (live) live.remove();
    const o = document.createElement("option");
    o.value = text;
    o.textContent = text + " · 文件里现在是这个";
    o.setAttribute("data-live", "1");
    el.appendChild(o);
  }
  el.value = text;
}

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
  // What the four menu values above actually send. The translation is the layer's (and OptiScaler's),
  // not something the page works out for itself: showing it is what lets a reader line a setting up
  // against the runtime's own status line instead of trusting that the two agree.
  const wires = document.getElementById("wires");
  if (wires){
    const n2 = x => (Math.round(x * 100) / 100).toFixed(2);
    wires.innerHTML = "实际送给运行时的四个特征缩放：<b>tone " + n2(s.wtone) + " · structure " +
                      n2(s.wstructure) + " · skin " + n2(s.wskin) + " · other " + n2(s.wother) +
                      "</b>（掩码" + (s.mask ? "开" : "关") + "）";
  }
  // The anti-flicker filter's own state. `afframes` and `afresets` are the layer's counters, and
  // `afvalid`/`afmag` come from a box of the history texture read back and decoded on the CPU --
  // the only claim about the history that is measured rather than assumed: a filter whose history
  // is never written, never read or holding nothing reads as zero here.
  const afl = document.getElementById("afline");
  if (afl){
    if (!s.antiflicker){
      afl.innerHTML = "抗闪烁：<b>关</b> —— 模型的修正量每帧重算，静止画面上的闪动不会被压制。";
    } else {
      const sample = s.afframe
        ? ("第 " + s.afframe + " 帧采样的历史里，<b>" + (s.afvalid * 100).toFixed(1) +
           "%</b> 的像素带着修正量，平均 |修正| <b>" + s.afmag.toFixed(5) +
           "</b>，历史平均叠了 <b>" + (s.afdepth || 0).toFixed(2) + "</b> 帧")
        : "还没采到样本（开启后跑几帧会自动采一次）";
      afl.innerHTML = "抗闪烁：<b>开</b>（强度 " + s.antiflicker.toFixed(2) + "，门槛 " +
                      s.afgate.toFixed(3) + "） · 已累积 <b>" + (s.afframes || 0) +
                      "</b> 帧 · 历史重置 <b>" + (s.afresets || 0) + "</b> 次 · " + sample;
    }
  }
  // The pass count is hot-reloaded, so a mismatch between the two numbers is not "wait for the next
  // launch" -- it is the second or so the runtime takes to notice the file, or a sign that something
  // is overriding the files. Say it from the two numbers rather than claiming either.
  const ps = document.getElementById("passstate");
  if (ps){
    if (!s.rtpasses){
      ps.textContent = "运行时还没报告实际在跑的遍数（走到 3D 画面后才有）。";
    } else if (s.rtpasses === s.passes){
      ps.textContent = "面板写 " + s.passes + " 遍，运行时正在跑的也是 " + s.rtpasses + " 遍 —— 已经同步。";
    } else {
      ps.textContent = "面板写的是 " + s.passes + " 遍，运行时正在跑 " + s.rtpasses +
                       " 遍 —— 热重载一秒左右会追上；若一直不追上，多半是同名环境变量盖住了文件。";
    }
  }
  // The runtime's own hot-reloaded switches, which the layer only ever reads back out of the files.
  // Each shows the file's value when a layer sets it and the runtime's built-in default when none does:
  // -1 is the layer's "no file sets it", and the two are different states that must not render the
  // same. The defaults are named here rather than in the page, because they are the runtime's.
  const orDefault = (v, d) => (v >= 0 ? v : d);
  setSelect(document.getElementById("rtstrength"), s.rtstrength || "auto");
  setSelect(document.getElementById("rtnotice"), orDefault(s.rtnotice, 2));
  setSelect(document.getElementById("rtshowfps"), orDefault(s.rtshowfps, 1));
  setSelect(document.getElementById("rtpredict"), orDefault(s.rtpredict, 1));
  setSelect(document.getElementById("rtskin"), orDefault(s.rtskin, 0));
  const rtsp = document.getElementById("rtstrength_px");
  if (rtsp) rtsp.textContent = s.rtstrength ? ("文件：" + s.rtstrength) : "文件里没设";
  // The style control, which unlike the pass count the layer cannot even see: it goes from these
  // config files straight into the runtime, so the only value the panel can show is the one the files
  // hold. -1 is the layer's "no file sets it" and has no meaning to the runtime, which falls back to 1.
  const rs = document.getElementById("rtstyle");
  if (rs && rs !== document.activeElement) rs.value = (s.rtstyle >= 0) ? String(s.rtstyle) : "1";
  const rsp = document.getElementById("rtstyle_px");
  if (rsp){
    rsp.textContent = (s.rtstyle < 0) ? "文件里没设，用出厂档位 1" : ("文件里现在是 " + s.rtstyle);
  }
  // The automatic tier, which is a checkbox rather than one of the sliders above, so it is handled
  // here instead of through the generic `ids` loop. The scale shown is the one the layer actually
  // settled on, reported back from its frame loop -- the panel never computes it.
  const aq = document.getElementById("autoq");
  if (aq && aq !== document.activeElement) aq.checked = !!s.autoq;
  const ql = document.getElementById("qline");
  if (ql){
    if (!s.autoq){
      ql.innerHTML = "自动档位：<b>关</b> —— 窗口用的就是你设的值。";
    } else {
      const net = s.framems > 0 ? (s.framems.toFixed(1) + " ms") : "还没测到";
      const fr = s.freeres ? "<b>FREE_RES=1</b>（缩窗口才真的省 GPU）"
                           : "<b>FREE_RES=0</b>（缩窗口会被吸回 720 档，要重启一次才生效）";
      ql.innerHTML = "自动档位：<b>开</b> · 当前档位 <b>x" + (s.quality || 1).toFixed(2) + "</b>" +
                     " · 层实测每帧 NR 耗时 <b>" + net + "</b> · " + fr;
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
    el.addEventListener("change", () => { dragging = false; push(id, el.value); if (n) n.value = el.value; setTimeout(pull, 350); });
  }
  if (n){
    n.addEventListener("change", () => { push(id, n.value); if (el) el.value = n.value; setTimeout(pull, 350); });
  }
}
ids.forEach(wire);

document.getElementById("mbtn").addEventListener("click", () => {
  push("enabled", enabled ? 0 : 1);
  setTimeout(pull, 120);
});
document.getElementById("rtstyle").addEventListener("change", () => {
  push("rtstyle", document.getElementById("rtstyle").value);
  setTimeout(pull, 300);
});
// The runtime's own hot-reloaded switches. They are file writes rather than frame writes, and the
// runtime re-stats those files at most once a second, so the read-back waits longer than the 350 ms
// the frame fields use -- reading back too early would show the old value and read as a failed write.
["rtstrength","rtnotice","rtshowfps","rtpredict","rtskin"].forEach(id => {
  const el = document.getElementById(id);
  if (el) el.addEventListener("change", () => { push(id, el.value); setTimeout(pull, 1300); });
});
// Only a bump: the value itself means nothing, it is the change that drops the history.
document.getElementById("mreset").addEventListener("click", async () => {
  await push("motionreset", 1);
  setTimeout(pull, 120);
});
// The automatic tier, off by default. Turning it on hands the window size to the controller and
// turning it off hands it back to the sliders, with no state kept on either side.
document.getElementById("autoq").addEventListener("change", () => {
  push("autoq", document.getElementById("autoq").checked ? 1 : 0);
  setTimeout(pull, 200);
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
