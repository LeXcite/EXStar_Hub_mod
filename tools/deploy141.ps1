$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\deploy141.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8
foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper|einscan_net_svr|softwareUpgrade|informationCollect)$' }) {
    "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep 3
try {
    [IO.File]::Copy('D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll',
                    'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll', $true)
    'copied merge_select.dll v1.4.1' | Out-File $logf -Append -Encoding utf8
} catch {
    "COPY FAILED: $($_.Exception.Message)" | Out-File $logf -Append -Encoding utf8
}
[IO.File]::Copy('D:\Code\Reverse\ExStarHub\release\ExStarModManager_1.6.3.exe',
                'C:\Users\LeX\Downloads\ExStarModManager.exe', $true)
'manager -> Downloads' | Out-File $logf -Append -Encoding utf8
Get-Content 'C:\Program Files\Shining3d\EXStar Hub\modules.ini' | Out-File $logf -Append -Encoding utf8
'DONE OK' | Out-File $logf -Append -Encoding utf8
