# probe_deploy.ps1 (ELEVATED) - copy fresh host+proxy+probe into the app dir
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
Copy-Item "$root\build\ExStarRec.dll"          "$app\ExStarRec.dll"          -Force
Copy-Item "$root\build\winmm.dll"              "$app\winmm.dll"              -Force
Copy-Item "$root\build\modules\marker_probe.dll" "$app\modules\marker_probe.dll" -Force
Copy-Item "$root\modules\marker_probe\marker_probe.ini" "$app\modules\marker_probe.ini" -Force -ErrorAction SilentlyContinue
'--- deployed ---'
foreach ($f in 'ExStarRec.dll','winmm.dll','modules\marker_probe.dll') {
    "{0,-28} {1,9}  {2}" -f $f, (Get-Item "$app\$f").Length, (Get-Item "$app\$f").LastWriteTime
}
