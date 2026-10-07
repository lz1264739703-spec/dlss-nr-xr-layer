// Solves the near plane the title projects with, out of the one measurement a layer can take without
// being told anything: the disparity between its two eyes.
//
// The relationship is one line. Two eyes a known distance apart look at a point d metres away; the
// point lands somewhere different in each image, and the difference is IPD / (d * tanWidth) of the
// image width -- in pixels, IPD * eyeWidth / (tanWidth * d). The layer already holds everything here
// except d: the eye images, the frustum tangents the runtime hands it every frame, and the two eye
// positions. It holds d too, in the depth it captures -- except that the depth the reduction
// publishes is inverse distance scaled by the near plane, so it reads n/z, and n is the unknown.
//
// Which leaves the unknown as the only free parameter. For a candidate n, the depth gives a distance
// for every pixel, the distance gives a disparity, and the disparity says where in the other eye the
// same content should be. The candidate that makes the two images agree, over thousands of samples,
// is the one the title projected with. One sweep and one score per candidate; the minimum is the
// answer. No game internals are read, nothing is guessed at, and the equation is the same one any
// reprojection would use.
//
// What it assumes, and what would show up as a failure rather than a wrong number: that the two eyes
// are a horizontal offset of each other, and that the captured depth is scaled by the same near plane
// on both sides. It does *not* assume the two eyes share a frustum -- this runtime hands out an
// asymmetric one per eye, skewed outwards, so the same direction of gaze lands hundreds of pixels
// apart in the two images. That offset is a constant, it comes from the frustum tangents below, and
// every candidate has to be displaced by it or the whole sweep is comparing the wrong pixels. (Its
// absence is what made an earlier version of this report a near plane pinned to the edge of its
// range.) A canted pair would push the best candidate wide and flatten the curve, which is visible in
// the report as a minimum that is not much lower than its neighbours.
//
// Off unless AMDNR_XR_NEAR_PROBE is set to something other than a leading zero.

#pragma once

#include <windows.h>

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Offers one view of the frame to the solver. The two views of a frame carry the same packed eye
// texture, so the solve runs once, when the second view has arrived. Everything is in the coordinates
// the layer already works in: `eyeRect` is the view's whole rectangle inside the packed image -- the
// eye, not the neural window, because the two eyes' windows are placed independently and comparing
// them would compare two different parts of the scene -- and the frustum tangents are the runtime's
// own for that view, which is what puts its optical axis inside that rectangle.
void NearProbeView(ID3D11Device* device,
                   ID3D11DeviceContext* context,
                   uint32_t view,
                   ID3D11Texture2D* packedEye,
                   const RECT& eyeRect,
                   float tanWidth,
                   float tanLeft,
                   float tanRight,
                   float positionX,
                   float positionY,
                   float positionZ);

// Drops the staging textures this holds. Called when the session goes away, before the device does.
void NearProbeRelease();
