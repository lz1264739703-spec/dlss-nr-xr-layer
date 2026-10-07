# Builds the AMDNR OpenXR implicit API layer and stages it next to its manifest.
#
# The layer is a plain DLL with no third party dependencies, so a direct cl.exe invocation is
# enough - no solution or project file to keep in sync.

$ErrorActionPreference = 'Stop'

$source      = $PSScriptRoot
$output      = 'C:\Users\Administrator\vdxr\xr-layer'
$openXrInc   = 'C:\Users\Administrator\vdxr\external\OpenXR-SDK\include'
# The FSR headers are compiled into the layer (CPU side constants) and read back at runtime by the
# shaders' #include resolver, which looks next to the DLL.
$fsrInc      = (Resolve-Path (Join-Path $source '..\vdxr-src\external\FidelityFX-FSR\ffx-fsr')).Path

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
    $fsrInc
) -join ';'

$env:LIB = @(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'

New-Item -ItemType Directory -Force -Path $output | Out-Null

# The panel and NrSettingsSet are two halves of one contract: a slider must not offer a value the
# layer would clamp, because the panel would then display a number it does not honour. Checked first,
# so a drift costs a second rather than a compile and a manual read of the panel.
& (Join-Path $source 'check-panel-ranges.ps1')
if ($LASTEXITCODE -ne 0) {
    throw 'the panel offers a value the layer would clamp (see the list above)'
}

Push-Location $output
try {
    & (Join-Path $msvc 'bin\Hostx64\x64\cl.exe') `
        /nologo /W4 /O2 /EHsc /std:c++20 /utf-8 /LD /MD `
        /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
        (Join-Path $source 'AmdnrXrLayer.cpp') `
        (Join-Path $source 'EyeInterop.cpp') `
        (Join-Path $source 'Fsr4Upscale.cpp') `
        (Join-Path $source 'NrCore.cpp') `
        (Join-Path $source 'NrControlServer.cpp') `
        (Join-Path $source 'AmdnrDepthCapture.cpp') `
        (Join-Path $source 'AmdnrProjectionProbe.cpp') `
        (Join-Path $source 'AmdnrStereoProbe.cpp') `
        (Join-Path $source 'AmdnrDepthMsaaRead.cpp') `
        (Join-Path $source 'AmdnrDepthInput.cpp') `
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
