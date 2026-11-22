<#
.SYNOPSIS
  One-click fix for "my phone does not see the scanner Wi-Fi".

.DESCRIPTION
  The scanner picks its AP channel from a regulatory table on the
  device (/shining3d/country_code.db). That table allows 5.8 GHz
  channels (149..161) for almost every country, but phones legally
  sold in e.g. Kazakhstan/Russia may not scan there, so such phones
  never see the AP (real incident: AP landed on ch153, a KZ-region
  phone saw every network except the scanner).

  This script narrows the device's own country row to the non-DFS
  low channels (36,40,44,48) that local phones are guaranteed to
  see, restarts the AP through the vendor's own init script and
  verifies the result over a fresh connection.

  Device-side writes: exactly one line of the regulatory table, plus
  /tmp files (RAM). The factory table is backed up on first use to
  %LOCALAPPDATA%\ExStarHubMods\scanner\country_code.db.factory, so
  -Mode rollback restores the original state.

  Requires the optional Windows feature "OpenSSH Client" (preinstalled
  on Windows 11). Login is the vendor factory-default account that the
  vendor's own PC software uses as well.

.USAGE
  powershell -ExecutionPolicy Bypass -File scanner-wifi-fix.ps1 [-Mode status|fix|rollback]
                 [-Ip 192.168.42.3] [-Password ***] [-FallbackChannels 36,40,44,48]

  Default mode is "status" (read-only, always safe).
#>
param(
    [ValidateSet('status','fix','rollback')] [string]$Mode = 'status',
    [string]$Ip = '192.168.42.3',
    [string]$Password = 'sn3d',
    [string]$FallbackChannels = '36,40,44,48'
)
$ErrorActionPreference = 'Stop'

$DB      = '/shining3d/country_code.db'
$CCFILE  = '/backup/country_code'
$User    = 'root'
$backupDir = Join-Path $env:LOCALAPPDATA 'ExStarHubMods\scanner'
$factory = Join-Path $backupDir 'country_code.db.factory'

if (-not (Get-Command ssh.exe -ErrorAction SilentlyContinue)) {
    throw "ssh.exe not found - install the optional Windows feature 'OpenSSH Client'."
}
[void](New-Item -Force -ItemType Directory -Path $backupDir)

# --- transport: ssh with a temp askpass helper (password never on a
#     command line; temp file lives in the user profile, deleted after)
$askPath = Join-Path $env:TEMP ("exsm_askpass_{0}.cmd" -f $PID)
$khPath  = Join-Path $env:TEMP  'exsm_known_hosts'
function Invoke-Ssh([string]$RemoteCmd, [string]$StdinData = $null) {
    Set-Content -Path $askPath -Value "@echo off`r`necho $Password`r`n" -Encoding ascii
    $env:SSH_ASKPASS = $askPath
    $env:SSH_ASKPASS_REQUIRE = 'force'
    $sshArgs = @('-o','StrictHostKeyChecking=accept-new',
                 '-o',"UserKnownHostsFile=$khPath",
                 '-o','ConnectTimeout=8','-o','LogLevel=ERROR',
                 "$User@$Ip", $RemoteCmd)
    try {
        if ($null -ne $StdinData) {
            $out = $StdinData | & ssh.exe @sshArgs 2>&1
        } else {
            $out = & ssh.exe @sshArgs 2>&1
        }
        return @{ code = $LASTEXITCODE; text = ($out | Out-String) }
    } finally {
        Remove-Item Env:SSH_ASKPASS, Env:SSH_ASKPASS_REQUIRE -ErrorAction SilentlyContinue
        Remove-Item $askPath -Force -ErrorAction SilentlyContinue
    }
}

$STATUS = "cc=`$(tr -d '\040\011\012\015' < $CCFILE); echo CC=`$cc; grep ^`$cc: $DB; " +
          "iw dev wlan0 info 2>/dev/null | grep -E 'channel|width'; " +
          "hostapd_cli status 2>/dev/null | grep state="

