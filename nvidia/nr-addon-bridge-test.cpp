// Smoke test for the add-on bridge and the panel route that feeds it -- the NVIDIA build's copy.
//
// Compiles NrControlServer.cpp standalone -- the file is written to allow that, so the panel can be
// exercised without a game -- and runs the two paths that matter, in the order a session meets them:
//
//   1. the file read, synchronously, exactly where a real launch does it (NrSettingsInitFromEnvironment
//      runs the bridge's first read): the environment names an ini, and the report and the block are
//      printed for the caller to check;
//   2. the panel, over HTTP, from outside this process -- the paste box (`/set?addontext=`, percent
//      encoded, the only value here that is text rather than a number) and the watcher (the caller
//      rewrites the ini while this process sleeps). Both are read back through `/state`.
//
// The process stays up for twenty-five seconds so the caller can drive those two, then prints what
// the block holds afterwards. Phase two is the shell's script around it, not this file.
//
// LayerLog is stubbed to stdout: the panel's file logger belongs to the layer's process, and here the
// log lines are the test's output.

#include <cstdarg>
#include <cstdio>
#include <windows.h>

#include "LayerLog.h"

void LayerLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

#include "NrSettings.h"

namespace {

void PrintBlock(const char* title) {
    const NrAddonReport report = NrAddonBridgeReport();
    const NrSettings s = NrSettingsGet();
    std::printf("[%s] path=\"%s\" applied=%d ignored=%d fromPaste=%d\n",
                title,
                report.path.c_str(),
                report.applied,
                report.ignored,
                report.fromPaste ? 1 : 0);
    std::printf("[%s] intensity=%.3f tone=%.3f structure=%.3f skin=%.3f mask=%d style=%.3f\n",
                title,
                s.intensity,
                s.controlTone,
                s.controlStructure,
                s.controlSkin,
                s.controlMask ? 1 : 0,
                s.controlStyle);
    std::fflush(stdout);
}

} // namespace

int main() {
    std::printf("== phase 1: the file read ==\n");
    NrSettingsInitFromEnvironment();
    PrintBlock("file");

    std::printf("== phase 2: the server is up for 25 s ==\n");
    NrControlServerStart();
    for (int i = 0; i < 250; ++i) {
        Sleep(100);
    }
    NrControlServerStop();

    PrintBlock("end");
    return 0;
}