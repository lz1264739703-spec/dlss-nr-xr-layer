// Watches the constant buffers the application's immediate context binds, looking for the
// projection its scene is drawn through.
//
// Two questions come out of the one object, and neither is answered anywhere else the layer can
// reach. The near plane is what the depth the layer captures is scaled by -- the hardware depth is
// a projective function of 1/z whose coefficients are the near and far planes -- and OpenXR never
// tells a layer either of them. The same matrix answers whether the title jitters its projection
// between frames, which is the question an upscaler that accumulates history lives or dies on: a
// history carried across two frames by motion vectors is only correct if the two projections differ
// by nothing but the motion the vectors describe, and a per-frame sub-pixel jitter is exactly the
// difference they do not describe.
//
// Read-only in the same sense AmdnrDepthCapture is. The hooks remember pointers and nothing else;
// every buffer is copied and inspected from ProjectionProbePublish, which the layer calls from its
// own per-frame point, never from inside a hook -- a copy issued on the context from within one of
// its own calls would re-enter it.
//
// Off unless AMDNR_XR_CBUF_PROBE is set to something other than a leading zero.

#pragma once

#include <windows.h>

struct ID3D11DeviceContext;

// Patches the constant-buffer entry points of the context's virtual table. Safe to call more than
// once; only the first call does anything.
void ProjectionProbeAttach(ID3D11DeviceContext* context);

// Puts the patched entries back and lets every remembered buffer go. Must run before this module is
// unloaded, or the table keeps a pointer into code that no longer exists.
void ProjectionProbeDetach();

// Copies and inspects the remembered buffers, once per frame. Cheap once a projection has been
// found -- from then on it follows that one buffer -- and a scan of the whole set until then.
void ProjectionProbePublish();
