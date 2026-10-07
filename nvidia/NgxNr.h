// The NVIDIA DLSS-NR (NGX) neural renderer, wrapped for the OpenXR layer.
//
// The second backend for the pass NrCore drives on AMD. It takes the same crop of the eye buffer,
// denoises it through DLSS-NR and hands the answer back for the layer's own blend, and it exposes
// the same method surface so the two classes are interchangeable to EyeInterop. Where the split
// falls is different, though: the runtime is asked to run while the producer's command list is
// being recorded, because that is the only point at which a command list is available before the
// answer is needed. Launch/Enqueue/WaitAnswer therefore only exist to keep the surface identical
// and do nothing.
//
// Ported from the MIT-licensed VirtualDesktop-OpenXR implementation of this pass; see the
// attribution at the top of NgxNr.cpp.

#pragma once

#include <windows.h>

#include <d3d12.h>

#include <chrono>
#include <cstdint>

#include <wrl/client.h>

#include "LayerLog.h"
#include "NrTypes.h"

using Microsoft::WRL::ComPtr;

// Opaque to this header; the NGX SDK types are only needed in the translation unit.
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

// What the anti-flicker filter runs with on this frame, as the consumer's shader reads it. The
// history itself is a pair of the layer's own textures (see NgxNr); this is only the knobs and
// whether there is anything in them to read yet.
//
// The filter exists on this backend for the same reason it does on the AMD one: the feature is reset
// every frame (this layer hands it no motion vectors, so its own history would smear whatever moved),
// which makes the answer a fresh guess each frame -- and what flickers is the correction it adds over
// the untouched crop. That difference, not the picture, is what is carried across frames here.
struct NgxFilter {
    bool on{false};
    float strength{0.f};
    // How far the correction may move, per channel, before the history is dropped entirely for that
    // pixel and the current frame is taken instead.
    float gate{0.f};
    bool historyValid{false};
};

class NgxNr {
  public:
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();
    bool IsReady() const {
        return m_parameter != nullptr;
    }

    // Nothing here asks for one session per eye. DLSS-NR carries its history inside the feature and
    // the layer's motion field is an lmxxf idea, so the eye a call belongs to is named explicitly
    // instead -- see SelectEye.
    bool TemporalEnabled() const {
        return false;
    }

    uint32_t MaxInputWidth() const {
        return m_maxWidth;
    }
    uint32_t MaxInputHeight() const {
        return m_maxHeight;
    }
    const char* LastError() const {
        return m_lastError;
    }

    // The network runs inside RecordProducer, so there is no enqueue and no wait to account for.
    void TakeLaunchTotals(double& enqueue, double& wait);

    // The crop is the network's input at the resolution it already has, so the passed conversion
    // geometry is only used to name the answer: DLSS-NR denoises at the size it is handed. Which
    // feature is driven is whatever SelectEye last named.
    bool RecordProducer(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* source,
                        DXGI_FORMAT sourceViewFormat,
                        const NrConvert& convert,
                        const NrMotion& motion);
    ID3D12Resource* InputFp16() const {
        return nullptr;
    }

    // The synchronous path never leaves the answer outstanding, so Launch does nothing. The
    // asynchronous path does: it submits the producer's list and moves on, which is what Enqueue
    // and WaitAnswer are here to make safe. See their definitions.
    bool Launch();
    bool Enqueue();
    bool WaitAnswer();

    // The answer DLSS-NR wrote during the producer's list is blended into `destination`, the same
    // conversion NrCore runs on its own answer so the two backends look alike.
    bool RecordConsumer(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* destination,
                        DXGI_FORMAT destinationFormat,
                        ID3D12Resource* original,
                        DXGI_FORMAT originalViewFormat,
                        const NrConvert& convert);

