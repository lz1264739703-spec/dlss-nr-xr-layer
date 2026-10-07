// The geometry the layer's host code and the neural backend agree on.
//
// These were declared alongside the lmxxf backend while this tree carried both renderers. It is the
// NVIDIA build now, so they live on their own: the host (AmdnrXrLayer / EyeInterop) fills them in and
// the backend consumes them, and neither has to know about the other's runtime.

#pragma once

#include <cstdint>

// One view's motion field, as the network's temporal path wants it: for every pixel of the network
// input, where that pixel's content sat in the previous answer.
//
// The layer cannot ask the application for motion vectors -- an OpenXR layer sees the finished eye
// buffer and nothing else -- but it does not need to. The eye buffer is a camera image, so the
// dominant motion is the head's own rotation, and that is a pure function of the two view poses:
// rotating a pixel's view ray into the previous frame's view gives the previous pixel exactly, with
// no depth involved. What this cannot express is an object moving inside the world, which is the
// price of driving the temporal path from a layer.
struct NrMotion {
    // Takes a view-space direction in this frame to the previous frame's view space.
    float matrix[9]{};
    // Network pixel -> normalised image coordinate: s = a + (p + 0.5) * b, per axis.
    float a[2]{};
    float b[2]{};
    // Half-tangents of the view frustum, and their spans.
    float tanLeft{0.f};
    float tanWidth{0.f};
    float tanUp{0.f};
    float tanHeight{0.f};
    // Converts the normalised displacement into network pixels: the view's image extent times the
    // resample scale.
    float extentScale[2]{};
    // Carries the window's own travel into the answer's pixel grid. The window jumps on a coarse
    // grid, and without this the history would appear to slide sideways whenever it did.
    float originShift[2]{};
    bool valid{false};
};

// Geometry and look of one conversion pass.
//
// The source and the destination differ in size whenever the neural window covers more of the eye
// buffer than the network's input ceiling allows: the window is then squeezed into the network and
// stretched back on the way out. Everything else about the two directions is asymmetric:
//
//   producer  colour crop -> network input     no blend, no encode
//   consumer  network answer -> colour crop    blends against the untouched crop over `feather`
//                                              pixels and undoes the network's tone drift
struct NrConvert {
    uint32_t sourceWidth{0};
    uint32_t sourceHeight{0};
    uint32_t destinationWidth{0};
    uint32_t destinationHeight{0};
    bool encodeSrgb{false};
    bool blend{false};
    uint32_t feather{0};
    // How round the feathered region is: 0 leaves it the rectangle the window is, 1 turns it into the
    // ellipse inscribed in it. Four straight edges meeting at four corners is the shape a rim reads
    // as, and it reads worst at the corners, where the band has to change direction inside a few
    // pixels. Bending it towards a circle moves the transition off them.
    float roundness{0.f};
    float gain{1.f};
    float gamma{1.f};
    // Where the source is read for a given destination pixel, as a fraction of the image. Only the
    // asynchronous path sets it: the answer it writes describes last frame's crop, and without this the
    // window would sit one frame behind the rest of the frame while the head turns.
    float shiftX{0.f};
    float shiftY{0.f};
};

// Second view rebuilt from the first one's answer instead of through the network.
//
// The two eyes look at one scene from a few centimetres apart, so their answers differ by little more
// than the parallax of that baseline. Moving the first view's detail across by that amount reproduces
// the second view's real answer to within about two grey levels, measured offline against a network
// run on it, where the denoise itself moves pixels by ten. A stereo frame therefore needs one network
// pass, not two -- which is the difference between a tier that misses 90 Hz and one that meets it.
struct NrTransfer {
    // Parallax of the second view, in first-view pixels, positive when the same content sits that far
    // to the right in the first view's image.
    int32_t shift{0};
};

// Where to look for that parallax and how finely. The offsets are tried coarsely because the answer
// only has to be good to a couple of pixels: the detail being carried across is low amplitude, and
// the measurement is between a shift that reproduces the second answer to two grey levels and one
// that does it to three.
struct NrShiftSearch {
    int32_t base{-96};
    int32_t step{4};
    uint32_t count{57};
    // Sampled on a grid this many points wide. A correlation needs enough of them to mean anything,
    // and they are cheap next to the network pass they save.
    uint32_t samples{4096};
    uint32_t margin{64};
    // Last frame's answer, when there is one. Repeated texture gives several offsets that match about
    // equally well, and taking the global best makes the answer jump between those peaks from frame
    // to frame -- which reads as the detail sliding across the picture. Given a previous answer, the
    // search takes the nearest of the near-equal peaks instead.
    int32_t prefer{INT32_MIN};
};
