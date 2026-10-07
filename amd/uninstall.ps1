# Removes the layer registration. The DLL and manifest are left in place.

$ErrorActionPreference = 'Stop'

$manifest = 'C:\Users\Administrator\vdxr\xr-layer\AmdnrXrLayer.json'
$key      = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

if (Test-Path $key) {
    Remove-ItemProperty -Path $key -Name $manifest -ErrorAction SilentlyContinue
}

'--- registered implicit layers ---'
$key2 = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $key2) {
    $props = (Get-ItemProperty $key2).PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' }
    if ($props) { $props | ForEach-Object { "  $($_.Name)  =>  $($_.Value)" } } else { '  (none)' }
} else {
    '  (none)'
}
