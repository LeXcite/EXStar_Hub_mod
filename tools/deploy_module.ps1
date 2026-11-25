<#
.SYNOPSIS
    Install a freshly built selective_mesh module, migrate everything the
    1.6.0 rename touched, and restart the hub.

.DESCRIPTION
    Run elevated (the module lives under Program Files).  It stops the
    vendor processes, copies build\modules\selective_mesh.dll over the
    installed one, prints both sha256 values so the log and the file can
    be tied together, writes the switches given by -On and -Off into the
    module .ini (uncommented) and starts the hub again.

    Until 1.6.0 the module was named merge_select, and the host derives a
    module's id from the DLL basename and defaults every section of
    modules.ini to ENABLED - a leftover merge_select.dll would therefore
    hook the same gate a second time in the same process.  So this script
    also, in this order:

      1. copies modules\merge_select.ini  -> modules\selective_mesh.ini
         (the user's switches; never overwrites an existing new file),
      2. rewrites modules.ini: the [merge_select] section becomes
         [selective_mesh] (same enabled value), and every `merge_select`
         token in [host] sibling_modules becomes `selective_mesh`,
      3. deletes modules\merge_select.dll and modules\merge_select.ini,
      4. reports the state of the data file: the module itself copies
         C:\Users\Public\Shining3d\EXStar Hub\merge-select.ini to
         selective-mesh.ini on first start, this only logs which of the
         two exists now.

.PARAMETER On
    Comma-separated switches to force on (value 1).  A plain string, because
    a script started through Start-Process cannot pass a real array.

.PARAMETER Off
    Comma-separated switches to force off (value 0).  A switch named by
    neither is left as the file has it.

.PARAMETER NoRename
    Skip steps 1..3 (only for a machine that never had the old name).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_module.ps1 `
        -On verbose,eye_gate,eye_reapply,scene_watch -Off scene_filter `
        -Note 1.6.0_rename_to_selective_mesh

.NOTES
    Writes C:\Users\LeX\AppData\Local\Temp\opencode\deploy_module.txt.
    Nothing in Program Files is touched except the module dlls, their
    .ini files and modules.ini.  The user's selection file under
    C:\Users\Public is never deleted, only copied by the module itself.
#>
param(
    [string]$On = 'verbose,eye_gate,eye_reapply,scene_watch',
    [string]$Off = 'scene_filter',
    [string]$Note = '',
    [switch]$NoRename
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

$app   = 'C:\Program Files\Shining3d\EXStar Hub'
$pub   = 'C:\Users\Public\Shining3d\EXStar Hub'
$src   = 'D:\Code\Reverse\ExStarHub\build\modules\selective_mesh.dll'
$dst   = "$app\modules\selective_mesh.dll"
$ini   = "$app\modules\selective_mesh.ini"
$oldDll = "$app\modules\merge_select.dll"
$oldIni = "$app\modules\merge_select.ini"

if (-not $NoRename) {
    # 1. the user's switches keep their content under the new name
    if ((Test-Path $oldIni) -and -(Test-Path $ini)) {
        Copy-Item $oldIni $ini
        "ini  migrated modules\merge_select.ini -> modules\selective_mesh.ini" |
            Out-File $logf -Append -Encoding utf8
    }

    # 2. modules.ini: the section follows the id, the sibling list too.
    #    (An old [merge_select] section left behind would default a
    #    phantom module to enabled; [selective_mesh] is what the host reads.)
    $mIni = "$app\modules.ini"
    if (Test-Path $mIni) {
        $mLines = [IO.File]::ReadAllLines($mIni)
        $mChanged = $false
        for ($i = 0; $i -lt $mLines.Count; $i++) {
            $l = $mLines[$i]
            if ($l -match '^\s*\[merge_select\]\s*$') {
                $mLines[$i] = '[selective_mesh]'
                $mChanged = $true
            }
            elseif ($l -match '^\s*sibling_modules\s*=' -and
                    $l -match '\bmerge_select\b') {
                $mLines[$i] = $l -replace '\bmerge_select\b', 'selective_mesh'
                $mChanged = $true
            }
        }
        if ($mChanged) {
            [IO.File]::WriteAllLines($mIni, $mLines,
                                     (New-Object Text.UTF8Encoding($false)))
            'modules.ini: merge_select renamed to selective_mesh' |
                Out-File $logf -Append -Encoding utf8
        }
    }

    # 3. the old id must not survive: same gate, two detours
    if (Test-Path $oldDll) {
        Remove-Item $oldDll -Force
        'removed modules\merge_select.dll' | Out-File $logf -Append -Encoding utf8
    }
    if (Test-Path $oldIni) { Remove-Item $oldIni -Force }
}

[IO.File]::Copy($src, $dst, $true)
"dll  src $((Get-FileHash $src -Algorithm SHA256).Hash)" | Out-File $logf -Append -Encoding utf8
"dll  dst $((Get-FileHash $dst -Algorithm SHA256).Hash)" | Out-File $logf -Append -Encoding utf8

# 4. the selection data file migrates itself inside the module; this only
#    records what the first start will find
$pubNew = "$pub\selective-mesh.ini"
$pubOld = "$pub\merge-select.ini"
"data selective-mesh.ini: $(if (Test-Path $pubNew) { 'present' } else { 'absent' }), " +
    "merge-select.ini: $(if (Test-Path $pubOld) { 'present (the module copies it on first start)' } else { 'absent' })" |
    Out-File $logf -Append -Encoding utf8

if (-not (Test-Path $ini)) { '[selective_mesh]' | Out-File $ini -Encoding utf8 }
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
Start-Process "$app\EXStar Hub.exe" -WorkingDirectory $app
'DONE OK' | Out-File $logf -Append -Encoding utf8
