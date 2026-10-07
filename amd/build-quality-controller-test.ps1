# Builds and runs the NrQualityController self test.
#
# The controller is header-only and the test is a plain console program, so a direct cl.exe
# invocation is enough. This mirrors the INCLUDE/LIB setup of build.ps1 without touching that file.

$ErrorActionPreference = 'Stop'

$source = $PSScriptRoot

$msvcRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$sdkRoot  = 'C:\Program Files (x86)\Windows Kits\10'

$msvcVersion = (Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$sdkVersion  = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

$msvc   = Join-Path $msvcRoot $msvcVersion
$sdkInc = Join-Path $sdkRoot "Include\$sdkVersion"
$sdkLib = Join-Path $sdkRoot "Lib\$sdkVersion"

$env:INCLUDE = @(
    (Join-Path $msvc 'include'),
    (Join-Path $sdkInc 'ucrt'),
    (Join-Path $sdkInc 'shared'),
    (Join-Path $sdkInc 'um')
) -join ';'

$env:LIB = @(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'

Push-Location $source
try {
    & (Join-Path $msvc 'bin\Hostx64\x64\cl.exe') `
        /nologo /W4 /O2 /EHsc /std:c++20 /utf-8 /MD `
        'nr-quality-controller-test.cpp' `
        /Fe:"nr-quality-controller-test.exe"
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE"
    }

    & '.\nr-quality-controller-test.exe'
    $testExit = $LASTEXITCODE
} finally {
    Pop-Location
}

exit $testExit
