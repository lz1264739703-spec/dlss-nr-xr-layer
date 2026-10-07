// The in-process ABI between the ReShade depth bridge add-on and this OpenXR layer.
//
// Both sides live in hlvr.exe: ReShade is injected through its dxgi.dll proxy, and the layer is
// loaded by the OpenXR loader. Nothing has to cross a process boundary, so there is no shared
// memory, no handle duplication and no copy of the picture to arrange -- the add-on hands over the
// native ID3D11Texture2D* that the game's depth was written into and the layer copies it on the
// GPU while the texture is still alive.
//
// A struct rather than a growing argument list is what keeps this extensible: a new field is
// appended, and `size`/`version` tell the consumer whether it may read it. An add-on built against
// an older revision of this header keeps working against a newer layer and the other way round.

#pragma once

#include <stdint.h>

#define AMDNR_DEPTH_BRIDGE_VERSION 2u

// The layer exports AMDNR_DEPTH_SUPPLY_EXPORT from AMDNR_DEPTH_LAYER_MODULE. The add-on resolves
// both with GetModuleHandle/GetProcAddress rather than linking to the layer: the add-on may be
// loaded before the layer (the OpenXR loader creates the layer lazily), and linking would make the
// add-on fail to start in every process ReShade is injected into that is not a VR application.
#define AMDNR_DEPTH_LAYER_MODULE L"AmdnrXrLayer.dll"
#define AMDNR_DEPTH_SUPPLY_EXPORT "AmdnrXrSupplyDepth"

enum AmdnrDepthFlags {
    // Values are linear depth over 0..1 (0 = near plane, 1 = far plane), not raw hardware depth.
    // Reversed-Z has already been undone by the producer, so no convention flag is needed.
    AMDNR_DEPTH_LINEARISED = 1u << 0,
    // Only meaningful without AMDNR_DEPTH_LINEARISED: the raw values use a reversed-Z range.
    AMDNR_DEPTH_REVERSED_Z = 1u << 1,
};

typedef struct AmdnrDepthFrame {
    uint32_t size;        // sizeof(AmdnrDepthFrame), set by the producer
    uint32_t version;     // AMDNR_DEPTH_BRIDGE_VERSION
    uint32_t width;
    uint32_t height;
    uint32_t format;      // DXGI_FORMAT of the texture below
    uint32_t flags;       // AmdnrDepthFlags
    uint64_t texture;     // ID3D11Texture2D* the producer's shader wrote the depth into
    uint64_t frame_index; // producer-side counter; the layer uses it to spot a stale frame
    float near_z;
    float far_z;
    // Added in version 2. The size of the packed eye image the layer works in, both eyes side by
    // side. A depth buffer this layer finds is the application's own, at whatever internal
    // resolution it chose, and that is not the resolution the layer crops colour from -- but it is
    // the same view at the same aspect, so reducing it to this size puts a depth in exactly the
    // layout the layer's existing crop geometry already describes.
    uint32_t eye_width;
    uint32_t eye_height;
} AmdnrDepthFrame;

#ifdef __cplusplus
extern "C" {
#endif

// The layer defines AMDNR_DEPTH_BRIDGE_EXPORTS so that including this header declares the export
// rather than an import; the add-on sees a plain declaration and resolves it at runtime.
#if defined(AMDNR_DEPTH_BRIDGE_EXPORTS)
#define AMDNR_DEPTH_BRIDGE_API __declspec(dllexport)
#else
#define AMDNR_DEPTH_BRIDGE_API
#endif

// Implemented by the layer. Never takes ownership of `frame` or of the texture it names: the texture
// is only guaranteed to be alive until the producer's present returns, so the layer has to consume
// it (or copy it) before returning.
AMDNR_DEPTH_BRIDGE_API void AmdnrXrSupplyDepth(const AmdnrDepthFrame* frame);

// How a producer calls it: the add-on resolves the export out of the layer with GetProcAddress
// rather than linking against it, so this is the type that pointer is cast to.
typedef void (*AmdnrXrSupplyDepthFn)(const AmdnrDepthFrame* frame);

#ifdef __cplusplus
}
#endif
