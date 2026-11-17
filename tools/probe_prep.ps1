# probe_prep.ps1 - ELEVATED prep for the probe session: refresh host DLLs and
# temporarily disable marker_inframe so the host worker is not blocked by its
# GL wait and the probe log stays clean.
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
Copy-Item "$root\build\ExStarRec.dll" "$app\ExStarRec.dll" -Force
Copy-Item "$root\build\winmm.dll"     "$app\winmm.dll"     -Force
$ini = "$app\modules.ini"
$txt = Get-Content $ini -Raw
$txt = $txt -replace '(?m)(\[marker_inframe\]\r?\n)enabled = 1', '$1enabled = 0'
Set-Content -Path $ini -Value $txt.TrimEnd() -Encoding ASCII -NoNewline
'--- modules.ini ---'
Get-Content $ini | Out-String
'--- host version on disk ---'
(Get-Item "$app\ExStarRec.dll").VersionInfo.FileVersion
