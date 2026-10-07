// Finds the projection the application draws its scene through, and watches it frame to frame.
//
// The scan looks for the shape a projection has and nothing else: the depth column carries the near
// and far planes, and the last element of that column is the only one that is one and zero after it.
// A title that stores its projection as one half of a combined view-projection matrix will not match,
// and that is the honest outcome -- the answer is then "not found", not a wrong number.
//
// Everything here runs on the application's own immediate context, and the same rule AmdnrDepthCapture
// set applies: the hooks only remember pointers, and no resource is ever released from inside a
// D3D11 call. A buffer displaced from a table is parked and let go with the session, so a reference
// can never be dropped on the render thread.

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "AmdnrProjectionProbe.h"
#include "LayerLog.h"

namespace {

    using Microsoft::WRL::ComPtr;

    // ID3D11DeviceContext declares its methods in interface order after the three IUnknown slots and
    // the four ID3D11DeviceChild ones. These are the two entry points that announce a constant
    // buffer, and their indices are fixed for the same reason the depth hook's are.
    constexpr size_t kSlotVSSetConstantBuffers = 7;
    constexpr size_t kSlotPSSetConstantBuffers = 16;

    using SetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                          UINT,
                                                          UINT,
                                                          ID3D11Buffer* const*);

    SetConstantBuffersFn g_originalVS = nullptr;
    SetConstantBuffersFn g_originalPS = nullptr;
    void** g_patchedVtable = nullptr;
    bool g_installed = false;

    ComPtr<ID3D11DeviceContext> g_context;

    bool ProbeRequested() {
        static const bool requested = [] {
            char text[8]{};
            return GetEnvironmentVariableA("AMDNR_XR_CBUF_PROBE", text, (DWORD)sizeof(text)) != 0 &&
                   text[0] != '0';
        }();
        return requested;
    }

    // How many distinct buffers are remembered per stage. A frame binds a handful that could carry a
    // projection and many that could not, so the table is small and the newest bind wins the slot it
    // needs -- the projection is bound immediately before the scene is drawn, which is the last thing
    // that happens to it in the frame.
    constexpr size_t kMaxRemembered = 16;

    struct Remembered {
        ComPtr<ID3D11Buffer> buffer;
        UINT startSlot{0};
        UINT byteWidth{0};
    };

    struct Source {
        const char* name;
        Remembered entries[kMaxRemembered];
        size_t count{0};
        ID3D11Buffer* last{nullptr};
    };

    Source g_sources[2] = {{"vs"}, {"ps"}};

    // Buffers displaced from a table while the render thread was inside a D3D11 call. They keep the
    // reference they were stored with until the session ends, which is also what keeps a tracked
    // pointer below valid for as long as it is watched.
    std::vector<ComPtr<ID3D11Buffer>> g_parked;

    void NoteBuffer(Source& source, UINT slot, ID3D11Buffer* buffer) {
        if (buffer == nullptr) {
            return; // an unbind, which says nothing about a projection
        }
        if (buffer == source.last) {
            return; // the common case: the same buffer bound again by the next pass
        }

        for (size_t i = 0; i < source.count; i++) {
            if (source.entries[i].buffer.Get() == buffer) {
                source.last = buffer;
                return;
            }
        }

        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        Remembered newcomer;
        newcomer.buffer = buffer; // takes its own reference; never released from inside the hook
        newcomer.startSlot = slot;
        newcomer.byteWidth = desc.ByteWidth;
        source.last = buffer;

        if (source.count < kMaxRemembered) {
            source.entries[source.count++] = std::move(newcomer);
            return;
        }
        // Full: the oldest goes. A projection is bound every frame, so the entry that has gone
        // longest without being seen again is the one least likely to be it.
        g_parked.push_back(std::move(source.entries[0].buffer));
        for (size_t i = 0; i + 1 < source.count; i++) {
            source.entries[i] = std::move(source.entries[i + 1]);
        }
        source.entries[source.count - 1] = std::move(newcomer);
    }

    void STDMETHODCALLTYPE Hook_VSSetConstantBuffers(ID3D11DeviceContext* context,
                                                     UINT startSlot,
                                                     UINT numBuffers,
                                                     ID3D11Buffer* const* buffers) {
        for (UINT i = 0; buffers != nullptr && i < numBuffers; i++) {
            NoteBuffer(g_sources[0], startSlot + i, buffers[i]);
        }
        g_originalVS(context, startSlot, numBuffers, buffers);
    }

    void STDMETHODCALLTYPE Hook_PSSetConstantBuffers(ID3D11DeviceContext* context,
                                                     UINT startSlot,
                                                     UINT numBuffers,
                                                     ID3D11Buffer* const* buffers) {
        for (UINT i = 0; buffers != nullptr && i < numBuffers; i++) {
            NoteBuffer(g_sources[1], startSlot + i, buffers[i]);
        }
        g_originalPS(context, startSlot, numBuffers, buffers);
    }

    bool PatchSlot(void** vtable, size_t slot, void* replacement, void*& original) {
        DWORD previous = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &previous)) {
            return false;
        }
        original = vtable[slot];
        vtable[slot] = replacement;
        DWORD ignored = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), previous, &ignored);
        return true;
    }

    // The largest buffer the probe reads. A constant buffer is at most 64 KB, and a title that packs a
    // frame's constants into one dynamic buffer -- which is what a renderer this size does -- puts the
    // projection somewhere inside a buffer that large. Reading only the head of it found nothing at
    // all: the first run's ceiling was 4 KB and its two blocks came from a pair of sub-kilobyte
    // buffers, while the buffers the scene actually draws through were never looked at.
    constexpr UINT kMaxReadBytes = 65536;

    ComPtr<ID3D11Buffer> g_staging;

    // Whether a block is a projection, and if so what planes its depth terms carry.
    //
    // The test is deliberately narrow, and the narrowness is the whole point. Matching "the last
    // column ends in a one and then a zero" is not enough to call something a projection: every view
    // matrix, every normal matrix, and a good deal of plain data satisfies it, and a first run that
    // matched on that alone spent all four of its slots on blocks that were zero-filled, a normal
    // matrix, and a pair of numbers that were not a frustum at all. So a block is accepted only when
    // every element a projection is required to have is where it has to be, both frustum scales are
    // the size a field of view gives, the off-centre terms are the small ratios they are, and the
    // near plane the two depth terms imply is a distance a camera could actually have.
    //
    // Two layouts are recognised because both are in use: the row-vector form DirectX matrices are
    // built in and uploaded as they stand, and the transposed form a shader consumes them in.
    constexpr float kZero = 1e-4f;

    bool PlanesFrom(float scaleX, float scaleY, float depthA, float depthB, float& nearPlane, float& farPlane) {
        const auto usable = [](float v) { return std::isfinite(v) && std::fabs(v) > 1e-6f; };
        // 1/tan(half the field of view): a wide VR frustum lands near 0.5, a narrow one on 3. Outside
        // this the number is not a field of view, so the block is not a projection.
        if (!usable(scaleX) || std::fabs(scaleX) < 0.2f || std::fabs(scaleX) > 8.f) {
            return false;
        }
        if (!usable(scaleY) || std::fabs(scaleY) < 0.2f || std::fabs(scaleY) > 8.f) {
            return false;
        }
        if (!std::isfinite(depthA) || std::fabs(depthA) < 1e-3f || std::fabs(depthA) > 1e6f) {
            return false;
        }
        if (!usable(depthB)) {
            return false;
        }
        // The depth terms are m22 = f/(f-n) and m32 = -n*f/(f-n), so the near plane is their ratio and
        // no depth buffer at all lives outside a metre-and-a-bit of the eye.
        nearPlane = std::fabs(depthB) / std::fabs(depthA);
        if (!(nearPlane > 1e-4f) || nearPlane > 20.f) {
            return false;
        }
        // The far plane follows from the same two terms, and is left at zero where the pair implies an
        // infinite one -- which is a legal projection and not an error.
        farPlane = 0.f;
        if (std::fabs(depthA - 1.f) > 1e-4f) {
            // Not named `far`: windef.h keeps the 16-bit-era `far` and `near` as empty macros, and a
            // local of either name is silently deleted out of the line by the preprocessor.
            const float computed = depthA * nearPlane / (depthA - 1.f);
            if (std::isfinite(computed) && std::fabs(computed) < 1e6f) {
                farPlane = std::fabs(computed);
            }
        }
        return true;
    }

    bool LooksLikeProjection(const float* m, bool& columnMajor, float& nearPlane, float& farPlane) {
        const auto zero = [](float v) { return std::fabs(v) <= kZero; };
        // Not named `small`: rpcndr.h still defines that one as `char`.
        const auto bounded = [](float v) { return std::isfinite(v) && std::fabs(v) <= 4.f; };

        // [ m00  0   0   0 ]
        // [ 0   m11  0   0 ]
        // [ m20 m21 m22  1 ]
        // [ 0    0  m32  0 ]
        if (std::fabs(m[11] - 1.f) <= 1e-3f && zero(m[15]) && zero(m[1]) && zero(m[2]) && zero(m[3]) &&
            zero(m[4]) && zero(m[6]) && zero(m[7]) && zero(m[12]) && zero(m[13]) &&
            bounded(m[8]) && bounded(m[9])) {
            if (PlanesFrom(m[0], m[5], m[10], m[14], nearPlane, farPlane)) {
                columnMajor = false;
                return true;
            }
        }

        // [ m00  0   m20  0 ]
        // [ 0   m11  m21  0 ]
        // [ 0    0   m22 m32 ]
        // [ 0    0    1   0 ]
        if (std::fabs(m[14] - 1.f) <= 1e-3f && zero(m[15]) && zero(m[1]) && zero(m[3]) && zero(m[4]) &&
            zero(m[7]) && zero(m[8]) && zero(m[9]) && zero(m[12]) && zero(m[13]) &&
            bounded(m[2]) && bounded(m[6])) {
            if (PlanesFrom(m[0], m[5], m[10], m[11], nearPlane, farPlane)) {
                columnMajor = true;
                return true;
            }
        }

        return false;
    }

    // Copies one buffer into the staging buffer and hands back its bytes, or null. The copy is issued
    // on the application's context, which is why this only ever runs from Publish.
    const float* ReadBuffer(ID3D11Buffer* buffer, UINT byteWidth) {
        if (buffer == nullptr || byteWidth < 64 || g_context == nullptr) {
            return nullptr;
        }
        const UINT bytes = byteWidth < kMaxReadBytes ? byteWidth : kMaxReadBytes;

        if (g_staging == nullptr) {
            ComPtr<ID3D11Device> device;
            g_context->GetDevice(device.GetAddressOf());
            if (device == nullptr) {
                return nullptr;
            }
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = kMaxReadBytes;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateBuffer(&desc, nullptr, g_staging.ReleaseAndGetAddressOf()))) {
                g_staging.Reset();
                return nullptr;
            }
        }

        const D3D11_BOX box{0, 0, 0, bytes, 1, 1};
        g_context->CopySubresourceRegion(g_staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
        // The mapping below blocks until the copy has completed, but the copy only reaches the GPU
        // once the context is flushed, and nothing else in this frame would do it for us.
        g_context->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_context->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return nullptr;
        }
        return static_cast<const float*>(mapped.pData);
    }

    // A block of sixteen floats that matched the shape of a projection. Held by buffer pointer rather
    // than by table index: the table displaces its oldest entry, which would silently slide an index
    // onto a different buffer.
    struct Found {
        const char* stage{nullptr};
        ID3D11Buffer* buffer{nullptr};
        UINT byteWidth{0};
        UINT startSlot{0};
        uint32_t offset{0};
        bool columnMajor{true};
        float nearPlane{0.f};
        float farPlane{0.f};
        float scaleX{0.f};
        float scaleY{0.f};
        float depthA{0.f};
        float depthB{0.f};
        float value[16]{};
        bool announced{false};
    };

    constexpr size_t kMaxFound = 4;
    Found g_found[kMaxFound];
    size_t g_foundCount = 0;

    // Frame-to-frame movement of one block over a window. A projection the title jitters changes in a
    // couple of its off-centre terms on every frame by a sub-pixel amount, even while the head is
    // still; one that is not jittered only changes when the pose does. Reporting the lowest and
    // highest value each term reached, and how many frames moved at all, is what tells them apart --
    // a window over a still head that reports nothing is as strong an answer as one that reports a
    // repeating sub-pixel walk.
    struct Tracked {
        const char* stage{nullptr};
        ID3D11Buffer* buffer{nullptr};
        UINT byteWidth{0};
        uint32_t offset{0};
        uint32_t frames{0};
        uint32_t changedFrames{0};
        uint32_t reportedAt{0};
        float previous[16]{};
        float lowest[16]{};
        float highest[16]{};
    };

    constexpr size_t kMaxTracked = kMaxFound;
    Tracked g_tracked[kMaxTracked];
    size_t g_trackedCount = 0;

    uint32_t g_frames = 0;
    bool g_gaveUp = false;

    // How often a full scan of everything remembered is worth the copies. It is a handful of
    // sub-kilobyte copies, but they are paid for on the application's own context, so the scan stays
    // a fraction of the frames until it lands -- after which only the block found is read at all.
    constexpr uint32_t kScanInterval = 60;
    // The window a report covers, and the number of samples a window needs before it is worth one.
    constexpr uint32_t kReportInterval = 300;
    constexpr uint32_t kMinWindow = 60;

    void ReportTracked(const Tracked& tracked);

    // One raw look at everything remembered, taken once the level is up rather than on the first
    // sweep. The first run took it at frame 60, which turned out to be before the scene was drawing
    // anything: the only vertex constant buffer bound at that point was 64 bytes of screen-space
    // mapping, and the three pixel-stage ones were all smaller than a matrix. The scan below is the
    // question; this is the evidence, and it is only worth spending when there is a frame to look at.
    //
    // Four floats to a line, so the rows of a 4x4 block line up with the rows of the text. The dump
    // stops at kDumpBytes so that the log stays readable; the scan itself reads the whole buffer, so a
    // projection past this point is still found, just not printed.
    constexpr uint32_t kReportFrame = 600;
    constexpr UINT kDumpBytes = 1024;
    bool g_reported = false;

    void ReportBuffers() {
        if (g_reported || g_sources[0].count + g_sources[1].count == 0) {
            return;
        }
        g_reported = true;

        // The inventory first, including the buffers too small to hold a matrix: what the table is
        // holding is the context every number below has to be read against.
        for (size_t s = 0; s < 2; s++) {
            const Source& source = g_sources[s];
            for (size_t e = 0; e < source.count; e++) {
                LayerLog("projection probe: inventory [%s] #%zu slot %u, %u bytes\n",
                         source.name,
                         e,
                         source.entries[e].startSlot,
                         source.entries[e].byteWidth);
            }
        }

        for (size_t s = 0; s < 2; s++) {
            Source& source = g_sources[s];
            for (size_t e = 0; e < source.count; e++) {
                const UINT byteWidth = source.entries[e].byteWidth;
                if (byteWidth < 64) {
                    continue; // too small to hold a 4x4 at all
                }
                const float* floats = ReadBuffer(source.entries[e].buffer.Get(), byteWidth);
                if (floats == nullptr) {
                    continue;
                }
                const uint32_t floatCount =
                    (byteWidth < kDumpBytes ? byteWidth : kDumpBytes) / 4u;
                LayerLog("projection probe:   [%s] #%zu slot %u, %u bytes\n",
                         source.name,
                         e,
                         source.entries[e].startSlot,
                         byteWidth);
                for (uint32_t i = 0; i + 4 <= floatCount; i += 4) {
                    LayerLog("projection probe:     +%04u %13.6f %13.6f %13.6f %13.6f\n",
                             i * 4u,
                             floats[i],
                             floats[i + 1],
                             floats[i + 2],
                             floats[i + 3]);
                }
                g_context->Unmap(g_staging.Get(), 0);
            }
        }
    }

    // The offset form of the bind is a separate entry point, and a title that packs a frame's constants
    // into one buffer uses it: each draw points the stage at its own sub-range of the shared
    // allocation. Those calls never reach the two hooks above, so a buffer aimed at that way is
    // invisible to the remember table -- which is the likeliest reason a scene projection was never
    // seen, given that the buffers the table does hold turned out to carry view matrices, frustum
    // planes and target sizes and nothing else. Asking the context directly cannot be missed, needs no
    // extra entry point to be patched, and changes nothing: these are the getters.
    constexpr UINT kProbeSlots = 16;
    uint8_t g_boundLogged[2][kProbeSlots]{};

    void ProbeBoundSlots() {
        ComPtr<ID3D11DeviceContext1> context1;
        if (FAILED(g_context.As(&context1))) {
            return;
        }
        for (size_t s = 0; s < 2; s++) {
            for (UINT slot = 0; slot < kProbeSlots; slot++) {
                ID3D11Buffer* buffer = nullptr;
                UINT firstConstant = 0;
                UINT numConstants = 0;
                if (s == 0) {
                    context1->VSGetConstantBuffers1(slot, 1, &buffer, &firstConstant, &numConstants);
                } else {
                    context1->PSGetConstantBuffers1(slot, 1, &buffer, &firstConstant, &numConstants);
                }
                if (buffer == nullptr) {
                    continue;
                }
                NoteBuffer(g_sources[s], slot, buffer);
                // The sub-range is the interesting part: a shared allocation bound by offset says
                // where in the buffer this pass's constants start, which is what turns a 36 KB
                // monolith into something that can be read.
                if (g_frames >= kReportFrame && g_boundLogged[s][slot] == 0) {
                    g_boundLogged[s][slot] = 1;
                    LayerLog("projection probe: bound [%s] slot %u at +%u for %u constants\n",
                             g_sources[s].name,
                             slot,
                             firstConstant,
                             numConstants);
                }
                buffer->Release(); // NoteBuffer holds its own reference when the buffer was new
            }
        }
    }

    // A block that has the shape of a projection but not its numbers. Logged so that a run which finds
    // nothing still says what the title does keep there. The shape on its own proves nothing -- the
    // first runs matched on it alone and accepted plain data -- but it is the right thing to look at
    // once the numbers have ruled everything else out, and it is the difference between a blank page
    // and a lead.
    constexpr size_t kMaxNearMiss = 12;
    size_t g_nearMissCount = 0;

    bool NearMissShape(const float* m, bool& columnMajor) {
        const auto zero = [](float v) { return std::fabs(v) <= kZero; };
        int nonzero = 0;
        for (int i = 0; i < 16; i++) {
            if (std::fabs(m[i]) > kZero) {
                nonzero++;
            }
        }
        // A matrix's worth of content: a block of nothing but zeros and ones is data that happens to
        // line up with the shape, not a transform that failed the numbers.
        if (nonzero < 4) {
            return false;
        }
        if (std::fabs(m[11] - 1.f) <= 1e-3f && zero(m[15]) && zero(m[1]) && zero(m[2]) && zero(m[3]) &&
            zero(m[4]) && zero(m[6]) && zero(m[7]) && zero(m[12]) && zero(m[13])) {
            columnMajor = false;
            return true;
        }
        if (std::fabs(m[14] - 1.f) <= 1e-3f && zero(m[15]) && zero(m[1]) && zero(m[3]) && zero(m[4]) &&
            zero(m[7]) && zero(m[8]) && zero(m[9]) && zero(m[12]) && zero(m[13])) {
            columnMajor = true;
            return true;
        }
        return false;
    }

    void ScanSource(size_t sourceIndex) {
        Source& source = g_sources[sourceIndex];
        for (size_t e = 0; e < source.count; e++) {
            const UINT byteWidth = source.entries[e].byteWidth;
            const float* floats = ReadBuffer(source.entries[e].buffer.Get(), byteWidth);
            if (floats == nullptr) {
                continue;
            }
            const uint32_t floatCount = (byteWidth < kMaxReadBytes ? byteWidth : kMaxReadBytes) / 4u;
            for (uint32_t offset = 0; offset + 16 <= floatCount; offset += 4) {
                bool columnMajor = true;
                float nearPlane = 0.f;
                float farPlane = 0.f;
                const float* block = floats + offset;
                if (!LooksLikeProjection(block, columnMajor, nearPlane, farPlane)) {
                    // What the shape was, so that a sweep which accepts nothing is still a lead rather
                    // than a blank page. Held until the level is up: the startup frames' buffers are
                    // not the scene's, and their near misses are noise.
                    if (g_frames >= kReportFrame && g_nearMissCount < kMaxNearMiss) {
                        bool nearColumnMajor = false;
                        if (NearMissShape(block, nearColumnMajor)) {
                            g_nearMissCount++;
                            LayerLog("projection probe: near miss [%s] #%zu +%u %s: "
                                     "[ %f %f %f %f | %f %f %f %f | %f %f %f %f | %f %f %f %f ]\n",
                                     source.name,
                                     e,
                                     offset * 4u,
                                     nearColumnMajor ? "column-major" : "row-major",
                                     block[0], block[1], block[2], block[3],
                                     block[4], block[5], block[6], block[7],
                                     block[8], block[9], block[10], block[11],
                                     block[12], block[13], block[14], block[15]);
                        }
                    }
                    continue;
                }
                bool seen = false;
                for (size_t i = 0; i < g_foundCount; i++) {
                    if (g_found[i].buffer == source.entries[e].buffer.Get() &&
                        g_found[i].offset == offset * 4u) {
                        seen = true;
                        break;
                    }
                }
                if (seen || g_foundCount >= kMaxFound) {
                    continue;
                }
                Found& found = g_found[g_foundCount++];
                found.stage = source.name;
                found.buffer = source.entries[e].buffer.Get();
                found.byteWidth = byteWidth;
                found.startSlot = source.entries[e].startSlot;
                found.offset = offset * 4u;
                found.columnMajor = columnMajor;
                std::memcpy(found.value, floats + offset, sizeof(found.value));
                found.depthA = found.value[10];
                found.depthB = columnMajor ? found.value[11] : found.value[14];
                found.nearPlane = nearPlane;
                found.farPlane = farPlane;
                // m00 and m11 are 1/tan of the half-angles the title projected with, so they can be
                // read straight against the frustum the runtime handed the layer.
                found.scaleX = found.value[0];
                found.scaleY = found.value[5];
            }
            g_context->Unmap(g_staging.Get(), 0);
        }
    }

    void AnnounceFound() {
        for (size_t i = 0; i < g_foundCount; i++) {
            Found& found = g_found[i];
            if (found.announced) {
                continue;
            }
            found.announced = true;
            LayerLog("projection probe: [%s] +%u bytes (slot %u, %u wide), %s, zn=%.5f "
                     "zf=%.1f, m00/m11=%.4f/%.4f, depth=(%.6f, %.6f)\n",
                     found.stage,
                     found.offset,
                     found.startSlot,
                     found.byteWidth,
                     found.columnMajor ? "column-major" : "row-major",
                     found.nearPlane,
                     found.farPlane,
                     found.scaleX,
                     found.scaleY,
                     found.depthA,
                     found.depthB);
            LayerLog("projection probe:   [ %f %f %f %f | %f %f %f %f | %f %f %f %f | %f %f %f %f ]\n",
                     found.value[0], found.value[1], found.value[2], found.value[3],
                     found.value[4], found.value[5], found.value[6], found.value[7],
                     found.value[8], found.value[9], found.value[10], found.value[11],
                     found.value[12], found.value[13], found.value[14], found.value[15]);

            Tracked& tracked = g_tracked[g_trackedCount++];
            tracked.stage = found.stage;
            tracked.buffer = found.buffer;
            tracked.byteWidth = found.byteWidth;
            tracked.offset = found.offset;
        }
    }

    void SampleTracked() {
        for (size_t i = 0; i < g_trackedCount; i++) {
            Tracked& tracked = g_tracked[i];
            const float* floats = ReadBuffer(tracked.buffer, tracked.byteWidth);
            if (floats == nullptr) {
                // ReadBuffer only ever returns null before it has mapped anything, so there is
                // nothing to release and the next frame tries again.
                continue;
            }
            const float* block = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(floats) + tracked.offset);
            if (tracked.frames == 0) {
                std::memcpy(tracked.previous, block, sizeof(tracked.previous));
                std::memcpy(tracked.lowest, block, sizeof(tracked.lowest));
                std::memcpy(tracked.highest, block, sizeof(tracked.highest));
            } else {
                bool changed = false;
                for (uint32_t f = 0; f < 16; f++) {
                    if (block[f] != tracked.previous[f]) {
                        changed = true;
                    }
                    tracked.lowest[f] = block[f] < tracked.lowest[f] ? block[f] : tracked.lowest[f];
                    tracked.highest[f] = block[f] > tracked.highest[f] ? block[f] : tracked.highest[f];
                }
                if (changed) {
                    tracked.changedFrames++;
                }
                std::memcpy(tracked.previous, block, sizeof(tracked.previous));
            }
            tracked.frames++;
            g_context->Unmap(g_staging.Get(), 0);

            if (tracked.frames >= kMinWindow && tracked.frames - tracked.reportedAt >= kReportInterval) {
                tracked.reportedAt = tracked.frames;
                ReportTracked(tracked);
            }
        }
    }

    void ReportTracked(const Tracked& tracked) {
        LayerLog("projection probe: [%s] +%u held over %u frames, %u moved\n",
                 tracked.stage,
                 tracked.offset,
                 tracked.frames,
                 tracked.changedFrames);
        // Only the terms that actually moved are listed, so a still head shows either nothing (no
        // jitter) or a handful of off-centre terms with a sub-pixel spread (jitter).
        for (uint32_t f = 0; f < 16; f++) {
            const float spread = tracked.highest[f] - tracked.lowest[f];
            if (spread > 1e-7f) {
                LayerLog("projection probe:   [%u] spread %.6f (%.6f .. %.6f)\n",
                         f,
                         spread,
                         tracked.lowest[f],
                         tracked.highest[f]);
            }
        }
    }

} // namespace

