; one-shot elevated deploy for the current module set (ASCII only).
; Installs the integrated laser50 (25x25 lines + "50 lines" caption)
; and fitfill, removes the retired standalone captions module, and
; removes its section from modules.ini. Run elevated once:
;   powershell -ExecutionPolicy Bypass -File tools\new3_deploy.ps1
; Alternative: run release\ExStarModManager_1.3.7.exe and flip the
; switches - same result, plus whitelist bookkeeping.
$ErrorActionPreference = 'Stop'
$src = 'D:\Code\Reverse\ExStarHub\build'
$app = 'C:\Program Files\Shining3d\EXStar Hub'

foreach ($p in Get-Process | Where-Object {
    $_.Name -match '^(EXStar Hub|scanservice|scanhub)$' }) {
    Stop-Process -Id $p.Id -Force
}
Start-Sleep 3

foreach ($m in 'laser50', 'fitfill') {
    Copy-Item "$src\modules\$m.dll" "$app\modules\$m.dll" -Force
    Copy-Item "D:\Code\Reverse\ExStarHub\modules\$m\$m.ini" "$app\modules\$m.ini" -Force
}
# retired: standalone captions module (merged into laser50 v1.1.0)
Remove-Item "$app\modules\captions.dll" -Force -ErrorAction SilentlyContinue
Remove-Item "$app\modules\captions.ini" -Force -ErrorAction SilentlyContinue

$ini = "$app\modules.ini"
$t = Get-Content $ini -Raw
foreach ($m in 'laser50', 'fitfill') {
    if ($t -notmatch "(?m)^\[$m\]") {
        $t = $t.TrimEnd() + "`r`n`r`n[$m]`r`nenabled = 1`r`n"
    }
}
$t = $t -replace '(?m)\[captions\]\r?\nenabled = 1\r?\n', ''
# sibling whitelist exactly as the manager would sync it:
# needsSiblings = marker_quality, popup_dim, laser50, fitfill
$want = 'marker_quality,popup_dim,laser50,fitfill'
if ($t -match 'sibling_modules = (\S*)') {
    if ($Matches[1].Trim() -ne $want) {
        $t = $t -replace 'sibling_modules = \S*', ('sibling_modules = ' + $want)
    }
}
$t = $t -replace 'sibling_all = 0', 'sibling_all = 1'
Set-Content $ini $t -Encoding UTF8

Write-Host 'DEPLOY OK - laser50 (integrated) + fitfill installed, captions removed'
Select-String -Path $ini -Pattern 'sibling|laser50|fitfill' |
    ForEach-Object { $_.Line }
Write-Host ('manager to use going forward: release\ExStarModManager_1.3.7.exe')
