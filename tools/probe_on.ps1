# probe_on.ps1 - deploy the marker_probe diagnostic module (run ELEVATED)
# Copies marker_probe.dll into the app modules dir, enables it in modules.ini
# and turns host logging on. Nothing else about the install changes.
# WARNING: marker_probe competes with marker_quality for the same anchors
# (color slot in the renderer + setMarkerPoints export). Whichever module
# loads first wins; marker_quality then logs "candidates=0"/"hook failed"
# and its recolor/enlargement stay dead. Disable marker_quality (or remove
# the probe with probe_off.ps1) as soon as the diagnostic run is done.
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
$dll  = Join-Path $root 'build\modules\marker_probe.dll'
if (!(Test-Path $dll)) { throw "not built: $dll (run build.ps1 first)" }
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first (modules.ini is read at app start)'
}

Copy-Item $dll (Join-Path $app 'modules\marker_probe.dll') -Force
Copy-Item (Join-Path $root 'modules\marker_probe\marker_probe.ini') `
          (Join-Path $app 'modules\marker_probe.ini') -Force -ErrorAction SilentlyContinue

$ini = Join-Path $app 'modules.ini'
$txt = Get-Content $ini -Raw
if ($txt -notmatch '(?m)^\s*log\s*=\s*1') {
    $txt = $txt -replace '(?m)^\s*log\s*=\s*\d', 'log = 1'
}
if ($txt -notmatch '\[marker_probe\]') {
    $txt += "`r`n[marker_probe]`r`nenabled = 1`r`n"
} elseif ($txt -match '(?m)^\s*enabled\s*=\s*0(\s*$|\s*;)' -and $txt -match '\[marker_probe\][^\[]*') {
    $sec = [regex]::Match($txt, '(?s)\[marker_probe\].*?(?=\[|$)')
    $fixed = $sec.Value -replace '(?m)^\s*enabled\s*=\s*0', 'enabled = 1'
    $txt = $txt.Remove($sec.Index, $sec.Length).Insert($sec.Index, $fixed)
}
Set-Content -Path $ini -Value $txt -Encoding ASCII -NoNewline
'probe ON: modules.ini ='
Get-Content $ini | Out-String