void ProjectionProbeAttach(ID3D11DeviceContext* context) {
    if (g_installed || context == nullptr || !ProbeRequested()) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr) {
        return;
    }

    void* originalVS = nullptr;
    void* originalPS = nullptr;
    if (!PatchSlot(vtable, kSlotVSSetConstantBuffers, (void*)&Hook_VSSetConstantBuffers, originalVS)) {
        LayerLog("projection probe: could not make the virtual table writable, staying off\n");
        return;
    }
    if (!PatchSlot(vtable, kSlotPSSetConstantBuffers, (void*)&Hook_PSSetConstantBuffers, originalPS)) {
        void* ignored = nullptr;
        PatchSlot(vtable, kSlotVSSetConstantBuffers, originalVS, ignored);
        LayerLog("projection probe: could not hook the pixel stage, staying off\n");
        return;
    }

    g_originalVS = (SetConstantBuffersFn)originalVS;
    g_originalPS = (SetConstantBuffersFn)originalPS;
    g_patchedVtable = vtable;
    // Held rather than borrowed: the copy in Publish has to be issued on the same context the hooks
    // are installed for, and this is the only place it is ever handed over.
    g_context = context;
    g_installed = true;
    LayerLog("projection probe: watching the constant buffers, looking for the projection\n");
}

