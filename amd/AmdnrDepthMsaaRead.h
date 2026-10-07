// Reads the application's multisampled depth-stencil, which D3D11 offers no other way to reach.
//
// A depth format has no multisample resolve -- the driver's own answer for D24_UNORM_S8_UINT is a
// flag word without D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE -- and a multisampled texture cannot be
// copied or sampled. What is left is to load the four samples one at a time in a shader and reduce
// them there, which is what this does: a fullscreen pass, one invocation per output texel, reading
// the application's depth through a multisampled shader resource view.
//
// The output is a single float per texel, at whatever size the caller asks for, read out of the
// `offsetX, offsetY` corner of the source.
//
// That rectangle is a parameter rather than an assumption because the old assumption was wrong. The
// caller used to ask for the layer's own packed eye-image size on the theory that this title draws
// its eye buffer into exactly that corner of a larger pooled depth allocation. Measured against the
// live buffer (18 consecutive samples, 2026-10-05), the drawn region of the latched target is
// 4781 x 1125 at (0,0) inside a 6774 x 2718 allocation -- 28.5% of it -- while the colour eye pair
// is 4784 x 1920. The width agrees to within the sampling grid and the height does not, so the
// reduction was publishing `1 - raw` over the clear for 71% of the image, and that is what the
// layer's depth consumers were reading. Everything left of the clear is real depth.
//
// Each block is reduced with the nearest of the samples in it, and what is written is inverse
// distance -- `1 - raw`, proportional to 1/z for a standard projection. See the shader for why.

#pragma once

#include <d3d11.h>

#include <cstdint>

// Reduces the `width` x `height` rectangle at (`offsetX`, `offsetY`) of `depth` into a width x height
// single-float image. Safe to call every frame; the view, the shader and the states are built once,
// and the target is rebuilt only when the requested size changes. `nearPlane` and `farPlane` are used
// only when `linearise` is set. Returns false if the pass could not be set up at all.
//
// The reduce itself always runs. `readback` additionally copies the result into the staging texture
// for a CPU sample; leaving it false still refreshes the target, which is what a consumer reading the
// target on the GPU needs, without paying for a copy of it back to the host every frame.
bool MsaaDepthReadIssue(ID3D11Device* device,
                        ID3D11DeviceContext* context,
                        ID3D11Texture2D* depth,
                        DXGI_FORMAT viewFormat,
                        uint32_t offsetX,
                        uint32_t offsetY,
                        uint32_t width,
                        uint32_t height,
                        bool linearise,
                        float nearPlane,
                        float farPlane,
                        bool readback);

// The texture the reduce renders into, as a D3D11 shared handle, so another API (the layer's D3D12
// device) can read it. Zero until the first issue has built the target. The handle names one texture
// and stays valid until the target is rebuilt or released, so a consumer may open it once and keep
// the result rather than reopening it every frame; `outWidth` and `outHeight` are that texture's
// size, which is the size the last issue asked for.
HANDLE MsaaDepthReadTargetHandle(uint32_t* outWidth, uint32_t* outHeight);

// The same texture as a D3D11 resource, for a consumer that reads it through the application's own
// device rather than through the shared handle. Null until the first issue. Borrowed, not a
// reference: it lives exactly as long as the handle above does.
ID3D11Texture2D* MsaaDepthReadTexture(uint32_t* outWidth, uint32_t* outHeight);

// Maps the staging texture without waiting for the GPU. On success the four outputs describe a
// single-float image that stays valid until the next issue; `outData` is its first byte and
// `outRowPitch` its row stride. Returns false while the GPU is still drawing, and when there is
// nothing new to read since the last map.
bool MsaaDepthReadTryMap(const uint8_t** outData,
                         uint32_t* outRowPitch,
                         uint32_t* outWidth,
                         uint32_t* outHeight);

// Drops every resource this holds. Called when the session goes away, before the device does.
void MsaaDepthReadRelease();
