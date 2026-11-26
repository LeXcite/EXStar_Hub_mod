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
      4. logs whether the session data file
         C:\Users\Public\Shining3d\EXStar Hub\selective-mesh.ini exists.  It
         holds only the exclusions of the current run: the hub drops those
         sections when it starts (1.8.0), and showing an eye deletes its
         section, so there is nothing for this script to migrate.

.PARAMETER On
    Comma-separated switches to force on (value 1).  A plain string, because
    a script started through Start-Process cannot pass a real array.

.PARAMETER Off
    Comma-separated switches to force off (value 0).  A switch named by
    neither is left as the file has it.

.PARAMETER NoRename
    Skip steps 1..3 (only for a machine that never had the old name).

.PARAMETER ResetIni
    Replace the module .ini with the repository template before applying
    -On/-Off (fresh switch set; the user's selection data file is not
    touched).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_module.ps1 `
        -On verbose,eye_gate,scene_watch,all_hidden -Off scene_filter,eye_reapply `
        -Note 1.7.0_all_hidden_guard

.NOTES
    Writes C:\Users\LeX\AppData\Local\Temp\opencode\deploy_module.txt.
    Nothing in Program Files is touched except the module dlls, their
    .ini files and modules.ini.  The user's session file under
    C:\Users\Public is never deleted by this script: since 1.8.0 the
    module itself drops its [uuid] exclusions when the hub starts, and
    deleting the file by hand would only remove the evidence of what the
    last session did.

    Hashes: the printed sha256 identifies THIS file, not a build of a
    commit.  MSVC link is not byte-reproducible here - two builds of one
    unchanged source produced different hashes on 2026-11-26 - so a hash
    is only meaningful as "the file that is installed now"; tie it to code
    by the commit it was built from, and re-hash the published artifact
    once, at publish time.
#>
param(
    [string]$On = 'verbose,eye_gate,scene_watch',
    [string]$Off = 'scene_filter,eye_reapply',
    [string]$Note = '',
    [switch]$NoRename,
    [switch]$ResetIni
)

# Split into separate variables ON PURPOSE: a typed [string] parameter
# keeps its type constraint for the whole script, so `$On = @(... -split)`
# would coerce the array back to a space-joined string (that is how every
# -On/-Off list silently degraded to one bogus key until 1.6.1).
$onKeys  = @($On  -split '[,\s]+' | Where-Object { $_ -match '\S' })
$offKeys = @($Off -split '[,\s]+' | Where-Object { $_ -match '\S' })

$ErrorActionPreference = 'Continue'
$logf = 'C:\Users\LeX\AppData\Local\Temp\opencode\deploy_module.txt'
"start $(Get-Date -Format o)" | Out-File $logf -Encoding utf8

# The manager is shipped as ExStarModManager_<version>.exe, so the name is
# matched as a prefix (a trailing $ left a running 1.6.9/1.7.0 instance
# untouched, which is only harmless when nothing needs its files).
foreach ($p in Get-Process | Where-Object { $_.Name -match '^(EXStar Hub|scanservice|scanhub|Sn3DprocessManager|TestOpenglHelper|einscan_net_svr|softwareUpgrade|informationCollect|ExStarModManager)' }) {
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
    #    (1.6.1 fix: `-(Test-Path)` is a double negation - numeric -1 is
    #    truthy - so this migration never ran and the live ini stayed at
    #    its defaults; -not is the only correct form here)
    if ((Test-Path $oldIni) -and -not (Test-Path $ini)) {
        try {
            Copy-Item $oldIni $ini -ErrorAction Stop
            "ini  migrated modules\merge_select.ini -> modules\selective_mesh.ini" |
                Out-File $logf -Append -Encoding utf8
        } catch {
            "ini  MIGRATION FAILED: $($_.Exception.Message)" |
                Out-File $logf -Append -Encoding utf8
        }
    } else {
        "ini  no migration (old present: $(Test-Path $oldIni), " +
            "new present: $(Test-Path $ini))" | Out-File $logf -Append -Encoding utf8
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

# 4. the session file.  Since 1.8.0 it holds only the exclusions of the run
#    that is happening now - the hub clears its [uuid] sections on start and
#    the vendor shows every scan again when a project re-opens - so this
#    only records what the first start will find.
$pubNew = "$pub\selective-mesh.ini"
"data selective-mesh.ini: $(if (Test-Path $pubNew) { 'present (its exclusions belong to the previous run; the hub drops them on start)' } else { 'absent' })" |
    Out-File $logf -Append -Encoding utf8

# A module .ini that only holds the section name means "every switch
# default", and the shipped default for scene_filter is on - so the
# template is written when -ResetIni is given (fresh id, empty file).
if ($ResetIni) {
    $tpl = 'D:\Code\Reverse\ExStarHub\modules\selective_mesh\selective_mesh.ini'
    try {
        Copy-Item $tpl $ini -Force -ErrorAction Stop
        "ini  reset from the repository template" | Out-File $logf -Append -Encoding utf8
    } catch {
        "ini  RESET FAILED: $($_.Exception.Message)" | Out-File $logf -Append -Encoding utf8
    }
}

if (-not (Test-Path $ini)) { '[selective_mesh]' | Out-File $ini -Encoding utf8 }
$known = 'verbose', 'eyewatch', 'eye_gate', 'all_hidden', 'eye_reapply',
         'scene_watch', 'scene_filter', 'sibling_all', 'sibling_modules',
         'file'
$pat   = '^\s*;?\s*(' + ($known -join '|') + ')\s*='
$lines = [IO.File]::ReadAllText($ini) -split "`r?`n"
$out = New-Object System.Collections.Generic.List[string]
$seen = @{}
foreach ($l in $lines) {
    if ($l -match $pat) {
        $k = $matches[1]
        if ($onKeys -contains $k)  { $out.Add("$k = 1"); $seen[$k] = $true; continue }
        if ($offKeys -contains $k) { $out.Add("$k = 0"); $seen[$k] = $true; continue }
    }
    $out.Add($l)
}
foreach ($k in ($onKeys + $offKeys)) {
    if ($seen[$k]) { continue }
    if ($known -notcontains $k) {
        "skip unknown key '$k'" | Out-File $logf -Append -Encoding utf8
        continue
    }
    if ($onKeys -contains $k) { $out.Add("$k = 1") } else { $out.Add("$k = 0") }
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
