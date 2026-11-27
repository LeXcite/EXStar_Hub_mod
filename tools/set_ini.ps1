# set_ini.ps1 (ELEVATED) - rewrite modules.ini to an exact given state.
# Developer tool: the Mod Manager is what a normal install should use, it
# writes the same keys from its toggles (see writeSiblingKeys in
# src\manager.c).  This script exists for diagnostic sessions, where the
# host has to be woken up in sibling processes by the legacy dev_all /
# dev_modules pair.
param([string]$log='1', [string]$probe='0',
      [string]$devall='0', [string]$quality='0',
      [string]$devmodules='marker_probe', [string]$prod='0')
$app = 'C:\Program Files\Shining3d\EXStar Hub'
if ($prod -eq '1') {
    # what the Mod Manager would write: the capable modules that are on
    $sibOn = if ($quality -eq '1') { '1' } else { '0' }
    $txt = @"
; ExStar Hub Mods - module switches (applied at app start)

[host]
log = $log
sibling_all = $sibOn
sibling_modules = marker_quality,selective_mesh

[marker_probe]
enabled = $probe

[marker_quality]
enabled = $quality
"@
} else {
    $txt = @"
; ExStar Hub Mods - module switches (applied at app start)

[host]
log = $log
dev_all = $devall
dev_modules = $devmodules

[marker_probe]
enabled = $probe

[marker_quality]
enabled = $quality
"@
}
Set-Content -LiteralPath "$app\modules.ini" -Value $txt -Encoding ASCII
Get-Content "$app\modules.ini" | Out-String
