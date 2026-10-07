// The NVIDIA DLSS-NR (NGX) backend of the neural rendering pass.
//
// Ported from the DLSS-NR implementation in VirtualDesktop-OpenXR, which is MIT licensed:
//
//   MIT License
//
//   Copyright(c) 2026 Matthieu Bucchianeri
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files(the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and /or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The parameter names below, the load/init flow and the GetModuleFileNameW shim are that
// implementation's; the integration around them is this layer's.
//
// Where this layer differs is that it is self-contained: it drives the nvngx_dlssnr.dll beside it
// directly and supplies its own NVSDK_NGX_Parameter map, so NVIDIA's nvngx.dll and its
// GetCapabilityParameters export are not needed at all.

#include "NgxNr.h"

#include "FsrShaders.h"
#include "NrSettings.h"

#include <dxgi.h>
#include <dxgi1_4.h> // IDXGIFactory4::EnumAdapterByLuid, the D3D12 way back to an adapter

// ID3D12InfoQueue. The debug layer keeps the validation messages it produced, and the removal it
// performs carries the reason DXGI_ERROR_INVALID_CALL -- so the message that names the offending
// call is already on the device we were handed, if only it is asked for.
#include <d3d12sdklayers.h>

#include <nvsdk_ngx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

    // DLSS-NR is not a public feature, so none of its parameter names are in the shipped headers.
    // They are spelled out here exactly as the reference implementation carries them.
    #define NVSDK_NGX_Parameter_DLSSNR_Width "DLSSNR.Width"
    #define NVSDK_NGX_Parameter_DLSSNR_Height "DLSSNR.Height"
    #define NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset "DLSSNR.Hint.Render.Preset"
    #define NVSDK_NGX_Parameter_DLSSNR_Enabled "DLSSNR.Enabled"
    #define NVSDK_NGX_Parameter_DLSSNR_DepthInverted "DLSSNR.DepthInverted"

    #define NVSDK_NGX_Parameter_DLSSNR_Color "DLSSNR.Color"
    #define NVSDK_NGX_Parameter_DLSSNR_Output "DLSSNR.Output"
    #define NVSDK_NGX_Parameter_DLSSNR_Backbuffer "DLSSNR.Backbuffer"
    #define NVSDK_NGX_Parameter_DLSSNR_Depth "DLSSNR.Depth"
    #define NVSDK_NGX_Parameter_DLSSNR_MVec "DLSSNR.MVec"
    #define NVSDK_NGX_Parameter_DLSSNR_MVecScaleX "DLSSNR.MVecScaleX"
    #define NVSDK_NGX_Parameter_DLSSNR_MVecScaleY "DLSSNR.MVecScaleY"
    #define NVSDK_NGX_Parameter_DLSSNR_Reset "DLSSNR.Reset"
    #define NVSDK_NGX_Parameter_DLSSNR_UICorrection "DLSSNR.UICorrection"
    #define NVSDK_NGX_Parameter_DLSSNR_UseAutoMask "DLSSNR.UseAutoMask"
    #define NVSDK_NGX_Parameter_DLSSNR_Style "DLSSNR.Style"
    #define NVSDK_NGX_Parameter_DLSSNR_Intensity "DLSSNR.Intensity"
    #define NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength "DLSSNR.LocalToneStrength"
    #define NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength "DLSSNR.LocalStructureStrength"
    #define NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength "DLSSNR.SkinStructureStrength"
    #define NVSDK_NGX_Parameter_DLSSNR_ScalingRatio "DLSSNR.ScalingRatio"

    #define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX "DLSSNR.ColorSubrectBaseX"
    #define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY "DLSSNR.ColorSubrectBaseY"
    #define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth "DLSSNR.ColorSubrectWidth"
    #define NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight "DLSSNR.ColorSubrectHeight"

    #define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX "DLSSNR.OutputSubrectBaseX"
    #define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY "DLSSNR.OutputSubrectBaseY"
    #define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth "DLSSNR.OutputSubrectWidth"
    #define NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight "DLSSNR.OutputSubrectHeight"

    #define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX "DLSSNR.MVecSubrectBaseX"
    #define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY "DLSSNR.MVecSubrectBaseY"
    #define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth "DLSSNR.MVecSubrectWidth"
    #define NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight "DLSSNR.MVecSubrectHeight"

    #define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX "DLSSNR.DepthSubrectBaseX"
    #define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY "DLSSNR.DepthSubrectBaseY"
    #define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth "DLSSNR.DepthSubrectWidth"
    #define NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight "DLSSNR.DepthSubrectHeight"

    // The identity the snippet is handed. Both values are the ones the working DLSS-NR integrations
    // use, observed from the reference rather than issued to us: the core is given the project id,
    // the snippet the application id. A made-up value is not interchangeable here -- the core turns
    // an unknown project away before the snippet is ever reached.
    constexpr unsigned long long kAppId = 141959980ULL;
    constexpr const char* kProjectId = "53f803cc-a12f-4d69-90d5-19b7599cad19";

    // The version the snippet's own Init_Ext is called with. It is not the SDK's version constant:
    // the snippet is a pre-release build and names a version of its own. The core is handed one
    // version after another instead -- see LoadSnippet.
    constexpr int kSnippetVersion = 0x15;
    // The range the core's project registration is tried over. Which SDK versions an installed core
    // knows is a property of the driver, not of the header this layer was built against.
    constexpr int kCoreVersionFirst = 0x13;
    constexpr int kCoreVersionLast = 0x20;

    // The runtime asks for two float channels for the motion field.
    constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R32G32_FLOAT;
    constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // matrix, a, b, four tangents, extent scale, origin shift, width, height, the camera's
    // translation, one over the near plane, and the depth's rectangle inside the eye image.
    constexpr uint32_t kMotionConstants = 31;

    // source, original, destination, motion, history in, history out.
    constexpr uint32_t kConvertDescriptors = 6;
    // Source/destination sizes, encode and blend flags, feather width, roundness, gain, gamma, shift,
    // then the anti-flicker filter: on, strength, gate, whether a history exists yet.
    constexpr uint32_t kConvertConstants = 16;

    // How wide the box of the history the probe reads back is. Big enough that the numbers mean
    // something, small enough that the copy and the read cost a fraction of a millisecond.
    constexpr uint32_t kFilterProbeSide = 256;

    // IEEE half to float, for the probe's readback. One function is not worth a dependency, and the
    // values it reads are the layer's own corrections -- finite, and rarely subnormal.
    float HalfToFloat(uint16_t value) {
        const uint32_t sign = uint32_t(value & 0x8000u) << 16;
        const uint32_t exponent = (value >> 10) & 0x1Fu;
        uint32_t mantissa = value & 0x3FFu;
        uint32_t bits = 0;
        if (exponent == 0) {
            if (mantissa == 0) {
                bits = sign;
            } else {
                uint32_t e = 127 - 15;
                while ((mantissa & 0x400u) == 0) {
                    mantissa <<= 1;
                    --e;
                }
                bits = sign | (e << 23) | ((mantissa & 0x3FFu) << 13);
            }
        } else if (exponent == 0x1Fu) {
            bits = sign | 0x7F800000u | (mantissa << 13);
        } else {
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
        float out = 0.f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // The same conversion NrCore runs on its answer, kept textually identical so the two backends
    // put the same picture on screen: the network's answer is bent back towards the crop's tone,
    // mixed with the untouched crop over a soft rim, and encoded if the eye buffer is sRGB.
    const char* kConvertShader = R"(
cbuffer Params : register(b0)
{
    uint g_source_width;
    uint g_source_height;
    uint g_destination_width;
    uint g_destination_height;
    uint g_encode_srgb;
    uint g_blend;
    uint g_feather;
    float g_gain;
    float g_gamma;
    // Where the source is read for this pixel, as a fraction of the image. Zero on every path but the
    // asynchronous one, where the answer being written describes last frame's crop and has to be
    // carried onto this frame's pixels before it is mixed with them.
    float g_shift_x;
    float g_shift_y;
    // How round the feathered region is: 0 is the rectangle the window is, 1 the ellipse inscribed in
    // it. See ShapeDistance below.
    float g_roundness;
    // The anti-flicker filter: whether it runs, how much of the history it keeps at most, how far the
    // correction may move before the current frame wins outright, and whether there is a history to
    // read at all (0 on the first frame with it on, and after anything invalidates one).
    uint g_filter;
    float g_filter_strength;
    float g_filter_gate;
    uint g_filter_history_valid;
};

Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_original : register(t1);
SamplerState g_sampler : register(s0);
RWTexture2D<float4> g_destination : register(u0);
// The motion field, in network pixels over the network grid, and the filtered correction of the
// previous frame, in the destination grid. Both are written and read by the consumer only.
Texture2D<float2> g_motion : register(t2);
Texture2D<float4> g_history : register(t3);
RWTexture2D<float4> g_history_out : register(u1);

// How far this pixel sits from the middle of the window, with 1 on the boundary of the region and 0
// at its centre.
//
// The window is the region the network saw, so the boundary of the region is the edge of the window
// and this is the rectangle's own distance function. Roundness slides the same number between the box
// and the circle inscribed in it: at 0 the corners are as far as the edges and it is a box, at 1 it is
// the ellipse. Four straight edges meeting at four corners is the shape a rim reads as, and the corner
// is where it reads worst, because the band doubles back on itself there and the transition has to
// change direction within a few pixels.
float ShapeDistance(float2 centered, float roundness)
{
    const float2 scaled = abs(centered);
    return lerp(max(scaled.x, scaled.y), length(centered), saturate(roundness));
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    const float3 low = c * 12.92f;
    const float3 high = 1.055f * pow(max(c, 1e-5f), 1.f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, c));
}

// A single bilinear tap only reaches one source texel, so squeezing a big window into the network's
// input would alias. Widening the tap to cover the extra footprint fixes that, and because the extra
// footprint is zero at 1:1 all four taps land on the same texel there and the result is unchanged.
float3 SampleSource(float2 uv)
{
    const float2 span = 0.5f * max(1.0f / float2(g_destination_width, g_destination_height) -
                                       1.0f / float2(g_source_width, g_source_height),
                                   0.0f);
    float3 sum = g_source.SampleLevel(g_sampler, uv + float2(-span.x, -span.y), 0).rgb;
    sum += g_source.SampleLevel(g_sampler, uv + float2(span.x, -span.y), 0).rgb;
    sum += g_source.SampleLevel(g_sampler, uv + float2(-span.x, span.y), 0).rgb;
    sum += g_source.SampleLevel(g_sampler, uv + float2(span.x, span.y), 0).rgb;
    return sum * 0.25f;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_destination_width || tid.y >= g_destination_height)
    {
        return;
    }

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(g_destination_width, g_destination_height);
    float3 color;
    // What the anti-flicker filter leaves for the next frame: the correction this frame kept, and how
    // many frames of corrections that value stands for (1 = fresh). Pixels outside the window store an
    // invalid entry rather than a correction of something they are not showing.
    float4 history = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (g_blend == 0)
    {
        // Producer. The window is being squeezed into the network's input, so the source is the only
        // thing read and there is nothing to mix it with.
        color = SampleSource(uv + float2(g_shift_x, g_shift_y));
    }
    else
    {
        // Consumer. `centered` runs -1..1 across the window, so `distance` reaches 1 exactly on the
        // region's boundary and the shape is whatever roundness makes of it.
        const float2 centered =
            (float2(tid.xy) + 0.5f) / (0.5f * float2(g_destination_width, g_destination_height)) - 1.0f;
        const float distance = ShapeDistance(centered, g_roundness);

        // The feather is a fraction of the region rather than a count of pixels, so the band keeps its
        // proportion when the window changes size with the crop cover instead of widening or thinning
        // against it. The reference is the shorter radius, which is the axis the band runs out of
        // first -- the same reason the host measures the setting against the shorter window side.
        const float shorter_radius = 0.5f * (float)min(g_destination_width, g_destination_height);
        const float normalized_feather = g_feather > 0u ? (float)g_feather / shorter_radius : 0.0f;
        const float weight = g_feather > 0u
            ? 1.0f - smoothstep(max(0.0f, 1.0f - normalized_feather), 1.0f, distance)
            : 1.0f;
        const float2 originalUV = uv + float2(g_shift_x, g_shift_y);

        if (weight <= 0.0f)
        {
            // Outside the region the answer contributes nothing, so the original is all that is left
            // and the four-tap source read is skipped. It is still read through the shift, because it
            // is last frame's crop: reading it at `uv` would put the untouched band a whole window's
            // travel from where its content belongs, and the seam that leaves sits exactly on the
            // region's boundary, which is the one place a rim is looked at directly.
            color = g_original.SampleLevel(g_sampler, originalUV, 0).rgb;
        }
        else
        {
            // Both ends are read through the same shift: taken from different places the band stops
            // being a crossfade between two pictures of one thing and becomes a ghost of both -- a
            // visible band exactly as wide as the feather, and the wider the shift the worse it reads.
            const float3 original = g_original.SampleLevel(g_sampler, originalUV, 0).rgb;
            float3 answer = SampleSource(originalUV);

            // The network's answer comes back as roughly in^1.055 in linear light, measured off the
            // pixel dumps, so a gamma of 1/1.055 puts the window's brightness back in step with the rim.
            answer = pow(max(answer, 1e-5f), g_gamma) * g_gain;

            // ---- anti-flicker ----
            //
            // The network is run with no history of its own on this path, so every frame is a fresh
            // guess and the detail it invents moves from frame to frame. What flickers is the
            // correction it adds over the untouched crop, so that is what is carried across frames,
            // reprojected by the motion field the temporal path already builds. A correction that
            // barely moved is averaged with the history; one that moved further than the gate is the
            // scene changing, and the current frame is taken whole so nothing trails a fast edge. The
            // current frame's own detail survives either way: the answer is never read from the
            // history, only its difference from the original is damped.
            if (g_filter != 0)
            {
                float3 correction = answer - original;
                // The alpha lane carries how many frames of correction this pixel's history stands
                // for, not merely "one exists": a fresh write is 1, and every frame that genuinely
                // blends against the previous history adds one more, discounted by how much of it was
                // kept -- the fixed point of `n = keep * n + 1` is the 1/(1-keep) window the blend
                // averages over. This is what the probe reports, and it is the one number that can
                // only come from the history being read back: a filter whose read was broken would
                // write 1 here forever.
                float accumulated = 1.0f;
                if (g_filter_history_valid != 0)
                {
                    // The motion is written in network pixels over the network grid, which is the grid
                    // the answer is read in, so it converts with the answer's own size.
                    const float2 motionUV = originalUV + g_motion.SampleLevel(g_sampler, originalUV, 0).xy /
                                                          float2((float)g_source_width, (float)g_source_height);
                    const float4 past = g_history.SampleLevel(g_sampler, motionUV, 0);
                    const float moved = max(max(abs(correction.r - past.r), abs(correction.g - past.g)),
                                            abs(correction.b - past.b));
                    // `min(past.a, 1)` is what this lane used to be read as: a 0 or 1 validity, and
                    // saturated the same way for anything that only asks whether a history is there.
                    const float keep = g_filter_strength *
                                       saturate(1.0f - moved / max(g_filter_gate, 1e-8f)) *
                                       min(past.a, 1.0f);
                    correction = lerp(correction, past.rgb, keep);
                    accumulated = min(keep * min(past.a, 8.0f) + 1.0f, 8.0f);
                }
                history = float4(correction, accumulated);
                answer = original + correction;
            }

            color = lerp(original, answer, weight);
        }
    }

    if (g_encode_srgb != 0)
    {
        color = LinearToSrgb(color);
    }

    // Written only when the filter is on, so the producer path (and every frame with the filter off)
    // leaves the history exactly as it found it.
    if (g_filter != 0)
    {
        g_history_out[tid.xy] = history;
    }

    g_destination[tid.xy] = float4(color, 1.0f);
}
)";

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID,
                                          SIZE_T,
                                          LPCSTR,
                                          const D3D_SHADER_MACRO*,
                                          ID3DInclude*,
                                          LPCSTR,
                                          LPCSTR,
                                          UINT,
                                          UINT,
                                          ID3DBlob**,
                                          ID3DBlob**);

    // The shader compiler is taken from the system directory by full path, never by its bare name.
    // The bare name resolves with the directory of the running executable ahead of the system one,
    // so whatever sits beside the game's own binary wins -- and a game folder carrying a stub, an
    // injected proxy or an old copy under this name hands back a blob that compiles cleanly here and
    // is then refused when the pipeline state is created from it. Which copy was actually taken goes
    // into the log, so a failure that keeps coming back can be pinned on this or ruled out in one
    // run rather than argued about.
    D3DCompileFn GetD3DCompile() {
        static D3DCompileFn compile = []() -> D3DCompileFn {
            wchar_t directory[MAX_PATH] = {};
            const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) {
                LayerLog("NgxNr: the system directory could not be read; no shader can be compiled\n");
                return nullptr;
            }

            const std::wstring path = std::wstring(directory) + L"\\d3dcompiler_47.dll";
            const HMODULE module = LoadLibraryW(path.c_str());
            if (module == nullptr) {
                LayerLog("NgxNr: %ls could not be loaded; no shader can be compiled\n", path.c_str());
                return nullptr;
            }

            wchar_t loaded[MAX_PATH] = {};
            if (GetModuleFileNameW(module, loaded, MAX_PATH) != 0) {
                LayerLog("NgxNr: shaders are compiled by %ls\n", loaded);
            }
            return reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile"));
        }();
        return compile;
    }

    void Transition(ID3D12GraphicsCommandList* cmd,
                    ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
        if (before == after) {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &barrier);
    }

    DXGI_FORMAT WritableFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        default:
            return format;
        }
    }

    // A view the conversion writes through cannot name an sRGB format, so a resource that is written
    // as sRGB has to be created typeless and named by the plain member of its family. The rest of the
    // time the format is passed through untouched.
    DXGI_FORMAT TypelessFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:
            return format;
        }
    }

    std::wstring LayerDirectoryWide() {
        const std::string directory = LayerDirectory();
        if (directory.empty()) {
            return std::wstring();
        }
        const int length = MultiByteToWideChar(
            CP_ACP, 0, directory.c_str(), (int)directory.size(), nullptr, 0);
        std::wstring wide((size_t)length, L'\0');
        MultiByteToWideChar(CP_ACP, 0, directory.c_str(), (int)directory.size(), wide.data(), length);
        return wide;
    }

    // ---- The parameter map ----
    //
    // NVSDK_NGX_Parameter is a pure abstract interface, so the layer can carry its own map instead
    // of asking a loader for one. Deriving from the SDK interface makes the vtable layout the
    // header's by construction; a name maps to one of the eight value kinds and only the kind that
    // was stored is ever read back. Nothing here needs NVIDIA's nvngx.dll, or a
    // NVSDK_NGX_*_GetCapabilityParameters export, to be present.
    class ParameterMap final : public NVSDK_NGX_Parameter {
      public:
        void Set(const char* name, unsigned long long value) override { Store(name, Kind::Ull).ull = value; }
        void Set(const char* name, float value) override { Store(name, Kind::Float).f = value; }
        void Set(const char* name, double value) override { Store(name, Kind::Double).d = value; }
        void Set(const char* name, unsigned int value) override { Store(name, Kind::Uint).ui = value; }
        void Set(const char* name, int value) override { Store(name, Kind::Int).i = value; }
        void Set(const char* name, ID3D11Resource* value) override { Store(name, Kind::D3d11).d3d11 = value; }
        void Set(const char* name, ID3D12Resource* value) override { Store(name, Kind::D3d12).d3d12 = value; }
        void Set(const char* name, void* value) override { Store(name, Kind::Void).pointer = value; }

        NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override {
            return Read(name, Kind::Ull, out, &Entry::ull);
        }
        NVSDK_NGX_Result Get(const char* name, float* out) const override {
            return Read(name, Kind::Float, out, &Entry::f);
        }
        NVSDK_NGX_Result Get(const char* name, double* out) const override {
            return Read(name, Kind::Double, out, &Entry::d);
        }
        NVSDK_NGX_Result Get(const char* name, unsigned int* out) const override {
            return Read(name, Kind::Uint, out, &Entry::ui);
        }
        NVSDK_NGX_Result Get(const char* name, int* out) const override {
            return Read(name, Kind::Int, out, &Entry::i);
        }
        NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override {
            return Read(name, Kind::D3d11, out, &Entry::d3d11);
        }
        NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override {
            return Read(name, Kind::D3d12, out, &Entry::d3d12);
        }
        NVSDK_NGX_Result Get(const char* name, void** out) const override {
            return Read(name, Kind::Void, out, &Entry::pointer);
        }

        void Reset() override { m_values.clear(); }

      private:
        enum class Kind { None, Ull, Float, Double, Uint, Int, D3d11, D3d12, Void };

        struct Entry {
            Kind kind{Kind::None};
            unsigned long long ull{0};
            float f{0.0f};
            double d{0.0};
            unsigned int ui{0};
            int i{0};
            ID3D11Resource* d3d11{nullptr};
            ID3D12Resource* d3d12{nullptr};
            void* pointer{nullptr};
        };

        Entry& Store(const char* name, Kind kind) {
            if (name == nullptr) {
                static Entry discarded;
                discarded = Entry{};
                discarded.kind = kind;
                return discarded;
            }
            Entry& entry = m_values[name];
            entry = Entry{};
            entry.kind = kind;
            return entry;
        }

        // A name that was never set, or was set as another kind, reads as an invalid parameter --
        // the failure code the SDK names for exactly that case.
        template <typename T>
        NVSDK_NGX_Result Read(const char* name, Kind kind, T* out, T Entry::* field) const {
            if (name == nullptr || out == nullptr) {
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
            const auto it = m_values.find(name);
            if (it == m_values.end() || it->second.kind != kind) {
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
            *out = it->second.*field;
            return NVSDK_NGX_Result_Success;
        }

        std::map<std::string, Entry> m_values;
    };

    NVSDK_NGX_Parameter* NewParameterMap() {
        return new ParameterMap();
    }

    void DeleteParameterMap(NVSDK_NGX_Parameter* map) {
        delete static_cast<ParameterMap*>(map);
    }

    // ---- The NGX entry points ----
    //
    // Held once for the process: NGX is initialised per device, the parameter map is shared between
    // features, and the layer may bring up a session per eye. Entry points are fetched by name
    // because the SDK headers declare them for linking and this tree has no import library.
    struct NgxApi {
        // The three modules the chain is made of. The core is the driver's own NGX component and is
        // the one that decides whether the snippet may run at all; the shim is the module the
        // snippet insists on seeing as its caller; the snippet carries DLSS-NR itself.
        HMODULE core{nullptr};
        HMODULE shim{nullptr};
        HMODULE snippet{nullptr};

        // Set when a call into one of them faulted instead of returning. The SDK's process-wide
        // state is then suspect, so nothing else is tried and the pass simply runs without a
        // neural stage.
        bool faulted{false};

        // Core entry points. The identity is registered here and the parameter object is handed out
        // here, which is why the core is loaded even though the snippet is called directly.
        //
        // The argument order below is the core's, not the SDK header's: the header declares the
        // FeatureCommonInfo before the version, and the core wants the version first. Declaring the
        // pointer types in the callee's order is what puts the values where the core reads them.
        using PfnInitProject = NVSDK_NGX_Result(NVSDK_CONV*)(const char*,
                                                             NVSDK_NGX_EngineType,
                                                             const char*,
                                                             const wchar_t*,
                                                             ID3D12Device*,
                                                             NVSDK_NGX_Version,
                                                             const NVSDK_NGX_FeatureCommonInfo*);
        using PfnCoreInitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long,
                                                             const wchar_t*,
                                                             ID3D12Device*,
                                                             NVSDK_NGX_Version,
                                                             const NVSDK_NGX_FeatureCommonInfo*);
        using PfnAllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
        using PfnDestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

        PfnInitProject initProject{nullptr};
        PfnCoreInitExt coreInitExt{nullptr};
        PfnAllocateParameters allocateParameters{nullptr};
        PfnDestroyParameters destroyParameters{nullptr};

        // The shim's forwarding exports, together with the snippet entry points each of them is
        // handed. The snippet is never called directly -- see caller_shim.cpp for why.
        using PfnShimInit = int(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*,
                                          int, const void*);
        using PfnShimCreate = int(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, const void*, void**);
        using PfnShimEvaluate = int(__cdecl*)(void*, ID3D12GraphicsCommandList*, const void*,
                                              const void*, void*);
        using PfnShimRelease = int(__cdecl*)(void*, void*);

        PfnShimInit shimInit{nullptr};
        PfnShimCreate shimCreate{nullptr};
        PfnShimEvaluate shimEvaluate{nullptr};
        PfnShimRelease shimRelease{nullptr};

        void* snippetInit{nullptr};
        void* snippetCreate{nullptr};
        void* snippetEvaluate{nullptr};
        void* snippetRelease{nullptr};

        NVSDK_NGX_Parameter* parameter{nullptr};
        // Whether `parameter` is the layer's own map (deleted with `delete`) or one the core handed
        // out (released through the core's DestroyParameters).
        bool parameterOurs{false};
        bool deviceReady{false};
        uint32_t users{0};
    };

    NgxApi& SharedApi() {
        static NgxApi api;
        return api;
    }

    // Puts the reason a machine is not on DLSS-NR into words, with the card that was actually found
    // rather than a vendor id written into the message.
    const char* NgxText(const DXGI_ADAPTER_DESC& desc) {
        static char buffer[320];
        sprintf_s(buffer, "DLSS-NR is NVIDIA-only and this device is on %ls (vendor 0x%04X)",
                  desc.Description, (unsigned)desc.VendorId);
        return buffer;
    }

    const char* NgxError(const char* what, NVSDK_NGX_Result result) {
        static char buffer[256];
        sprintf_s(buffer, "%s failed (NGX result 0x%08X)", what, (unsigned)result);
        return buffer;
    }

    // The debug layer stores the validation messages it produced on the device it was switched on
    // for, so this layer's own device is the one to ask: the application's device carries a queue
    // too, but not for the commands recorded here, and the two are not shared. Draining it hands
    // over the exact call the runtime objected to, which is the one thing that cannot be worked out
    // from the outside.
    //
    // Messages already handed over. The queue keeps every message it ever produced, so a checkpoint
    // that arrives later has to be told only what is new to it: printing the whole queue each time
    // would repeat everything before it and bury the one thing worth reading -- the order the
    // messages were produced in, and which of this layer's steps they fell between.
    UINT64 g_messagesDumped = 0;

    void DumpDeviceMessages(ID3D12Device* device, const char* where) {
        if (device == nullptr) {
            return;
        }

        ComPtr<ID3D12InfoQueue> queue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(queue.ReleaseAndGetAddressOf()))) ||
            queue == nullptr) {
            // Said once. Every checkpoint would report the same absence, and it is a property of the
            // device rather than of the step that happened to notice it.
            static bool said = false;
            if (!said) {
                said = true;
                LayerLog("NgxNr: the device carries no validation queue at %s; nothing to drain "
                         "(is the D3D12 debug layer on for this device?)\n",
                         where);
            }
            return;
        }

        const UINT64 stored = queue->GetNumStoredMessages();
        if (stored <= g_messagesDumped) {
            return;
        }
        LayerLog("NgxNr: %llu new validation message(s) at %s\n",
                 (unsigned long long)(stored - g_messagesDumped),
                 where);

        for (UINT64 index = g_messagesDumped; index < stored; index++) {
            SIZE_T bytes = 0;
            if (FAILED(queue->GetMessage(index, nullptr, &bytes)) || bytes == 0) {
                continue;
            }
            std::vector<char> buffer(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (FAILED(queue->GetMessage(index, message, &bytes))) {
                continue;
            }
            // INFO and MESSAGE are the debug layer narrating its own work, and there are a great
            // many of them; CORRUPTION, ERROR and WARNING are what it found wrong.
            if ((int)message->Severity > (int)D3D12_MESSAGE_SEVERITY_WARNING) {
                continue;
            }
            const int length =
                message->DescriptionByteLength > 0 ? (int)message->DescriptionByteLength - 1 : 0;
            LayerLog("NgxNr:   severity %d id %d: %.*s\n",
                     (int)message->Severity,
                     (int)message->ID,
                     length,
                     message->pDescription);
        }
        g_messagesDumped = stored;
    }

    // A device is taken away by work recorded well before the call that finally reports it, so its
    // state is written down at each step that hands work to the GPU. The first line that says it is
    // gone names the step the loss happened under. If none of them does and a later call still
    // reports DXGI_ERROR_DEVICE_REMOVED, then the CPU-side calls were all legal and it is the
    // recorded commands themselves the runtime objected to, when they were executed.
    bool NoteDeviceState(ID3D12Device* device, const char* where) {
        if (device == nullptr) {
            return true;
        }
        const HRESULT reason = device->GetDeviceRemovedReason();
        if (reason != S_OK) {
            // Said once. Every step after the first would repeat the same reading, and it is the
            // first line that names the step the loss happened under.
            static bool reported = false;
            if (!reported) {
                reported = true;
                LayerLog("NgxNr: the device was already gone at %s (0x%08lX)\n",
                         where,
                         (unsigned long)reason);
            }
        }
        // Taken at every checkpoint rather than only at the failing one: what is printed is only
        // what is new, so the checkpoint a message appears behind is the step it belongs to.
        DumpDeviceMessages(device, where);
        return reason == S_OK;
    }

    // A fault inside the feature DLL belongs to NVIDIA, not to the game: it must not be what takes
    // the process down. Every call into that 165 MB DLL this layer does not control can fault rather
    // than return an error -- wrong build, wrong driver, or a DllMain that objects to where it was
    // loaded from -- so each entry point is wrapped to turn that into the failure the caller already
    // knows how to handle. LoadNgx answers a fault by leaving the pass with no neural stage at all;
    // the per-frame calls answer by switching it off.
    //
    // Every helper is kept free of C++ objects, because the compiler refuses an SEH frame in a
    // function that needs unwinding.
    //
    // A guard that swallows the fault leaves the same log as a guard that never ran, so the filter
    // below writes down what the fault was before returning. The exception record is only reachable
    // where the SDK hands it over -- inside the filter -- so that is where the reading happens, and
    // the faulting address is named as module + offset because the feature DLL ships no symbols.
    int NgxFaultFilter(const char* what, _EXCEPTION_POINTERS* info) {
        const EXCEPTION_RECORD* record = info != nullptr ? info->ExceptionRecord : nullptr;
        if (record == nullptr) {
            LayerLog("NgxNr: %s faulted, with no exception record to read\n", what);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        const void* address = record->ExceptionAddress;
        wchar_t module[MAX_PATH] = L"(unknown)";
        unsigned long long offset = 0;
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &owner) &&
            owner != nullptr) {
            if (GetModuleFileNameW(owner, module, MAX_PATH) == 0) {
                lstrcpynW(module, L"(unnamed)", MAX_PATH);
            }
            offset = static_cast<unsigned long long>(
                reinterpret_cast<const unsigned char*>(address) -
                reinterpret_cast<const unsigned char*>(owner));
        }

        LayerLog("NgxNr: %s faulted: exception 0x%08X at %p [%ls + 0x%llX]\n",
                 what, (unsigned)record->ExceptionCode, address, module, offset);

        // An access violation carries what was attempted and where: 0 reads, 1 writes, 8 executes.
        // The operation and the address together say whether a pointer came back null, whether an
        // object was the wrong type, or whether a call landed on unmapped code.
        if (record->ExceptionCode == 0xC0000005u && record->NumberParameters >= 2) {
            const unsigned long long operation = record->ExceptionInformation[0];
            LayerLog("NgxNr:   that was a %s of %p\n",
                     operation == 0 ? "read" : (operation == 1 ? "write" : "execute"),
                     reinterpret_cast<const void*>(record->ExceptionInformation[1]));
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    HMODULE GuardedLoadLibrary(const wchar_t* path, bool* faulted) {
        __try {
            *faulted = false;
            return LoadLibraryW(path);
        } __except (NgxFaultFilter("LoadLibraryW", GetExceptionInformation())) {
            *faulted = true;
            return nullptr;
        }
    }

    // The core is registered as a project as well as an application. The reference does both and
    // the project registration is the one that comes first; a failure there is reported and
    // carried past rather than treated as fatal, because the application registration below is
    // what actually brings the device up.
    bool GuardedInitProject(NgxApi::PfnInitProject fn, const char* projectId, const wchar_t* directory,
                            ID3D12Device* device, int version,
                            const NVSDK_NGX_FeatureCommonInfo* commonInfo, NVSDK_NGX_Result* out) {
        __try {
            *out = fn(projectId,
                      static_cast<NVSDK_NGX_EngineType>(0),
                      "1.0.0",
                      directory,
                      device,
                      static_cast<NVSDK_NGX_Version>(version),
                      commonInfo);
            return true;
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_Init_with_ProjectID", GetExceptionInformation())) {
            return false;
        }
    }

    bool GuardedCoreInitExt(NgxApi::PfnCoreInitExt fn, unsigned long long appId,
                            const wchar_t* directory, ID3D12Device* device, int version,
                            const NVSDK_NGX_FeatureCommonInfo* commonInfo, NVSDK_NGX_Result* out) {
        __try {
            *out = fn(appId, directory, device, static_cast<NVSDK_NGX_Version>(version), commonInfo);
            return true;
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_Init_Ext", GetExceptionInformation())) {
            return false;
        }
    }

    // The snippet's Init_Ext is reached through the shim, which is what puts a return address in the
    // shim's module -- the thing the snippet checks before it will talk to its caller.
    bool GuardedSnippetInit(NgxApi::PfnShimInit shim, void* real, unsigned long long appId,
                            const wchar_t* directory, ID3D12Device* device, int version,
                            const NVSDK_NGX_FeatureCommonInfo* commonInfo, NVSDK_NGX_Result* out) {
        __try {
            *out = static_cast<NVSDK_NGX_Result>(
                shim(real, appId, directory, device, version, commonInfo));
            return true;
        } __except (NgxFaultFilter("nvngx_dlssnr.dll Init_Ext", GetExceptionInformation())) {
            return false;
        }
    }

    bool GuardedAllocateParameters(NgxApi::PfnAllocateParameters fn, NVSDK_NGX_Parameter** out) {
        __try {
            return NVSDK_NGX_SUCCEED(fn(out));
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_AllocateParameters", GetExceptionInformation())) {
            return false;
        }
    }

    bool GuardedCreateFeature(NgxApi::PfnShimCreate shim, void* real, ID3D12GraphicsCommandList* cmd,
                              NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameter,
                              NVSDK_NGX_Handle** out, NVSDK_NGX_Result* result) {
        __try {
            *result = static_cast<NVSDK_NGX_Result>(shim(
                real, cmd, static_cast<int>(feature), parameter, reinterpret_cast<void**>(out)));
            return true;
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_CreateFeature", GetExceptionInformation())) {
            return false;
        }
    }

    bool GuardedEvaluateFeature(NgxApi::PfnShimEvaluate shim, void* real,
                                ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle,
                                const NVSDK_NGX_Parameter* parameter, NVSDK_NGX_Result* out) {
        __try {
            *out = static_cast<NVSDK_NGX_Result>(
                shim(real, cmd, handle, parameter, nullptr));
            return true;
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_EvaluateFeature", GetExceptionInformation())) {
            return false;
        }
    }

    bool GuardedReleaseFeature(NgxApi::PfnShimRelease shim, void* real, NVSDK_NGX_Handle* handle) {
        __try {
            shim(real, handle);
            return true;
        } __except (NgxFaultFilter("NVSDK_NGX_D3D12_ReleaseFeature", GetExceptionInformation())) {
            return false;
        }
    }

    // Finds the NGX core: the driver component that decides whether DLSS-NR may run at all. It is
    // not part of this layer, so it is looked for where a display driver keeps it. The order is the
    // reference's -- a copy dropped beside the layer wins, which is what makes one specific
    // driver's build usable on a machine whose own core is the wrong generation, then the ordinary
    // loader search, then the driver packages themselves.
    HMODULE LoadCoreNgx(const std::wstring& directory) {
        const std::wstring beside[] = {
            directory + L"\\runtime\\_nvngx.dll",
            directory + L"\\_nvngx.dll",
        };
        for (const std::wstring& candidate : beside) {
            if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
                continue;
            }
            if (HMODULE core = LoadLibraryW(candidate.c_str())) {
                LayerLog("NgxNr: the NGX core beside the layer is in use: %ls\n", candidate.c_str());
                return core;
            }
        }

        if (HMODULE core = LoadLibraryW(L"_nvngx.dll")) {
            LayerLog("NgxNr: the NGX core answered to its bare name\n");
            return core;
        }

        // The driver's own copy sits under a package name that varies with OEM and driver
        // generation -- nv_dispi is only the common case -- so every NVIDIA-looking package is
        // opened and the newest copy taken. DriverStore holds on to older packages after an update,
        // and a core older than the driver is worse than none.
        wchar_t windows[MAX_PATH] = {};
        const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return nullptr;
        }
        const std::wstring repository =
            std::wstring(windows) + L"\\System32\\DriverStore\\FileRepository\\";

        struct Candidate {
            std::wstring path;
            unsigned long long stamp;
        };
        std::vector<Candidate> found;
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW((repository + L"nv*.inf_*").c_str(), &entry);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    continue;
                }
                const std::wstring copy = repository + entry.cFileName + L"\\_nvngx.dll";
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (!GetFileAttributesExW(copy.c_str(), GetFileExInfoStandard, &attributes) ||
                    (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    continue;
                }
                ULARGE_INTEGER stamp{};
                stamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
                stamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
                found.push_back({copy, stamp.QuadPart});
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }

        std::sort(found.begin(), found.end(),
                  [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });
        for (const Candidate& candidate : found) {
            if (HMODULE core = LoadLibraryW(candidate.path.c_str())) {
                LayerLog("NgxNr: the NGX core from the driver is in use: %ls\n",
                         candidate.path.c_str());
                return core;
            }
        }

        LayerLog("NgxNr: no NGX core was found beside the layer, on the loader path, or in "
                 "DriverStore\n");
        return nullptr;
    }

    // Brings the snippet up through the chain the working integrations use: the core is told who is
    // asking, the shim makes the call into the snippet come from a module the snippet accepts, and
    // the snippet is then handed the device. Nothing here is optional -- a snippet reached without
    // the core behind it, or from this layer's own module, is refused.
    bool LoadSnippet(NgxApi& api, ID3D12Device* device, const std::wstring& directory) {
        // The caller shim first: without it the snippet answers 0xBAD00002 before it reads anything
        // else, so none of the work below would be reached.
        if (api.shim == nullptr) {
            // Named nvngx.dll, and it has to stay that way. The runtime checks the module the call
            // comes back to *and* what that module is called; the same shim under any other name is
            // answered with 0xBAD00002, exactly as a direct call from this layer is. Both
            // integrations this was built from keep the name for the same reason.
            const std::wstring path = directory + L"\\caller\\nvngx.dll";
            api.shim = LoadLibraryW(path.c_str());
            if (api.shim == nullptr) {
                LayerLog("NgxNr: the caller shim is not at %ls; a snippet called from this layer "
                         "would reject it\n",
                         path.c_str());
                return false;
            }
            api.shimInit = reinterpret_cast<NgxApi::PfnShimInit>(
                GetProcAddress(api.shim, "DLSSNR_CallInit"));
            api.shimCreate = reinterpret_cast<NgxApi::PfnShimCreate>(
                GetProcAddress(api.shim, "DLSSNR_CallCreate"));
            api.shimEvaluate = reinterpret_cast<NgxApi::PfnShimEvaluate>(
                GetProcAddress(api.shim, "DLSSNR_CallEvaluate"));
            api.shimRelease = reinterpret_cast<NgxApi::PfnShimRelease>(
                GetProcAddress(api.shim, "DLSSNR_CallRelease"));
            if (api.shimInit == nullptr || api.shimCreate == nullptr || api.shimEvaluate == nullptr ||
                api.shimRelease == nullptr) {
                LayerLog("NgxNr: the caller shim does not export the forwarding entry points\n");
                return false;
            }
            LayerLog("NgxNr: the caller shim is loaded\n");
        }

        if (api.core == nullptr) {
            api.core = LoadCoreNgx(directory);
            if (api.core == nullptr) {
                return false;
            }
            // The project-id export is spelled one way in the older cores and another in the SDK
            // header, so both names are tried.
            api.initProject = reinterpret_cast<NgxApi::PfnInitProject>(
                GetProcAddress(api.core, "NVSDK_NGX_D3D12_Init_ProjectID"));
            if (api.initProject == nullptr) {
                api.initProject = reinterpret_cast<NgxApi::PfnInitProject>(
                    GetProcAddress(api.core, "NVSDK_NGX_D3D12_Init_with_ProjectID"));
            }
            api.coreInitExt = reinterpret_cast<NgxApi::PfnCoreInitExt>(
                GetProcAddress(api.core, "NVSDK_NGX_D3D12_Init_Ext"));
            api.allocateParameters = reinterpret_cast<NgxApi::PfnAllocateParameters>(
                GetProcAddress(api.core, "NVSDK_NGX_D3D12_AllocateParameters"));
            api.destroyParameters = reinterpret_cast<NgxApi::PfnDestroyParameters>(
                GetProcAddress(api.core, "NVSDK_NGX_D3D12_DestroyParameters"));
            if (api.coreInitExt == nullptr) {
                LayerLog("NgxNr: the NGX core does not expose NVSDK_NGX_D3D12_Init_Ext\n");
                return false;
            }
        }

        if (api.snippet == nullptr) {
            const std::wstring path = directory + L"\\nvngx_dlssnr.dll";
            // Logged before the step that can fault, so the last line names the one that did.
            LayerLog("NgxNr: loading the snippet beside the layer: %ls\n", path.c_str());
            bool faulted = false;
            api.snippet = GuardedLoadLibrary(path.c_str(), &faulted);
            if (api.snippet == nullptr) {
                api.faulted = faulted;
                LayerLog("NgxNr: %s\n",
                         faulted ? "nvngx_dlssnr.dll faulted while being loaded"
                                 : "nvngx_dlssnr.dll is not beside the layer");
                return false;
            }
            api.snippetInit = reinterpret_cast<void*>(
                GetProcAddress(api.snippet, "NVSDK_NGX_D3D12_Init_Ext"));
            api.snippetCreate = reinterpret_cast<void*>(
                GetProcAddress(api.snippet, "NVSDK_NGX_D3D12_CreateFeature"));
            api.snippetRelease = reinterpret_cast<void*>(
                GetProcAddress(api.snippet, "NVSDK_NGX_D3D12_ReleaseFeature"));
            api.snippetEvaluate = reinterpret_cast<void*>(
                GetProcAddress(api.snippet, "NVSDK_NGX_D3D12_EvaluateFeature"));
            if (api.snippetInit == nullptr || api.snippetCreate == nullptr ||
                api.snippetRelease == nullptr || api.snippetEvaluate == nullptr) {
                LayerLog("NgxNr: nvngx_dlssnr.dll does not expose the D3D12 entry points\n");
                return false;
            }
            LayerLog("NgxNr: nvngx_dlssnr.dll is loaded\n");
        }

        if (api.deviceReady) {
            return true;
        }

        // The features are looked for beside the layer first, which is what lets an installation
        // carry its own copy the way the runtime's assets are carried.
        const wchar_t* paths[] = {directory.c_str()};
        NVSDK_NGX_FeatureCommonInfo commonInfo{};
        commonInfo.PathListInfo.Path = paths;
        commonInfo.PathListInfo.Length = 1;
        commonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

        // The identity, on the core. Which SDK versions an installed core speaks is a property of
        // the driver rather than of the header this layer was built against, so the version is
        // found by trying instead of assumed.
        int accepted = 0;
        if (api.initProject == nullptr) {
            LayerLog("NgxNr: the NGX core exposes no project-id entry point; carrying on\n");
        } else {
            for (int version = kCoreVersionFirst; version <= kCoreVersionLast; version++) {
                NVSDK_NGX_Result result{};
                if (!GuardedInitProject(api.initProject, kProjectId, directory.c_str(), device,
                                        version, &commonInfo, &result)) {
                    LayerLog("NgxNr: NVSDK_NGX_D3D12_Init_with_ProjectID faulted\n");
                    api.faulted = true;
                    return false;
                }
                if (NVSDK_NGX_SUCCEED(result)) {
                    accepted = version;
                    LayerLog("NgxNr: the core took the project id at version 0x%X\n", version);
                    break;
                }
            }
            if (accepted == 0) {
                LayerLog("NgxNr: the core refused the project id at every version from 0x%X to 0x%X; "
                         "carrying on\n",
                         kCoreVersionFirst, kCoreVersionLast);
            }
        }

        // Then the application, on the core. A refusal is carried past for the same reason as one
        // above: the snippet is what has to agree in the end, and the core has been told who asks.
        {
            NVSDK_NGX_Result result{};
            const int version = accepted != 0 ? accepted : static_cast<int>(NVSDK_NGX_Version_API);
            if (!GuardedCoreInitExt(api.coreInitExt, kAppId, directory.c_str(), device, version,
                                    &commonInfo, &result)) {
                LayerLog("NgxNr: NVSDK_NGX_D3D12_Init_Ext faulted on the core\n");
                api.faulted = true;
                return false;
            }
            if (NVSDK_NGX_FAILED(result)) {
                LayerLog("NgxNr: %s (carrying on)\n",
                         NgxError("NVSDK_NGX_D3D12_Init_Ext on the core", result));
            }
        }

        // Finally the snippet, through the shim. This is the call the shim exists for.
        LayerLog("NgxNr: calling the snippet's Init_Ext through the shim (version 0x%X, device %p, "
                 "path %ls)\n",
                 kSnippetVersion, reinterpret_cast<void*>(device), directory.c_str());
        NVSDK_NGX_Result init{};
        if (!GuardedSnippetInit(api.shimInit, api.snippetInit, kAppId, directory.c_str(), device,
                                kSnippetVersion, &commonInfo, &init)) {
            LayerLog("NgxNr: the snippet's Init_Ext faulted; it is unusable in this process\n");
            // Left mapped on purpose. The fault may have happened half way through the runtime's
            // own initialisation, and running its detach path on that state is another chance to
            // fault.
            api.faulted = true;
            api.snippet = nullptr;
            return false;
        }
        if (NVSDK_NGX_FAILED(init)) {
            LayerLog("NgxNr: the snippet refused the device: %s\n",
                     NgxError("nvngx_dlssnr.dll Init_Ext", init));
            return false;
        }
        api.deviceReady = true;
        return true;
    }

    // A diagnostic asked for through the environment and off unless named. The device is taken away
    // asynchronously, so no amount of CPU-side checkpointing says which recorded command the runtime
    // objected to -- and on a machine without the D3D12 debug layer there is no validation message to
    // read either. What is left is to leave a stage out and see whether the loss moves, which is what
    // the two switches below are for.
    bool DiagnosticSwitch(const char* name) {
        char value[8]{};
        return GetEnvironmentVariableA(name, value, (DWORD)sizeof(value)) > 0 && value[0] != '0';
    }

    // A texture this layer owns, to work on in a format or a usage the other API's resources cannot
    // be made to take. Every one of these carries the unordered-access flag, because each is written
    // through a UAV by the conversion shader: a resource that came from D3D11 is created for reading
    // and cannot be given that flag, which is the whole reason these exist.
    ComPtr<ID3D12Resource> CreateOwnedTexture(ID3D12Device* device,
                                              uint32_t width,
                                              uint32_t height,
                                              DXGI_FORMAT format,
                                              const wchar_t* name,
                                              D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON) {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> texture;
        if (FAILED(device->CreateCommittedResource(&heapProperties,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &desc,
                                                   state,
                                                   nullptr,
                                                   IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())))) {
            return nullptr;
        }
        texture->SetName(name);
        return texture;
    }

} // namespace

