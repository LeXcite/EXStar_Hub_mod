param([switch]$Revert)
$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\probe2.txt'
"start $(Get-Date -Format o) revert=$($Revert.IsPresent)" | Out-File $logf -Encoding utf8
try {
    foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper)$' }) {
        "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep 2
    $ini = 'C:\Program Files\Shining3d\EXStar Hub\modules.ini'
    $t = [IO.File]::ReadAllText($ini)
    if ($Revert) {
        $t = $t -replace '(?m)^sibling_all\s*=.*$', 'sibling_all = 0'
        $t = $t -replace '(?m)^sibling_modules\s*=.*$', 'sibling_modules = '
    } else {
        $t = $t -replace '(?m)^sibling_all\s*=.*$', 'sibling_all = 1'
        $t = $t -replace '(?m)^sibling_modules\s*=.*$', 'sibling_modules = merge_select'
    }
    [IO.File]::WriteAllText($ini, $t)
    "ini set" | Out-File $logf -Append -Encoding utf8
    if (-not $Revert) {
        [IO.File]::Copy('D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll',
                        'C:\Program Files\Shining3d\EXStar Hub\modules\merge_select.dll', $true)
        'copied dll' | Out-File $logf -Append -Encoding utf8
    }
    Get-Content $ini | Out-File $logf -Append -Encoding utf8
    'DONE OK' | Out-File $logf -Append -Encoding utf8
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File $logf -Append -Encoding utf8
}
