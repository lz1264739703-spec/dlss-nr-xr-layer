// Receives the depth a producer publishes, and (stage 1) proves it is real.
//
// The layer used to have no depth of its own: the application submits colour only (the layer logs
// "the projection layer carries no depth"), so nothing downstream can tell a moving object from
// camera parallax. The producer that fills that gap is now the layer's own D3D11 capture
// (AmdnrDepthCapture), which watches the application's immediate context for the depth-stencil view
// its scene is drawn with. A ReShade add-on was the earlier producer and still speaks this same
// interface, so the two can be running at once -- and when they are, they interleave here.
//
// Stage 1 measures rather than consumes: the published texture is copied to a staging texture and
// summarised, so the chain can be confirmed before anything depends on it. The copy never blocks a
// frame -- a diagnostic that stalls the render thread would corrupt the very frame timings this
// project exists to protect. The map is attempted without waiting, and a frame that is not ready is
// simply read a frame or two later.

#include <windows.h>

#include <d3d11.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <wrl/client.h>

#define AMDNR_DEPTH_BRIDGE_EXPORTS
#include "AmdnrDepthBridge.h"
#include "AmdnrDepthCapture.h"
#include "AmdnrDepthMsaaRead.h"
#include "LayerLog.h"

namespace {

    using Microsoft::WRL::ComPtr;

    // The published frame, kept for the stage that consumes it. The producer is ReShade's present,
    // the consumer is the layer's own frame path, so the hand-off has to be locked.
    std::mutex g_lock;
    AmdnrDepthFrame g_latest{};
    bool g_hasLatest = false;
    uint64_t g_supplied = 0;
    uint64_t g_rejected = 0;

    // ---- Non-blocking sampler ----
    //
    // One staging texture, reused for every sample. `g_samplePending` means a copy has been issued
    // and its map has not succeeded yet; no second copy is issued until it has, so a stall can only
    // cost samples, never frames.
    ComPtr<ID3D11Texture2D> g_staging;
    uint32_t g_stagingWidth = 0;
    uint32_t g_stagingHeight = 0;
    uint32_t g_stagingFormat = 0;
    // The producer's view format, which is the typed one: the texture itself is often typeless, and
    // how to decode a texel is decided by this rather than by the resource's own format.
    uint32_t g_stagingDepthFormat = 0;
    // How many samples the source had. Two candidates can share a size and a format and differ only
    // in this, so the line below is not readable without it.
    uint32_t g_stagingSamples = 1;
    bool g_samplePending = false;
    uint64_t g_sampleFrame = 0;

    // A multisampled depth cannot be copied or sampled the way this path reads a depth, so it goes
    // through the shader reduction in AmdnrDepthMsaaRead instead. That has its own staging, so the
    // two ways of taking a sample are tracked separately and only one is ever in flight.
    bool g_msaaPending = false;
    bool g_msaaReducedSizeLogged = false;