void NgxNr::Fail(const char* message) {
    m_lastError = message;
    LayerLog("NgxNr: %s\n", message);
}

void NgxNr::TakeLaunchTotals(double& enqueue, double& wait) {
    // The network runs while the producer's list is recorded, so there is nothing to account for.
    enqueue = 0;
    wait = 0;
}

bool NgxNr::LoadNgx(ID3D12Device* device) {
    NgxApi& api = SharedApi();

    if (api.users > 0 && api.parameter != nullptr) {
        // Already up for this process. The parameter map and the entry points are shared, and NGX
        // is initialised once per device -- which is also why the second eye does not re-init it.
        return true;
    }

    // A fault is not a refusal. Once a call has faulted, the SDK's process-wide state cannot be
    // trusted, and bringing the same runtime up a second time is more likely to fault again than to
    // help. The pass then runs with no neural stage at all, which costs the effect and keeps the
    // game alive.
    if (api.faulted) {
        Fail("DLSS-NR is unavailable: a call into the neural rendering runtime faulted");
        return false;
    }

    const std::wstring directory = LayerDirectoryWide();
    if (!LoadSnippet(api, device, directory)) {
        Fail("DLSS-NR is unavailable: it needs nvngx_dlssnr.dll and caller\\nvngx.dll beside the "
             "layer, and an NGX core from the display driver");
        return false;
    }

    if (api.parameter == nullptr) {
        // The core hands out the parameter object its feature expects, with the device's own
        // capabilities already in it. The layer's own map is the fallback: a map the feature can
        // read and write, but one that knows nothing about the card.
        if (api.allocateParameters != nullptr) {
            NVSDK_NGX_Parameter* allocated = nullptr;
            if (GuardedAllocateParameters(api.allocateParameters, &allocated) && allocated != nullptr) {
                api.parameter = allocated;
                api.parameterOurs = false;
            } else {
                LayerLog("NgxNr: NVSDK_NGX_D3D12_AllocateParameters gave nothing; falling back to "
                         "the layer's own parameter map\n");
            }
        }
        if (api.parameter == nullptr) {
            api.parameter = NewParameterMap();
            api.parameterOurs = true;
        }
    }

    return true;
}

