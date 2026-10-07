# Checks that every numeric control on the panel offers only values the layer will keep.
#
# The panel and the clamp are two halves of one contract: the slider decides what a reader can ask
# for, and NrSettingsSet decides what the layer will honour. When the two drift, the panel shows a
# number the layer silently changes, and the reading is a lie. The pass count is the one setting
# where that lie already cost a debugging session, so the rest are checked here instead of by eye.
#
# The panel's range must sit *inside* the clamp, not equal it: a slider is deliberately narrower
# than the guard in places (the near plane bottoms out at a centimetre on the slider but accepts 0
# as "off"), and the number box beside it is wider in two places. Both the slider and the box are
# checked, so the union of what the panel can send is what has bounds.
#
# The bounds are read from the source, not repeated here: the floats from the ClampFinite calls in
# NrControlServer.cpp, the pass and ceiling bounds from std::clamp, and the ceiling's named limits
# from NrSettings.h. A clamp that changes without this script being touched changes the answer.

$ErrorActionPreference = 'Stop'

$source = $PSScriptRoot
$server = Join-Path $source 'NrControlServer.cpp'
$header = Join-Path $source 'NrSettings.h'

$serverText = Get-Content $server -Raw
$headerText = Get-Content $header -Raw

# --- the clamp, per struct field ------------------------------------------------------------

# kNrCeilingMin / kNrCeilingMaxWidth / kNrCeilingMaxHeight, which the ceiling's std::clamp names.
$constants = @{}
foreach ($m in [regex]::Matches($headerText, 'constexpr\s+uint32_t\s+(kNrCeiling\w+)\s*=\s*(\d+)')) {
    $constants[$m.Groups[1].Value] = [double]$m.Groups[2].Value
}

function Resolve-Bound([string]$token) {
    $name = $token.Trim()
    if ($constants.ContainsKey($name)) { return $constants[$name] }
    # 1u / 4u / 0.25f / -1.f -- the suffix is C++, the digits are the bound.
    return [double]($name -replace '[uf]$', '')
}

$clamp = @{}   # field -> @{ low = [double]; high = [double]; note = string }

# clamped.<field> = ClampFinite(settings.<field>, <low>, <high>);
# The statement may wrap, so the bounds are matched with [^;] rather than a line.
$reFloat = [regex]'clamped\.(\w+)\s*=\s*[^;]*?ClampFinite\(\s*settings\.\w+\s*,\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*\)'
foreach ($m in $reFloat.Matches($serverText)) {
    $clamp[$m.Groups[1].Value] = @{ low = [double]$m.Groups[2].Value; high = [double]$m.Groups[3].Value; note = '' }
}

# clamped.<field> = std::clamp(settings.<field>, <low>, <high>);
$reClamp = [regex]'clamped\.(\w+)\s*=\s*std::clamp\(\s*settings\.\w+\s*,\s*([\w.]+)\s*,\s*([\w.]+)\s*\)'
foreach ($m in $reClamp.Matches($serverText)) {
    $clamp[$m.Groups[1].Value] = @{ low = Resolve-Bound $m.Groups[2].Value; high = Resolve-Bound $m.Groups[3].Value; note = '' }
}

# clamped.<field> = std::min(settings.<field>, <high>); -- no lower bound but zero.
$reMin = [regex]'clamped\.(\w+)\s*=\s*std::min\(\s*settings\.\w+\s*,\s*([\w.]+)\s*\)'
foreach ($m in $reMin.Matches($serverText)) {
    $clamp[$m.Groups[1].Value] = @{ low = 0.0; high = Resolve-Bound $m.Groups[2].Value; note = '' }
}

# One that is not a single call: the near plane treats 0 as "off", so its lower guard is not the
# slider's lower end.
if ($clamp.ContainsKey('motionNear')) { $clamp['motionNear'].note = '0 = off' }

# --- what the panel offers ------------------------------------------------------------------

