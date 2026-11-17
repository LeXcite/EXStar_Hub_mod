# probe_off.ps1 - remove the marker_probe diagnostic module (run ELEVATED)
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first'
}
Remove-Item (Join-Path $app 'modules\marker_probe.dll') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $app 'modules\marker_probe.ini') -Force -ErrorAction SilentlyContinue

$ini = Join-Path $app 'modules.ini'
$txt = Get-Content $ini -Raw
$txt = [regex]::Replace($txt, '(?s)\[marker_probe\].*?(?=\[|$)', '')
$txt = $txt -replace '(?m)^\s*log\s*=\s*1', 'log = 0'   # back to quiet
# re-enable the shipping module that probe_prep switched off
$txt = $txt -replace '(?m)(\[marker_inframe\]\r?\n)enabled = 0', '$1enabled = 1'
Set-Content -Path $ini -Value $txt.TrimEnd() -Encoding ASCII
'probe OFF: modules.ini ='
Get-Content $ini | Out-String