    // Undoing the projection needs the application's near and far planes, which nothing in the layer
    // is told: the depth buffer arrives already written. They are read from the environment and the
    // reduction is left as raw hardware depth until they are supplied, because a guessed pair would
    // silently rescale every value the renderer gets.
    bool LineariseDepth() {
        static const bool linearise = [] {
            char text[32]{};
            if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_LINEAR", text, (DWORD)sizeof(text)) == 0) {
                return false;
            }
            return std::strtol(text, nullptr, 10) != 0;
        }();
        return linearise;
    }

    float DepthPlane(const char* name, float fallback) {
        char text[32]{};
        if (GetEnvironmentVariableA(name, text, (DWORD)sizeof(text)) == 0) {
            return fallback;
        }
        const double value = std::strtod(text, nullptr);
        return value > 0.0 ? (float)value : fallback;
    }

    // Read once: how many supplied frames apart the samples are taken. 0 disables sampling.
    uint32_t SampleInterval() {
        static const uint32_t interval = [] {
            char text[32]{};
            if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_SAMPLE", text, (DWORD)std::size(text)) == 0) {
                return 300u;
            }
            const long value = std::strtol(text, nullptr, 10);
            return value < 0 ? 300u : (uint32_t)value;
        }();
        return interval;
    }

    uint64_t g_nextSampleAt = 0;

    // The rectangle of the depth allocation the application actually draws into: `x,y,w,h` in source
    // texels. It is a measurement rather than a constant because it is not the layer's packed eye
    // size -- the reduction used to assume that and published 71% clear depth as a result (see the
    // header). Unset or unparsable leaves the reduction on the packed eye size, which is what it has
    // always used, so this changes nothing until it is given a value.
    bool DepthRegion(uint32_t& x, uint32_t& y, uint32_t& width, uint32_t& height) {
        char text[64]{};
        if (GetEnvironmentVariableA("AMDNR_XR_DEPTH_REGION", text, (DWORD)sizeof(text)) == 0) {
            return false;
        }

        const char* cursor = text;
        char* end = nullptr;
        const unsigned long rx = std::strtoul(cursor, &end, 10);
        if (end == cursor || *end != ',') {
            return false;
        }
        cursor = end + 1;
        const unsigned long ry = std::strtoul(cursor, &end, 10);
        if (end == cursor || *end != ',') {
            return false;
        }
        cursor = end + 1;
        const unsigned long rw = std::strtoul(cursor, &end, 10);
        if (end == cursor || *end != ',') {
            return false;
        }
        cursor = end + 1;
        const unsigned long rh = std::strtoul(cursor, &end, 10);
        if (end == cursor || rw == 0 || rh == 0) {
            return false;
        }

        x = (uint32_t)rx;
        y = (uint32_t)ry;
        width = (uint32_t)rw;
        height = (uint32_t)rh;
        return true;
    }

    // The turn this producer was given is spent even when no sample can be taken from it. Leaving the
    // schedule where it is would make the next producer sampled immediately as well, which walks the
    // rotation in steps that skip candidates.
    void SkipTurn(const AmdnrDepthFrame& frame) {
        const uint32_t interval = SampleInterval();
        g_nextSampleAt = frame.frame_index + (interval ? interval : 300);
    }

    // What a sample reports. Enough to tell real depth from a buffer that was never written:
    // a flat range or a single saturated value is the signature of the latter.
    struct SampleStats {
        float minimum = 0.f;
        float maximum = 0.f;
        float mean = 0.f;
        float deviation = 0.f;
        uint64_t nearSaturated = 0; // >= 0.999: the far plane / sky
        uint64_t zeroed = 0;        // <= 0.001: nothing was written here, or the near plane
        uint64_t counted = 0;
        uint64_t histograms[8]{};
    };

    // Turns one texel of the staging texture into a depth over 0..1.
    //
    // The staging texture has to be created with the source's own format for the copy to be legal,
    // and that format is usually a packed integer one: a D24_UNORM_S8_UINT scene depth is four bytes
    // per texel holding 24 bits of UNORM, so reading those bytes back as a float yields a denormal
    // that rounds to zero for every pixel. The decode therefore follows the format the producer
    // reported (its view format, which is the typed one) rather than assuming the texel is a float.
    float DecodeDepth(uint32_t raw, uint32_t format) {
        switch (format) {
        case DXGI_FORMAT_R24G8_TYPELESS:        // 44
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     // 45
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: // 46
            // 24 bits of UNORM depth in the low three bytes, stencil or padding above.
            return (float)(raw & 0x00FFFFFFu) / 16777215.0f;
        case DXGI_FORMAT_R16_TYPELESS: // 53
        case DXGI_FORMAT_D16_UNORM:    // 55
            return (float)(raw & 0xFFFFu) / 65535.0f;
        default:
            // Everything else seen here is a 32-bit float depth (D32_FLOAT, R32_FLOAT), which the
            // staging bytes already are.
            return std::bit_cast<float>(raw);
        }
    }

    bool Is16BitDepth(uint32_t format) {
        return format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM;
    }

    // Strides through the picture rather than reading all of it: the statistics only need to be
    // indicative, and a full read of a 2K x 2K float buffer every sample is 16 MB of memory
    // traffic on the render thread for a diagnostic.
    void Summarise(const uint8_t* base,
                   size_t rowBytes,
                   uint32_t height,
                   uint32_t format,
                   SampleStats& stats) {
        const size_t texelBytes = Is16BitDepth(format) ? 2 : 4;
        const size_t rowTexels = rowBytes / texelBytes;
        const size_t texelCount = rowTexels * (size_t)height;
        const size_t stride = texelCount > 262144 ? 16 : 1;
        size_t index = 0;
        double sum = 0.0;
        double sumSquares = 0.0;

        for (size_t i = 0; i < texelCount; i += stride, index++) {
            uint32_t raw = 0;
            if (texelBytes == 4) {
                std::memcpy(&raw, base + i * 4, 4);
            } else {
                uint16_t narrow = 0;
                std::memcpy(&narrow, base + i * 2, 2);
                raw = narrow;
            }
            const float value = DecodeDepth(raw, format);
            if (index == 0) {
                stats.minimum = value;
                stats.maximum = value;
            }
            stats.minimum = value < stats.minimum ? value : stats.minimum;
            stats.maximum = value > stats.maximum ? value : stats.maximum;
            sum += value;
            sumSquares += (double)value * (double)value;
            if (value >= 0.999f) {
                stats.nearSaturated++;
            }
            if (value <= 0.001f) {
                stats.zeroed++;
            }
            int bucket = (int)(value * 8.f);
            bucket = bucket < 0 ? 0 : (bucket > 7 ? 7 : bucket);
            stats.histograms[bucket]++;
        }

        stats.counted = index;
        if (index == 0) {
            return;
        }
        stats.mean = (float)(sum / (double)index);
        const double variance = sumSquares / (double)index - (double)stats.mean * (double)stats.mean;
        stats.deviation = variance > 0.0 ? (float)sqrt(variance) : 0.f;
    }

    // Whether the sampled buffer is described as a picture in the log. Reading a depth buffer is a
    // visual question -- the same min/mean could be a room, a stale clear or a two-valued mask -- and
    // numbers alone cannot separate those.
    bool DumpThumbnails() {
        static const bool dump = [] {
            char text[8]{};
            return GetEnvironmentVariableA("AMDNR_XR_DEPTH_DUMP", text, (DWORD)sizeof(text)) != 0 &&
                   text[0] != '0';
        }();
        return dump;
    }

    // A coarse ASCII rendering of the buffer, one character per cell, so the shape can be read in
    // the log without a viewer.
    //
    // The ramp is stretched over this sample's own range rather than over 0..1. A depth that carries
    // inverse distance puts the whole scene inside a few percent of the range -- a room's worth of
    // geometry lives between 0 and about 0.06 -- so a ramp fixed to 0..1 renders every frame as one
    // blank character, which is exactly the frame worth looking at.
    void LogThumbnail(const uint8_t* base,
                      size_t rowBytes,
                      uint32_t width,
                      uint32_t height,
                      uint32_t format,
                      float minimum,
                      float maximum) {
        constexpr int kCols = 72;
        constexpr int kRows = 22;
        constexpr char kRamp[] = " .:-=+*#%@";
        constexpr int kLast = (int)sizeof(kRamp) - 2;
        const size_t texelBytes = Is16BitDepth(format) ? 2 : 4;
        const float span = maximum > minimum ? (maximum - minimum) : 0.f;

        for (int row = 0; row < kRows; row++) {
            const uint32_t y = (uint32_t)((uint64_t)row * height / kRows);
            const uint8_t* source = base + (size_t)y * rowBytes;
            char line[kCols + 1];
            for (int col = 0; col < kCols; col++) {
                const uint32_t x = (uint32_t)((uint64_t)col * width / kCols);
                uint32_t raw = 0;
                if (texelBytes == 4) {
                    std::memcpy(&raw, source + (size_t)x * 4, 4);
                } else {
                    uint16_t narrow = 0;
                    std::memcpy(&narrow, source + (size_t)x * 2, 2);
                    raw = narrow;
                }
                const float value = DecodeDepth(raw, format);
                int index = span > 0.f ? (int)((value - minimum) / span * (float)kLast + 0.5f) : kLast;
                index = index < 0 ? 0 : (index > kLast ? kLast : index);
                line[col] = kRamp[index];
            }
            line[kCols] = '\0';
            LayerLog("depth dump: %ux%u [%.4f..%.4f] |%s|\n", width, height, minimum, maximum, line);
        }
    }

    // Which rectangle of the buffer anything was actually drawn into. The reduction is handed the
    // size of the layer's packed eye image, on the assumption that the application fills exactly that
    // rectangle of the allocation its depth lives in; if it fills something else, the reduction
    // publishes cleared depth over the difference and every consumer of it is wrong. Nothing in the
    // log has ever said which rectangle it really is.
    //
    // "Drawn" is read as "not the clear value": the reduction publishes `1 - raw`, the application
    // clears to `raw = 1`, so an untouched texel comes out exactly 0. Scanned on a coarse grid -- a
    // full read of 6774 x 2718 floats is 18 MB of traffic on the render thread, and the bounds do not
    // need texel accuracy to be useful.
    void LogDrawnBounds(const uint8_t* base,
                        size_t rowBytes,
                        uint32_t width,
                        uint32_t height,
                        uint32_t format) {
        constexpr uint32_t kGrid = 4;
        const size_t texelBytes = Is16BitDepth(format) ? 2 : 4;
        uint32_t left = width;
        uint32_t top = height;
        uint32_t right = 0;
        uint32_t bottom = 0;
        uint64_t drawn = 0;
        uint64_t seen = 0;
        for (uint32_t y = 0; y < height; y += kGrid) {
            const uint8_t* row = base + (size_t)y * rowBytes;
            for (uint32_t x = 0; x < width; x += kGrid) {
                uint32_t raw = 0;
                if (texelBytes == 4) {
                    std::memcpy(&raw, row + (size_t)x * 4, 4);
                } else {
                    uint16_t narrow = 0;
                    std::memcpy(&narrow, row + (size_t)x * 2, 2);
                    raw = narrow;
                }
                seen++;
                if (DecodeDepth(raw, format) <= 0.001f) {
                    continue;
                }
                drawn++;
                left = x < left ? x : left;
                top = y < top ? y : top;
                right = x > right ? x : right;
                bottom = y > bottom ? y : bottom;
            }
        }
        if (drawn == 0) {
            LayerLog("depth bounds: %ux%u (%u texel grid) nothing is drawn -- every sample reads the "
                     "clear value\n",
                     width,
                     height,
                     kGrid);
            return;
        }
        LayerLog("depth bounds: %ux%u (%u texel grid) drawn x %u..%u y %u..%u -> %ux%u at (%u,%u), "
                 "%.1f%% of the buffer\n",
                 width,
                 height,
                 kGrid,
                 left,
                 right,
                 top,
                 bottom,
                 right - left + 1,
                 bottom - top + 1,
                 left,
                 top,
                 100.0 * (double)drawn / (double)seen);
    }

    // The same sample as a picture on disk. The ASCII rendering answers "is anything there" but not
    // "what shape is it" -- 72 x 22 cells cannot say whether the two halves of a packed stereo layout
    // carry the same room twice, and that is exactly the question a depth-based reprojection has to
    // have answered before anything is built on it.
    //
    // Written as P5 (binary greyscale), downsampled by `kDepthDumpStep` and stretched over this
    // sample's own range for the same reason the ramp is: an inverse-distance depth keeps a whole
    // room inside a few percent of the range, and against a fixed 0..1 ramp every frame is black.
    void DumpDepthImage(const uint8_t* base,
                        size_t rowBytes,
                        uint32_t width,
                        uint32_t height,
                        uint32_t format,
                        float minimum,
                        float maximum,
                        const char* name) {
        constexpr uint32_t kDepthDumpStep = 4;
        const uint32_t outWidth = (width + kDepthDumpStep - 1) / kDepthDumpStep;
        const uint32_t outHeight = (height + kDepthDumpStep - 1) / kDepthDumpStep;
        if (outWidth == 0 || outHeight == 0) {
            return;
        }
        const size_t texelBytes = Is16BitDepth(format) ? 2 : 4;
        const float span = maximum > minimum ? (maximum - minimum) : 0.f;

        std::string path = LayerDirectory() + "\\dump";
        CreateDirectoryA(path.c_str(), nullptr);
        path += "\\";
        path += name;
        path += ".pgm";

        FILE* file = nullptr;
        if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
            return;
        }
        fprintf(file, "P5\n%u %u\n255\n", outWidth, outHeight);
        for (uint32_t row = 0; row < outHeight; row++) {
            const uint8_t* source = base + (size_t)(row * kDepthDumpStep) * rowBytes;
            for (uint32_t col = 0; col < outWidth; col++) {
                uint32_t raw = 0;
                const uint32_t x = col * kDepthDumpStep;
                if (texelBytes == 4) {
                    std::memcpy(&raw, source + (size_t)x * 4, 4);
                } else {
                    uint16_t narrow = 0;
                    std::memcpy(&narrow, source + (size_t)x * 2, 2);
                    raw = narrow;
                }
                float normalised = span > 0.f ? (DecodeDepth(raw, format) - minimum) / span : 0.f;
                normalised = normalised < 0.f ? 0.f : (normalised > 1.f ? 1.f : normalised);
                fputc((int)(normalised * 255.f + 0.5f), file);
            }
        }
        fclose(file);
        LayerLog("depth dump: wrote %s (%ux%u, range %.4f..%.4f)\n",
                 path.c_str(),
                 outWidth,
                 outHeight,
                 minimum,
                 maximum);
    }

    // Finishes a sample taken through the shader reduction. The reduce has already brought the four
    // samples down to one float per texel, so what comes back is read exactly like a single-sample
    // depth -- it is only the way it was produced that differs.
    bool TryFinishMsaaSample() {
        const uint8_t* data = nullptr;
        uint32_t rowPitch = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!MsaaDepthReadTryMap(&data, &rowPitch, &width, &height)) {
            return false;
        }
        g_msaaPending = false;

        SampleStats stats;
        Summarise(data, rowPitch, height, DXGI_FORMAT_R32_FLOAT, stats);

        if (DumpThumbnails()) {
            LogThumbnail(data, rowPitch, width, height, DXGI_FORMAT_R32_FLOAT, stats.minimum, stats.maximum);
            DumpDepthImage(data,
                           rowPitch,
                           width,
                           height,
                           DXGI_FORMAT_R32_FLOAT,
                           stats.minimum,
                           stats.maximum,
                           "depth-msaa");
            LogDrawnBounds(data, rowPitch, width, height, DXGI_FORMAT_R32_FLOAT);
        }

        LayerLog("depth msaa: sample frame %llu  source %ux%u fmt=%u samples=%u reduced to %ux%u%s  "
                 "min=%.4f max=%.4f mean=%.4f sd=%.4f  near=%.1f%% zero=%.1f%%  "
                 "hist=[%llu %llu %llu %llu %llu %llu %llu %llu]\n",
                 (unsigned long long)g_sampleFrame,
                 g_stagingWidth,
                 g_stagingHeight,
                 g_stagingDepthFormat,
                 g_stagingSamples,
                 width,
                 height,
                 LineariseDepth() ? " (linear, from the environment's planes)" : " (inverse distance)",
                 stats.minimum,
                 stats.maximum,
                 stats.mean,
                 stats.deviation,
                 100.0 * (double)stats.nearSaturated / (double)(stats.counted ? stats.counted : 1),
                 100.0 * (double)stats.zeroed / (double)(stats.counted ? stats.counted : 1),
                 (unsigned long long)stats.histograms[0],
                 (unsigned long long)stats.histograms[1],
                 (unsigned long long)stats.histograms[2],
                 (unsigned long long)stats.histograms[3],
                 (unsigned long long)stats.histograms[4],
                 (unsigned long long)stats.histograms[5],
                 (unsigned long long)stats.histograms[6],
                 (unsigned long long)stats.histograms[7]);
        return true;
    }

    // Reads the pending sample if the GPU is done with it. Returns false while it is not, which
    // leaves the sample armed for the next frame.
    bool TryFinishSample() {
        if (g_msaaPending) {
            return TryFinishMsaaSample();
        }
        if (!g_samplePending || g_staging == nullptr) {
            return false;
        }

        ComPtr<ID3D11Device> device;
        g_staging->GetDevice(&device);
        if (device == nullptr) {
            g_samplePending = false;
            return false;
        }
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (context == nullptr) {
            g_samplePending = false;
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(g_staging.Get(), 0, D3D11_MAP_READ,
                                            D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
            return false; // not ready: try again on a later frame
        }
        if (FAILED(result)) {
            g_samplePending = false;
            LayerLog("depth bridge: the staging map failed (0x%08lX); sampling stays off\n",
                     (unsigned long)result);
            return false;
        }

        // The mapped buffer is laid out by RowPitch rather than by an assumed width, so the row
        // float count has to come from the pitch. A padded row contributes its padding columns to
        // the statistics; that is noise in a diagnostic that only has to distinguish real depth
        // from an empty buffer.
        SampleStats stats;
        Summarise((const uint8_t*)mapped.pData, mapped.RowPitch, g_stagingHeight, g_stagingDepthFormat, stats);
        if (DumpThumbnails()) {
            LogThumbnail((const uint8_t*)mapped.pData,
                         mapped.RowPitch,
                         g_stagingWidth,
                         g_stagingHeight,
                         g_stagingDepthFormat,
                         stats.minimum,
                         stats.maximum);
            DumpDepthImage((const uint8_t*)mapped.pData,
                           mapped.RowPitch,
                           g_stagingWidth,
                           g_stagingHeight,
                           g_stagingDepthFormat,
                           stats.minimum,
                           stats.maximum,
                           "depth-bridge");
        }
        context->Unmap(g_staging.Get(), 0);
        g_samplePending = false;

        LayerLog("depth bridge: sample frame %llu  %ux%u fmt=%u samples=%u  min=%.4f max=%.4f mean=%.4f "
                 "sd=%.4f  near=%.1f%% zero=%.1f%%  hist=[%llu %llu %llu %llu %llu %llu %llu %llu]\n",
                 (unsigned long long)g_sampleFrame,
                 g_stagingWidth,
                 g_stagingHeight,
                 g_stagingDepthFormat,
                 g_stagingSamples,
                 stats.minimum,
                 stats.maximum,
                 stats.mean,
                 stats.deviation,
                 100.0 * (double)stats.nearSaturated / (double)(stats.counted ? stats.counted : 1),
                 100.0 * (double)stats.zeroed / (double)(stats.counted ? stats.counted : 1),
                 (unsigned long long)stats.histograms[0],
                 (unsigned long long)stats.histograms[1],
                 (unsigned long long)stats.histograms[2],
                 (unsigned long long)stats.histograms[3],
                 (unsigned long long)stats.histograms[4],
                 (unsigned long long)stats.histograms[5],
                 (unsigned long long)stats.histograms[6],
                 (unsigned long long)stats.histograms[7]);
        return true;
    }

    void MaybeSample(const AmdnrDepthFrame& frame) {
        // A pending sample is finished first: it is holding the staging texture, so the copy that
        // would overwrite it must not be issued until it has been read. The frame carries on after
        // that rather than returning, because the reduction below is no longer only a diagnostic.
        if (g_samplePending || g_msaaPending) {
            TryFinishSample();
        }
        if (!frame.texture) {
            return;
        }

        ID3D11Texture2D* source = (ID3D11Texture2D*)(uintptr_t)frame.texture;
        D3D11_TEXTURE2D_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);

        ComPtr<ID3D11Device> device;
        source->GetDevice(&device);
        if (device == nullptr) {
            return;
        }
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (context == nullptr) {
            return;
        }

        // A multisampled depth cannot be copied into a single-sample staging texture, and the driver
        // offers no resolve for a depth format, so the only way to read one is the shader reduction.
        if (sourceDesc.SampleDesc.Count > 1) {
            // Reduced to the layer's own packed eye-image size rather than to the depth texture's: the
            // depth only uses one corner of the allocation it lives in, that corner is the swapchain's
            // own size, and it is the layout the layer's crop geometry already describes. Before a
            // colour swapchain has been seen there is nothing to reduce to, so the source size stands
            // in and the picture is simply not aligned yet.
            const uint32_t outWidth = frame.eye_width != 0 ? frame.eye_width : sourceDesc.Width;
            const uint32_t outHeight = frame.eye_height != 0 ? frame.eye_height : sourceDesc.Height;
            // Where inside the allocation the application actually drew. Left at the top-left packed
            // eye rectangle by default, which is what this reduced before the rectangle was measured.
            uint32_t originX = 0;
            uint32_t originY = 0;
            uint32_t reduceWidth = outWidth;
            uint32_t reduceHeight = outHeight;
            // The rectangle the application actually draws into, taken from the viewport it set on the
            // depth target. The packed eye size is only the fallback for a session with no viewport
            // seen yet, and it is the wrong answer for this title: it publishes 71% cleared depth.
            const bool viewportKnown = DepthCaptureSelectedViewport(originX, originY, reduceWidth, reduceHeight);
            const bool regionGiven = DepthRegion(originX, originY, reduceWidth, reduceHeight);
            const bool linearise = LineariseDepth();
            const float nearPlane = DepthPlane("AMDNR_XR_DEPTH_NEAR", 0.01f);
            const float farPlane = DepthPlane("AMDNR_XR_DEPTH_FAR", 1000.f);

            if (!g_msaaReducedSizeLogged) {
                g_msaaReducedSizeLogged = true;
                LayerLog("depth msaa: reducing the %ux%u depth to %ux%u, %s\n",
                         sourceDesc.Width,
                         sourceDesc.Height,
                         reduceWidth,
                         reduceHeight,
                         regionGiven    ? "AMDNR_XR_DEPTH_REGION"
                         : viewportKnown ? "the application's own viewport"
                         : frame.eye_width != 0 ? "the layer's packed eye image"
                                                : "the depth's own size, no eye swapchain seen yet");
                if (regionGiven || viewportKnown) {
                    LayerLog("depth msaa: the reduction reads (%u,%u)+%ux%u%s\n",
                             originX,
                             originY,
                             reduceWidth,
                             reduceHeight,
                             regionGiven ? ", which overrides the viewport" : "");
                }
                LayerLog("depth msaa: the reduction %s\n",
                         linearise ? "linearises the depth, which needs the application's planes and "
                                     "takes them from the environment -- nothing in the layer is told "
                                     "them, so a wrong pair rescales every value silently"
                                   : "publishes inverse distance (1 - raw), which needs no planes");
            }

            // The reduction runs on every frame. The motion field the network reprojects its history
            // with reads it, so a depth held back to the diagnostic's cadence would put one frame's
            // stale parallax under another frame's pixels. What the cadence gates is the copy back to
            // the host, which only the diagnostic numbers need.
            const uint32_t interval = SampleInterval();
            const bool readback = interval != 0 && !g_samplePending && !g_msaaPending &&
                                  frame.frame_index >= g_nextSampleAt;
            if (readback) {
                g_nextSampleAt = frame.frame_index + interval;
            }

            if (!MsaaDepthReadIssue(device.Get(),
                                    context.Get(),
                                    source,
                                    (DXGI_FORMAT)frame.format,
                                    originX,
                                    originY,
                                    reduceWidth,
                                    reduceHeight,
                                    linearise,
                                    nearPlane,
                                    farPlane,
                                    readback)) {
                SkipTurn(frame);
                return;
            }
            if (readback) {
                g_stagingWidth = sourceDesc.Width;
                g_stagingHeight = sourceDesc.Height;
                g_stagingDepthFormat = frame.format;
                g_stagingSamples = sourceDesc.SampleDesc.Count;
                g_sampleFrame = frame.frame_index;
                g_msaaPending = true;
            }
            return;
        }

        // The single-sample path is still only a diagnostic, so it keeps the interval gate whole: a
        // zero interval turns it off and a frame that is not due copies nothing at all.
        const uint32_t interval = SampleInterval();
        if (interval == 0 || g_samplePending || frame.frame_index < g_nextSampleAt) {
            return;
        }

        if (g_staging == nullptr || g_stagingWidth != sourceDesc.Width || g_stagingHeight != sourceDesc.Height ||
            g_stagingFormat != (uint32_t)sourceDesc.Format) {
            D3D11_TEXTURE2D_DESC stagingDesc{};
            stagingDesc.Width = sourceDesc.Width;
            stagingDesc.Height = sourceDesc.Height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = sourceDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
                LayerLog("depth bridge: could not create the %ux%u staging texture; sampling stays off\n",
                         sourceDesc.Width,
                         sourceDesc.Height);
                return;
            }
            g_staging = staging;
            g_stagingWidth = sourceDesc.Width;
            g_stagingHeight = sourceDesc.Height;
            g_stagingFormat = (uint32_t)sourceDesc.Format;
        }

        context->CopyResource(g_staging.Get(), source);
        g_stagingDepthFormat = frame.format;
        g_stagingSamples = sourceDesc.SampleDesc.Count;
        g_samplePending = true;
        g_sampleFrame = frame.frame_index;
        g_nextSampleAt = frame.frame_index + interval;
    }

} // namespace

