# sync_fan_check.ps1 - copies fanBuildPidBlob verbatim from src\manager.c
# into the /*START*/ ... /*END*/ markers of tools\fan_blob_check.c so the
# offline validator can never drift from the shipped builder.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$m  = [IO.File]::ReadAllText((Join-Path $root 'src\manager.c'))
$s  = $m.IndexOf('static int fanBuildPidBlob')
if ($s -lt 0) { throw 'fanBuildPidBlob not found in manager.c' }
$e  = $m.IndexOf("`n}", $m.IndexOf('return (int) n;', $s)) + 2
$core = $m.Substring($s, $e - $s)
$v  = [IO.File]::ReadAllText((Join-Path $root 'tools\fan_blob_check.c'))
$vs = $v.IndexOf('/*START*/'); $ve = $v.IndexOf('/*END*/')
if ($vs -lt 0 -or $ve -lt 0) { throw 'markers missing in fan_blob_check.c' }
$out = $v.Substring(0, $vs + 9) + "`n" + $core + "`n" + $v.Substring($ve)
[IO.File]::WriteAllText((Join-Path $root 'tools\fan_blob_check.c'), $out)
Write-Host "synced fanBuildPidBlob ($($core.Length) bytes) -> fan_blob_check.c"
