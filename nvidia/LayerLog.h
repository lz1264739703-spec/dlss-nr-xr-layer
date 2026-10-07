// Shared logger: implemented in AmdnrXrLayer.cpp so every translation unit writes the same file.

#pragma once

#include <string>

void LayerLog(const char* fmt, ...);

// The folder the layer DLL was loaded from. The log and the pixel dumps live there: a per-user
// data folder is write protected in some sessions, which silently swallows all diagnostics.
std::string LayerDirectory();
