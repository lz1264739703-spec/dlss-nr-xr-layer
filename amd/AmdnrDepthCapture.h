// Watches the application's D3D11 device for the depth-stencil buffer its scene is drawn into.
//
// The layer already holds the application's own device and immediate context, so the only thing
// missing to get depth is the one D3D11 entry point that announces a depth buffer. Nothing here
// needs a second present hook, a proxy DLL, or another process to cooperate.

#pragma once

#include <windows.h>

#include <stdint.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Starts watching. Safe to call more than once; only the first call with a non-null context patches
// anything.
void DepthCaptureAttach(ID3D11Device* device, ID3D11DeviceContext* context);

// Restores the patched virtual table and drops every held reference. Must run before this module is
// unloaded, or the table keeps a pointer into code that no longer exists.
void DepthCaptureDetach();

// Records a colour swapchain size the application created. The largest one seen is taken as the
// per-eye render size, which is what lets a depth buffer at the eye resolution be preferred over one
// that merely happens to be bound often. A title creates small auxiliary chains too, hence the
// largest-wins rule rather than the first or last.
void DepthCaptureSetEyeSize(uint32_t width, uint32_t height);

// The rectangle the application draws into when it binds the depth target that is currently selected:
// the viewport it set, in texels. This is the one piece of geometry that cannot be derived from the
// resource, and a depth consumer needs it -- the scene depth of this title lives in a 6774x2718
// allocation and is drawn into a much smaller part of it, so a reduction built for the allocation
// would publish cleared depth over the difference. False until a viewport has been seen for the
// selected target, in which case the caller has nothing better than the allocation's own size.
bool DepthCaptureSelectedViewport(uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height);

// Hands the selected depth buffer downstream, once per frame.
//
// Never called from inside the hook: the copy this triggers goes through the application's own
// immediate context, and issuing that from within an OMSetRenderTargets call on that same context
// would re-enter it.
void DepthCapturePublish();

// The reduced, single-float depth as a D3D11 shared handle, so the layer's D3D12 motion pass can put
// a real distance under every pixel of the motion it builds. Zero until a multisampled depth has been
// reduced once. `width` and `height` are the reduction's size, which is the eye image size the layer
// asked for.
HANDLE DepthCaptureReduceHandle(uint32_t* width, uint32_t* height);

// The same reduced depth as a D3D11 resource on the application's own device. A reader that works
// through that device -- rather than through the shared handle the layer's D3D12 side opens -- wants
// this one, because copying it needs no second API and no fence. Borrowed, not a reference: it is
// rebuilt whenever the size changes and goes away with the session.
ID3D11Texture2D* DepthCaptureReducedTexture(uint32_t* width, uint32_t* height);
