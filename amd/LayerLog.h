// Shared logger: implemented in AmdnrXrLayer.cpp so every translation unit writes the same file.

#pragma once

#include <d3d12.h>
#include <string>

void LayerLog(const char* fmt, ...);

// AMD's driver can remove the device without a TDR and without an installed debug layer to explain
// it, so the first call that notices the removal is the only evidence of where it happened. Every
// healthy call advances the checkpoint, so the log names both the first dead call and the last live
// one -- together they bracket whatever actually killed it. Reported once per process.
inline void NoteDeviceRemoved(ID3D12Device* device, const char* where) {
    if (device == nullptr) {
        return;
    }
    static bool reported = false;
    static const char* lastHealthy = "(layer entry)";
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (reason == S_OK) {
        lastHealthy = where;
        return;
    }
    if (reported) {
        return;
    }
    reported = true;
    LayerLog("device: removed (0x%08lX) first seen at %s; last healthy checkpoint: %s\n",
             (unsigned long)reason,
             where,
             lastHealthy);
}

// The folder the layer DLL was loaded from. The log and the pixel dumps live there: a per-user
// data folder is write protected in some sessions, which silently swallows all diagnostics.
std::string LayerDirectory();
