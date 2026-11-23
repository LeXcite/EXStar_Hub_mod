$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\fix_sib.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8
foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper)$' }) {
    "stop $($p.Name) pid=$($p.Id)" | Out-File $logf -Append -Encoding utf8
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep 2
$ini = 'C:\Program Files\Shining3d\EXStar Hub\modules.ini'
$t = [IO.File]::ReadAllText($ini)
$t = $t -replace '(?m)^sibling_all\s*=.*$', 'sibling_all = 0'
$t = $t -replace '(?m)^sibling_modules\s*=.*$', 'sibling_modules = '
[IO.File]::WriteAllText($ini, $t)
'ini reverted' | Out-File $logf -Append -Encoding utf8
Get-Content $ini | Out-File $logf -Append -Encoding utf8
'=== DONE ===' | Out-File $logf -Append -Encoding utf8