bool NgxNr::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (m_parameter != nullptr) {
        return true;
    }

    m_device = device;
    m_queue = queue;

    // DLSS-NR only exists on NVIDIA hardware, so the adapter is checked before NGX is touched: an
    // AMD machine keeps the lmxxf path and one clear line in the log instead of a pile of errors
    // from a runtime that was never going to serve it.
    //
    // The adapter is reached through the device's LUID. Asking a device for IDXGIDevice -- the way a
    // D3D11 device is asked -- cannot work here: an ID3D12Device does not implement that interface,
    // so the QueryInterface fails with E_NOINTERFACE on every machine and every card, NVIDIA
    // included, looks like it is not one.
    DXGI_ADAPTER_DESC adapterDesc{};
    bool nvidia = false;
    bool identified = false;
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (device != nullptr &&
            SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),
                                                 IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()))) &&
            SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
            identified = true;
            nvidia = adapterDesc.VendorId == 0x10DE;
        }
    }
    if (!nvidia) {
        if (identified) {
            // Read cleanly and it simply is not NVIDIA: the lmxxf runtime is that machine's path.
            Fail(NgxText(adapterDesc));
            return false;
        }
        // Not the same as "not NVIDIA". The backend is only reached when the application's own
        // adapter already said 0x10DE, so a failed lookup is carried with its reason in the log
        // rather than refused, and the feature gets its chance.
        LayerLog("NgxNr: the adapter behind the D3D12 device could not be identified (device %s); "
                 "carrying on because the application's adapter is NVIDIA\n",
                 device == nullptr ? "missing" : "present");
    }

    if (!LoadNgx(device)) {
        return false;
    }
    // Asked here as well as past the feature, because the bring-up itself -- the project id, the two
    // Init_Ext calls -- is a suspect, and this is the first moment that can be asked after it ran.
    // It has since come back clean, and a checkpoint taken 250 ms later with nothing of this layer's
    // in between was clean too: the runtime does not take the device down on its own after the
    // bring-up, so what does is recorded further down this file.
    NoteDeviceState(device, "after the DLSS-NR bring-up");

    m_parameter = SharedApi().parameter;
    SharedApi().users++;
    m_registered = true;

    LayerLog("NgxNr: DLSS-NR ready on %ls\n", adapterDesc.Description);
    return true;
}