    // No cross-eye detail transfer on this backend: DLSS-NR is run for each eye on its own.
    bool RecordShiftSearch(ID3D12GraphicsCommandList* cmd,
                           ID3D12Resource* left,
                           DXGI_FORMAT leftFormat,
                           ID3D12Resource* right,
                           DXGI_FORMAT rightFormat,
                           uint32_t width,
                           uint32_t height,
                           const NrShiftSearch& search);
    bool TakeShiftSearch(const NrShiftSearch& search, int32_t& bestShift);
    bool RecordTransfer(ID3D12GraphicsCommandList* cmd,
                        ID3D12Resource* destination,
                        DXGI_FORMAT destinationFormat,
                        ID3D12Resource* rightCrop,
                        DXGI_FORMAT rightCropFormat,
                        ID3D12Resource* leftOutput,
                        DXGI_FORMAT leftOutputFormat,
                        ID3D12Resource* leftCrop,
                        DXGI_FORMAT leftCropFormat,
                        const NrTransfer& transfer,
                        const NrConvert& convert);

    // Names the eye the next producer/consumer pair belongs to. One session can serve both eyes,
    // and a DLSS-NR feature holds a history of its own, so the two must never share one.
    void SelectEye(uint32_t eye) {
        m_eye = eye & 1u;
    }