// The add-on's entry point; see AmdnrDepthBridge.h for the contract and the lifetime rules.
extern "C" void AmdnrXrSupplyDepth(const AmdnrDepthFrame* frame) {
    if (frame == nullptr) {
        return;
    }

    // Anything that does not match the contract is refused rather than read: a mismatched struct
    // means the add-on and the layer were built from different revisions of the header, and
    // guessing at the remaining fields would read whatever the producer happened to leave there.
    if (frame->size < sizeof(AmdnrDepthFrame) || frame->version != AMDNR_DEPTH_BRIDGE_VERSION ||
        frame->texture == 0 || frame->width == 0 || frame->height == 0) {
        if (g_rejected++ == 0) {
            LayerLog("depth bridge: refused a frame (size=%u version=%u texture=%llu %ux%u); "
                     "the add-on and the layer disagree about the bridge ABI\n",
                     frame->size,
                     frame->version,
                     (unsigned long long)frame->texture,
                     frame->width,
                     frame->height);
        }
        return;
    }

    {
        const std::lock_guard<std::mutex> guard(g_lock);
        g_latest = *frame;
        g_hasLatest = true;
    }

    if (g_supplied++ == 0) {
        LayerLog("depth bridge: first frame from a producer: %ux%u fmt=%u flags=0x%X "
                 "nearZ=%.3f farZ=%.3f\n",
                 frame->width,
                 frame->height,
                 frame->format,
                 frame->flags,
                 frame->near_z,
                 frame->far_z);
    }

    MaybeSample(*frame);
}
