<#
.SYNOPSIS
    Install a freshly built merge_select module and make its switches
    explicit, then restart the hub.

.DESCRIPTION
    Run elevated (the module lives under Program Files).  It stops the
    vendor processes, copies build\modules\merge_select.dll over the
    installed one, prints both sha256 values so the log and the file can
    be tied together, writes the switches given by -On and -Off into the
    module .ini (uncommented) and starts the hub again.

.PARAMETER On
    Comma-separated switches to force on (value 1).  A plain string, because
    a script started through Start-Process cannot pass a real array.

.PARAMETER Off
    Comma-separated switches to force off (value 0).  A switch named by
    neither is left as the file has it.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_module.ps1 `
        -On verbose,eye_gate,eye_reapply,scene_watch -Off scene_filter `
        -Note 1.5.7_re-apply_eyes_after_env_switch

.NOTES
    Writes C:\Users\LeX\AppData\Local\Temp\opencode\deploy_module.txt.
    Nothing in Program Files is touched except the module dll and its .ini.
#>
param(
    [string]$On = 'verbose,eye_gate,eye_reapply,scene_watch',
    [string]$Off = 'scene_filter',
    [string]$Note = ''
)

$On  = @($On  -split ',' | Where-Object { $_ -match '\S' })
$Off = @($Off -split ',' | Where-Object { $_ -match '\S' })

$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\deploy_module.txt'
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
$known = 'verbose', 'eyewatch', 'eye_gate', 'eye_reapply', 'scene_watch',
         'scene_filter', 'sibling_all', 'sibling_modules', 'file'
$pat   = '^\s*;?\s*(' + ($known -join '|') + ')\s*='
$lines = [IO.File]::ReadAllText($ini) -split "`r?`n"
$out = New-Object System.Collections.Generic.List[string]
$seen = @{}
foreach ($l in $lines) {
    if ($l -match $pat) {
        $k = $matches[1]
        if ($On -contains $k)  { $out.Add("$k = 1"); $seen[$k] = $true; continue }
        if ($Off -contains $k) { $out.Add("$k = 0"); $seen[$k] = $true; continue }
    }
    $out.Add($l)
}
foreach ($k in ($On + $Off)) {
    if ($seen[$k]) { continue }
    if ($known -notcontains $k) {
        "skip unknown key '$k'" | Out-File $logf -Append -Encoding utf8
        continue
    }
    if ($On -contains $k) { $out.Add("$k = 1") } else { $out.Add("$k = 0") }
}
[IO.File]::WriteAllLines($ini, $out, (New-Object Text.UTF8Encoding($false)))
'ini now:' | Out-File $logf -Append -Encoding utf8
(Get-Content $ini | Where-Object { $_ -match '\S' }) |
    ForEach-Object { "  $_" } | Out-File $logf -Append -Encoding utf8

if ($Note) { $Note | Out-File $logf -Append -Encoding utf8 }
' restarting hub' | Out-File $logf -Append -Encoding utf8
# the vendor app resolves parts of its environment from the current
# directory: starting it from System32 (the default of an elevated shell)
# makes it exit again without a word
Start-Process 'C:\Program Files\Shining3d\EXStar Hub\EXStar Hub.exe' `
    -WorkingDirectory 'C:\Program Files\Shining3d\EXStar Hub'
'DONE OK' | Out-File $logf -Append -Encoding utf8