# Panel id -> struct field. The debug selector is absent on purpose: it is a <select>, not a range.
# The native-mask switch is absent for the same reason -- the four pre-block values it routes are not
# sliders of their own, and the translation between them and the menu values is NrControlWireOf's.
$fieldOf = @{
    transfer = 'transferStrength'; color = 'colorStrength'; scale = 'modelScale'
    passes = 'passes'; antiflicker = 'antiFlicker'; afgate = 'antiFlickerGate'
    motionnear = 'motionNear'
    ceilw = 'ceilingWidth'; ceilh = 'ceilingHeight'
    winw = 'windowWidth'; winh = 'windowHeight'
    feather = 'feather'; roundness = 'roundness'
    tone = 'controlTone'; structure = 'controlStructure'
    skin = 'controlSkin'
}

# Zero is a documented value of its own for these, so a panel minimum of 0 is not below the guard.
$zeroAllowed = @('motionNear')

$inputRe = [regex]'<input\b[^>]*\bid="([A-Za-z0-9_]+)"[^>]*>'
$attrRe = [regex]'\b(min|max)\s*=\s*"(-?[\d.]+)"'

Write-Output 'panel range vs layer clamp'
Write-Output ''

$checked = 0
$failed = 0
$seen = @{}

foreach ($m in $inputRe.Matches($serverText)) {
    $id = $m.Groups[1].Value
    $tag = $m.Value
    $base = $id -replace '_n$', ''
    if (-not $fieldOf.ContainsKey($base)) { continue }

    $min = $null
    $max = $null
    foreach ($a in $attrRe.Matches($tag)) {
        if ($a.Groups[1].Value -eq 'min') { $min = [double]$a.Groups[2].Value }
        if ($a.Groups[1].Value -eq 'max') { $max = [double]$a.Groups[2].Value }
    }
    if ($null -eq $min -or $null -eq $max) { continue }

    $field = $fieldOf[$base]
    if (-not $clamp.ContainsKey($field)) {
        Write-Output ("  {0,-23} [{1}, {2}]  no clamp found for {3}" -f $id, $min, $max, $field)
        $failed++
        continue
    }

    $low = $clamp[$field].low
    $high = $clamp[$field].high

    $ok = ($max -le $high) -and (($min -ge $low) -or (($min -eq 0.0) -and ($zeroAllowed -contains $field)))

    $range = "[{0}, {1}]" -f $min, $max
    $guard = "[{0}, {1}]" -f $low, $(if ([double]::IsPositiveInfinity($high)) { 'inf' } else { $high })
    $tagNote = ''
    if ($id -match '_n$') { $tagNote = ' (box)' }
    if ($ok -and $clamp[$field].note) { $tagNote += ' ' + $clamp[$field].note }

    if ($ok) {
        Write-Output ("  {0,-23} {1,-18} inside {2,-18} ok{3}" -f $id, $range, $guard, $tagNote)
    } else {
        Write-Output ("  {0,-23} {1,-18} OUTSIDE {2,-18} FAIL" -f $id, $range, $guard)
        $failed++
    }
    $checked++
    $seen[$id] = $true
}

# Every mapped field must actually be on the panel. A mapping that matches nothing is a rename
# that half-landed, and it would leave a control unguarded while this script reported success.
foreach ($base in $fieldOf.Keys) {
    if (-not ($seen.ContainsKey($base) -or $seen.ContainsKey($base + '_n'))) {
        Write-Output ("  {0,-23} missing from the panel (mapped to {1})" -f $base, $fieldOf[$base])
        $failed++
    }
}

Write-Output ''
Write-Output ("{0} checked, {1} failed" -f $checked, $failed)

# Stated rather than implied: a caller that runs this with & reads $LASTEXITCODE, and a script that
# never sets it leaves whatever the last native command put there -- which reads as a failure here
# on a green run.
if ($failed -ne 0) { exit 1 }
exit 0
