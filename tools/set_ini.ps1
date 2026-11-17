# set_ini.ps1 (ELEVATED) - rewrite modules.ini to an exact given state
# -prod 1 writes the production sibling whitelist (sibling_all/sibling_modules)
# instead of the legacy developer keys (dev_all/dev_modules).
param([string]$log='1', [string]$inframe='0', [string]$probe='0',
      [string]$devall='0', [string]$quality='0',
      [string]$devmodules='marker_probe', [string]$prod='0')
$app = 'C:\Program Files\Shining3d\EXStar Hub'
if ($prod -eq '1') {
    $sibOn = if ($quality -eq '1') { '1' } else { '0' }
    $txt = @"
; ExStar Hub Mods - module switches (applied at app start)

[host]
log = $log
sibling_all = $sibOn
sibling_modules = marker_quality

[marker_inframe]
enabled = $inframe

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

[marker_inframe]
enabled = $inframe

[marker_probe]
enabled = $probe

[marker_quality]
enabled = $quality
"@
}
Set-Content -LiteralPath "$app\modules.ini" -Value $txt -Encoding ASCII
Get-Content "$app\modules.ini" | Out-String