void NgxNr::Shutdown() {
    NgxApi& api = SharedApi();

    for (uint32_t eye = 0; eye < 2; eye++) {
        if (m_feature[eye] != nullptr) {
            if (api.shimRelease != nullptr && api.snippetRelease != nullptr) {
                GuardedReleaseFeature(api.shimRelease, api.snippetRelease, m_feature[eye]);
            }
            m_feature[eye] = nullptr;
        }
        m_featureWidth[eye] = 0;
        m_featureHeight[eye] = 0;
    }
    m_inputFp16.Reset();
    m_answer.Reset();
    m_result8.Reset();
    m_motion.Reset();
    m_motionHeap.Reset();
    m_motionPipeline.Reset();
    m_motionRootSignature.Reset();
    m_filterHistory[0].Reset();
    m_filterHistory[1].Reset();
    m_filterWidth = 0;
    m_filterHeight = 0;
    m_filterValid = false;
    m_filterStrength = -1.f;
    m_filterGate = -1.f;
    m_filterProbe.Reset();
    m_filterProbeWidth = 0;
    m_filterProbeHeight = 0;
    m_filterProbePending = false;
    m_lastFrameAtValid = false;
    m_convertHeap.Reset();
    m_convertPipeline.Reset();
    m_convertRootSignature.Reset();
    m_inputWidth = 0;
    m_inputHeight = 0;
    m_answerWidth = 0;
    m_answerHeight = 0;
    m_resultWidth = 0;
    m_resultHeight = 0;
    m_resultFormat = DXGI_FORMAT_UNKNOWN;
    m_produced = false;
    m_answerFence.Reset();
    m_answerValue = 0;
    if (m_answerEvent != nullptr) {
        CloseHandle(m_answerEvent);
        m_answerEvent = nullptr;
    }
    m_parameter = nullptr;
    m_device.Reset();
    m_queue.Reset();

    if (m_registered && api.users > 0) {
        m_registered = false;
        api.users--;
        if (api.users == 0) {
            // NGX itself is deliberately left initialised: the host application may be using DLSS,
            // and shutting the SDK down would take that with it. The modules are left mapped for the
            // same reason -- the core holds its own references into the snippet, and unloading one
            // out from under it is another chance to fault. Only the parameter object this layer
            // asked the core for is handed back.
            if (api.parameter != nullptr) {
                if (api.parameterOurs) {
                    DeleteParameterMap(api.parameter);
                } else if (api.destroyParameters != nullptr) {
                    api.destroyParameters(api.parameter);
                }
                api.parameter = nullptr;
                api.parameterOurs = false;
            }
        }
    }
}

