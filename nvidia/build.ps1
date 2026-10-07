# Builds the NVIDIA variant of the AMDNR OpenXR implicit API layer and stages it next to its manifest.
#
# This tree is the DLSS-NR through NGX build. It drives the driver's NGX core and carries its own
# caller shim (caller\nvngx.dll), which the DLSS-NR runtime requires before it will accept a call.
# It carries no lmxxf backend and no runtime selection between the two, so nothing here is decided
# by the adapter's vendor. The AMD variant is the sibling tree, ..\xr-layer, with its own build.ps1
# and its own output folder.
#
# The layer is a plain DLL with no third party dependencies, so a direct cl.exe invocation is
# enough - no solution or project file to keep in sync.

$ErrorActionPreference = 'Stop'

$source      = $PSScriptRoot
# Its own output folder: this tree must never overwrite the layer the AMD build installs.
$output      = 'C:\Users\Administrator\vdxr\xr-layer-ngx'
$openXrInc   = 'C:\Users\Administrator\vdxr\external\OpenXR-SDK\include'
# The FSR headers are compiled into the layer (CPU side constants) and read back at runtime by the
# shaders' #include resolver, which looks next to the DLL.
$fsrInc      = (Resolve-Path (Join-Path $source '..\vdxr-src\external\FidelityFX-FSR\ffx-fsr')).Path
# The NGX SDK headers for the DLSS-NR backend. Only the headers are needed: the entry points are
# resolved by name at runtime, exactly as the AMD runtime is, so nothing is linked against them.
$ngxInc      = (Resolve-Path (Join-Path $source '..\vdxr-src\external\DLSS\include')).Path

$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$sdkRoot  = 'C:\Program Files (x86)\Windows Kits\10'

$msvcVersion = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkVersion  = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

$msvc = Join-Path $msvcRoot $msvcVersion
$sdkInc = Join-Path $sdkRoot "Include\$sdkVersion"
$sdkLib = Join-Path $sdkRoot "Lib\$sdkVersion"

$env:INCLUDE = @(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkInc 'ucrt'),
    (Join-Path $sdkInc 'shared'),
    (Join-Path $sdkInc 'um'),
    (Join-Path $sdkInc 'winrt'),
    $openXrInc,
    $fsrInc,
    $ngxInc
) -join ';'

$env:LIB = @(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'

New-Item -ItemType Directory -Force -Path $output | Out-Null

# The caller shim is a second DLL, built first because the layer refuses to start without it
# beside the layer. It exists because the DLSS-NR runtime checks which module owns the return
# address of the call into it, and only accepts a call made from a module of its own kind -- a
# direct call from this layer is answered with 0xBAD00002. /Od is not a debugging leftover: an
# optimiser that turned the forwarding call into a jump would leave the return address in the
# layer again and bring the rejection back with it.
#
# It is deliberately named nvngx.dll, and it must stay that way. The runtime does not only check
# which module owns the return address -- it also looks at what that module is called, and a caller
# the layer supplies under any other name is answered with 0xBAD00002 exactly as a direct call from
# the layer is. Both known-working integrations keep this name for that reason: one ships it as
# nvngx.dll, the other as nvngx.dll_comfy.dll, which shares the prefix.
$callerDir = Join-Path $output 'caller'
New-Item -ItemType Directory -Force -Path $callerDir | Out-Null

Push-Location $callerDir
try {
    & (Join-Path $msvc 'bin\Hostx64\x64\cl.exe') `
        /nologo /W4 /Od /EHsc /std:c++17 /utf-8 /LD /MT `
        /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
        (Join-Path $source 'caller_shim.cpp') `
        /Fo:"$callerDir\\" /Fe:"$callerDir\nvngx.dll"
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed building the caller shim with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Push-Location $output
try {
    & (Join-Path $msvc 'bin\Hostx64\x64\cl.exe') `
        /nologo /W4 /O2 /EHsc /std:c++20 /utf-8 /LD /MD `
        /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
        (Join-Path $source 'AmdnrXrLayer.cpp') `
        (Join-Path $source 'EyeInterop.cpp') `
        (Join-Path $source 'NgxNr.cpp') `
        (Join-Path $source 'NrControlServer.cpp') `
        /Fo:"$output\\" /Fe:"$output\AmdnrXrLayer.dll" `
        /link d3d11.lib d3d12.lib dxgi.lib uuid.lib ole32.lib winmm.lib ws2_32.lib
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Copy-Item (Join-Path $source 'AmdnrXrLayer.json') (Join-Path $output 'AmdnrXrLayer.json') -Force

# The shaders include these by name at runtime, so they have to sit beside the DLL.
Copy-Item (Join-Path $fsrInc 'ffx_a.h') (Join-Path $output 'ffx_a.h') -Force
Copy-Item (Join-Path $fsrInc 'ffx_fsr1.h') (Join-Path $output 'ffx_fsr1.h') -Force

Get-ChildItem $output | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