void ProjectionProbeDetach() {
    if (g_installed && g_patchedVtable != nullptr) {
        DWORD previous = 0;
        if (VirtualProtect(&g_patchedVtable[kSlotVSSetConstantBuffers],
                           sizeof(void*),
                           PAGE_READWRITE,
                           &previous)) {
            g_patchedVtable[kSlotVSSetConstantBuffers] = (void*)g_originalVS;
            DWORD ignored = 0;
            VirtualProtect(&g_patchedVtable[kSlotVSSetConstantBuffers], sizeof(void*), previous, &ignored);
        }
        if (VirtualProtect(&g_patchedVtable[kSlotPSSetConstantBuffers],
                           sizeof(void*),
                           PAGE_READWRITE,
                           &previous)) {
            g_patchedVtable[kSlotPSSetConstantBuffers] = (void*)g_originalPS;
            DWORD ignored = 0;
            VirtualProtect(&g_patchedVtable[kSlotPSSetConstantBuffers], sizeof(void*), previous, &ignored);
        }
    }

    if (g_installed) {
        LayerLog("projection probe: detached after %u frames, %zu block(s) found\n", g_frames, g_foundCount);
    }
    g_installed = false;
    g_patchedVtable = nullptr;
    g_originalVS = nullptr;
    g_originalPS = nullptr;

    for (Source& source : g_sources) {
        for (size_t i = 0; i < source.count; i++) {
            source.entries[i].buffer.Reset();
        }
        source.count = 0;
        source.last = nullptr;
    }
    g_parked.clear();
    g_staging.Reset();
    g_foundCount = 0;
    g_trackedCount = 0;
    g_nearMissCount = 0;
    std::memset(g_boundLogged, 0, sizeof(g_boundLogged));
    g_frames = 0;
    g_gaveUp = false;
    g_reported = false;
    g_context.Reset();
}

