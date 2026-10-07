# Registers the layer as an OpenXR implicit API layer.
#
# Implicit layers apply to every OpenXR application that goes through the standard loader, without
# touching the system's ActiveRuntime.
#
# This is the NVIDIA tree's own manifest: build.ps1 stages to vdxr\xr-layer-ngx, and the sibling
# tree's folder must stay untouched, so the registration points at this tree's output.

$ErrorActionPreference = 'Stop'

$manifest = 'C:\Users\Administrator\vdxr\xr-layer-ngx\AmdnrXrLayer.json'
$key      = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

if (-not (Test-Path $manifest)) {
    throw "Manifest not found: $manifest (run build.ps1 first)"
}

New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name $manifest -PropertyType DWord -Value 0 -Force | Out-Null

'--- registered implicit layers ---'
(Get-ItemProperty $key).PSObject.Properties |
    Where-Object { $_.Name -notlike 'PS*' } |
    ForEach-Object { "  $($_.Name)  =>  $($_.Value)" }
