// See the header for what this is for. What is here is a shift scan that asks whether the two eyes
// are a stereo pair at all, without any depth in it, followed by the sweep: one score per candidate
// near plane, and the candidate that scores lowest.
//
// The score is the mean absolute difference between a pixel of one eye and the place in the other eye
// the candidate says the same content is. Every sample is a piece of evidence for or against the
// candidate, and the samples are taken over the whole eye image, so texture, edges and flat regions
// all contribute and no single bad match decides anything. Nothing is thresholded and nothing is
// rejected as an outlier: a candidate that is wrong spends most of its samples comparing different
// things, and that should show up as a mean several times higher than the right candidate's. It did
// not: every run's winner sat on the edge of the candidate range or was scored on a handful of
// samples, which is why the scan below now runs first.

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <wrl/client.h>

#include "AmdnrDepthCapture.h"
#include "AmdnrStereoProbe.h"
#include "LayerLog.h"

namespace {

    using Microsoft::WRL::ComPtr;

    bool ProbeRequested() {
        static const bool requested = [] {
            char text[8]{};
            return GetEnvironmentVariableA("AMDNR_XR_NEAR_PROBE", text, (DWORD)sizeof(text)) != 0 &&
                   text[0] != '0';
        }();
        return requested;
    }

    struct View {
        bool valid{false};
        RECT box{};
        float tanWidth{0.f};
        // Where this view's optical axis lands inside its own rectangle, in pixels. Not the rectangle's
        // centre: the frustum is asymmetric, and skewed a different way for each eye, so the two axes
        // are hundreds of pixels apart and "the same pixel column" in the two eyes is a different
        // direction. Every correspondence below has to carry this displacement.
        float center{0.f};
        float position[3]{};
    };

    View g_views[2];
    uint32_t g_frames = 0;
    uint32_t g_runs = 0;

    // A near plane is a small number and the interesting range spans more than an order of magnitude,
    // so the candidates are spaced geometrically. Ninety of them resolve the minimum to about four
    // percent, which is far finer than the plane has to be known for a motion field.
    constexpr uint32_t kCandidateCount = 90;
    constexpr float kNearestCandidate = 0.015f;
    constexpr float kFarthestCandidate = 2.f;

    // Every sixteenth pixel. The match runs over the whole eye image, so this is still tens of
    // thousands of samples, and a denser grid does not move the answer.
    constexpr uint32_t kStride = 16;

    // The first solve waits for the level: before that the eye image holds a loading screen on both
    // sides, and two loading screens agree at every candidate. After that the solves are spread over
    // several minutes of play rather than taken back to back, because whether the two eyes carry a
    // measurable disparity is a property of the scene and of where the head is pointing, and one
    // corner of one room is not enough to call it. The question these runs answer is whether any
    // frame at all shows a minimum; forty-eight of them over three minutes is enough of a spread for
    // that, and a single frame is not.
    constexpr uint32_t kFirstRun = 400;
    constexpr uint32_t kRunInterval = 150;
    constexpr uint32_t kMaxRuns = 48;

    struct Image {
        std::vector<uint8_t> pixels;
        uint32_t width{0};
        uint32_t height{0};
        uint32_t pitch{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };

    ComPtr<ID3D11Texture2D> g_staging;
    Image g_eye;
    Image g_depth;

    bool ReadInto(ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  ID3D11Texture2D* source,
                  Image& image,
                  ComPtr<ID3D11Texture2D>& staging) {
        if (source == nullptr) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0) {
            return false;
        }