bool NgxNr::EnsureInput(uint32_t width, uint32_t height) {
    if (m_inputFp16 != nullptr && m_motion != nullptr && m_inputWidth == width && m_inputHeight == height) {
        return true;
    }

    m_inputFp16.Reset();
    m_motion.Reset();
    m_inputWidth = 0;
    m_inputHeight = 0;

    m_inputFp16 = CreateOwnedTexture(m_device.Get(), width, height,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS-NR input");
    if (m_inputFp16 == nullptr) {
        Fail("the DLSS-NR input texture could not be created");
        return false;
    }
    // The motion field lives on the same grid as the picture it describes: for every pixel of the
    // network's input, where that pixel's content sat in the previous frame. Only the anti-flicker
    // filter reads it -- the feature is handed no motion vectors at all.
    m_motion = CreateOwnedTexture(m_device.Get(), width, height, kMotionFormat, L"DLSS-NR motion", kSrv);
    if (m_motion == nullptr) {
        Fail("the motion field could not be created");
        return false;
    }
    m_inputWidth = width;
    m_inputHeight = height;
    return true;
}

bool NgxNr::EnsureAnswer(uint32_t width, uint32_t height) {
    if (m_answer != nullptr && m_answerWidth == width && m_answerHeight == height) {
        return true;
    }

    m_answer.Reset();
    m_answerWidth = 0;
    m_answerHeight = 0;

    m_answer = CreateOwnedTexture(m_device.Get(), width, height,
                                  DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS-NR answer");
    if (m_answer == nullptr) {
        Fail("the DLSS-NR answer texture could not be created");
        return false;
    }
    m_answerWidth = width;
    m_answerHeight = height;
    return true;
}

bool NgxNr::EnsureResultTexture(ID3D12Resource* destination, DXGI_FORMAT format) {
    const D3D12_RESOURCE_DESC destinationDesc = destination->GetDesc();
    const uint32_t width = (uint32_t)destinationDesc.Width;
    const uint32_t height = destinationDesc.Height;
    if (m_result8 != nullptr && m_resultWidth == width && m_resultHeight == height &&
        m_resultFormat == format) {
        return true;
    }

    m_result8.Reset();
    m_resultWidth = 0;
    m_resultHeight = 0;
    m_resultFormat = DXGI_FORMAT_UNKNOWN;

    // Typeless of the format the caller works in, because the view the conversion writes through is
    // the non-sRGB member of that family and a view cannot be made of a resource in a format other
    // than the resource's own unless the resource is typeless.
    m_result8 = CreateOwnedTexture(m_device.Get(), width, height, TypelessFormat(format),
                                   L"DLSS-NR result");
    if (m_result8 == nullptr) {
        Fail("the DLSS-NR result texture could not be created");
        return false;
    }
    m_resultWidth = width;
    m_resultHeight = height;
    m_resultFormat = format;
    return true;
}

void NgxNr::ReleaseFeature(uint32_t eye) {
    if (m_feature[eye] == nullptr) {
        return;
    }
    NgxApi& api = SharedApi();
    if (api.shimRelease != nullptr && api.snippetRelease != nullptr) {
        GuardedReleaseFeature(api.shimRelease, api.snippetRelease, m_feature[eye]);
    }
    m_feature[eye] = nullptr;
    m_featureWidth[eye] = 0;
    m_featureHeight[eye] = 0;
}

bool NgxNr::EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height) {
    NgxApi& api = SharedApi();
    if (m_feature[m_eye] != nullptr && m_featureWidth[m_eye] == width && m_featureHeight[m_eye] == height) {
        return true;
    }

    if (api.snippetCreate == nullptr || api.shimCreate == nullptr) {
        m_failed = true;
        Fail("DLSS-NR does not have a feature-creation entry point");
        return false;
    }

    ReleaseFeature(m_eye);

    // The size and the preset are not set here: they belong to the same group as the picture and the
    // answer, and the caller sets that whole group before asking for the feature, so that the
    // description the feature is created against is complete.
    NVSDK_NGX_Result result{};
    if (!GuardedCreateFeature(api.shimCreate,
                              api.snippetCreate,
                              cmd,
                              NVSDK_NGX_Feature_Reserved18 /* DLSS-NR */,
                              m_parameter,
                              &m_feature[m_eye],
                              &result)) {
        m_failed = true;
        Fail("NVSDK_NGX_D3D12_CreateFeature faulted; DLSS-NR is not available in this process");
        return false;
    }
    if (NVSDK_NGX_FAILED(result)) {
        m_failed = true;
        Fail(NgxError("NVSDK_NGX_D3D12_CreateFeature", result));
        return false;
    }

    m_featureWidth[m_eye] = width;
    m_featureHeight[m_eye] = height;
    LayerLog("NgxNr: eye %u feature up at %ux%u\n", m_eye, width, height);
    return true;
}

bool NgxNr::RecordProducer(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* source,
                           DXGI_FORMAT sourceViewFormat,
                           const NrConvert& convert,
                           const NrMotion& motion) {
    if (m_parameter == nullptr || m_failed) {
        m_lastError = m_failed ? "DLSS-NR is not available" : "not initialized";
        return false;
    }

    const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
    const uint32_t width = (uint32_t)sourceDesc.Width;
    const uint32_t height = sourceDesc.Height;
    if (width == 0 || height == 0) {
        m_lastError = "empty crop";
        return false;
    }

    // The size the network works at. It is the crop's own size -- the network is not a scaler here,
    // the upscale is this layer's -- so the conversion below is a straight read, one pixel per pixel,
    // rather than a resample.
    const uint32_t networkWidth = convert.destinationWidth != 0 ? convert.destinationWidth : width;
    const uint32_t networkHeight = convert.destinationHeight != 0 ? convert.destinationHeight : height;

    NoteDeviceState(m_device.Get(), "at the top of the neural pass");

    if (!EnsureConvertPipeline() || !EnsureInput(networkWidth, networkHeight) ||
        !EnsureAnswer(networkWidth, networkHeight)) {
        return false;
    }
    NoteDeviceState(m_device.Get(), "after the network's own textures are made");

    // ---- a picture that arrives after a long stall ----
    //
    // A loading screen or a menu is written to the same eye buffer with no motion between the two
    // pictures, so the history's motion field warps nothing and the first frames after the gap are
    // the old picture fading out of the new one. Half a second is the threshold the AMD build uses:
    // far longer than any frame this layer runs at, and far shorter than a screen transition.
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastFrameAtValid) {
            const double gapMs = std::chrono::duration<double, std::milli>(now - m_lastFrameAt).count();
            if (gapMs > 500.0) {
                InvalidateHistory();
                LayerLog("NgxNr: %.0f ms since the last frame, the history is dropped\n", gapMs);
            }
        }
        m_lastFrameAt = now;
        m_lastFrameAtValid = true;
    }

    // ---- colour crop -> fp16 network input ----
    //
    // The network is handed a half-float picture of this layer's own, not the crop as it stands. The
    // crop is a texture opened from the other API and carries no unordered-access flag, which is a
    // flag the feature wants on both the picture it reads and the answer it writes, and half float is
    // the format both working integrations convert into at this point. The conversion is this layer's
    // own shader -- the same one the consumer uses to put the answer back into the eye.
    Transition(cmd, m_inputFp16.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RecordConvert(cmd,
                  source,
                  sourceViewFormat,
                  nullptr,
                  sourceViewFormat,
                  m_inputFp16.Get(),
                  DXGI_FORMAT_R16G16B16A16_FLOAT,
                  convert,
                  NgxFilter{});
    Transition(cmd, m_inputFp16.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    NoteDeviceState(m_device.Get(), "after the crop was converted to fp16");

    // ---- where every pixel of that input was in the previous frame ----
    //
    // Recorded for the layer's own anti-flicker filter and for nothing else: the feature is handed no
    // motion vectors at all (see the Reset parameter below), and the filter is the only reader this
    // field has. With the filter off the dispatch is skipped entirely, which is what makes the field
    // free by default.
    const NrSettings live = NrSettingsGet();
    if (live.antiFlicker > 0.f && motion.valid) {
        if (!RecordMotion(cmd, motion, convert.destinationWidth, convert.destinationHeight)) {
            return false;
        }
    }

    // Everything the feature reads and writes is named before it is asked to create anything, not
    // only before it is asked to run. Both working integrations set the picture, the answer and the
    // back buffer ahead of CreateFeature, and a feature that is created without them has nothing to
    // size its own working textures from -- which leaves the first neural pass running against a
    // description that was never complete.
    //
    // The answer is named twice: as the output the network writes, and as the back buffer it is told
    // it is working towards.
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Width, (unsigned int)networkWidth);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Height, (unsigned int)networkHeight);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
    // The style and the four strengths under it choose and weight the kernels the feature runs. Neither
    // working integration leaves any of them unwritten: a key that was never set is not read back as
    // some default, it is read back as whatever the parameter object happened to be holding. The panel
    // drives them now, under the feature's own names -- the same numbers OptiScaler's menu shows on
    // this backend -- so a setting that reads the same on either host is the same picture.
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Style, (int)live.controlStyle);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, live.intensity);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, live.controlTone);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, live.controlStructure);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, live.controlSkin);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, live.controlMask ? 1 : 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, 1);

    // ---- what the panel changed since the last frame ----
    //
    // The values above are written every frame, so a change is heard on the next one either way. A
    // rebuild is still forced when one of them moves, because a feature is allowed to have baked a
    // value at creation and this is the only way to be sure it is not -- the cost is one hitch on the
    // frame the panel's write lands, not one per frame. The release waits for the eye it belongs to:
    // this producer names one eye, and the other eye's feature is released by its own producer on the
    // same frame.
    const float controls[6] = {live.intensity,
                               live.controlTone,
                               live.controlStructure,
                               live.controlSkin,
                               live.controlStyle,
                               live.controlMask ? 1.f : 0.f};
    bool changed = !m_controlsValid;
    for (int i = 0; i < 6; ++i) {
        changed = changed || controls[i] != m_controls[i];
    }
    if (changed) {
        const bool firstTime = !m_controlsValid;
        for (int i = 0; i < 6; ++i) {
            m_controls[i] = controls[i];
        }
        m_controlsValid = true;
        if (!firstTime) {
            m_rebuildFeature[0] = true;
            m_rebuildFeature[1] = true;
            LayerLog("NgxNr: model-strength controls changed (intensity %.2f, tone %.2f, structure %.2f, "
                     "skin %.2f, style %.2f, native mask %s) -- the feature is rebuilt\n",
                     live.intensity,
                     live.controlTone,
                     live.controlStructure,
                     live.controlSkin,
                     live.controlStyle,
                     live.controlMask ? "on" : "off");
        }
    }
    if (m_rebuildFeature[m_eye]) {
        m_rebuildFeature[m_eye] = false;
        ReleaseFeature(m_eye);
    }
    // The one scalar that is a size: it is what the feature scales its own working set by, and both
    // integrations pin it at exactly 1.0.
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_ScalingRatio, 1.0f);
    // Every frame is treated as a fresh picture. The feature keeps a history of its own, but no
    // motion vectors are handed to it -- this layer denoises eye buffers the application has already
    // finished -- so that history has nothing to be reprojected with, and reusing it would smear
    // whatever moved. The reference resets in its no-motion-vector mode for the same reason.
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, 1);

    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Color, m_inputFp16.Get());
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, (unsigned int)networkWidth);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, (unsigned int)networkHeight);

    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Output, m_answer.Get());
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_Backbuffer, m_answer.Get());
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, (unsigned int)networkWidth);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, (unsigned int)networkHeight);

    // There is no motion field, and the absence is written out in full rather than partly implied:
    // with no resource to read, the region that resource would have been read over has to be empty as
    // well. Both integrations zero the subrect here, and the player does not write the key at all --
    // neither of them declares a rectangle beside a null handle, which is a request to read a
    // rectangle of nothing.
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, (ID3D12Resource*)nullptr);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, 1.0f);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, 1.0f);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, 0);
    m_parameter->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, 0);

    // Read once, on the first frame. Leaving a stage out is the only way left to find which one the
    // runtime objects to on a machine that has no D3D12 debug layer: the pass is taken apart with
    // both switches off, with the answer's own recording left out, and with the feature left out
    // altogether, and the run in which the device survives names the stage.
    static const bool skipFeature = DiagnosticSwitch("AMDNR_XR_NR_SKIP_FEATURE");
    static const bool skipEvaluate = DiagnosticSwitch("AMDNR_XR_NR_SKIP_EVALUATE");
    if (skipFeature) {
        static bool said = false;
        if (!said) {
            said = true;
            LayerLog("NgxNr: AMDNR_XR_NR_SKIP_FEATURE is on -- no feature is created and the network "
                     "is never asked to run, so this pass is the conversion alone\n");
        }
    } else if (!EnsureFeature(cmd, networkWidth, networkHeight)) {
        return false;
    }
    if (!skipFeature) {
        NoteDeviceState(m_device.Get(), "after CreateFeature");
    }

    // A DLSS output is written as a UAV and read as an SRV, so the answer is parked back in COMMON
    // between the two lists rather than left in whichever state the feature finished in.
    Transition(cmd, m_answer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NoteDeviceState(m_device.Get(), "after the answer went to UAV");

    if (skipFeature || skipEvaluate) {
        if (skipEvaluate && !skipFeature) {
            static bool said = false;
            if (!said) {
                said = true;
                LayerLog("NgxNr: AMDNR_XR_NR_SKIP_EVALUATE is on -- the feature is created but never "
                         "asked to run, so the network's own recording is left out of this list\n");
            }
        }
        Transition(cmd, m_answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Transition(cmd, m_inputFp16.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        m_produced = true;
        return true;
    }

    NVSDK_NGX_Result result{};
    const bool evaluated = GuardedEvaluateFeature(SharedApi().shimEvaluate,
                                                  SharedApi().snippetEvaluate,
                                                  cmd,
                                                  m_feature[m_eye],
                                                  m_parameter,
                                                  &result);
    // The checkpoint sits between the feature's own recording and the barrier this layer records
    // after it, so the two are told apart: a loss reported here belongs to what the feature wrote
    // into the list, and one reported below belongs to what this layer wrote around it.
    NoteDeviceState(m_device.Get(), "the moment EvaluateFeature returned");
    Transition(cmd, m_answer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    // The input is handed back in the state it was found in. It is left readable for the feature,
    // which reads it off this list when the list finally runs, and then returned to COMMON here --
    // while the next frame's conversion wants to write it again from COMMON, and a barrier that
    // claims COMMON for a texture sitting in NON_PIXEL_SHADER_RESOURCE is one the runtime objects to.
    Transition(cmd, m_inputFp16.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    NoteDeviceState(m_device.Get(), "after EvaluateFeature");

    if (!evaluated) {
        m_failed = true;
        Fail("NVSDK_NGX_D3D12_EvaluateFeature faulted; DLSS-NR is not available in this process");
        return false;
    }
    if (NVSDK_NGX_FAILED(result)) {
        m_failed = true;
        Fail(NgxError("NVSDK_NGX_D3D12_EvaluateFeature", result));
        return false;
    }

    m_produced = true;
    return true;
}

bool NgxNr::Launch() {
    // The answer already exists: the feature ran while the producer's list was recorded, and the
    // caller waited for that list before reading it.
    return m_parameter != nullptr && !m_failed;
}

bool NgxNr::Enqueue() {
    if (m_parameter == nullptr || m_failed) {
        return false;
    }

    // Only the asynchronous path gets here, and it submits the producer's list without waiting for
    // it. Signalling behind that list gives WaitAnswer something real to wait on; the reference's
    // no-op works there only because its producer list is executed and drained in the same call.
    if (m_answerFence == nullptr) {
        if (FAILED(m_device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_answerFence.ReleaseAndGetAddressOf())))) {
            m_lastError = "answer fence creation failed";
            return false;
        }
        m_answerEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_answerEvent == nullptr) {
            m_lastError = "answer event creation failed";
            return false;
        }
    }

    m_answerValue++;
    if (FAILED(m_queue->Signal(m_answerFence.Get(), m_answerValue))) {
        m_lastError = "answer fence signal failed";
        return false;
    }
    return true;
}

bool NgxNr::WaitAnswer() {
    if (m_parameter == nullptr || m_failed) {
        return false;
    }
    if (m_answerFence == nullptr || m_answerFence->GetCompletedValue() >= m_answerValue) {
        return true;
    }
    if (FAILED(m_answerFence->SetEventOnCompletion(m_answerValue, m_answerEvent))) {
        m_lastError = "answer fence wait could not be armed";
        return false;
    }
    if (WaitForSingleObject(m_answerEvent, 5000) != WAIT_OBJECT_0) {
        m_lastError = "the DLSS-NR answer did not finish in time";
        LayerLog("NgxNr: %s\n", m_lastError);
        return false;
    }
    return true;
}

bool NgxNr::EnsureConvertPipeline() {
    if (m_convertPipeline != nullptr) {
        return true;
    }

    D3D12_DESCRIPTOR_RANGE ranges[kConvertDescriptors]{};
    for (uint32_t i = 0; i < kConvertDescriptors; ++i) {
        ranges[i].NumDescriptors = 1;
        ranges[i].OffsetInDescriptorsFromTableStart = i;
    }
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].BaseShaderRegister = 1;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].BaseShaderRegister = 0;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].BaseShaderRegister = 2;
    ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[4].BaseShaderRegister = 3;
    ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[5].BaseShaderRegister = 1;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = kConvertConstants;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = kConvertDescriptors;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 2;
    signatureDesc.pParameters = parameters;
    signatureDesc.NumStaticSamplers = 1;
    signatureDesc.pStaticSamplers = &sampler;
    signatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureError;
    if (FAILED(D3D12SerializeRootSignature(&signatureDesc,
                                           D3D_ROOT_SIGNATURE_VERSION_1,
                                           signature.ReleaseAndGetAddressOf(),
                                           signatureError.ReleaseAndGetAddressOf()))) {
        Fail("D3D12SerializeRootSignature failed");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_convertRootSignature.ReleaseAndGetAddressOf())))) {
        Fail("CreateRootSignature failed");
        return false;
    }

    const auto compile = GetD3DCompile();
    if (compile == nullptr) {
        Fail("d3dcompiler_47.dll is unavailable");
        return false;
    }

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> shaderError;
    const HRESULT hr = compile(kConvertShader,
                               strlen(kConvertShader),
                               "NgxConvertCS",
                               nullptr,
                               nullptr,
                               "main",
                               "cs_5_0",
                               0,
                               0,
                               shader.ReleaseAndGetAddressOf(),
                               shaderError.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Fail(shaderError ? static_cast<const char*>(shaderError->GetBufferPointer()) : "shader compilation failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = m_convertRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
    pipelineDesc.CS.BytecodeLength = shader->GetBufferSize();
    const HRESULT pipelineResult = m_device->CreateComputePipelineState(
        &pipelineDesc, IID_PPV_ARGS(m_convertPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(pipelineResult)) {
        // The code is what tells the two readings apart, and they lead in opposite directions: a
        // device that has been taken away fails every call whatever is asked of it, while a
        // description the device refused is a defect in what is being asked for. The root signature
        // just above was created on this same device a moment ago, which is why the second reading is
        // the one to expect -- and the size of the blob is worth having beside it for the same
        // reason.
        const HRESULT removed = m_device->GetDeviceRemovedReason();
        // Static because Fail keeps the pointer rather than copying the text, and this buffer
        // outlives the call the same way a literal would.
        static char detail[192];
        sprintf_s(detail,
                  "CreateComputePipelineState failed (0x%08lX, device 0x%08lX, %llu byte shader)",
                  (unsigned long)pipelineResult,
                  (unsigned long)removed,
                  (unsigned long long)shader->GetBufferSize());
        Fail(detail);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kConvertDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_convertHeap.ReleaseAndGetAddressOf())))) {
        Fail("descriptor heap creation failed");
        return false;
    }

    return true;
}

