// Caller-validating shim for the NVIDIA DLSS 5 Neural Rendering runtime.
//
// nvngx_dlssnr.dll checks which module a call into it was made from: it looks at the return
// address left on the stack and refuses a caller it does not recognise with 0xBAD00002. This layer
// is built /O2, so a direct call into one of the runtime's entry points is tail-called -- the call
// becomes a JMP and the return address stays in the layer's own module -- and is refused.
//
// The fix is to put a frame from another module between the two. This file is that module: it is
// built on its own, as its own DLL, and always /Od, so every forwarder keeps a real CALL. A
// __declspec(noinline) frame plus a volatile write after the call stop the tail call even if the
// optimiser is turned back on. Each exported function takes the real entry point as its first
// argument and forwards to it; the return address the runtime sees belongs here.
//
// Build (see build.ps1; this file is NOT part of the layer's source list -- linking it into the
// layer would put the return addresses back where the runtime refuses them):
//   cl /nologo /std:c++17 /EHsc /MT /LD /Od NvngxCaller.cpp /Fe:...\caller\nvngx.dll
//
// Every function body here is kept free of C++ objects that need unwinding: an SEH frame in such a
// function is refused with C2712 under /EHsc. Plain pointers and POD only.

#include <windows.h>

namespace {

    // Written after every forwarded call. A volatile store is a side effect the optimiser may not
    // discard, which is what keeps the forwarded call from being turned back into a tail call.
    volatile long g_callerSink = 0;

    // A fault inside the runtime must not escape as an exception. The layer reads a returned
    // failure code the same way it reads one the runtime produced itself.
    constexpr long kCallerFault = (long)0xBAD00000L;

    inline long Sink(long value) {
        g_callerSink = value;
        return value;
    }

} // namespace

extern "C" __declspec(dllexport) __declspec(noinline) long __cdecl DLSSNR_CallInit(
    void* fn, unsigned long long appId, const wchar_t* path, void* device, const void* commonInfo,
    int version) {
    // The snippet's Init_Ext ABI is (appId, path, device, FeatureCommonInfo*, version): its last
    // two parameters are the reverse of the core SDK's (appId, path, device, version, commonInfo).
    using Fn = long(__cdecl*)(unsigned long long, const wchar_t*, void*, const void*, int);
    long result = kCallerFault;
    __try {
        result = reinterpret_cast<Fn>(fn)(appId, path, device, commonInfo, version);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kCallerFault;
    }
    return Sink(result);
}

extern "C" __declspec(dllexport) __declspec(noinline) long __cdecl DLSSNR_CallCreate(
    void* fn, void* cmd, int feature, const void* parameters, void** handle) {
    using Fn = long(__cdecl*)(void*, int, const void*, void**);
    long result = kCallerFault;
    __try {
        result = reinterpret_cast<Fn>(fn)(cmd, feature, parameters, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kCallerFault;
    }
    return Sink(result);
}

extern "C" __declspec(dllexport) __declspec(noinline) long __cdecl DLSSNR_CallEvaluate(
    void* fn, void* cmd, const void* handle, const void* parameters, void* callback) {
    using Fn = long(__cdecl*)(void*, const void*, const void*, void*);
    long result = kCallerFault;
    __try {
        result = reinterpret_cast<Fn>(fn)(cmd, handle, parameters, callback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kCallerFault;
    }
    return Sink(result);
}

extern "C" __declspec(dllexport) __declspec(noinline) long __cdecl DLSSNR_CallRelease(void* fn,
                                                                                      void* handle) {
    using Fn = long(__cdecl*)(void*);
    long result = kCallerFault;
    __try {
        result = reinterpret_cast<Fn>(fn)(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kCallerFault;
    }
    return Sink(result);
}