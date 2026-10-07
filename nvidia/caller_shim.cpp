// The caller shim DLSS-NR requires.
//
// nvngx_dlssnr.dll checks which module owns the RETURN ADDRESS of the call into it, and rejects
// the caller outright with 0xBAD00002 when that module is not the one it expects. A layer that
// calls the snippet's entry points directly -- even from its own code -- fails that check, and so
// does a wrapper the compiler tail-call optimised into a JMP, because the return address then
// still belongs to the layer.
//
// This DLL exists to be that module. Each export below takes the real entry point as its first
// argument and makes an ordinary CALL through it, so the return address lives here. The build
// compiles this file with /Od for the same reason the source marks every export noinline: an
// optimiser that turned the call into a jump would put the address back in the layer's module and
// bring the rejection back.
//
// The exports keep the layer-facing argument order; the snippet's own ABI is where the two agree
// with the SDK and where they differ, so the reordering happens here rather than at the call site.
//
// The type spellings are deliberately local: this file must compile on its own, with no NGX SDK
// headers and no import library, and the pointers it forwards are opaque to it.
//
// Adapted from the MIT-licensed caller shim in ComfyUI-DLSS5-NR
// (https://github.com/lisitskyaa/ComfyUI-DLSS5-NR), which carries the same mechanism for the same
// 0xBAD00002.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <d3d12.h>

using NGXResult = int;
struct NGXHandle {
    unsigned int Id;
};
struct NGXParameter;

// The snippet build's own ABI. Init_Ext differs from the SDK's: the FeatureCommonInfo comes before
// the version, where the SDK header puts the version first.
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using CreateFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvalFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFn = NGXResult(__cdecl*)(NGXHandle*);

// Read after the call so the wrapper cannot be reduced to a tail call. The value is never used for
// anything else; its only job is to be a use of the result that the optimiser cannot remove.
static volatile LONG g_post_call_sink = 0;
static __forceinline NGXResult FinishCall(NGXResult result) {
    g_post_call_sink = static_cast<LONG>(result);
    return result;
}

extern "C" {

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallInit(
    void* real_fn, unsigned long long app_id, const wchar_t* path,
    ID3D12Device* device, int version, const void* common_info) {
    NGXResult result =
        reinterpret_cast<SnippetInitFn>(real_fn)(app_id, path, device, common_info, version);
    return FinishCall(result);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallCreate(
    void* real_fn, ID3D12GraphicsCommandList* list, int feature_id,
    NGXParameter* parameter, NGXHandle** handle) {
    NGXResult result = reinterpret_cast<CreateFn>(real_fn)(list, feature_id, parameter, handle);
    return FinishCall(result);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallEvaluate(
    void* real_fn, ID3D12GraphicsCommandList* list, const NGXHandle* handle,
    const NGXParameter* parameter, void* callback) {
    NGXResult result = reinterpret_cast<EvalFn>(real_fn)(list, handle, parameter, callback);
    return FinishCall(result);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallRelease(
    void* real_fn, NGXHandle* handle) {
    NGXResult result = reinterpret_cast<ReleaseFn>(real_fn)(handle);
    return FinishCall(result);
}

} // extern "C"