bool NgxNr::EnsureMotionPipeline() {
    if (m_motionPipeline != nullptr) {
        return true;
    }

    // Two ranges in one table: the motion field the pass writes, and the depth it would read. This
    // backend has no depth and never binds one, but the slot stays -- the shader declares it, and a
    // descriptor table has to cover every register the shader names.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = kMotionConstants;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 2;
    signatureDesc.pParameters = parameters;
    signatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureError;
    if (FAILED(D3D12SerializeRootSignature(
            &signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.ReleaseAndGetAddressOf(), signatureError.ReleaseAndGetAddressOf()))) {
        Fail("motion root signature serialisation failed");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_motionRootSignature.ReleaseAndGetAddressOf())))) {
        Fail("motion root signature creation failed");
        return false;
    }

    const auto compile = GetD3DCompile();
    if (compile == nullptr) {
        Fail("d3dcompiler_47.dll is unavailable");
        return false;
    }

    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> shaderError;
    if (FAILED(compile(amdnr_fsr::kMotionShader,
                       strlen(amdnr_fsr::kMotionShader),
                       "NgxMotionCS",
                       nullptr,
                       nullptr,
                       "main",
                       "cs_5_0",
                       0,
                       0,
                       shader.ReleaseAndGetAddressOf(),
                       shaderError.ReleaseAndGetAddressOf()))) {
        Fail(shaderError ? static_cast<const char*>(shaderError->GetBufferPointer()) : "motion shader compilation failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = m_motionRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
    pipelineDesc.CS.BytecodeLength = shader->GetBufferSize();
    const HRESULT pipelineResult =
        m_device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(m_motionPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(pipelineResult)) {
        // Static because Fail keeps the pointer rather than copying the text, and this buffer outlives
        // the call the same way a literal would.
        static char detail[96];
        sprintf_s(detail, "motion pipeline creation failed (0x%08lX)", (unsigned long)pipelineResult);
        Fail(detail);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2; // the motion field, then the depth
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_motionHeap.ReleaseAndGetAddressOf())))) {
        Fail("motion descriptor heap creation failed");
        return false;
    }

    return true;
}

