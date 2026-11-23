$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\deploy_probe.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8
try {
    foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper)$' }) {
        "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep 2
    [IO.File]::Copy('D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll',
                    'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll', $true)
    'copied merge_select.dll v1.1.2-probe' | Out-File $logf -Append -Encoding utf8
    (Get-Item 'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll') |
        Select-Object Length, LastWriteTime | Out-String | Out-File $logf -Append -Encoding utf8
    'DONE OK' | Out-File $logf -Append -Encoding utf8
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File $logf -Append -Encoding utf8
}
