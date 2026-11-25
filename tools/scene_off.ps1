$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\safeoff.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8

$ini = 'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.ini'
if (-not (Test-Path $ini)) {
    'module ini missing - created' | Out-File $logf -Append -Encoding utf8
    '[merge_select]' | Out-File $ini -Encoding utf8
}
$text = [IO.File]::ReadAllText($ini)
$lines = $text -split "`r?`n"
$out = New-Object System.Collections.Generic.List[string]
$seenW = $false; $seenF = $false
foreach ($l in $lines) {
    if ($l -match '^\s*;?\s*scene_watch\s*=') { $out.Add('scene_watch = 0'); $seenW = $true; continue }
    if ($l -match '^\s*;?\s*scene_filter\s*=') { $out.Add('scene_filter = 0'); $seenF = $true; continue }
    $out.Add($l)
}
if (-not $seenW) { $out.Add('scene_watch = 0') }
if (-not $seenF) { $out.Add('scene_filter = 0') }
[IO.File]::WriteAllLines($ini, $out, (New-Object Text.UTF8Encoding($false)))
"ini now:" | Out-File $logf -Append -Encoding utf8
(Get-Content $ini | Select-String -Pattern 'scene_|eyewatch|eye_gate|verbose') | ForEach-Object { $_.ToString() } |
    Out-File $logf -Append -Encoding utf8

foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper|einscan_net_svr|softwareUpgrade|informationCollect|ExStarModManager)$' }) {
    "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
'DONE OK' | Out-File $logf -Append -Encoding utf8