bool NgxNr::RecordMotion(ID3D12GraphicsCommandList* cmd,
                         const NrMotion& motion,
                         uint32_t width,
                         uint32_t height) {
    if (!EnsureMotionPipeline() || m_motion == nullptr) {
        return false;
    }

    const UINT descriptorStride =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE motionSlot = m_motionHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE depthSlot = motionSlot;
    depthSlot.ptr += descriptorStride;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kMotionFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_motion.Get(), nullptr, &uav, motionSlot);

    // This backend carries no depth, so the shader's parallax term is never taken -- its switch is the
    // near plane, which the constants below leave at zero. The slot still has to name a resource whose
    // format the view can describe, and a null descriptor has to describe the view it stands in for:
    // a NULL description beside a NULL resource would leave the slot a Texture2D<float> nobody
    // described.
    D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
    depthView.Format = DXGI_FORMAT_R32_FLOAT;
    depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthView.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(nullptr, &depthView, depthSlot);

    float constants[kMotionConstants]{};
    memcpy(constants, motion.matrix, sizeof(motion.matrix));
    constants[9] = motion.a[0];
    constants[10] = motion.a[1];
    constants[11] = motion.b[0];
    constants[12] = motion.b[1];
    constants[13] = motion.tanLeft;
    constants[14] = motion.tanWidth;
    constants[15] = motion.tanUp;
    constants[16] = motion.tanHeight;
    constants[17] = motion.extentScale[0];
    constants[18] = motion.extentScale[1];
    constants[19] = motion.originShift[0];
    constants[20] = motion.originShift[1];
    memcpy(&constants[21], &width, sizeof(width));
    memcpy(&constants[22], &height, sizeof(height));
    // 23..30 stay zero: the camera's travel, one over the near plane, and the depth's rectangle. Zero
    // is exactly the rotation-only reprojection the AMD build runs with no depth either, so the two
    // hosts produce the same field from the same shader -- the text above is shared between them.

    Transition(cmd, m_motion.Get(), kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {m_motionHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_motionRootSignature.Get());
    cmd->SetComputeRoot32BitConstants(0, kMotionConstants, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, m_motionHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(m_motionPipeline.Get());
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    Transition(cmd, m_motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);
    return true;
}

bool NgxNr::EnsureFilterTextures(uint32_t width, uint32_t height) {
    if (m_filterHistory[0] != nullptr && m_filterHistory[1] != nullptr && m_filterWidth == width &&
        m_filterHeight == height) {
        return true;
    }
    m_filterHistory[0].Reset();
    m_filterHistory[1].Reset();
    // Read as a shader resource and written as an unordered access target, alternating every frame.
    // At the start of a frame the one being read is the one written on the previous frame (UAV) and
    // the one being written is the one read on the previous frame (shader resource), so the pair is
    // created in those two states to match the invariant the consumer maintains.
    m_filterHistory[0] = CreateOwnedTexture(m_device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                            L"DLSS-NR anti-flicker history 0", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_filterHistory[1] = CreateOwnedTexture(m_device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                            L"DLSS-NR anti-flicker history 1", kSrv);
    if (!m_filterHistory[0] || !m_filterHistory[1]) {
        Fail("anti-flicker history creation failed");
        return false;
    }
    // `m_filterValid` is what keeps the first frame's read from being believed: the pair is in the
    // right states, but what it holds is not about this image yet.
    m_filterWidth = width;
    m_filterHeight = height;
    m_filterRead = 0;
    InvalidateHistory();
    LayerLog("NgxNr: anti-flicker history %ux%u\n", width, height);
    return true;
}

void NgxNr::InvalidateHistory() {
    m_filterValid = false;
    m_filterResets++;
}

void NgxNr::ProbeFilterHistory(ID3D12GraphicsCommandList* cmd, uint32_t written) {
    if (m_filterHistory[written] == nullptr) {
        return;
    }
    const uint32_t side = std::min({kFilterProbeSide, m_filterWidth, m_filterHeight});
    if (side < 8) {
        return;
    }
    const uint32_t rowPitch = ((side * 8u) + 255u) / 256u * 256u;
    if (m_filterProbe == nullptr || m_filterProbeWidth != side) {
        m_filterProbe.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = uint64_t(rowPitch) * side;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&heap,
                                                     D3D12_HEAP_FLAG_NONE,
                                                     &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr,
                                                     IID_PPV_ARGS(m_filterProbe.ReleaseAndGetAddressOf())))) {
            // A probe that cannot be allocated is not worth failing a frame over: the filter itself is
            // what the frame is for, and this only ever answers a question about it.
            return;
        }
        m_filterProbeWidth = side;
        m_filterProbeHeight = side;
    }

    // The middle of the window, which is the region the eye is actually on.
    const uint32_t left = (m_filterWidth - side) / 2;
    const uint32_t top = (m_filterHeight - side) / 2;
    Transition(cmd,
               m_filterHistory[written].Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_filterHistory[written].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = m_filterProbe.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    target.PlacedFootprint.Footprint.Width = side;
    target.PlacedFootprint.Footprint.Height = side;
    target.PlacedFootprint.Footprint.Depth = 1;
    target.PlacedFootprint.Footprint.RowPitch = rowPitch;
    const D3D12_BOX box{left, top, 0, left + side, top + side, 1};
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
    Transition(cmd,
               m_filterHistory[written].Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_filterProbePending = true;
    m_filterProbeFrame = m_filterFrames;
}

void NgxNr::ReadFilterProbe() {
    if (m_filterProbe == nullptr || m_filterProbeWidth == 0) {
        return;
    }
    const uint32_t side = m_filterProbeWidth;
    const uint32_t rowPitch = ((side * 8u) + 255u) / 256u * 256u;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, size_t(rowPitch) * side};
    if (FAILED(m_filterProbe->Map(0, &range, &mapped))) {
        return;
    }
    const uint16_t* texels = static_cast<const uint16_t*>(mapped);
    const size_t stride = rowPitch / 2; // in 16 bit words
    double valid = 0.0;
    double magnitude = 0.0;
    double accumulation = 0.0;
    const double sampled = double(side) * double(side);
    for (uint32_t y = 0; y < side; ++y) {
        const uint16_t* row = texels + size_t(y) * stride;
        for (uint32_t x = 0; x < side; ++x) {
            const uint16_t* texel = row + size_t(x) * 4;
            // The alpha lane is what the shader calls validity: 0 outside the window, and 1 or more
            // inside -- see the accumulation count in the filter. Below one there is no correction of
            // this pixel's own to speak of.
            const float alpha = HalfToFloat(texel[3]);
            if (alpha <= 0.5f) {
                continue;
            }
            magnitude += (std::fabs(HalfToFloat(texel[0])) + std::fabs(HalfToFloat(texel[1])) +
                          std::fabs(HalfToFloat(texel[2]))) / 3.0;
            accumulation += alpha;
            valid += 1.0;
        }
    }
    m_filterProbe->Unmap(0, nullptr);

    NrFilterReport report = NrFilterReportGet();
    report.validShare = sampled > 0.0 ? valid / sampled : 0.0;
    report.magnitude = valid > 0.0 ? magnitude / valid : 0.0;
    report.accumulation = valid > 0.0 ? accumulation / valid : 0.0;
    report.sampleFrame = m_filterProbeFrame;
    report.frames = m_filterFrames;
    report.resets = m_filterResets;
    report.on = true;
    NrFilterReportSet(report);
    // The accumulation is the number that proves the read: a fresh write is 1.0 and only a frame that
    // actually blended against the previous history can write more, so a mean above 1.0 here cannot
    // be produced by a filter whose history was never read back.
    LayerLog("NgxNr: anti-flicker probe: %ux%u box of the history, %.1f%% of it carried a correction, "
             "mean |correction| %.5f, mean accumulation %.2f frames (sampled on filter frame %llu)\n",
             side,
             side,
             report.validShare * 100.0,
             report.magnitude,
             report.accumulation,
             m_filterProbeFrame);
}

