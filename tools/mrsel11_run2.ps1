"start $(Get-Date -Format o)" | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Encoding utf8
try {
    foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager)$' }) {
        "stop $($p.Name) pid=$($p.Id)" | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
        Stop-Process -Id $p.Id -Force
    }
    Start-Sleep 2
    $app = 'C:\Program Files\Shining3d\EXStar Hub'
    [IO.File]::Copy('D:\Code\Reverse\ExStarHub\build\modules\merge_select.dll', "$app\modules\merge_select.dll", $true)
    'copied merge_select.dll' | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
    [IO.File]::Copy('D:\Code\Reverse\ExStarHub\modules\merge_select\merge_select.ini', "$app\modules\merge_select.ini", $true)
    'copied merge_select.ini' | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8

    $ini = "$app\modules.ini"
    $t = [IO.File]::ReadAllText($ini)
    if ($t -notmatch 'sibling_all\s*=') {
        $t = $t -replace '\[host\]', "[host]`r`nsibling_all = 1`r`nsibling_modules = marker_quality,popup_dim,laser50,fitfill,gpu_gate,merge_select"
    } else {
        $t = $t -replace '(?m)^sibling_all\s*=.*$', 'sibling_all = 1'
        if ($t -match '(?m)^sibling_modules\s*=') {
            $t = $t -replace '(?m)^sibling_modules\s*=.*$', 'sibling_modules = marker_quality,popup_dim,laser50,fitfill,gpu_gate,merge_select'
        } else {
            $t = $t -replace '(?m)^sibling_all\s*=.*$', "sibling_all = 1`r`nsibling_modules = marker_quality,popup_dim,laser50,fitfill,gpu_gate,merge_select"
        }
    }
    [IO.File]::WriteAllText($ini, $t)
    'edited modules.ini' | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8

    [IO.File]::Copy('D:\Code\Reverse\ExStarHub\release\ExStarModManager_1.6.0.exe', 'C:\Users\LeX\Downloads\ExStarModManager.exe', $true)
    'copied manager' | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
    Get-Content $ini | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
    'DONE OK' | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
} catch {
    "FAILED: $($_.Exception.Message)" | Out-File C:\Users\LeX\AppData\Local\Temp\opencode\deploy3.txt -Append -Encoding utf8
}
