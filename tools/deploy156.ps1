$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\deploy156.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8

foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper|einscan_net_svr|softwareUpgrade|informationCollect|ExStarModManager)$' }) {
    "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep 3

$src = 'D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll'
$dst = 'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll'
[IO.File]::Copy($src, $dst, $true)
"dll  src $((Get-FileHash $src -Algorithm SHA256).Hash)" | Out-File $logf -Append -Encoding utf8
"dll  dst $((Get-FileHash $dst -Algorithm SHA256).Hash)" | Out-File $logf -Append -Encoding utf8

$ini = 'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.ini'
if (-not (Test-Path $ini)) { '[merge_select]' | Out-File $ini -Encoding utf8 }
$lines = [IO.File]::ReadAllText($ini) -split "`r?`n"
$out = New-Object System.Collections.Generic.List[string]
$seen = @{}
foreach ($l in $lines) {
    if ($l -match '^\s*;?\s*(scene_watch|scene_filter|eye_gate|verbose)\s*=') {
        $k = $matches[1]
        $out.Add("$k = 1"); $seen[$k] = $true; continue
    }
    $out.Add($l)
}
foreach ($k in @('scene_watch', 'scene_filter', 'eye_gate', 'verbose')) {
    if (-not $seen[$k]) { $out.Add("$k = 1") }
}
[IO.File]::WriteAllLines($ini, $out, (New-Object Text.UTF8Encoding($false)))
'ini now:' | Out-File $logf -Append -Encoding utf8
(Get-Content $ini | Where-Object { $_ -match 'scene_|eyewatch|eye_gate|verbose' }) |
    ForEach-Object { "  $_" } | Out-File $logf -Append -Encoding utf8
'merge_select 1.5.6 deployed (deep command probe + SetModelNodesVisible filter)' |
    Out-File $logf -Append -Encoding utf8
'DONE OK' | Out-File $logf -Append -Encoding utf8
