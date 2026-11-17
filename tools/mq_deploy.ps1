# mq_deploy.ps1 (ELEVATED) - copy host + module binaries/ini into the app dir
# and rewrite modules.ini. -Prod writes the production sibling keys instead
# of the developer keys (what the Mod Manager does on install).
param([string]$Prod = '0', [string]$Inframe = '0')
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first (modules.ini is read at app start)'
}
Copy-Item "$root\build\ExStarRec.dll" "$app\ExStarRec.dll" -Force
Copy-Item "$root\build\modules\marker_quality.dll" "$app\modules\marker_quality.dll" -Force
Copy-Item "$root\modules\marker_quality\marker_quality.ini" "$app\modules\marker_quality.ini" -Force
Copy-Item "$root\build\modules\marker_inframe.dll" "$app\modules\marker_inframe.dll" -Force
if ($Inframe -eq '1') {
    # the shipped template changed (no outline_color); replace stale copies
    Copy-Item "$root\modules\marker_inframe\marker_inframe.ini" "$app\modules\marker_inframe.ini" -Force
}
if ($Prod -eq '1') {
    & (Join-Path $PSScriptRoot 'set_ini.ps1') -log 1 -inframe $Inframe -probe 0 -quality 1 -prod 1
} else {
    & (Join-Path $PSScriptRoot 'set_ini.ps1') -log 1 -inframe $Inframe -probe 0 -devall 1 -quality 1 -devmodules 'marker_quality'
}
'--- deployed ---'
foreach ($f in 'ExStarRec.dll','modules\marker_quality.dll','modules\marker_quality.ini','modules\marker_inframe.dll','modules\marker_inframe.ini','modules.ini') {
    if (Test-Path "$app\$f") {
        "{0,-34} {1,9}  {2}" -f $f, (Get-Item "$app\$f").Length, (Get-Item "$app\$f").LastWriteTime
    }
}