void ProjectionProbePublish() {
    if (!g_installed) {
        return;
    }
    g_frames++;

    // Every frame, and before anything else: this is what makes a buffer bound by offset visible at
    // all, and it is the only step here that cannot be defeated by which entry point the title chose.
    ProbeBoundSlots();

    if (g_trackedCount == 0) {
        if (g_frames == kReportFrame) {
            ReportBuffers();
        }
        // Nothing found yet: sweep everything remembered, but only occasionally -- each sweep copies
        // every buffer in both tables, on the application's own context.
        if (g_frames % kScanInterval != 0) {
            return;
        }
        ScanSource(0);
        ScanSource(1);
        if (g_foundCount > 0) {
            AnnounceFound();
        }
        // One "nothing yet" report, not one per sweep: a title whose projection never matches would
        // otherwise fill the log with the same line twice a second for the whole session.
        else if (g_frames >= kScanInterval * 4 && !g_gaveUp) {
            g_gaveUp = true;
            LayerLog("projection probe: no projection-shaped block in %zu buffers after %u frames -- "
                     "the title may keep its projection inside a combined view-projection matrix\n",
                     g_sources[0].count + g_sources[1].count,
                     g_frames);
        }
        return;
    }

    // Found: from here on only the block itself is read, one small copy a frame.
    SampleTracked();
}
