// EASU + RCAS shader sources and the #include resolver they need.
//
// The layer and the offline probe compile exactly these strings, so the two can never drift apart.
// The shader text is written the way AMD publishes it and pulls the FSR headers in through
// ID3DInclude, which reads them from a directory beside the binary at runtime.

#pragma once

#include <windows.h>

#include <d3dcommon.h>

#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <utility>

namespace amdnr_fsr {

    // One oversized triangle covers the viewport without a vertex buffer, and `uv` reaches 0..1 over
    // exactly the part of it that lands inside. Shared by both passes.
    inline const char* const kVertexShader = R"(
struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut main_vs(uint id : SV_VertexID)
{
    VsOut output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

    // Pass 1, EASU.
    //
    // Which view of the eye image the layer managed to bind decides this shader's input. The
    // pass-through view is what it wants and what it normally gets: it hands the kernel the
    // display-encoded bytes the application wrote -- the input EASU is documented to take -- with no
    // conversion anywhere, so the encode below is switched off. Only when the image is a *typed* sRGB
    // resource, which refuses the pass-through view, does the read go through the sRGB view, which
    // decodes in hardware; the gathers then encode back so the kernel still sees encoded values.
    //
    // That fallback is the expensive one and deliberately second choice. `FsrEasuRH/GH/BH` run once
    // per texel per channel -- twelve gathers a pixel -- so encoding there is four pow operations per
    // gather, tens of pow a pixel, and on a full-resolution frame that is on the order of a billion a
    // frame; enough to cost more than the resample it is part of.
    inline const char* const kEasuShader = R"(
cbuffer Params : register(b0)
{
    uint4 con0;
    uint4 con1;
    uint4 con2;
    uint4 con3;
    float g_slice;
    float g_encode;
    float2 g_pad;
};

#define A_GPU 1
#define A_HLSL 1
#define A_HALF 1
#include "ffx_a.h"

SamplerState g_clamp : register(s0);
// float4, not AH4: the source is R8G8B8A8, and only the gathered result is narrowed to the half
// precision type the EASU kernel computes in.
Texture2DArray<float4> g_src : register(t0);

float FsrSrgbEncode1(float c)
{
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 Encode4(float4 c)
{
    return g_encode > 0.5 ? float4(FsrSrgbEncode1(c.x), FsrSrgbEncode1(c.y), FsrSrgbEncode1(c.z), FsrSrgbEncode1(c.w)) : c;
}

AH4 FsrEasuRH(AF2 p) { return AH4(Encode4(g_src.GatherRed(g_clamp, float3(p, g_slice)))); }
AH4 FsrEasuGH(AF2 p) { return AH4(Encode4(g_src.GatherGreen(g_clamp, float3(p, g_slice)))); }
AH4 FsrEasuBH(AF2 p) { return AH4(Encode4(g_src.GatherBlue(g_clamp, float3(p, g_slice)))); }

#define FSR_EASU_H 1
#include "ffx_fsr1.h"

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main_ps(VsOut input) : SV_Target
{
    // The output rectangle starts at the viewport origin, so the fragment's integer position is the
    // kernel's output coordinate.
    AH3 c;
    FsrEasuH(c, AU2(input.position.xy), con0, con1, con2, con3);
    return float4(c.r, c.g, c.b, 1.0);
}
)";

    // Pass 2, RCAS. Its own pass because it is defined as a sharpening step over EASU's finished
    // output, not as part of the resample.
    //
    // The intermediate always holds display-encoded values, so RCAS runs on them directly. The result
    // then goes through whichever view of the target the layer could bind: the pass-through view
    // leaves the encoded value alone, which is the normal case; only a typed sRGB target forces the
    // sRGB view, which encodes in hardware, and there the result is decoded back to linear first so
    // that the pair cancels.
    inline const char* const kRcasShader = R"(
cbuffer Params : register(b0)
{
    uint4 con;
    float2 g_invSize;
    float g_decode;
    float g_pad;
};

#define A_GPU 1
#define A_HLSL 1
#define A_HALF 1
#include "ffx_a.h"

SamplerState g_clamp : register(s0);
Texture2D<float4> g_easu : register(t0);

// Sampled rather than loaded: a load that falls outside the texture returns zero, which paints a
// black rim around the result, while clamp addressing repeats the edge texel. The coordinate lands
// on the texel centre, so the bilinear tap still returns exactly that texel.
AH4 FsrRcasLoadH(ASW2 p)
{
    return AH4(g_easu.SampleLevel(g_clamp, (AF2(p) + 0.5) * g_invSize, 0.0));
}

void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}

#define FSR_RCAS_H 1
#include "ffx_fsr1.h"

float FsrSrgbDecode1(float c)
{
    c = max(c, 0.0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main_ps(VsOut input) : SV_Target
{
    AH1 r, g, b;
    FsrRcasH(r, g, b, AU2(input.position.xy), con);
    float3 c = float3(r, g, b);
    if (g_decode > 0.5)
    {
        c = float3(FsrSrgbDecode1(c.r), FsrSrgbDecode1(c.g), FsrSrgbDecode1(c.b));
    }
    return float4(c, 1.0);
}
)";

    // Constant buffer layouts, matching the cbuffers above. The trailing padding keeps each one a
    // multiple of 16 bytes, which D3D11 requires of a constant buffer.
    struct EasuConstants {
        uint32_t con0[4];
        uint32_t con1[4];
        uint32_t con2[4];
        uint32_t con3[4];
        float slice;
        // 1 only on the fallback path: the source is read through an sRGB view, so the gathers have
        // to re-encode. 0 on the pass-through path, where the gathered value is already encoded.
        float encode;
        float pad[2];
    };

    struct RcasConstants {
        uint32_t con[4];
        float invSize[2];
        // 1 only on the fallback path: the target is written through an sRGB view, which encodes on
        // write, so the result has to be decoded first. 0 on the pass-through path.
        float decode;
        float pad;
    };

    // Resolves `#include "..."` against `directory`, so the shader sources stay written the way AMD
    // publishes them instead of being flattened into one string by hand.
    class IncludeHandler final : public ID3DInclude {
      public:
        explicit IncludeHandler(std::string directory) : m_directory(std::move(directory)) {}

        HRESULT STDMETHODCALLTYPE
        Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data, UINT* bytes) override {
            if (fileName == nullptr || data == nullptr || bytes == nullptr) {
                return E_FAIL;
            }

            const std::string path = m_directory + "\\" + fileName;
            FILE* file = nullptr;
            if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
                return E_FAIL;
            }

            fseek(file, 0, SEEK_END);
            const long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (size <= 0) {
                fclose(file);
                return E_FAIL;
            }

            char* buffer = new (std::nothrow) char[(size_t)size];
            if (buffer == nullptr) {
                fclose(file);
                return E_FAIL;
            }
            const size_t read = fread(buffer, 1, (size_t)size, file);
            fclose(file);
            if (read != (size_t)size) {
                delete[] buffer;
                return E_FAIL;
            }

            *data = buffer;
            *bytes = (UINT)size;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override {
            delete[] static_cast<const char*>(data);
            return S_OK;
        }

      private:
        std::string m_directory;
    };

