$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\sib_one.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8
foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper|einscan_net_svr|softwareUpgrade|informationCollect|ExStarModManager)$' }) {
    "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep 3
[IO.File]::Copy('D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll',
                'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll', $true)
[IO.File]::Copy('D:\Code\Reverse\ExStarHub\release\ExStarModManager_1.6.6.exe',
                'C:\Users\LeX\Downloads\ExStarModManager.exe', $true)
'manager 1.6.6 -> Downloads' | Out-File $logf -Append -Encoding utf8
$ini = 'C:\Program Files\Shining3d\EXStar Hub\modules.ini'
$t = [IO.File]::ReadAllText($ini)
$t = $t -replace '(?m)^sibling_all\s*=.*$', 'sibling_all = 1'
$t = $t -replace '(?m)^sibling_modules\s*=.*$', 'sibling_modules = merge_select'
[IO.File]::WriteAllText($ini, $t)
Get-Content $ini | Out-File $logf -Append -Encoding utf8
'DONE OK' | Out-File $logf -Append -Encoding utf8