  private:
    bool LoadNgx(ID3D12Device* device);
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height);
    // Hands one eye's feature back to the runtime. Called where the feature is being replaced: the
    // size changed, or a model-strength control did.
    void ReleaseFeature(uint32_t eye);
    bool EnsureInput(uint32_t width, uint32_t height);
    bool EnsureAnswer(uint32_t width, uint32_t height);
    bool EnsureResultTexture(ID3D12Resource* destination, DXGI_FORMAT format);
    bool EnsureConvertPipeline();
    // The reprojection field the anti-flicker filter warps its history with. Built here rather than
    // asked for: the host hands RecordProducer its view geometry, and the feature is given no motion
    // vectors at all -- this field serves the layer's own filter and nothing else.
    bool EnsureMotionPipeline();
    bool RecordMotion(ID3D12GraphicsCommandList* cmd, const NrMotion& motion, uint32_t width, uint32_t height);
    // The history pair, at the size the consumer writes (the colour crop). Both are allocated
    // together and both are dropped together: a pair that disagreed about the image would be worse
    // than no history at all.
    bool EnsureFilterTextures(uint32_t width, uint32_t height);
    // Copies a box of the history that was just written into a readback buffer, on the sampling
    // frames only, and reads the previous sample back on the frame after. Reading one frame late is
    // what makes it free: the frame that recorded the copy was submitted and waited on before the
    // next one runs, so the map cannot stall the GPU.
    void ProbeFilterHistory(ID3D12GraphicsCommandList* cmd, uint32_t written);
    void ReadFilterProbe();
    // Everything that invalidates one history invalidates the other, because they describe the same
    // image: a rebuild, a control change, a resolution change, a gap. Written once here so no caller
    // can forget the second one.
    void InvalidateHistory();
    void RecordConvert(ID3D12GraphicsCommandList* cmd,
                       ID3D12Resource* source,
                       DXGI_FORMAT sourceViewFormat,
                       ID3D12Resource* original,
                       DXGI_FORMAT originalViewFormat,
                       ID3D12Resource* destination,
                       DXGI_FORMAT destinationFormat,
                       const NrConvert& convert,
                       const NgxFilter& filter);
    void Fail(const char* message);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;

    NVSDK_NGX_Parameter* m_parameter{nullptr};
    // One feature per eye: each keeps its own history, and the two eyes are two different pictures.
    NVSDK_NGX_Handle* m_feature[2]{};
    uint32_t m_featureWidth[2]{};
    uint32_t m_featureHeight[2]{};

    // The half-float picture the network is handed, and the answer it writes. The network works in
    // half float on both ends -- that is the format both working integrations feed it and read from
    // -- so the eight bit crop is converted into m_inputFp16 on the way in and the answer is
    // converted back out of m_answer on the way to the eye. Neither is shared with the other API:
    // these are committed textures this layer owns, with the unordered-access flag the feature needs
    // to be able to write through a UAV.
    ComPtr<ID3D12Resource> m_inputFp16;
    uint32_t m_inputWidth{0};
    uint32_t m_inputHeight{0};

    ComPtr<ID3D12Resource> m_answer;
    uint32_t m_answerWidth{0};
    uint32_t m_answerHeight{0};
    bool m_produced{false};

    // Where the conversion lands on the way back to the eye. The texture the other API reads is
    // shared with it and carries no unordered-access flag, so a view of it that a compute shader
    // writes through cannot be made of it -- that is a call the runtime refuses and takes the device
    // down over. The result is converted into this one instead, and copied across afterwards.
    ComPtr<ID3D12Resource> m_result8;
    uint32_t m_resultWidth{0};
    uint32_t m_resultHeight{0};
    DXGI_FORMAT m_resultFormat{DXGI_FORMAT_UNKNOWN};

    // The asynchronous path submits the producer's list without waiting for it, so the answer is
    // still in flight when RecordProducer returns. Enqueue signals this fence behind that list and
    // WaitAnswer waits on it, which is what makes the next frame's reset of the allocator the list
    // was recorded from legal.
    ComPtr<ID3D12Fence> m_answerFence;
    HANDLE m_answerEvent{nullptr};
    uint64_t m_answerValue{0};

    ComPtr<ID3D12RootSignature> m_convertRootSignature;
    ComPtr<ID3D12PipelineState> m_convertPipeline;
    ComPtr<ID3D12DescriptorHeap> m_convertHeap;

    // The motion field, rebuilt every frame the anti-flicker filter is on, from the view geometry the
    // host hands the producer. One field per session serves both eyes because each eye records its
    // own before it consumes it, which is the same pairing the AMD build runs with.
    ComPtr<ID3D12RootSignature> m_motionRootSignature;
    ComPtr<ID3D12PipelineState> m_motionPipeline;
    ComPtr<ID3D12DescriptorHeap> m_motionHeap;
    ComPtr<ID3D12Resource> m_motion;

    // The anti-flicker history, ping-ponged: `m_filterRead` names the one sampled this frame and the
    // other is written. Both live at the colour crop's size, the grid the correction is computed on.
    ComPtr<ID3D12Resource> m_filterHistory[2];
    uint32_t m_filterWidth{0};
    uint32_t m_filterHeight{0};
    uint32_t m_filterRead{0};
    // Whether the pair holds a history about the picture being shown. False until the first frame
    // writes one, and again after anything that invalidates it.
    bool m_filterValid{false};
    // The strength and gate the pair was accumulated with. A live change starts a clean history
    // rather than mixing two settings; -1 is "the filter has not run yet".
    float m_filterStrength{-1.f};
    float m_filterGate{-1.f};
    uint64_t m_filterFrames{0};
    uint64_t m_filterResets{0};
    // The probe: a box of the history copied to a readback buffer and decoded on the CPU. Staged on
    // one frame and read on the next, so the map never stalls the GPU.
    ComPtr<ID3D12Resource> m_filterProbe;
    uint32_t m_filterProbeWidth{0};
    uint32_t m_filterProbeHeight{0};
    bool m_filterProbePending{false};
    uint64_t m_filterProbeFrame{0};

    // When the last producer ran, for the gap rule: a picture that arrives after a long stall (a
    // loading screen, a menu, a minimised window) has no usable relationship with the history, and
    // the motion field cannot say how far the content travelled while nothing was rendered.
    std::chrono::steady_clock::time_point m_lastFrameAt;
    bool m_lastFrameAtValid{false};

    // The model-strength controls as the last frame saw them -- intensity, tone, structure, skin,
    // style, mask -- cached so a change can be told from a steady value. `m_rebuildFeature` is then
    // set for both eyes: each eye's own producer releases its feature on its next pass, which is the
    // only point at which that eye's feature is safe to touch.
    float m_controls[6]{};
    bool m_controlsValid{false};
    bool m_rebuildFeature[2]{false, false};

    uint32_t m_eye{0};
    // Set the first time DLSS-NR refuses to create a feature; from then on the pass is simply off
    // rather than retried every frame with the same log line.
    bool m_failed{false};
    // Whether this session actually took a reference on the shared NGX state. A session that never
    // came up (the second one, on a backend that only opened the first) must not release it.
    bool m_registered{false};
    uint32_t m_maxWidth{1920};
    uint32_t m_maxHeight{1080};
    const char* m_lastError{""};
};