    // The reprojection field FSR 4 warps its history with, built on the application's own device.
    //
    // A pixel names a view ray, the head's rotation takes that ray into the previous frame's view, and
    // projecting it back names the previous pixel. That is exact for rotation, which is the motion a
    // headset actually produces, and with the captured depth under each pixel it carries the head's
    // travel as well. The result is a displacement as a fraction of the view, which is what the
    // effect's own per-frame `motionVectorScale` turns into pixels -- a fraction because the texture
    // this is written through holds halves, and a half keeps the sub-pixel part of a fraction and
    // loses the sub-pixel part of a few thousand pixels.
    //
    // Every cbuffer member is a scalar on purpose. A constant buffer packs a vector so that it never
    // straddles a 16 byte boundary, so a float2 anywhere but the right offset would silently shift
    // every field after it -- and a motion field that is quietly wrong reads as smearing rather than
    // as a bug. The struct below is the same 31 words in the same order, checked at compile time.
    struct MotionConstants {
        float matrix[9];   // takes a view-space direction in this frame to the previous frame's
        float a[2];        // view pixel -> normalised view coordinate: s = a + (p + 0.5) * b
        float b[2];
        float tanLeft;     // half-tangents of the view frustum, and their spans
        float tanWidth;
        float tanUp;
        float tanHeight;
        float extentScale[2];
        float originShift[2];
        uint32_t width;    // the field's own size, in pixels
        uint32_t height;
        float translation[3];  // the camera's travel, in the previous frame's view basis, in metres
        float invNear;         // one over the near plane; zero leaves the reprojection rotation-only
        float depthOrigin[2];  // where this view sits inside the packed eye image the depth describes
        float depthScale[2];
    };
    static_assert(sizeof(MotionConstants) == 31 * sizeof(float),
                  "the motion constants must be 31 packed words to match the shader's cbuffer");

    inline const char* const kMotionShader = R"(
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

// Where a point in the previous frame's view space lands in the previous frame's image, as a fraction
// of it. The caller has already checked that the point is in front of the camera.
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

    // Where this pixel sits inside the view image, as a fraction of it -- the same normalisation the
    // frustum's half-tangents are expressed in. A motion the layer could not compute arrives as a zero
    // matrix, which lands on the behind-the-camera branch below and writes the zero field that tells
    // the effect nothing moved.
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

    // The rotation-only answer. It treats every pixel as if it were at infinity, which is exact for the
    // rotation and wrong for the translation -- but it is never nonsense, so it is also what this falls
    // back to.
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

            // Past half a view is not a distance: a pixel's content cannot have travelled that far
            // between two frames of a head that is being worn. When the parallax says it has, the near
            // plane is not the one the application projected with, and the answer it produced is not
            // usable. This is not defensive for its own sake -- a motion field this wrong is written
            // into a temporal history, where each frame reprojects the previous mistake forward and the
            // window settles onto one stale picture it cannot leave. Ignoring the term for those pixels
            // keeps the field inside something the history can recover from.
            if (abs(withDepth.x - s.x) <= 0.5f && abs(withDepth.y - s.y) <= 0.5f)
            {
                previous = withDepth;
            }
        }
    }

    g_motion[tid.xy] = (previous - s) * float2(g_ext_x, g_ext_y) + float2(g_org_x, g_org_y);
}
)";

} // namespace amdnr_fsr
