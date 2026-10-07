#include "NrCore.h"

#include "LmxxfNrApi.h"
#include "NrSettings.h"

#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

    constexpr DXGI_FORMAT kFp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // The runtime asks for two float channels for the motion field.
    constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R32G32_FLOAT;
    constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // matrix, a, b, four tangents, extent scale, origin shift, width, height, the camera's
    // translation, one over the near plane, and the depth's rectangle inside the eye image.
    constexpr uint32_t kMotionConstants = 31;

    // Source/destination sizes, encode and blend flags, feather width, roundness, gain, gamma, shift,
    // then the anti-flicker filter: on, strength, gate, whether a history exists yet.
    constexpr uint32_t kConvertConstants = 16;

    // The controls travel in the frame only when one of them is off the shipped 1: the runtime reads
    // them only with the flag, and its documentation is explicit that the flag must not be sent for
    // the built-in values. Judged on the four values the frame would carry rather than on the menu
    // values behind them, because that is what the runtime sees. Used both where the frame is built
    // and where the status line reports what went out, so the two can never drift apart.
    bool ControlsOffShipped(const NrSettings& settings) {
        const NrControlWire wire = NrControlWireOf(settings);
        return settings.stylePinned ||
               wire.tone != 1.f || wire.structure != 1.f ||
               wire.skin != 1.f || wire.other != 1.f ||
               settings.controlStyle != 1.f;
    }

    // Reads the integer that follows `key=` in the runtime's status line, or 0 if the key is absent.
    // The line is a flat list of `key=value` pairs, so a scan that stops at the first non-digit after
    // the key is enough: this does not have to survive a malformed line, only to not read past its end.
    uint32_t StatusField(const char* status, const char* key) {
        const char* at = std::strstr(status, key);
        if (at == nullptr) {
            return 0;
        }
        return (uint32_t)std::strtoul(at + std::strlen(key), nullptr, 10);
    }
    // source, original, destination, motion, history in, history out.
    constexpr uint32_t kConvertDescriptors = 6;

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

    // Resamples one texture into another. Both are read as linear light, so an sRGB view decodes on
    // the way in; `g_encode_srgb` re-encodes on the way out.
    //
    // `g_blend` turns the pass into the consumer: the network's answer is bent back towards the
    // source's tone (the network runs a little dark), then mixed with the untouched original over a
    // soft border. Without the border the edge of the window is a straight line between filtered and
    // unfiltered pixels, which is what makes the window conspicuous.
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

    // The motion field the temporal path reprojects its history with, written a pixel at a time from
    // the two view poses.
    //
    // Nothing here needs depth. A pixel names a view ray; rotating that ray by the head's own
    // rotation lands it in the previous frame's view, and projecting it back gives the previous
    // pixel. That is exact for rotation, which is the motion a headset actually produces, and it is
    // the reason a layer can drive a temporal upscaler at all without the application's help.
    //
    // The window's own travel is folded in as a constant offset: the layer slides the neural window
    // on a coarse grid, and without it the history would appear to shift every time the window did.
    const char* kMotionShader = R"(
// Every member is a scalar on purpose. A constant buffer packs a vector so that it never straddles a
// 16 byte boundary, so a float2 anywhere but the right offset would silently shift every field after
// it -- and a motion field that is quietly wrong looks like smearing rather than like a bug.
cbuffer Motion : register(b0)
{
    float g_m0; float g_m1; float g_m2;
    float g_m3; float g_m4; float g_m5;
    float g_m6; float g_m7; float g_m8;
    float g_a_x; float g_a_y;
    float g_b_x; float g_b_y;
    float g_tan_left; float g_tan_width; float g_tan_up; float g_tan_height;
    float g_ext_x; float g_ext_y;
    float g_org_x; float g_org_y;
    uint g_width; uint g_height;
    float g_tx; float g_ty; float g_tz;
    float g_inv_near;
    float g_dep_org_x; float g_dep_org_y;
    float g_dep_scl_x; float g_dep_scl_y;
};

RWTexture2D<float2> g_motion : register(u0);
Texture2D<float> g_depth : register(t0);

