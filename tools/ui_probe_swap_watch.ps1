# wait for EXStar Hub to close, then deploy ui_probe v3, then wait for it to
# reopen; logs progress to a file the assistant reads.
$ErrorActionPreference = 'SilentlyContinue'
$log = Join-Path $env:TEMP 'opencode\uiprobe_swap.txt'
$src = 'D:\Code\Reverse\ExStarHub\build\modules\ui_probe.dll'
$ini = 'D:\Code\Reverse\ExStarHub\modules\ui_probe\ui_probe.ini'
$dst = 'C:\Program Files\Shining3d\EXStar Hub\modules\ui_probe.dll'
$dstini = 'C:\Program Files\Shining3d\EXStar Hub\modules\ui_probe.ini'
function say($m) { "$(Get-Date -Format HH:mm:ss) $m" | Add-Content $log }
say "swap-watcher started; waiting for hub to close"
# phase 1: wait for it to disappear
for ($i=0; $i -lt 360; $i++) {
  if (-not (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue)) { break }
  Start-Sleep -Seconds 2
}
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) { say "hub still up after 12min, aborting"; exit }
say "hub closed; copying v3"
Copy-Item $src $dst -Force
Copy-Item $ini $dstini -Force
$a=(Get-FileHash $dst -Algorithm MD5).Hash; $b=(Get-FileHash $src -Algorithm MD5).Hash
say "deployed same=$($a -eq $b)"
# phase 2: wait for user to reopen
say "waiting for hub to reopen"
for ($i=0; $i -lt 600; $i++) {
  if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) { break }
  Start-Sleep -Seconds 2
}
say "hub reopened (or timeout) - v3 is now live in it"