function Get-ScannerState {
    $r = Invoke-Ssh $STATUS
    if ($r.code -ne 0 -and -not $r.text.Trim()) {
        throw "cannot reach the scanner over ssh ($Ip) - is it on and connected?"
    }
    $cc  = if ($r.text -match 'CC=([A-Z]{2})')      { $Matches[1] } else { $null }
    $row = if ($cc -and $r.text -match "(?m)^$cc`:([\d,]+)") { $Matches[1] } else { $null }
    $ch  = if ($r.text -match 'channel\s+(\d+)')    { [int]$Matches[1] } else { -1 }
    return @{ cc = $cc; row = $row; chan = $ch; raw = $r.text }
}
function Restart-Ap {
    # write a tiny restart helper to /tmp (RAM) and run it detached:
    # the AP restart kills this very ssh session, so it must survive it
    Invoke-Ssh ("printf '%s\n' '#!/bin/sh' " +
        "'/etc/init.d/S99softap.sh stop' 'sleep 2' " +
        "'/etc/init.d/S99softap.sh start' > /tmp/exstar_ap.sh && " +
        "chmod +x /tmp/exstar_ap.sh && " +
        "setsid nohup /tmp/exstar_ap.sh >/tmp/exstar_ap.log 2>&1 </dev/null & " +
        "echo AP_RESTART_SCHEDULED") | Out-Null
    Write-Host '  AP restart scheduled, waiting 12 s...'
    Start-Sleep 12
}

Write-Host "scanner $Mode @ $Ip"
$st = Get-ScannerState
if (-not $st.cc -or -not $st.row) { throw "unexpected status output:`n$($st.raw)" }
Write-Host ("  country={0} row={1} ap_channel={2}" -f $st.cc, $st.row, $st.chan)

switch ($Mode) {
    'status' {
        $high = @($st.row.Split(',') | ForEach-Object { [int]$_ } | Where-Object { $_ -gt 64 })
        if     ($high)        { Write-Host '  verdict: row contains 5.8 GHz channels - phones with local regulation may not see the AP; run -Mode fix' }
        elseif ($st.chan -le 64 -and $st.chan -ge 1) { Write-Host '  verdict: OK - AP is on a locally visible channel' }
        else                  { Write-Host '  verdict: AP is currently on a high channel' }
    }
    'fix' {
        $keep = @($st.row.Split(',') | ForEach-Object { [int]$_ } | Where-Object { $_ -ge 1 -and $_ -le 64 }) -join ','
        if (-not $keep) { $keep = $FallbackChannels }
        if ($keep -eq $st.row) { Write-Host "  already narrowed to $keep - nothing to do"; break }

        if (-not (Test-Path $factory)) {
            $r = Invoke-Ssh "base64 $DB"
            if ($r.code -ne 0) { throw 'could not fetch a factory backup - refusing to patch' }
            $bytes = [Convert]::FromBase64String(($r.text -replace '\s',''))
            if ($bytes.Length -lt 100 -or [Text.Encoding]::ASCII.GetString($bytes[0..2]) -ne 'AD:') {
                throw 'factory backup looks wrong - refusing to patch'
            }
            [IO.File]::WriteAllBytes($factory, $bytes)
            Write-Host "  factory table saved: $factory"
        }
        $r = Invoke-Ssh "sed -i 's|^$($st.cc):.*|$($st.cc):$keep|' $DB && sync && echo PATCHED"
        if ($r.code -ne 0 -or $r.text -notmatch 'PATCHED') { throw "patch failed:`n$($r.text)" }
        Write-Host "  row $($st.cc): $($st.row) -> $keep"
        Restart-Ap
        $st2 = Get-ScannerState
        if ($st2.chan -ge 1 -and $st2.chan -le 64) {
            Write-Host "  RESULT: OK - AP now on channel $($st2.chan) (visible to phones with local regulation)"
        } else {
            Write-Host "  RESULT: restarted, but channel=$($st2.chan) - re-run -Mode status to check"
        }
    }
    'rollback' {
        if (-not (Test-Path $factory)) { throw "no factory backup in $factory" }
        $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($factory))
        $r = Invoke-Ssh "base64 -d > $DB && sync && echo RESTORED" $b64
        if ($r.code -ne 0 -or $r.text -notmatch 'RESTORED') { throw "restore failed:`n$($r.text)" }
        Write-Host '  factory table restored'
        Restart-Ap
        $st2 = Get-ScannerState
        Write-Host "  RESULT: AP channel $($st2.chan) (factory behaviour is back)"
    }
}