// Where a point in the previous frame's view space lands in the previous frame's image, as a
// fraction of it. The caller has already checked that the point is in front of the camera.
float2 PreviousImage(float3 p)
{
    const float denominator = -p.z;
    return float2((p.x / denominator - g_tan_left) / g_tan_width,
                  (g_tan_up - p.y / denominator) / g_tan_height);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_width || tid.y >= g_height)
    {
        return;
    }

    // Where this network pixel sits inside the view image, as a fraction of the image rectangle --
    // the same normalisation the frustum's half-tangents are expressed in. The window covers only
    // part of that rectangle, which is what makes this anything other than the pixel centre.
    const float2 s = float2(g_a_x, g_a_y) + (float2(tid.xy) + 0.5f) * float2(g_b_x, g_b_y);

    const float3 ray = float3(g_tan_left + s.x * g_tan_width, g_tan_up - s.y * g_tan_height, -1.0f);
    const float3 b = float3(g_m0 * ray.x + g_m1 * ray.y + g_m2 * ray.z,
                            g_m3 * ray.x + g_m4 * ray.y + g_m5 * ray.z,
                            g_m6 * ray.x + g_m7 * ray.y + g_m8 * ray.z);

    // A point that came out behind the camera has no previous pixel; a zero vector is the honest
    // answer and the reprojection simply samples where it already is.
    if (-b.z < 1e-4f)
    {
        g_motion[tid.xy] = float2(0.0f, 0.0f);
        return;
    }

    // The rotation-only answer. It treats every pixel as if it were at infinity, which is exact for
    // the rotation and wrong for the translation -- but it is never nonsense, so it is also what this
    // falls back to.
    float2 previous = PreviousImage(b);

    // How far away the surface under this pixel is, as 1/z. A head that moves sideways slides near
    // surfaces across the image further than far ones, and only a real distance can tell the two
    // apart. The depth arrives as inverse distance already scaled by the near plane, so dividing by
    // that plane turns it back into 1/z.
    //
    // Without a near plane -- which nothing in an OpenXR layer is told -- the term stays zero and the
    // reprojection is exactly what it was before there was a depth at all.
    if (g_inv_near > 0.0f)
    {
        const float2 uv = float2(g_dep_org_x + s.x * g_dep_scl_x, g_dep_org_y + s.y * g_dep_scl_y);
        uint depthWidth = 0;
        uint depthHeight = 0;
        g_depth.GetDimensions(depthWidth, depthHeight);
        const int2 coord = clamp(int2(uv * float2(depthWidth, depthHeight)),
                                 int2(0, 0),
                                 int2(depthWidth - 1, depthHeight - 1));
        const float invZ = g_depth.Load(int3(coord, 0)) * g_inv_near;

        // The point on the ray, carried into the previous frame's view. `b` is one unit of the ray per
        // unit of distance, so the camera's own travel contributes `translation * 1/z` and nothing
        // else.
        const float3 moved = float3(b.x + g_tx * invZ, b.y + g_ty * invZ, b.z + g_tz * invZ);

        if (-moved.z >= 1e-4f)
        {
            const float2 withDepth = PreviousImage(moved);

            // Past half a window is not a distance: a pixel's content cannot have travelled that far
            // between two frames of a head that is being worn. When the parallax says it has, the near
            // plane is not the one the application projected with, and the answer it produced is not
            // usable. This is not defensive for its own sake -- a motion field this wrong is written
            // into the temporal history, where each frame reprojects the previous mistake forward and
            // the window settles onto one stale picture it cannot leave. Ignoring the term for those
            // pixels keeps the field inside something the history can recover from.
            if (abs(withDepth.x - s.x) <= 0.5f && abs(withDepth.y - s.y) <= 0.5f)
            {
                previous = withDepth;
            }
        }
    }

    g_motion[tid.xy] = (previous - s) * float2(g_ext_x, g_ext_y) + float2(g_org_x, g_org_y);
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

    D3DCompileFn GetD3DCompile() {
        static D3DCompileFn compile = []() -> D3DCompileFn {
            const HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
            return module ? reinterpret_cast<D3DCompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
        }();
        return compile;
    }

    DXGI_FORMAT WritableFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
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

    DXGI_FORMAT ReadableFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return format;
        }
    }

    ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device,
                                         uint32_t width,
                                         uint32_t height,
                                         DXGI_FORMAT format,
                                         D3D12_RESOURCE_STATES state,
                                         bool allowUnorderedAccess,
                                         const wchar_t* name) {
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
        desc.Flags = allowUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

        ComPtr<ID3D12Resource> texture;
        if (FAILED(device->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())))) {
            return nullptr;
        }
        texture->SetName(name);
        return texture;
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

    // The runtime and its assets live in the staged build folder.
    const wchar_t* const kRuntimeDirectories[] = {
        L"C:\\Users\\Administrator\\vdxr\\xr-layer",
        L"C:\\Users\\Administrator\\vdxr\\bin\\x64\\Release",
    };

} // namespace

void NrCore::Fail(const char* message) {
    m_lastError = message;
    LayerLog("NrCore: %s\n", message);
}

bool NrCore::LoadRuntime() {
    std::wstring directory;
    for (const wchar_t* candidate : kRuntimeDirectories) {
        const std::wstring runtime = std::wstring(candidate) + L"\\LmxxfNrRuntime.dll";
        const std::wstring pak = std::wstring(candidate) + L"\\LmxxfNrRuntime.pak";
        if (GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(pak.c_str()) != INVALID_FILE_ATTRIBUTES) {
            directory = candidate;
            break;
        }
    }
    if (directory.empty()) {
        Fail("LmxxfNrRuntime.dll / .pak not found");
        return false;
    }

    const std::wstring runtimePath = directory + L"\\LmxxfNrRuntime.dll";
    m_assetsPath = directory + L"\\LmxxfNrRuntime.pak";

    SetDllDirectoryW(directory.c_str());
    m_module = LoadLibraryW(runtimePath.c_str());
    if (m_module == nullptr) {
        Fail("LmxxfNrRuntime.dll could not be loaded");
        return false;
    }

    const auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi*)>(
        GetProcAddress(m_module, "LmxxfNrGetApi"));
    if (getApi == nullptr) {
        Fail("LmxxfNrRuntime.dll does not export LmxxfNrGetApi");
        return false;
    }

    // Optional and older runtimes simply do not have it, which is why the status line treats a null
    // here as "do not report" rather than as an error: the pool is the runtime's own housekeeping, and
    // whether it is there is answered by the export, not by a version number read from a file.
    m_poolStats = reinterpret_cast<int32_t (*)(LmxxfNrImportPoolStats*)>(
        GetProcAddress(m_module, "LmxxfNrGetImportPoolStats"));

    m_api = new LmxxfNrApi{};
    m_api->struct_size = sizeof(LmxxfNrApi);
    if (getApi(LMXXF_NR_ABI_VERSION, m_api) != LMXXF_NR_OK || m_api->EnqueueHipAsync == nullptr ||
        m_api->AbandonJob == nullptr || m_api->OutputReady == nullptr) {
        Fail("runtime does not expose the asynchronous interface this layer drives");
        return false;
    }

    LmxxfNrCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    if (m_api->QueryCapabilities(&capabilities) == LMXXF_NR_OK) {
        if (capabilities.max_input_width) {
            m_maxWidth = capabilities.max_input_width;
        }
        if (capabilities.max_input_height) {
            m_maxHeight = capabilities.max_input_height;
        }
        // history_supported is the gate on the whole temporal path: a runtime built without it will
        // reject the flag or ignore the motion field, and that is worth knowing before believing any
        // measurement taken with motion vectors attached.
        LayerLog("NrCore: runtime ready (gfx1201=%u hip_ready=%u history=%u overlap=%u, input ceiling %ux%u)\n",
                 capabilities.gfx1201_target,
                 capabilities.hip_ready,
                 capabilities.history_supported,
                 capabilities.overlap_supported,
                 m_maxWidth,
                 m_maxHeight);
    }

    LmxxfNrCreateInfo createInfo{};
    createInfo.struct_size = sizeof(createInfo);
    createInfo.device = m_device.Get();
    createInfo.queue = m_queue.Get();
    createInfo.assets_directory = m_assetsPath.c_str();
    createInfo.flags = 0;

    void* context = nullptr;
    if (m_api->Create(&createInfo, &context) != LMXXF_NR_OK || context == nullptr) {
        Fail("the runtime refused to start with the packaged assets");
        return false;
    }

    // Nothing else is configured here: the deployed runtime exports LmxxfNrGetApi and nothing else, so
    // there is no tier policy to hand it and nothing below 1280x720 buys a cheaper network.
    if (m_api->PrepareSession(context) != LMXXF_NR_OK) {
        m_api->Destroy(context);
        Fail("PrepareSession failed");
        return false;
    }
    m_context = context;

    LayerLog("NrCore: neural session up\n");
    return true;
}

