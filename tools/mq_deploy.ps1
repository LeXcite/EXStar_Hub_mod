# mq_deploy.ps1 (ELEVATED) - developer shortcut: copy the host and the
# marker_quality module into the app dir and rewrite modules.ini.
# A normal install goes through ExStarModManager.exe, which embeds every
# payload and maintains the sibling keys from its toggles; this script is
# for iterating on marker_quality alone without rebuilding the manager.
param([string]$Prod = '0')
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first (modules.ini is read at app start)'
}
Copy-Item "$root\build\ExStarRec.dll" "$app\ExStarRec.dll" -Force
Copy-Item "$root\build\modules\marker_quality.dll" "$app\modules\marker_quality.dll" -Force
Copy-Item "$root\modules\marker_quality\marker_quality.ini" "$app\modules\marker_quality.ini" -Force
# 1.8.0 retired the size module: a leftover DLL would hook every
# glUniform4fv/glDraw* and fight marker_quality over the same uniforms
Remove-Item "$app\modules\marker_inframe.dll" -Force -ErrorAction SilentlyContinue
if ($Prod -eq '1') {
    & (Join-Path $PSScriptRoot 'set_ini.ps1') -log 1 -probe 0 -quality 1 -prod 1
} else {
    & (Join-Path $PSScriptRoot 'set_ini.ps1') -log 1 -probe 0 -devall 1 -quality 1 -devmodules 'marker_quality'
}
'--- deployed ---'
foreach ($f in 'ExStarRec.dll','modules\marker_quality.dll','modules\marker_quality.ini','modules.ini') {
    if (Test-Path "$app\$f") {
        "{0,-34} {1,9}  {2}" -f $f, (Get-Item "$app\$f").Length, (Get-Item "$app\$f").LastWriteTime
    }
}
