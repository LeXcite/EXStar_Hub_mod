# probe_prep.ps1 - ELEVATED prep for a marker_probe session: refresh the host
# binaries and temporarily disable marker_quality, because both modules take
# over the same vendor color slot - the probe must be the only owner while it
# is logging.  Finish the session with probe_off.ps1.
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
Copy-Item "$root\build\ExStarRec.dll" "$app\ExStarRec.dll" -Force
Copy-Item "$root\build\winmm.dll"     "$app\winmm.dll"     -Force
$ini = "$app\modules.ini"
$txt = Get-Content $ini -Raw
$txt = $txt -replace '(?m)(\[marker_quality\]\r?\n)enabled = 1', '$1enabled = 0'
Set-Content -Path $ini -Value $txt.TrimEnd() -Encoding ASCII -NoNewline
'--- modules.ini ---'
Get-Content $ini | Out-String
'--- host version on disk ---'
(Get-Item "$app\ExStarRec.dll").VersionInfo.FileVersion
