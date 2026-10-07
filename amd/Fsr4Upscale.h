// FidelityFX FSR 4 as the layer's output upscale.
//
// The layer's own output pass is D3D11: EASU resamples and RCAS sharpens, both as pixel shaders on
// the application's device. FSR 4 has no D3D11 entry point -- ffx_api is D3D12 only -- so it cannot
// be dropped into that pass. It runs instead on the D3D12 device the neural renderer already brings
// up, and the pixels cross between the two devices the same way the neural crops do: the view is
// copied into a shareable texture, upscaled there, and copied back into the image the compositor
// will sample.
//
// What is wired up here is the frame-to-frame half of the effect. FSR 4 is a temporal upscaler and
// wants jitter, motion vectors, exposure and depth:
//
//   jitter          supplied -- the layer offsets the frustum the application renders with and hands
//                   the same offset in here, so successive frames sample different sub-pixel
//                   positions.
//   motion vectors  supplied -- a shared texture the layer fills, from the head's motion and its
//                   captured depth, before this pass runs. It is built on the application's device
//                   rather than here: see the note in the layer's own build of it for why.
//   exposure        constant 1.0, which is what an eye buffer with no eye adaptation needs.
//   depth           supplied -- the layer's reduced depth, the same one the motion field measures its
//                   parallax against. It is offset by nothing and inverted, matching how the depth
//                   capture writes it.
//
// `reset` is raised only on the first frame of a geometry, and not on every frame: that is what lets
// the effect keep the history the jitter and the motion field are meant to fill.
//
// The ffx headers stay inside the .cpp. Nothing outside this file needs to know what an
// FfxApiResource is, and keeping them out of the layer's other translation units keeps the
// possibility of an FFX_ macro colliding with one of the layer's own out of the build.

#pragma once

#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>

#include <wrl/client.h>

class Fsr4Upscale {
  public:
    Fsr4Upscale() = default;
    ~Fsr4Upscale();

    Fsr4Upscale(const Fsr4Upscale&) = delete;
    Fsr4Upscale& operator=(const Fsr4Upscale&) = delete;

    // Brings the runtime up on the two devices the layer already has. Returns false with a reason in
    // LastError() when FSR 4 cannot run here, which is a normal outcome on a machine without the
    // signed runtime installed, not an error the caller has to treat as a fault.
    bool Initialize(ID3D11Device* device11,
                    ID3D11DeviceContext* context11,
                    ID3D12Device* device12,
                    ID3D12CommandQueue* queue12);

    void Shutdown();

    bool Ready() const {
        return m_ready;
    }

    const char* LastError() const {
        return m_lastError;
    }

    // Upscales the whole of `sourceRect` out of `source` (array slice `sourceSlice`) into the whole
    // of `dest` (array slice `destSlice`). Both textures belong to the application's D3D11 device.
    //
    // Sizes are taken from the arguments: the render size is the rectangle, the upscale size is the
    // destination texture. Geometry is settled on the first call and rebuilt if it ever changes.
    // Returns false when the pass could not run, so the caller can fall back to its own upscale
    // rather than presenting an image nobody wrote.
    //
    // `jitterX` / `jitterY` are the sub-pixel offset the caller put under the application's frustum
    // for this frame, in render pixels. They are the effect's only way to line the current frame up
    // with the history it kept, so an offset applied but not reported here is worse than none.
    //
    // `field` names the reprojection the layer built for this view on the application's own device:
    // a shared handle on a `fieldWidth` x `fieldHeight` texture of displacements as a fraction of the
    // view, written by a compute pass in the layer. It is the effect's only way to line this frame up
    // with the history it kept. A null handle is a view the layer could not reproject, and the effect
    // is then handed a zero field rather than one from another view.
    bool Run(ID3D11Texture2D* source,
             uint32_t sourceSlice,
             const RECT& sourceRect,
             ID3D11Texture2D* dest,
             uint32_t destSlice,
             float jitterX,
             float jitterY,
             HANDLE field,
             uint32_t fieldWidth,
             uint32_t fieldHeight);

  private:
    // Everything that has to see the ffx headers, so this one only forward declares them.
    struct Impl;

    Impl* m_impl{nullptr};
    bool m_ready{false};
    const char* m_lastError{""};
};