void NgxNr::RecordConvert(ID3D12GraphicsCommandList* cmd,
                          ID3D12Resource* source,
                          DXGI_FORMAT sourceViewFormat,
                          ID3D12Resource* original,
                          DXGI_FORMAT originalViewFormat,
                          ID3D12Resource* destination,
                          DXGI_FORMAT destinationFormat,
                          const NrConvert& convert,
                          const NgxFilter& filter) {
    const UINT increment = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE start = m_convertHeap->GetCPUDescriptorHandleForHeapStart();
    const auto slot = [&](uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
        handle.ptr += increment * index;
        return handle;
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    srv.Format = sourceViewFormat;
    m_device->CreateShaderResourceView(source, &srv, slot(0));

    // Every descriptor in the table has to name a real resource even when the shader does not read
    // it, so a missing original falls back to the source.
    srv.Format = originalViewFormat;
    m_device->CreateShaderResourceView(original != nullptr ? original : source, &srv, slot(1));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = WritableFormat(destinationFormat);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(destination, nullptr, &uav, slot(2));

    // Motion and history, both consumer-only. Every descriptor in the table still has to name a
    // resource whose format the view can describe, so a frame with the filter off (and every frame
    // of the producer) points them at the source and the destination under their own formats.
    if (filter.on && m_motion != nullptr) {
        srv.Format = kMotionFormat;
        m_device->CreateShaderResourceView(m_motion.Get(), &srv, slot(3));
    } else {
        srv.Format = sourceViewFormat;
        m_device->CreateShaderResourceView(source, &srv, slot(3));
    }
    if (filter.on && m_filterHistory[m_filterRead] != nullptr) {
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_device->CreateShaderResourceView(m_filterHistory[m_filterRead].Get(), &srv, slot(4));
    } else {
        srv.Format = sourceViewFormat;
        m_device->CreateShaderResourceView(source, &srv, slot(4));
    }
    if (filter.on && m_filterHistory[1 - m_filterRead] != nullptr) {
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_device->CreateUnorderedAccessView(m_filterHistory[1 - m_filterRead].Get(), nullptr, &uav, slot(5));
    } else {
        uav.Format = WritableFormat(destinationFormat);
        m_device->CreateUnorderedAccessView(destination, nullptr, &uav, slot(5));
    }

    uint32_t constants[kConvertConstants]{};
    constants[0] = convert.sourceWidth;
    constants[1] = convert.sourceHeight;
    constants[2] = convert.destinationWidth;
    constants[3] = convert.destinationHeight;
    constants[4] = convert.encodeSrgb ? 1u : 0u;
    constants[5] = convert.blend ? 1u : 0u;
    constants[6] = convert.feather;
    memcpy(&constants[7], &convert.gain, sizeof(convert.gain));
    memcpy(&constants[8], &convert.gamma, sizeof(convert.gamma));
    memcpy(&constants[9], &convert.shiftX, sizeof(convert.shiftX));
    memcpy(&constants[10], &convert.shiftY, sizeof(convert.shiftY));
    memcpy(&constants[11], &convert.roundness, sizeof(convert.roundness));
    // The filter never runs outside the consumer: the producer writes the network's input, and a
    // history has nothing to say about that.
    const bool filterActive = filter.on && convert.blend && m_filterHistory[0] != nullptr;
    constants[12] = filterActive ? 1u : 0u;
    memcpy(&constants[13], &filter.strength, sizeof(filter.strength));
    memcpy(&constants[14], &filter.gate, sizeof(filter.gate));
    constants[15] = (filterActive && filter.historyValid) ? 1u : 0u;

    ID3D12DescriptorHeap* heaps[] = {m_convertHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_convertRootSignature.Get());
    cmd->SetComputeRoot32BitConstants(0, kConvertConstants, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, m_convertHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(m_convertPipeline.Get());
    cmd->Dispatch((convert.destinationWidth + 7) / 8, (convert.destinationHeight + 7) / 8, 1);
}

bool NgxNr::RecordConsumer(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* destination,
                           DXGI_FORMAT destinationFormat,
                           ID3D12Resource* original,
                           DXGI_FORMAT originalViewFormat,
                           const NrConvert& convert) {
    if (m_parameter == nullptr || !m_produced || m_answer == nullptr) {
        m_lastError = m_produced ? "not initialized" : "nothing was produced";
        return false;
    }
    NoteDeviceState(m_device.Get(), "before the convert pipeline");
    if (!EnsureConvertPipeline()) {
        return false;
    }

    if (!EnsureResultTexture(destination, destinationFormat)) {
        return false;
    }

    // The previous frame's probe, if one is waiting: that frame's command list was submitted and
    // waited on before this call, so the copy it recorded is complete and mapping cannot stall.
    if (m_filterProbePending) {
        ReadFilterProbe();
        m_filterProbePending = false;
    }

    // The answer comes back half float and at the network's own size, so this is the conversion that
    // turns it into the eight bit picture the eye wants -- the same shader the producer ran to get
    // the crop the other way. The size is read from the answer itself rather than from what was asked
    // for, so a session that resized between the two lists still reads the texture it actually has.
    NrConvert toEye = convert;
    toEye.sourceWidth = m_answerWidth;
    toEye.sourceHeight = m_answerHeight;
    toEye.blend = true;

    // ---- the anti-flicker filter, if the panel turned it on ----
    //
    // Everything it needs is already on this list: the answer (read as a shader resource below), the
    // crop it is a correction over, and the motion field the producer recorded. A live change of
    // strength or gate starts a clean history rather than mixing two settings; the off state clears
    // the pair as well, so turning the filter back on after a gap cannot blend against a history that
    // describes a picture several frames old. The history pair is ping-ponged, so the texture read
    // here is the one written on the previous frame, and the write goes to the other one.
    const NrSettings filterSettings = NrSettingsGet();
    NgxFilter filter{};
    filter.on = filterSettings.antiFlicker > 0.f;
    filter.strength = std::clamp(filterSettings.antiFlicker, 0.f, 1.f);
    filter.gate = std::max(filterSettings.antiFlickerGate, 1e-6f);
    if (filter.on && (filter.strength != m_filterStrength || filter.gate != m_filterGate)) {
        const bool firstTime = m_filterStrength < 0.f;
        m_filterStrength = filter.strength;
        m_filterGate = filter.gate;
        InvalidateHistory();
        LayerLog("NgxNr: anti-flicker %s (strength %.2f, gate %.4f)\n",
                 firstTime ? "on" : "reconfigured",
                 filter.strength,
                 filter.gate);
    } else if (!filter.on && m_filterStrength >= 0.f) {
        m_filterStrength = -1.f;
        m_filterGate = -1.f;
        LayerLog("NgxNr: anti-flicker off\n");
    }
    if (filter.on && !EnsureFilterTextures(convert.destinationWidth, convert.destinationHeight)) {
        return false;
    }
    filter.historyValid = filter.on && m_filterValid;

    if (filter.on) {
        // One texture is sampled this frame and the other is written, and the roles swap every frame,
        // so each frame flips their states: the one being read comes back from being written (UAV ->
        // shader resource), the one being written goes the other way. Both are in the state the other
        // role left them in, which is the whole reason the pair exists rather than one texture.
        Transition(cmd,
                   m_filterHistory[m_filterRead].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   kSrv);
        Transition(cmd,
                   m_filterHistory[1 - m_filterRead].Get(),
                   kSrv,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // The conversion lands on this layer's own texture, not on the caller's. That one is shared with
    // the other API and carries no unordered-access flag -- a texture D3D11 made for reading is not
    // one a compute shader may write through a UAV, and a view asking to write it is a call the
    // runtime refuses and takes the device down over. The result is copied across afterwards, which
    // is what the AMD build has always done and what this one had lost.
    Transition(cmd, m_answer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (original != nullptr) {
        // The feather reads the region as it was before the pass, so the crop has to be readable here
        // as well as in the producer, and by the time this runs it is back in COMMON.
        Transition(cmd, original, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Transition(cmd, m_result8.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RecordConvert(cmd,
                  m_answer.Get(),
                  DXGI_FORMAT_R16G16B16A16_FLOAT,
                  original,
                  originalViewFormat,
                  m_result8.Get(),
                  destinationFormat,
                  toEye,
                  filter);
    Transition(cmd, m_result8.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (filter.on) {
        // Read next frame as what this one wrote, and swapped here rather than at the top so the two
        // states above always describe the pair the way the shader saw it.
        m_filterRead = 1 - m_filterRead;
        m_filterValid = true;
        m_filterFrames++;
        // The counters travel to the report every frame rather than only when a probe runs: "已累积 N
        // 帧" is the cheapest honest sign that the filter is live, and a number that only moved once
        // a minute would read as a stalled one. The sampled values stay the probe's to write.
        NrFilterReport counters = NrFilterReportGet();
        counters.on = true;
        counters.frames = m_filterFrames;
        counters.resets = m_filterResets;
        NrFilterReportSet(counters);
        // Sample the history once it has settled, and then rarely. The probe is what says whether the
        // history this filter writes is real, so it is worth the occasional copy -- but it is a
        // question, not a per-frame need.
        if (!m_filterProbePending && (m_filterFrames == 3 || (m_filterFrames % 1800) == 0)) {
            ProbeFilterHistory(cmd, m_filterRead);
        }
    } else {
        // A sample taken while the filter was on must not keep reading as live after it is switched
        // off, and the panel keys its readout off exactly that. Written only on the transition.
        NrFilterReport counters = NrFilterReportGet();
        if (counters.on) {
            counters.on = false;
            NrFilterReportSet(counters);
        }
    }

    if (original != nullptr) {
        Transition(cmd, original, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    Transition(cmd, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(destination, m_result8.Get());
    Transition(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

    Transition(cmd, m_result8.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmd, m_answer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    m_produced = false;
    return true;
}

bool NgxNr::RecordShiftSearch(ID3D12GraphicsCommandList* cmd,
                              ID3D12Resource* left,
                              DXGI_FORMAT leftFormat,
                              ID3D12Resource* right,
                              DXGI_FORMAT rightFormat,
                              uint32_t width,
                              uint32_t height,
                              const NrShiftSearch& search) {
    (void)cmd;
    (void)left;
    (void)leftFormat;
    (void)right;
    (void)rightFormat;
    (void)width;
    (void)height;
    (void)search;
    // DLSS-NR is run for each eye on its own, so there is no detail to carry from one to the other.
    m_lastError = "no cross-eye detail transfer on the DLSS-NR backend";
    return false;
}

bool NgxNr::TakeShiftSearch(const NrShiftSearch& search, int32_t& bestShift) {
    (void)search;
    (void)bestShift;
    return false;
}

bool NgxNr::RecordTransfer(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* destination,
                           DXGI_FORMAT destinationFormat,
                           ID3D12Resource* rightCrop,
                           DXGI_FORMAT rightCropFormat,
                           ID3D12Resource* leftOutput,
                           DXGI_FORMAT leftOutputFormat,
                           ID3D12Resource* leftCrop,
                           DXGI_FORMAT leftCropFormat,
                           const NrTransfer& transfer,
                           const NrConvert& convert) {
    (void)cmd;
    (void)destination;
    (void)destinationFormat;
    (void)rightCrop;
    (void)rightCropFormat;
    (void)leftOutput;
    (void)leftOutputFormat;
    (void)leftCrop;
    (void)leftCropFormat;
    (void)transfer;
    (void)convert;
    m_lastError = "no cross-eye detail transfer on the DLSS-NR backend";
    return false;
}