bool NrCore::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (m_context != nullptr) {
        return true;
    }

    m_device = device;
    m_queue = queue;

    char setting[32]{};
    if (GetEnvironmentVariableA("AMDNR_XR_STRENGTH", setting, (DWORD)sizeof(setting)) > 0) {
        m_strength = std::clamp((float)atof(setting), 0.f, 1.f);
    }
    // Three switches were removed from here rather than kept as decoration. AMDNR_XR_PASSES and
    // AMDNR_XR_DEBUG_VIEW seeded members nothing ever read -- both values reach the frame from the live
    // settings block, and the pass count's real source is the config file the panel writes.
    // AMDNR_XR_FULL_NETWORK set a flag the runtime's own header calls a no-op (0.41 runs all 71 blocks
    // unless DLSS5_SKIP_BLOCKS says otherwise). AMDNR_XR_SMALL_TIERS needed an export this runtime does
    // not have. A switch that changes nothing is worse than no switch: it reads as evidence.
    if (GetEnvironmentVariableA("AMDNR_XR_TEMPORAL", setting, (DWORD)sizeof(setting)) > 0 && setting[0] != '0') {
        m_temporal = true;
    }
    // AMDNR_XR_OUTPUT_SMOOTH and AMDNR_XR_SMOOTH_THRESHOLD used to be read here and into the frame's
    // output_smooth fields. They were removed rather than kept as decoration: the runtime this layer
    // loads reads neither the fields nor those keys -- the whole implementation is absent from it
    // (its only copy lives in the upstream native path, which nothing in this build compiles in), so
    // both keys were controls wired to nothing. The fields are still part of the ABI and stay zero,
    // which is their "off" value; a runtime that does implement them sees the shipped picture.
    LayerLog("NrCore: strength=%.2f temporal=%u\n", m_strength, m_temporal ? 1u : 0u);

    // The answer is polled, so a 1 ms timer keeps the wait tight without busy spinning.
    timeBeginPeriod(1);

    return LoadRuntime();
}

void NrCore::TakeLaunchTotals(double& enqueue, double& wait) {
    enqueue = m_enqueueMs;
    wait = m_waitMs;
    m_enqueueMs = 0;
    m_waitMs = 0;
}

void NrCore::Shutdown() {
    if (m_api != nullptr && m_context != nullptr) {
        m_api->Destroy(m_context);
        m_context = nullptr;
    }
    delete m_api;
    m_api = nullptr;
    if (m_module != nullptr) {
        FreeLibrary(m_module);
        m_module = nullptr;
    }
    timeEndPeriod(1);
    m_result8.Reset();
    m_motion.Reset();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        m_filterHistory[eye][0].Reset();
        m_filterHistory[eye][1].Reset();
    }
    m_filterWidth = 0;
    m_filterHeight = 0;
    m_filterValid[0] = false;
    m_filterValid[1] = false;
    // The depth is a texture opened from another device, so it goes with this one rather than
    // outliving it.
    m_depth.Reset();
    m_depthWidth = 0;
    m_depthHeight = 0;
    m_outputFp16.Reset();
    m_inputFp16.Reset();
    m_motionHeap.Reset();
    m_motionPipeline.Reset();
    m_motionRootSignature.Reset();
    m_convertHeap.Reset();
    m_convertPipeline.Reset();
    m_convertRootSignature.Reset();
    m_textureWidth = 0;
    m_textureHeight = 0;
    m_resultWidth = 0;
    m_resultHeight = 0;
    m_resultFormat = DXGI_FORMAT_UNKNOWN;
}

