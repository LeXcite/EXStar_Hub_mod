# ui_probe_on.ps1 - deploy the ui_probe diagnostic module (run ELEVATED)
# Copies ui_probe.dll into the app modules dir, enables it in modules.ini and
# turns host logging on. Nothing else about the install changes.
$ErrorActionPreference = 'Stop'
$app  = 'C:\Program Files\Shining3d\EXStar Hub'
$root = Split-Path -Parent $PSScriptRoot
$dll  = Join-Path $root 'build\modules\ui_probe.dll'
if (!(Test-Path $dll)) { throw "not built: $dll (run build.ps1 first)" }
if (Get-Process 'EXStar Hub' -ErrorAction SilentlyContinue) {
    throw 'close EXStar Hub first (modules.ini is read at app start)'
}

Copy-Item $dll (Join-Path $app 'modules\ui_probe.dll') -Force
Copy-Item (Join-Path $root 'modules\ui_probe\ui_probe.ini') `
          (Join-Path $app 'modules\ui_probe.ini') -Force -ErrorAction SilentlyContinue

$ini = Join-Path $app 'modules.ini'
$txt = Get-Content $ini -Raw
$txt = $txt -replace '(?m)^\s*log\s*=\s*\d', 'log = 1'
if ($txt -notmatch '\[ui_probe\]') {
    $txt += "`r`n[ui_probe]`r`nenabled = 1`r`n"
} else {
    $sec = [regex]::Match($txt, '(?s)\[ui_probe\].*?(?=\[|$)')
    $fixed = $sec.Value -replace '(?m)^\s*enabled\s*=\s*\d', 'enabled = 1'
    $txt = $txt.Remove($sec.Index, $sec.Length).Insert($sec.Index, $fixed)
}
Set-Content -Path $ini -Value $txt -Encoding ASCII -NoNewline
'ui_probe ON: modules.ini ='
Get-Content $ini | Out-String
