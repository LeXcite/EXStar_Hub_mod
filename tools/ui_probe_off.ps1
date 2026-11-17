# ui_probe_off.ps1 - remove the ui_probe diagnostic module (run ELEVATED)
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first'
}
Remove-Item (Join-Path $app 'modules\ui_probe.dll') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $app 'modules\ui_probe.ini') -Force -ErrorAction SilentlyContinue

$ini = Join-Path $app 'modules.ini'
$txt = Get-Content $ini -Raw
$txt = [regex]::Replace($txt, '(?s)\[ui_probe\].*?(?=\[|$)', '')
$txt = $txt -replace '(?m)^\s*log\s*=\s*1', 'log = 0'   # back to quiet
Set-Content -Path $ini -Value $txt.TrimEnd() -Encoding ASCII
'ui_probe OFF: modules.ini ='
Get-Content $ini | Out-String