bool NrCore::EnsureConvertPipeline() {
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

    // Static, so the shader does not need a descriptor for it. Linear filtering is what makes the
    // window resample instead of only cropping; the exact-centre sampling keeps the 1:1 case
    // bit-for-bit.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC signatureDesc{};
    signatureDesc.NumParameters = 2;
    signatureDesc.pParameters = parameters;
    signatureDesc.NumStaticSamplers = 1;
    signatureDesc.pStaticSamplers = &sampler;
    signatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signatureError;
    if (FAILED(D3D12SerializeRootSignature(
            &signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.ReleaseAndGetAddressOf(), signatureError.ReleaseAndGetAddressOf()))) {
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
                               "NrConvertCS",
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
    const HRESULT pipelineResult =
        m_device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(m_convertPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(pipelineResult)) {
        // Static because Fail keeps the pointer rather than copying the text, and this buffer outlives
        // the call the same way a literal would.
        static char detail[80];
        sprintf_s(detail, "CreateComputePipelineState failed (0x%08lX)", (unsigned long)pipelineResult);
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

bool NrCore::EnsureMotionPipeline() {
    if (m_motionPipeline != nullptr) {
        return true;
    }

    // Two ranges in one table: the motion field the pass writes, and the depth it reads. They are
    // adjacent slots of the one heap rather than a table plus a root descriptor, which is the shape
    // every other pass here uses -- and a null descriptor is a legal thing to bind when there is no
    // depth, which is what a session without one binds.
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
    if (FAILED(compile(kMotionShader,
                       strlen(kMotionShader),
                       "NrMotionCS",
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
        // Static for the same reason as above: Fail keeps the pointer it is given.
        static char detail[128];
        sprintf_s(detail, "motion pipeline creation failed (0x%08lX), device removed reason 0x%08lX",
                  (unsigned long)pipelineResult,
                  (unsigned long)m_device->GetDeviceRemovedReason());
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

bool NrCore::RecordMotion(ID3D12GraphicsCommandList* cmd,
                          const NrMotion& motion,
                          uint32_t width,
                          uint32_t height) {
    if (!EnsureMotionPipeline() || m_motion == nullptr) {
        return false;
    }
    NoteDeviceRemoved(m_device.Get(), "RecordMotion entry");

    // The depth is only read when the caller has a near plane to decode it with, so a session without
    // one still runs this pass -- it just runs it the way it ran before there was a depth.
    const bool withDepth = m_depth != nullptr && motion.invNear > 0.f;

    const UINT descriptorStride =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE motionSlot = m_motionHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE depthSlot = motionSlot;
    depthSlot.ptr += descriptorStride;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kMotionFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_motion.Get(), nullptr, &uav, motionSlot);
    NoteDeviceRemoved(m_device.Get(), "RecordMotion motion uav written");

    // A null descriptor where there is no depth is the honest binding: the shader keys off the near
    // plane being positive before it reads this, so a session without one never touches it.
    if (withDepth) {
        const D3D12_RESOURCE_DESC depthDesc = m_depth->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
        depthView.Format = depthDesc.Format;
        depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthView.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_depth.Get(), &depthView, depthSlot);
    } else {
        // A null descriptor still has to describe the view it stands in for. Passing a NULL pDesc
        // alongside a NULL pResource is not accepted here: AMD's driver treats it as an invalid call
        // and removes the device, which is how this was found. The format has to be the one the
        // shader declares, so the slot stays a Texture2D<float> rather than an untyped void.
        D3D12_SHADER_RESOURCE_VIEW_DESC depthView{};
        depthView.Format = DXGI_FORMAT_R32_FLOAT;
        depthView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthView.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &depthView, depthSlot);
    }

    NoteDeviceRemoved(m_device.Get(), "RecordMotion descriptors written");
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
    constants[23] = motion.translation[0];
    constants[24] = motion.translation[1];
    constants[25] = motion.translation[2];
    // The shader keys off this being positive to decide whether to load the depth at all, so it has to
    // say what is actually bound: a near plane with no texture behind it would be a load from an
    // unbound descriptor, which is undefined rather than simply zero.
    constants[26] = withDepth ? motion.invNear : 0.f;
    constants[27] = motion.depthOrigin[0];
    constants[28] = motion.depthOrigin[1];
    constants[29] = motion.depthScale[0];
    constants[30] = motion.depthScale[1];

    Transition(cmd, m_motion.Get(), kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NoteDeviceRemoved(m_device.Get(), "RecordMotion motion barrier");
    if (withDepth) {
        Transition(cmd, m_depth.Get(), D3D12_RESOURCE_STATE_COMMON, kSrv);
    }

    ID3D12DescriptorHeap* heaps[] = {m_motionHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_motionRootSignature.Get());
    cmd->SetComputeRoot32BitConstants(0, kMotionConstants, constants, 0);
    cmd->SetComputeRootDescriptorTable(1, m_motionHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(m_motionPipeline.Get());
    NoteDeviceRemoved(m_device.Get(), "RecordMotion state set");
    cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    NoteDeviceRemoved(m_device.Get(), "RecordMotion dispatched");

    if (withDepth) {
        Transition(cmd, m_depth.Get(), kSrv, D3D12_RESOURCE_STATE_COMMON);
    }
    Transition(cmd, m_motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);
    NoteDeviceRemoved(m_device.Get(), "RecordMotion exit");
    return true;
}

bool NrCore::SetDepthSharedHandle(HANDLE handle, uint32_t width, uint32_t height) {
    if (m_device == nullptr) {
        return false;
    }
    if (handle == nullptr) {
        m_depth.Reset();
        m_depthWidth = 0;
        m_depthHeight = 0;
        return true;
    }
    if (m_depth != nullptr && m_depthWidth == width && m_depthHeight == height) {
        return true;
    }

    ComPtr<ID3D12Resource> opened;
    const HRESULT result =
        m_device->OpenSharedHandle(handle, IID_PPV_ARGS(opened.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        LayerLog("NrCore: the depth reduction could not be opened for D3D12 (0x%08lX); the motion "
                 "field stays rotation-only\n",
                 (unsigned long)result);
        m_depth.Reset();
        return false;
    }

    m_depth = opened;
    m_depthWidth = width;
    m_depthHeight = height;
    LayerLog("NrCore: depth %ux%u opened for the motion pass\n", width, height);
    return true;
}

bool NrCore::EnsureTextures(uint32_t width, uint32_t height) {
    if (m_inputFp16 != nullptr && m_textureWidth == width && m_textureHeight == height) {
        return true;
    }

    m_inputFp16.Reset();
    m_outputFp16.Reset();
    m_motion.Reset();

    m_inputFp16 = CreateTexture(m_device.Get(), width, height, kFp16, kSrv, true, L"NR input fp16");
    m_outputFp16 = CreateTexture(m_device.Get(), width, height, kFp16, kSrv, false, L"NR answer fp16");
    m_motion = CreateTexture(m_device.Get(), width, height, kMotionFormat, kSrv, true, L"NR motion");
    if (!m_inputFp16 || !m_outputFp16 || !m_motion) {
        Fail("fp16 working texture creation failed");
        return false;
    }

    // A colour-size change tears the network down and builds it again, so whatever the previous
    // frame's answer was, it no longer describes this image.
    InvalidateHistory();

    m_textureWidth = width;
    m_textureHeight = height;
    LayerLog("NrCore: fp16 working set %ux%u\n", width, height);
    return true;
}

bool NrCore::EnsureResultTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (m_result8 != nullptr && m_resultWidth == width && m_resultHeight == height && m_resultFormat == format) {
        return true;
    }
    m_result8.Reset();
    m_result8 = CreateTexture(m_device.Get(), width, height, format, kSrv, true, L"NR result");
    if (!m_result8) {
        Fail("result texture creation failed");
        return false;
    }
    m_resultWidth = width;
    m_resultHeight = height;
    m_resultFormat = format;
    LayerLog("NrCore: result texture %ux%u fmt=%d\n", width, height, (int)format);
    return true;
}

bool NrCore::EnsureFilterTextures(uint32_t width, uint32_t height) {
    if (m_filterHistory[0][0] != nullptr && m_filterHistory[0][1] != nullptr &&
        m_filterHistory[1][0] != nullptr && m_filterHistory[1][1] != nullptr && m_filterWidth == width &&
        m_filterHeight == height) {
        return true;
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        m_filterHistory[eye][0].Reset();
        m_filterHistory[eye][1].Reset();
    }
    // Read as a shader resource and written as an unordered access target, alternating every frame.
    // At the start of a frame the one being read is the one written on the previous frame (UAV) and
    // the one being written is the one read on the previous frame (shader resource), so each pair is
    // created in those two states to match the invariant the consumer maintains. One pair per eye:
    // a stereo frame runs the consumer twice, and both runs need a history of their own picture.
    for (uint32_t eye = 0; eye < 2; ++eye) {
        m_filterHistory[eye][0] = CreateTexture(m_device.Get(), width, height, kFp16,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true,
                                                eye == 0 ? L"NR anti-flicker history L 0"
                                                         : L"NR anti-flicker history R 0");
        m_filterHistory[eye][1] = CreateTexture(m_device.Get(), width, height, kFp16, kSrv, true,
                                                eye == 0 ? L"NR anti-flicker history L 1"
                                                         : L"NR anti-flicker history R 1");
        if (!m_filterHistory[eye][0] || !m_filterHistory[eye][1]) {
            Fail("anti-flicker history creation failed");
            return false;
        }
        m_filterRead[eye] = 0;
    }
    // `m_filterValid` is what keeps the first frame's read from being believed: the pairs are in the
    // right states, but what they hold is not about this image yet.
    m_filterWidth = width;
    m_filterHeight = height;
    InvalidateHistory();
    LayerLog("NrCore: anti-flicker history %ux%u, one pair per eye\n", width, height);
    return true;
}

void NrCore::InvalidateHistory() {
    m_historyInvalid = true;
    m_filterValid[0] = false;
    m_filterValid[1] = false;
    m_filterResets++;
}

void NrCore::ProbeFilterHistory(ID3D12GraphicsCommandList* cmd, uint32_t written, uint32_t eye) {
    if (m_filterHistory[eye][written] == nullptr) {
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
               m_filterHistory[eye][written].Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_filterHistory[eye][written].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = m_filterProbe.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint.Footprint.Format = kFp16;
    target.PlacedFootprint.Footprint.Width = side;
    target.PlacedFootprint.Footprint.Height = side;
    target.PlacedFootprint.Footprint.Depth = 1;
    target.PlacedFootprint.Footprint.RowPitch = rowPitch;
    const D3D12_BOX box{left, top, 0, left + side, top + side, 1};
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
    Transition(cmd,
               m_filterHistory[eye][written].Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_filterProbePending = true;
    m_filterProbeFrame = m_frameIndex;
}

void NrCore::ReadFilterProbe() {
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

    NrRuntimeReport report = NrRuntimeReportGet();
    report.filterValidShare = sampled > 0.0 ? valid / sampled : 0.0;
    report.filterMagnitude = valid > 0.0 ? magnitude / valid : 0.0;
    report.filterAccumulation = valid > 0.0 ? accumulation / valid : 0.0;
    report.filterSampleFrame = m_filterProbeFrame;
    report.filterFrames = m_filterFrames;
    report.filterResets = m_filterResets;
    report.filterOn = true;
    NrRuntimeReportSet(report);
    // The accumulation is the number that proves the read: a fresh write is 1.0 and only a frame that
    // actually blended against the previous history can write more, so a mean above 1.0 here cannot
    // be produced by a filter whose history was never read back.
    LayerLog("NrCore: anti-flicker probe: %ux%u box of the history, %.1f%% of it carried a correction, "
             "mean |correction| %.5f, mean accumulation %.2f frames (sampled on frame %llu)\n",
             side,
             side,
             report.filterValidShare * 100.0,
             report.filterMagnitude,
             report.filterAccumulation,
             m_filterProbeFrame);
}

void NrCore::RecordConvert(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* source,
                           DXGI_FORMAT sourceViewFormat,
                           ID3D12Resource* original,
                           DXGI_FORMAT originalViewFormat,
                           ID3D12Resource* destination,
                           DXGI_FORMAT destinationFormat,
                           const NrConvert& convert,
                           const NrFilter& filter) {
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
    // it, so the producer points the original slot at its own source.
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
    // The filter's history is per eye: this run reads and writes the pair that belongs to the view
    // the consumer was recording (the producer's default-filter runs never get here).
    const uint32_t filterEye = filter.eye < 2 ? filter.eye : 0;
    if (filter.on && m_filterHistory[filterEye][m_filterRead[filterEye]] != nullptr) {
        srv.Format = kFp16;
        m_device->CreateShaderResourceView(m_filterHistory[filterEye][m_filterRead[filterEye]].Get(), &srv, slot(4));
    } else {
        srv.Format = sourceViewFormat;
        m_device->CreateShaderResourceView(source, &srv, slot(4));
    }
    if (filter.on && m_filterHistory[filterEye][1 - m_filterRead[filterEye]] != nullptr) {
        uav.Format = kFp16;
        m_device->CreateUnorderedAccessView(m_filterHistory[filterEye][1 - m_filterRead[filterEye]].Get(), nullptr, &uav, slot(5));
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

bool NrCore::RecordProducer(ID3D12GraphicsCommandList* cmd,
                            ID3D12Resource* source,
                            DXGI_FORMAT sourceViewFormat,
                            const NrConvert& convert,
                            const NrMotion& motion) {
    if (m_context == nullptr) {
        return false;
    }
    NoteDeviceRemoved(m_device.Get(), "RecordProducer entry");
    if (!EnsureConvertPipeline() || !EnsureTextures(convert.destinationWidth, convert.destinationHeight)) {
        return false;
    }
    NoteDeviceRemoved(m_device.Get(), "RecordProducer textures ready");

    // ---- what this frame runs with ----
    //
    // Read here rather than from what Initialize() cached, so a write from the control panel lands on
    // the next frame instead of on the next session -- and inside a headset there is no menu to reach
    // these with, so "next frame" is the only place they can be judged at all.
    const NrSettings live = NrSettingsGet();
    // The four values the frame will carry, from the four menu values the panel shows. One translation
    // for the whole of this function -- the change check, the log and the frame itself all read this
    // one, so what is logged is what the runtime is handed.
    const NrControlWire wire = NrControlWireOf(live);

    // The pre-block controls are per-frame fields, not build-time ones: the runtime scales a column of
    // its 32x16 input mix by each of them. What that means for the history differs by runtime, and the
    // difference is not cosmetic. A runtime that implements them rebuilds its network when a value
    // differs from the one the current network was built with, so the history describes a network that
    // no longer exists and is dropped here -- one step before the temporal path is decided for this
    // frame. This layer's runtime instead rewrites the device copy of that matrix in place, so nothing
    // is rebuilt and no answer goes stale; the history is still dropped, which costs one frame of
    // accumulation and keeps both cases on one code path. The comparison is on the wire values: two
    // menu settings that translate to the same four numbers are the same picture and must not log as a
    // change.
    if (!m_controlsValid || wire.tone != m_controls[0] ||
        wire.structure != m_controls[1] || wire.skin != m_controls[2] ||
        wire.other != m_controls[3] || live.controlStyle != m_controls[4]) {
        const bool firstTime = !m_controlsValid;
        m_controls[0] = wire.tone;
        m_controls[1] = wire.structure;
        m_controls[2] = wire.skin;
        m_controls[3] = wire.other;
        m_controls[4] = live.controlStyle;
        m_controlsValid = true;
        InvalidateHistory();
        if (!firstTime) {
            LayerLog("NrCore: controls changed to %.2f/%.2f/%.2f/%.2f/%.2f on the wire "
                     "(tone %.2f, structure %.2f, character structure %.2f%s, native mask %s)\n",
                     wire.tone,
                     wire.structure,
                     wire.skin,
                     wire.other,
                     live.controlStyle,
                     live.controlTone,
                     live.controlStructure,
                     live.controlSkin,
                     live.controlSkin < 0.f ? " following structure" : "",
                     live.controlMask ? "on" : "off");
        }
    }

    // The near plane decides what the motion field means: every pixel of the history in hand was
    // warped with the old value, so it cannot be warped correctly with the new one. Left alone, the
    // temporal path keeps re-warping its own mistake forward and the window settles onto one stale
    // picture, which no later setting brings back -- the history has to be dropped for that.
    if (live.motionNear != m_motionNear) {
        const bool firstTime = m_motionNear < 0.f;
        m_motionNear = live.motionNear;
        InvalidateHistory();
        if (!firstTime) {
            LayerLog("NrCore: motion near plane is now %.4f m, the history is dropped\n", live.motionNear);
        }
    }
    // The same reset, asked for by hand: the button exists precisely for a history that has already
    // been warped into a picture it cannot leave on its own.
    if (live.historyReset != m_historyResetSeen) {
        m_historyResetSeen = live.historyReset;
        InvalidateHistory();
    }

    // And the reset nothing asks for: a gap between frames long enough that the answer in hand is
    // describing a different scene. A loading screen or a menu is written to the same eye buffer with
    // no motion between the two pictures, so the history's motion field warps nothing and the first
    // frames after the gap are the old picture fading out of the new one. The threshold is the
    // reference's: half a second is far longer than any frame this layer runs at, and far shorter than
    // a screen transition.
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastFrameAtValid) {
            const double gapMs =
                std::chrono::duration<double, std::milli>(now - m_lastFrameAt).count();
            if (gapMs > 500.0) {
                InvalidateHistory();
                LayerLog("NrCore: %.0f ms since the last frame, the history is dropped\n", gapMs);
            }
        }
        m_lastFrameAt = now;
        m_lastFrameAtValid = true;
    }

    // ---- colour crop -> fp16 network input ----
    Transition(cmd, m_inputFp16.Get(), kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RecordConvert(cmd,
                  source,
                  sourceViewFormat,
                  nullptr,
                  sourceViewFormat,
                  m_inputFp16.Get(),
                  kFp16,
                  convert,
                  NrFilter{});
    Transition(cmd, m_inputFp16.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);

    // ---- where every pixel of that input was in the previous answer ----
    //
    // A single session owns a single history buffer, and a stereo frame is two runs against it, so
    // only the view whose answer the history actually holds may ask for the temporal path. The caller
    // decides which one that is; `motion.valid` is false for any other view, and a run without the
    // flag leaves the history alone rather than overwriting it with the other eye's picture.
    // The layer's own anti-flicker filter needs the same field and has nothing to do with the
    // runtime's temporal path, so with the filter on the motion is recorded whether or not the
    // runtime is being fed a history. Writing it while the flag stays off costs one dispatch and is
    // invisible to the runtime, which only reads the motion a frame carrying that flag names.
    const bool usable = m_temporal && motion.valid && !m_historyInvalid;
    const bool filterWantsMotion = live.antiFlicker > 0.f && motion.valid;
    if (usable || filterWantsMotion) {
        if (!RecordMotion(cmd, motion, convert.destinationWidth, convert.destinationHeight)) {
            return false;
        }
    }
    if (!usable && m_historyInvalid && m_temporal) {
        m_historyInvalid = false;
        m_historyInvalidFrames++;
        if (m_historyInvalidFrames == 1) {
            LayerLog("NrCore: temporal path on, history starts clean\n");
        }
    }

    // ---- hand the frame to the network ----
    const bool sendControls = ControlsOffShipped(live);

    LmxxfNrFrameInfo frameInfo{};
    frameInfo.struct_size = sendControls ? (uint32_t)sizeof(LmxxfNrFrameInfo)
                                         : 128u; // The size every runtime since 0.3.3.2 accepts.
    frameInfo.session_id = 1;
    frameInfo.frame_id = m_frameIndex;
    frameInfo.list_generation = m_frameIndex;
    frameInfo.command_list = cmd;
    frameInfo.color_width = convert.destinationWidth;
    frameInfo.color_height = convert.destinationHeight;
    frameInfo.color = m_inputFp16.Get();
    frameInfo.color_state = kSrv;
    frameInfo.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
    frameInfo.transfer_strength = live.transferStrength;
    frameInfo.color_strength = live.colorStrength;
    frameInfo.debug_view = live.debugView;
    frameInfo.model_scale = live.modelScale;
    frameInfo.passes = live.passes;
    // The ABI's output-side smoothing fields stay zero: no runtime this layer can load reads them (see
    // the note where the environment used to be read), and the layer's own anti-flicker filter is
    // what damps the answer's noise on this path now.
    frameInfo.output_smooth = 0.f;
    frameInfo.output_smooth_threshold = 0.f;
    if (sendControls) {
        frameInfo.flags |= LMXXF_NR_FRAME_FLAG_CONTROLS;
        frameInfo.control_tone = wire.tone;
        frameInfo.control_structure = wire.structure;
        frameInfo.control_skin = wire.skin;
        frameInfo.control_other = wire.other;
        frameInfo.control_style = live.controlStyle;
    }
    if (usable) {
        frameInfo.flags |= LMXXF_NR_FRAME_FLAG_TEMPORAL;
        frameInfo.motion = m_motion.Get();
        frameInfo.motion_state = kSrv;
        frameInfo.motion_width = convert.destinationWidth;
        frameInfo.motion_height = convert.destinationHeight;
        // The shader already writes pixel displacement, so there is nothing left to scale.
        frameInfo.motion_scale_x = 1.f;
        frameInfo.motion_scale_y = 1.f;
        frameInfo.history_reset = 0u;
    } else {
        frameInfo.motion = nullptr;
        frameInfo.history_reset = 1u; // Single frame inference.
    }
    m_frameIndex++;

    m_job = LmxxfNrJob{};
    m_job.struct_size = sizeof(m_job);

    if (m_api->PrepareFrame(m_context, &frameInfo, &m_job) != LMXXF_NR_OK) {
        char error[256]{};
        if (m_api->GetLastError) {
            m_api->GetLastError(error, sizeof(error));
        }
        LayerLog("NrCore: PrepareFrame failed (%s)\n", error);
        m_lastError = "PrepareFrame failed";
        return false;
    }

    if (m_api->RecordInputs(m_context, m_job.handle, cmd) != LMXXF_NR_OK) {
        m_api->CancelUnsubmitted(m_context, m_job.handle);
        Fail("RecordInputs failed");
        return false;
    }

    return true;
}

bool NrCore::Launch() {
    return Enqueue() && WaitAnswer();
}

bool NrCore::Enqueue() {
    if (m_context == nullptr) {
        return false;
    }

    const auto enqueueStart = std::chrono::steady_clock::now();
    if (m_api->EnqueueHipAsync(m_context, m_job.handle, m_queue.Get()) != LMXXF_NR_OK) {
        m_api->CancelUnsubmitted(m_context, m_job.handle);
        Fail("EnqueueHipAsync failed");
        return false;
    }
    m_enqueueMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - enqueueStart).count();
    return true;
}

bool NrCore::WaitAnswer() {
    if (m_context == nullptr) {
        return false;
    }

    // The first pass pays for weight upload and kernel compilation.
    const auto budget = m_warmedUp ? std::chrono::milliseconds(10000) : std::chrono::milliseconds(180000);
    const auto start = std::chrono::steady_clock::now();

    // The 1 ms timer (timeBeginPeriod) keeps this wait tight. Spinning instead was measured at the
    // same wait and only burned CPU, so the remaining time is the network's own latency, not the
    // poll's granularity.
    bool ready = false;
    uint32_t isReady = 0;
    while (std::chrono::steady_clock::now() - start < budget) {
        if (m_api->OutputReady(m_context, m_job.handle, &isReady) != LMXXF_NR_OK) {
            break;
        }
        if (isReady) {
            ready = true;
            break;
        }
        Sleep(1);
    }

    const double waited =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_waitMs += waited;

    if (!ready) {
        LayerLog("NrCore: the network did not answer (waited %.0f ms)\n", waited);
        m_api->AbandonJob(m_context, m_job.handle);
        m_lastError = "the network did not answer";
        return false;
    }

    // Steady state is a handful of milliseconds; a far larger answer points at the runtime or the
    // GPU being busy, which is what a level load looks like from in here.
    if (waited > 100.0) {
        LayerLog("NrCore: slow answer %.0f ms\n", waited);
    }

    if (!m_warmedUp) {
        m_warmedUp = true;
        LayerLog("NrCore: first answer ready\n");
    }

    // What the runtime actually built for this session, and what we asked it for. A block schedule or
    // a history state that does not match the requested flags is what a silently ignored flag looks
    // like from the outside, so this is worth sampling repeatedly: the very first frame always starts
    // a clean history, and reading only that one would answer the question wrongly.
    const NrSettings asked = NrSettingsGet();
    if (m_api->GetStatus != nullptr && (m_statusSamples % 300) == 0) {
        // The wire values again, so the line shows the four scales the runtime was actually handed --
        // the menu values they came from are named in the change line above instead.
        const NrControlWire askedWire = NrControlWireOf(asked);
        char status[256]{};
        if (m_api->GetStatus(m_context, status, (uint32_t)sizeof(status)) == LMXXF_NR_OK) {
            // Read-modify-write: the layer's frame loop owns `frameMs` and `qualityScale` in the same
            // report, and a fresh struct here would wipe them every sample.
            NrRuntimeReport runtime = NrRuntimeReportGet();
            // The runtime's own answer for the one value the panel cannot change on the next frame:
            // the pass count this session was actually built with. Reported up so the panel warns
            // "restart" only while the file it wrote and the live network disagree.
            runtime.passes = StatusField(status, "multi_pass=");
            NrRuntimeReportSet(runtime);

            // Reported from the live block rather than from what Initialize() cached. The whole point
            // of the block is that these move while the title runs, and a status line still printing
            // the session's starting values would read as "the panel does nothing".
            LayerLog("NrCore: status \"%s\" (asked flags=0x%x temporal=%u strength=%.2f/%.2f scale=%.2f "
                     "passes=%u debug=%u controls=%.2f/%.2f/%.2f/%.2f/%.2f)\n",
                     status,
                     LMXXF_NR_FRAME_FLAG_STRENGTH |
                         (ControlsOffShipped(asked) ? LMXXF_NR_FRAME_FLAG_CONTROLS : 0u),
                     m_temporal ? 1u : 0u,
                     asked.transferStrength,
                     asked.colorStrength,
                     asked.modelScale,
                     asked.passes,
                     asked.debugView,
                     askedWire.tone,
                     askedWire.structure,
                     askedWire.skin,
                     askedWire.other,
                     asked.controlStyle);

            // The import pool, on the same cadence. `imports` counts the bridge buffers the runtime had
            // to import and `reuses` the ones served from its free list instead; a rebuild that reuses
            // is what the window's size band is for -- band crossings stay rare (the window's own log
            // says when one happens) and this says the ones that happen are not paying an import each.
            if (m_poolStats != nullptr) {
                LmxxfNrImportPoolStats pool{};
                pool.struct_size = sizeof(pool);
                if (m_poolStats(&pool) == LMXXF_NR_OK) {
                    LayerLog("NrCore: import pool entries=%u busy=%u bytes=%llu imports=%llu reuses=%llu "
                             "enabled=%u\n",
                             pool.buffers,
                             pool.busy,
                             (unsigned long long)pool.bytes,
                             (unsigned long long)pool.imports,
                             (unsigned long long)pool.reuses,
                             pool.enabled);
                }
            }
        }
    }
    m_statusSamples++;
    return true;
}

bool NrCore::RecordConsumer(ID3D12GraphicsCommandList* cmd,
                            ID3D12Resource* destination,
                            DXGI_FORMAT destinationFormat,
                            ID3D12Resource* original,
                            DXGI_FORMAT originalViewFormat,
                            const NrConvert& convert,
                            uint32_t eye) {
    if (m_context == nullptr) {
        return false;
    }
    if (eye >= 2) {
        eye = 0;
    }
    if (!EnsureResultTexture(convert.destinationWidth, convert.destinationHeight, destinationFormat)) {
        return false;
    }

    // The previous frame's probe, if one is waiting: that frame's command list was submitted and
    // waited on before this call, so the copy it recorded is complete and mapping cannot stall.
    if (m_filterProbePending) {
        ReadFilterProbe();
        m_filterProbePending = false;
    }

    ID3D12Resource* const answer = static_cast<ID3D12Resource*>(m_job.private_output);

    if (m_api->RecordOutputs(m_context, m_job.handle, cmd) != LMXXF_NR_OK) {
        m_api->Retire(m_context, m_job.handle);
        Fail("RecordOutputs failed");
        return false;
    }
    m_api->Retire(m_context, m_job.handle);

    if (answer == nullptr) {
        Fail("the runtime produced no answer");
        return false;
    }

    // Move the answer out of the runtime's resource on the same list, then convert it. A plain
    // copy keeps the runtime's resource state assumptions out of the conversion shader.
    Transition(cmd, answer, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmd, m_outputFp16.Get(), kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_outputFp16.Get(), answer);
    Transition(cmd, m_outputFp16.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kSrv);

    // The feather reads the region the way it was before the pass, so the crop has to be readable
    // here as well.
    if (original != nullptr) {
        Transition(cmd, original, D3D12_RESOURCE_STATE_COMMON, kSrv);
    }

    // ---- the anti-flicker filter, if the panel turned it on ----
    //
    // Everything it needs is already on this list: the answer (the copy above), the crop it is a
    // correction over, and the motion field the producer recorded. A live change of strength or gate
    // starts a clean history rather than mixing two settings; so does the gap rule the producer
    // applies. The history pair is ping-ponged, so the texture read here is the one written on the
    // previous frame, and the write goes to the other one.
    const NrSettings filterSettings = NrSettingsGet();
    NrFilter filter{};
    filter.on = filterSettings.antiFlicker > 0.f;
    filter.strength = std::clamp(filterSettings.antiFlicker, 0.f, 1.f);
    filter.gate = std::max(filterSettings.antiFlickerGate, 1e-6f);
    filter.eye = eye;
    // A live change of strength or gate starts a clean history rather than mixing two settings. The
    // off state clears the pair as well, so turning the filter back on after a gap cannot blend
    // against a history that describes a picture several frames old -- the same reason the producer
    // drops the history on a frame gap.
    if (filter.on && (filter.strength != m_filterStrength || filter.gate != m_filterGate)) {
        const bool firstTime = m_filterStrength < 0.f;
        m_filterStrength = filter.strength;
        m_filterGate = filter.gate;
        InvalidateHistory();
        LayerLog("NrCore: anti-flicker %s (strength %.2f, gate %.4f)\n",
                 firstTime ? "on" : "reconfigured",
                 filter.strength,
                 filter.gate);
    } else if (!filter.on && m_filterStrength >= 0.f) {
        m_filterStrength = -1.f;
        m_filterGate = -1.f;
        LayerLog("NrCore: anti-flicker off\n");
    }
    if (filter.on && !EnsureFilterTextures(convert.destinationWidth, convert.destinationHeight)) {
        return false;
    }
    filter.historyValid = filter.on && m_filterValid[eye];

    if (filter.on) {
        // One texture is sampled this frame and the other is written, and the roles swap every frame,
        // so each frame flips their states: the one being read comes back from being written (UAV ->
        // shader resource), the one being written goes the other way. Both are in the state the other
        // role left them in, which is the whole reason the pair exists rather than one texture.
        Transition(cmd,
                   m_filterHistory[eye][m_filterRead[eye]].Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   kSrv);
        Transition(cmd,
                   m_filterHistory[eye][1 - m_filterRead[eye]].Get(),
                   kSrv,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    Transition(cmd, m_result8.Get(), kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RecordConvert(cmd,
                  m_outputFp16.Get(),
                  kFp16,
                  original,
                  originalViewFormat,
                  m_result8.Get(),
                  destinationFormat,
                  convert,
                  filter);
    Transition(cmd, m_result8.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    if (filter.on) {
        // Read next frame as what this one wrote, and swapped here rather than at the top so the two
        // states above always describe the pair the way the shader saw it.
        m_filterRead[eye] = 1 - m_filterRead[eye];
        m_filterValid[eye] = true;
        m_filterFrames++;
        // The counters travel to the report every frame rather than only when a probe runs: "已累积 N
        // 帧" is the cheapest honest sign that the filter is live, and a number that only moved once
        // a minute would read as a stalled one. The sampled values stay the probe's to write.
        NrRuntimeReport counters = NrRuntimeReportGet();
        counters.filterOn = true;
        counters.filterFrames = m_filterFrames;
        counters.filterResets = m_filterResets;
        NrRuntimeReportSet(counters);
        // Sample the history once it has settled, and then rarely. The probe is what says whether the
        // history this filter writes is real, so it is worth the occasional copy -- but it is a
        // question, not a per-frame need.
        if (!m_filterProbePending && (m_filterFrames == 3 || (m_filterFrames % 1800) == 0)) {
            ProbeFilterHistory(cmd, m_filterRead[eye], eye);
        }
    } else {
        // A sample taken while the filter was on must not keep reading as live after it is switched
        // off, and the panel keys its readout off exactly that. Written only on the transition.
        NrRuntimeReport counters = NrRuntimeReportGet();
        if (counters.filterOn) {
            counters.filterOn = false;
            NrRuntimeReportSet(counters);
        }
    }

    if (original != nullptr) {
        Transition(cmd, original, kSrv, D3D12_RESOURCE_STATE_COMMON);
    }

    Transition(cmd, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(destination, m_result8.Get());
    Transition(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

    Transition(cmd, m_result8.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
    Transition(cmd, answer, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);

    return true;
}