        if (staging == nullptr || image.width != desc.Width || image.height != desc.Height ||
            image.format != desc.Format) {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.ReleaseAndGetAddressOf()))) {
                staging.Reset();
                return false;
            }
            image.width = desc.Width;
            image.height = desc.Height;
            image.format = desc.Format;
            image.pitch = 0;
            image.pixels.clear();
        }

        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        image.pitch = mapped.RowPitch;
        image.pixels.resize((size_t)mapped.RowPitch * desc.Height);
        std::memcpy(image.pixels.data(), mapped.pData, image.pixels.size());
        context->Unmap(staging.Get(), 0);
        return true;
    }

    // The eye texture is eight bits a channel and display-encoded; matching on a luminance is enough
    // for a disparity and costs a third of what matching on three channels would.
    float Luma(const Image& image, uint32_t x, uint32_t y) {
        const uint8_t* pixel = image.pixels.data() + (size_t)y * image.pitch + (size_t)x * 4;
        return 0.2126f * (float)pixel[0] + 0.7152f * (float)pixel[1] + 0.0722f * (float)pixel[2];
    }

    // The disparity is fractional, so the sample has to be interpolated; nearest neighbour would
    // quantise the score and flatten the minimum that is being looked for.
    float LumaBilinear(const Image& image, float x, float y) {
        const int x0 = (int)x;
        const int y0 = (int)y;
        const float fx = x - (float)x0;
        const float fy = y - (float)y0;
        const float top = Luma(image, (uint32_t)x0, (uint32_t)y0) * (1.f - fx) +
                          Luma(image, (uint32_t)x0 + 1, (uint32_t)y0) * fx;
        const float bottom = Luma(image, (uint32_t)x0, (uint32_t)y0 + 1) * (1.f - fx) +
                             Luma(image, (uint32_t)x0 + 1, (uint32_t)y0 + 1) * fx;
        return top * (1.f - fy) + bottom * fy;
    }

    float DepthAt(const Image& depth, uint32_t x, uint32_t y) {
        const float* row = reinterpret_cast<const float*>(depth.pixels.data() + (size_t)y * depth.pitch);
        return row[x];
    }

    // One candidate, scored. `sign` is +1 when the content is expected to the right in the second
    // view and -1 when it is to the left, which is not known ahead of time: it turns on which half of
    // the packed image is which eye and on the handedness of the pair, and both are cheaper to test
    // than to reason about.
    //
    // `axisOffset` is the constant part of that displacement -- where the other view's optical axis
    // sits relative to this one's, in pixels. Without it the candidate is compared against a pixel
    // hundreds of places away from where the same content actually is, and no candidate can win.
    float Score(float candidate,
                float ipd,
                uint32_t eyeWidth,
                uint32_t eyeHeight,
                const RECT& reference,
                const RECT& other,
                int sign,
                float axisOffset,
                uint32_t& samples) {
        const float scale = ipd * (float)eyeWidth / (g_views[0].tanWidth * candidate);
        double total = 0.0;
        samples = 0;

        for (uint32_t y = 0; y < eyeHeight; y += kStride) {
            for (uint32_t x = 0; x < eyeWidth; x += kStride) {
                const uint32_t packedX = (uint32_t)reference.left + x;
                const uint32_t packedY = (uint32_t)reference.top + y;
                const float inverseDepth = DepthAt(g_depth, packedX, packedY);
                // Nothing was drawn into this pixel: the clear reads as zero, and it would otherwise
                // claim an infinite distance and a disparity of zero.
                if (!(inverseDepth > 1e-6f) || inverseDepth > 1.f) {
                    continue;
                }
                const float disparity = scale * inverseDepth;
                const float target = (float)x + axisOffset + (float)sign * disparity;
                if (target < 1.f || target > (float)eyeWidth - 2.f) {
                    continue;
                }
                const float here = Luma(g_eye, packedX, packedY);
                const float there = LumaBilinear(g_eye, (float)other.left + target, (float)other.top + (float)y);
                total += std::fabs(here - there);
                samples++;
            }
        }
        return samples > 0 ? (float)(total / (double)samples) : -1.f;
    }

    struct Sweep {
        float bestCandidate{0.f};
        float bestScore{-1.f};
        uint32_t samples{0};
    };

    // What the depth actually holds, over the same pixels the sweep uses. The solve rests entirely on
    // the captured depth being a distance, so a run that reports a range this narrow -- every pixel
    // the same value to within a percent -- is saying the depth is not carrying distance at all, and
    // that the answer below it is meaningless however sharp its minimum looks.
    void ReportDepth(const RECT& reference, uint32_t eyeWidth, uint32_t eyeHeight) {
        float lowest = 1e30f;
        float highest = -1e30f;
        double total = 0.0;
        uint64_t count = 0;
        for (uint32_t y = 0; y < eyeHeight; y += kStride) {
            for (uint32_t x = 0; x < eyeWidth; x += kStride) {
                const float value =
                    DepthAt(g_depth, (uint32_t)reference.left + x, (uint32_t)reference.top + y);
                if (!std::isfinite(value)) {
                    continue;
                }
                lowest = value < lowest ? value : lowest;
                highest = value > highest ? value : highest;
                total += value;
                count++;
            }
        }
        LayerLog("near probe: the depth reads %.5f .. %.5f, mean %.5f over %llu samples\n",
                 lowest,
                 highest,
                 count ? (float)(total / (double)count) : 0.f,
                 (unsigned long long)count);
    }

    // The sweep below assumes the two eyes are a stereo pair and that the depth says how far apart
    // their content is. Neither has been established. This scan establishes the first of the two on
    // its own, with no depth in it: it slides one eye over the other a whole pixel at a time and
    // reports where the images agree best. A pair whose correspondence really is a horizontal
    // offset puts that minimum at a small nonzero shift, with a score well below the score at zero.
    // Two halves that are not a pair, or a pair that is not a plain horizontal offset, put it at
    // zero or leave the curve flat -- and either of those retires the disparity route outright.
    // Wide enough to hold the whole displacement, which is not only the disparity: this runtime hands
    // out an asymmetric frustum per eye, skewed outwards, so the two images are displaced by a
    // constant on top of it -- over 400 px here, and the earlier 256 px range ran out before the
    // curve had turned over, which is why every scan so far reported its own boundary.
    constexpr int kMaxShift = 640;

    struct Shift {
        int dx{0};
        double score{-1.0};
    };

    // Every shift is scored over the same pixels: the border is skipped by the widest shift, not by
    // each shift's own reach, so the scores are comparable to one another. Comparing scores over
    // different sample counts is what let the sweep report a five-sample win.
    double ShiftScore(int dx, int dy, uint32_t margin, uint32_t& samples) {
        const RECT& reference = g_views[0].box;
        const RECT& other = g_views[1].box;
        const uint32_t eyeWidth = (uint32_t)(reference.right - reference.left);
        const uint32_t eyeHeight = (uint32_t)(reference.bottom - reference.top);
        double total = 0.0;
        samples = 0;
        for (uint32_t y = margin; y + margin < eyeHeight; y += kStride) {
            for (uint32_t x = margin; x + margin < eyeWidth; x += kStride) {
                const float here =
                    Luma(g_eye, (uint32_t)reference.left + x, (uint32_t)reference.top + y);
                const float there = Luma(g_eye,
                                         (uint32_t)((int)other.left + (int)x + dx),
                                         (uint32_t)((int)other.top + (int)y + dy));
                total += (double)std::fabs(here - there);
                samples++;
            }
        }
        return samples > 0 ? total / (double)samples : -1.0;
    }

    // The same comparison, restricted to one band of inverse depth. Reading the correspondence band by
    // band is what separates "this scene has almost no parallax" from "the reconstruction is ignoring
    // the depth" -- a single whole-frame shift cannot tell those apart, because both look like one
    // good shift. It also measures the depth scale directly: the shift a band lands on against the
    // depth that band holds, with nothing assumed about the near plane.
    double BandShiftScore(int dx, float depthLow, float depthHigh, uint32_t margin, uint32_t& samples) {
        const RECT& reference = g_views[0].box;
        const RECT& other = g_views[1].box;
        const uint32_t eyeWidth = (uint32_t)(reference.right - reference.left);
        const uint32_t eyeHeight = (uint32_t)(reference.bottom - reference.top);
        double total = 0.0;
        samples = 0;
        for (uint32_t y = margin; y + margin < eyeHeight; y += kStride) {
            for (uint32_t x = margin; x + margin < eyeWidth; x += kStride) {
                const float inverseDepth =
                    DepthAt(g_depth, (uint32_t)reference.left + x, (uint32_t)reference.top + y);
                if (!(inverseDepth >= depthLow && inverseDepth < depthHigh)) {
                    continue;
                }
                const float here =
                    Luma(g_eye, (uint32_t)reference.left + x, (uint32_t)reference.top + y);
                const float there = Luma(g_eye,
                                         (uint32_t)((int)other.left + (int)x + dx),
                                         (uint32_t)other.top + (uint32_t)y);
                total += (double)std::fabs(here - there);
                samples++;
            }
        }
        return samples > 0 ? total / (double)samples : -1.0;
    }

    struct LumaStats {
        double mean{0.0};
        double sd{1.0};
    };

    LumaStats RegionLumaStats(const RECT& box, uint32_t margin) {
        const uint32_t eyeWidth = (uint32_t)(box.right - box.left);
        const uint32_t eyeHeight = (uint32_t)(box.bottom - box.top);
        double total = 0.0;
        double square = 0.0;
        uint64_t count = 0;
        for (uint32_t y = margin; y + margin < eyeHeight; y += kStride) {
            for (uint32_t x = margin; x + margin < eyeWidth; x += kStride) {
                const double value =
                    (double)Luma(g_eye, (uint32_t)box.left + x, (uint32_t)box.top + y);
                total += value;
                square += value * value;
                count++;
            }
        }
        LumaStats stats;
        if (count == 0) {
            return stats;
        }
        stats.mean = total / (double)count;
        const double variance = square / (double)count - stats.mean * stats.mean;
        stats.sd = variance > 1e-9 ? std::sqrt(variance) : 1.0;
        return stats;
    }

    // The raw difference is sensitive to any overall brightness or contrast difference between the
    // two eyes: a second eye that is a little brighter or flatter makes sliding the comparison
    // toward the darker end of it keep lowering the mean, with no correspondence in the number at
    // all. Dividing each eye by its own mean and spread takes that out and leaves the pattern, so a
    // minimum that is real survives the normalisation and a slope that was brightness does not.
    double ShiftScoreNormalised(int dx,
                                int dy,
                                uint32_t margin,
                                const LumaStats& referenceStats,
                                const LumaStats& otherStats) {
        const RECT& reference = g_views[0].box;
        const RECT& other = g_views[1].box;
        const uint32_t eyeWidth = (uint32_t)(reference.right - reference.left);
        const uint32_t eyeHeight = (uint32_t)(reference.bottom - reference.top);
        double total = 0.0;
        uint32_t samples = 0;
        for (uint32_t y = margin; y + margin < eyeHeight; y += kStride) {
            for (uint32_t x = margin; x + margin < eyeWidth; x += kStride) {
                const double here = ((double)Luma(g_eye,
                                                  (uint32_t)reference.left + x,
                                                  (uint32_t)reference.top + y) -
                                     referenceStats.mean) /
                                    referenceStats.sd;
                const double there =
                    ((double)Luma(g_eye,
                                  (uint32_t)((int)other.left + (int)x + dx),
                                  (uint32_t)((int)other.top + (int)y + dy)) -
                     otherStats.mean) /
                    otherStats.sd;
                total += std::fabs(here - there);
                samples++;
            }
        }
        return samples > 0 ? total / (double)samples : -1.0;
    }

    Shift BestShift(int dy, int step, uint32_t margin, uint32_t& samples) {
        Shift best;
        for (int dx = -kMaxShift; dx <= kMaxShift; dx += step) {
            uint32_t used = 0;
            const double score = ShiftScore(dx, dy, margin, used);
            if (score >= 0.0 && (best.score < 0.0 || score < best.score)) {
                best.score = score;
                best.dx = dx;
                samples = used;
            }
        }
        return best;
    }

    void ScanShift(float ipd) {
        if (!g_views[0].valid || !g_views[1].valid || g_eye.pixels.empty()) {
            return;
        }
        const RECT& reference = g_views[0].box;
        const uint32_t eyeWidth = (uint32_t)(reference.right - reference.left);
        const uint32_t eyeHeight = (uint32_t)(reference.bottom - reference.top);
        const uint32_t margin = (uint32_t)kMaxShift + 2;
        if (eyeWidth <= 2 * margin || eyeHeight <= 2 * margin) {
            return;
        }

        uint32_t samples = 0;
        const Shift horizontal = BestShift(0, 1, margin, samples);
        if (horizontal.score < 0.0) {
            LayerLog("near probe: shift scan found no shift it could score\n");
            return;
        }

        LayerLog("near probe: shift scan over %ux%u, %u samples per shift, dx -%d..+%d\n",
                 eyeWidth - 2 * margin,
                 eyeHeight - 2 * margin,
                 samples,
                 kMaxShift,
                 kMaxShift);

        uint32_t atZero = 0;
        const double zero = ShiftScore(0, 0, margin, atZero);
        LayerLog("near probe:   best dx %+d, score %.3f; at dx 0 the score is %.3f\n",
                 horizontal.dx,
                 horizontal.score,
                 zero);

        // The curve either side of the answer, so a minimum that has turned over can be told from a
        // slope that is still falling when the range runs out. The sweep's every winner sat on the
        // edge of its range, and those two look nothing alike here.
        static const int kCurve[] = {-640, -512, -384, -256, -128, -64, -32, -16, -8, 0,
                                     8,    16,   32,   64,   128,  256,  384,  512,  640};
        char curve[512]{};
        size_t written = 0;
        for (int dx : kCurve) {
            uint32_t ignored = 0;
            const int added = sprintf_s(curve + written,
                                        sizeof(curve) - written,
                                        " %+d:%.2f",
                                        dx,
                                        ShiftScore(dx, 0, margin, ignored));
            if (added < 0 || (size_t)added >= sizeof(curve) - written) {
                break;
            }
            written += (size_t)added;
        }
        LayerLog("near probe:   curve%s\n", curve);

        // The same curve with each eye divided by its own mean and spread. If the slope above was
        // one eye being brighter than the other, it is gone here and a minimum that was hiding
        // under it appears; if the slope survives, it is not brightness.
        const LumaStats referenceStats = RegionLumaStats(g_views[0].box, margin);
        const LumaStats otherStats = RegionLumaStats(g_views[1].box, margin);
        LayerLog("near probe:   luma: first mean %.2f sd %.2f, second mean %.2f sd %.2f\n",
                 referenceStats.mean,
                 referenceStats.sd,
                 otherStats.mean,
                 otherStats.sd);

        char normalised[512]{};
        written = 0;
        for (int dx : kCurve) {
            const int added =
                sprintf_s(normalised + written,
                          sizeof(normalised) - written,
                          " %+d:%.3f",
                          dx,
                          ShiftScoreNormalised(dx, 0, margin, referenceStats, otherStats));
            if (added < 0 || (size_t)added >= sizeof(normalised) - written) {
                break;
            }
            written += (size_t)added;
        }
        LayerLog("near probe:   normalised%s\n", normalised);

        // The normalised curve's own answer, on a line of its own so that a run of forty frames can
        // be read as a column -- the question this run exists to settle is whether any frame anywhere
        // in that column has a minimum inside the range, and that is easier to see line by line.
        int normalisedBest = 0;
        double normalisedBestScore = -1.0;
        double normalisedAtZero = 0.0;
        for (int dx = -kMaxShift; dx <= kMaxShift; dx++) {
            const double score = ShiftScoreNormalised(dx, 0, margin, referenceStats, otherStats);
            if (score >= 0.0 && (normalisedBestScore < 0.0 || score < normalisedBestScore)) {
                normalisedBestScore = score;
                normalisedBest = dx;
            }
            if (dx == 0) {
                normalisedAtZero = score;
            }
        }
        LayerLog("near probe:   normalised best dx %+d, score %.3f; at dx 0 the score is %.3f\n",
                 normalisedBest,
                 normalisedBestScore,
                 normalisedAtZero);

        // Whether the pair is offset only sideways or also up and down. A canted pair does not share
        // a vertical, and the sweep's assumption of a shared one would show up here as a minimum
        // that moves away from dy 0.
        static const int kVerticals[] = {-8, -4, -2, 2, 4, 8};
        char vertical[512]{};
        written = 0;
        for (int dy : kVerticals) {
            uint32_t ignored = 0;
            const Shift found = BestShift(dy, 4, margin, ignored);
            const int added = sprintf_s(vertical + written,
                                        sizeof(vertical) - written,
                                        " dy %+d -> dx %+d (%.3f)",
                                        dy,
                                        found.dx,
                                        found.score);
            if (added < 0 || (size_t)added >= sizeof(vertical) - written) {
                break;
            }
            written += (size_t)added;
        }
        LayerLog("near probe:   vertical:%s\n", vertical);

        // The correspondence against distance: one scan per band of inverse depth, each with the whole
        // range to itself. This is what tells "the two images are displaced by a constant" -- the depth
        // is not carrying distance, or the scene is flat -- from "a constant plus a term that grows with
        // inverse depth", which is the parallax the reconstruction is built on. In a single whole-frame
        // number those two are indistinguishable, and they mean opposite things.
        static const float kBandEdges[] = {0.f, 0.015f, 0.03f, 0.045f, 0.06f, 1.f};
        char bands[512]{};
        written = 0;
        for (size_t band = 0; band + 1 < sizeof(kBandEdges) / sizeof(kBandEdges[0]); band++) {
            int bestDx = 0;
            double bestScore = -1.0;
            uint32_t bandSamples = 0;
            for (int dx = -kMaxShift; dx <= kMaxShift; dx += 2) {
                uint32_t used = 0;
                const double score =
                    BandShiftScore(dx, kBandEdges[band], kBandEdges[band + 1], margin, used);
                if (score >= 0.0 && (bestScore < 0.0 || score < bestScore)) {
                    bestScore = score;
                    bestDx = dx;
                    bandSamples = used;
                }
            }
            if (bestScore < 0.0) {
                continue;
            }
            const int added = sprintf_s(bands + written,
                                        sizeof(bands) - written,
                                        " [%.3f-%.3f]%+d(n%u,%.2f)",
                                        kBandEdges[band],
                                        kBandEdges[band + 1],
                                        bestDx,
                                        bandSamples,
                                        bestScore);
            if (added < 0 || (size_t)added >= sizeof(bands) - written) {
                break;
            }
            written += (size_t)added;
        }
        LayerLog("near probe:   by depth:%s\n", bands);

        // What that offset would mean if the depth is a distance: the sweep's relation solved for
        // the plane instead of for the offset. Printed as a bridge between the two, not as an
        // answer -- it is only as good as the mean depth under it, and the constant between the two
        // optical axes is not disparity at all, so it has to come out first.
        double depthTotal = 0.0;
        uint64_t depthCount = 0;
        for (uint32_t y = margin; y + margin < eyeHeight; y += kStride) {
            for (uint32_t x = margin; x + margin < eyeWidth; x += kStride) {
                const float value =
                    DepthAt(g_depth, (uint32_t)reference.left + x, (uint32_t)reference.top + y);
                if (std::isfinite(value) && value > 1e-6f && value <= 1.f) {
                    depthTotal += (double)value;
                    depthCount++;
                }
            }
        }
        const float axisConstant = g_views[1].center - g_views[0].center;
        const float disparity = (float)horizontal.dx - axisConstant;
        if (std::fabs(disparity) > 0.5f && depthCount > 0 && g_views[0].tanWidth != 0.f) {
            const float meanDepth = (float)(depthTotal / (double)depthCount);
            const float span = std::fabs(disparity);
            LayerLog("near probe:   %+d px, %+.1f once the %.1f px between the axes is out, at the "
                     "mean depth %.4f implies a near plane of %.4f m\n",
                     horizontal.dx,
                     disparity,
                     axisConstant,
                     meanDepth,
                     ipd * (float)eyeWidth * meanDepth / (g_views[0].tanWidth * span));
        }
    }

    Sweep RunSweep(int sign, bool swapEyes, float ipd) {
        const RECT reference = swapEyes ? g_views[1].box : g_views[0].box;
        const RECT other = swapEyes ? g_views[0].box : g_views[1].box;
        const uint32_t eyeWidth = (uint32_t)(reference.right - reference.left);
        const uint32_t eyeHeight = (uint32_t)(reference.bottom - reference.top);
        // The constant part of the correspondence, from the two optical axes. It is the same for every
        // candidate, so it is worked out once.
        const float axisOffset = (swapEyes ? g_views[0].center : g_views[1].center) -
                                 (swapEyes ? g_views[1].center : g_views[0].center);

        Sweep sweep;
        // The candidates are spaced geometrically, so the index maps to a distance with a ratio
        // rather than a step.
        const float ratio = kFarthestCandidate / kNearestCandidate;
        for (uint32_t i = 0; i < kCandidateCount; i++) {
            const float candidate =
                kNearestCandidate * std::pow(ratio, (float)i / (float)(kCandidateCount - 1));
            uint32_t samples = 0;
            const float score =
                Score(candidate, ipd, eyeWidth, eyeHeight, reference, other, sign, axisOffset, samples);
            if (score < 0.f) {
                continue;
            }
            if (sweep.bestScore < 0.f || score < sweep.bestScore) {
                sweep.bestScore = score;
                sweep.bestCandidate = candidate;
                sweep.samples = samples;
            }
        }
        return sweep;
    }

    void Solve(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* packedEye) {
        if (packedEye == nullptr || !g_views[0].valid || !g_views[1].valid) {
            return;
        }

        // The distance between the two eye positions is the only externally known length in the whole
        // equation, and it is the one thing the solve cannot get wrong and still work.
        const float dx = g_views[0].position[0] - g_views[1].position[0];
        const float dy = g_views[0].position[1] - g_views[1].position[1];
        const float dz = g_views[0].position[2] - g_views[1].position[2];
        const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(ipd > 0.01f) || ipd > 0.15f) {
            LayerLog("near probe: the two eye positions are %.4f m apart, which is not an eye "
                     "separation; no solve\n", ipd);
            return;
        }

        uint32_t depthWidth = 0;
        uint32_t depthHeight = 0;
        ID3D11Texture2D* depthTexture = DepthCaptureReducedTexture(&depthWidth, &depthHeight);
        if (depthTexture == nullptr) {
            return;
        }

        ComPtr<ID3D11Texture2D> depthStaging;
        if (!ReadInto(device, context, packedEye, g_eye, g_staging) ||
            !ReadInto(device, context, depthTexture, g_depth, depthStaging)) {
            LayerLog("near probe: the eye image or the depth could not be read back\n");
            return;
        }

        const RECT reference = g_views[0].box;
        const RECT other = g_views[1].box;
        LayerLog("near probe: eyes %dx%d at (%ld,%ld) and (%ld,%ld) inside a %ux%u packed image, "
                 "depth %ux%u, ipd %.4f m, tanWidth %.4f\n",
                 (int)(reference.right - reference.left),
                 (int)(reference.bottom - reference.top),
                 (long)reference.left,
                 (long)reference.top,
                 (long)other.left,
                 (long)other.top,
                 g_eye.width,
                 g_eye.height,
                 depthWidth,
                 depthHeight,
                 ipd,
                 g_views[0].tanWidth);

        ReportDepth(reference,
                    (uint32_t)(reference.right - reference.left),
                    (uint32_t)(reference.bottom - reference.top));

        // First, and without any depth in it, whether the two halves are a stereo pair at all.
        ScanShift(ipd);

        // Both eye orders and both signs, because which half of a packed image is which eye, and
        // which way the disparity runs, are conventions neither this file nor the runtime states. The
        // correct pair of choices is the one whose curve has a minimum far below the others; the wrong
        // ones score consistently badly at every candidate.
        struct Attempt {
            int sign;
            bool swap;
            Sweep sweep;
        };
        Attempt attempts[4] = {{1, false, {}}, {-1, false, {}}, {1, true, {}}, {-1, true, {}}};
        for (Attempt& attempt : attempts) {
            attempt.sweep = RunSweep(attempt.sign, attempt.swap, ipd);
            LayerLog("near probe:   %s eyes, disparity %s -> best %.4f m, score %.3f over %u samples\n",
                     attempt.swap ? "second-then-first" : "first-then-second",
                     attempt.sign > 0 ? "rightward" : "leftward",
                     attempt.sweep.bestCandidate,
                     attempt.sweep.bestScore,
                     attempt.sweep.samples);
        }

        const Attempt* best = &attempts[0];
        for (const Attempt& attempt : attempts) {
            if (attempt.sweep.bestScore >= 0.f &&
                (best->sweep.bestScore < 0.f || attempt.sweep.bestScore < best->sweep.bestScore)) {
                best = &attempt;
            }
        }
        if (best->sweep.bestScore < 0.f) {
            LayerLog("near probe: every candidate was rejected; nothing to report\n");
            return;
        }

        LayerLog("near probe: the near plane is %.4f m (%s eyes, disparity %s)\n",
                 best->sweep.bestCandidate,
                 best->swap ? "second-then-first" : "first-then-second",
                 best->sign > 0 ? "rightward" : "leftward");
        // Two references either side of the answer, so the report says how sharp the minimum is and
        // not only where it is. A candidate that is right and a candidate that is merely least bad
        // look the same in a single number and look nothing alike next to these.
        const RECT referenceBox = best->swap ? g_views[1].box : g_views[0].box;
        const RECT otherBox = best->swap ? g_views[0].box : g_views[1].box;
        const uint32_t eyeWidth = (uint32_t)(referenceBox.right - referenceBox.left);
        const uint32_t eyeHeight = (uint32_t)(referenceBox.bottom - referenceBox.top);
        const float bestAxisOffset = (best->swap ? g_views[0].center : g_views[1].center) -
                                     (best->swap ? g_views[1].center : g_views[0].center);
        uint32_t ignored = 0;
        LayerLog("near probe:   score %.3f at the answer, %.3f at 0.05 m, %.3f at 0.50 m\n",
                 best->sweep.bestScore,
                 Score(0.05f, ipd, eyeWidth, eyeHeight, referenceBox, otherBox, best->sign,
                       bestAxisOffset, ignored),
                 Score(0.50f, ipd, eyeWidth, eyeHeight, referenceBox, otherBox, best->sign,
                       bestAxisOffset, ignored));
    }

} // namespace

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
                   float positionZ) {
    if (!ProbeRequested() || view >= 2) {
        return;
    }

    View& slot = g_views[view];
    slot.valid = true;
    slot.box = eyeRect;
    slot.tanWidth = tanWidth;
    // Where this view's optical axis falls in its own rectangle: the column at which the tangent is
    // zero. An asymmetric frustum does not put it at the centre.
    const float span = tanRight - tanLeft;
    const float eyeWidth = (float)(eyeRect.right - eyeRect.left);
    slot.center = span > 0.f ? eyeWidth * (-tanLeft) / span : eyeWidth * 0.5f;
    slot.position[0] = positionX;
    slot.position[1] = positionY;
    slot.position[2] = positionZ;

    if (view != 1) {
        return;
    }

    // Both views of the frame are in hand: the first was offered earlier in this same call, and the
    // packed texture is the one they share.
    g_frames++;
    if (g_frames < kFirstRun || (g_frames - kFirstRun) % kRunInterval != 0 || g_runs >= kMaxRuns) {
        return;
    }
    g_runs++;

    if (!g_views[0].valid) {
        return;
    }
    Solve(device, context, packedEye);
}

void NearProbeRelease() {
    g_staging.Reset();
    g_eye = Image{};
    g_depth = Image{};
    g_views[0] = View{};
    g_views[1] = View{};
    g_frames = 0;
    g_runs = 0;
}
