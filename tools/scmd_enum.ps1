<#
.SYNOPSIS
    Enumerate the named scene commands a Sn3D scene plugin registers.

.DESCRIPTION
    sn3d.digit.scenecontrol.dll wires its command handlers in one
    constructor, as repeated blocks:

        lea rdx,[rip+"SetModelsVisible"]     ; the command name
        lea rcx,[rbp+var] / call QString::QString
        ...
        lea rcx,[rip+handler]                ; the handler itself
        mov [rsp+30],rcx                     ; stored with the pair

    This script walks .text for that pair and prints every
    { name, handler RVA } it finds, so the command surface of a vendor
    update can be diffed without a debugger and without any hardcoded
    RVA.  It is the offline twin of `sceneAnchor()` in
    modules\selective_mesh\selective_mesh.c: a command this script lists can
    be hooked by name, a command it does not list is not reachable that
    way.

.PARAMETER Dll
    Path to the plugin to enumerate, for example
    'C:\Program Files\Shining3d\EXStar Hub\sn3d.digit.scenecontrol.dll'.

.PARAMETER Table
    Print the result as a fixed-width two-column block, ready to paste
    into the command list of
    docs/research/merge-common-mesh.md (section 12).

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\scmd_enum.ps1 `
        -Dll 'C:\Program Files\Shining3d\EXStar Hub\sn3d.digit.scenecontrol.dll'

.NOTES
    Read-only: the file is opened with ReadAllBytes, never written.
    See docs/research/merge-common-mesh.md section 12 for the result this
    produced and what it rules out.
#>
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [switch]$Table
)

$b = [IO.File]::ReadAllBytes($Dll)

function U32([int]$o) { [BitConverter]::ToUInt32($b, $o) }
function I32([int]$o) { [BitConverter]::ToInt32($b, $o) }

# --- PE headers -> sections -------------------------------------------
$pe      = I32 0x3C
$numSec  = [BitConverter]::ToUInt16($b, $pe + 6)
$optSize = [BitConverter]::ToUInt16($b, $pe + 20)
$secBase = $pe + 24 + $optSize

$secs = @()
for ($i = 0; $i -lt $numSec; $i++) {
    $o = $secBase + $i * 40
    $secs += [pscustomobject]@{
        Name    = [Text.Encoding]::ASCII.GetString($b, $o, 8).Trim([char]0)
        VSize   = U32 ($o + 8)
        VA      = U32 ($o + 12)
        RawSize = U32 ($o + 16)
        Raw     = U32 ($o + 20)
        Flags   = U32 ($o + 36)
    }
}

function RvaToOff([uint32]$rva) {
    foreach ($s in $secs) {
        if ($rva -ge $s.VA -and $rva -lt ($s.VA + [Math]::Max($s.VSize, $s.RawSize))) {
            return [int]($s.Raw + ($rva - $s.VA))
        }
    }
    return -1
}

# printable, NUL-terminated ASCII at this RVA, or $null
function ReadStr([uint32]$rva) {
    $o = RvaToOff $rva
    if ($o -lt 0) { return $null }
    $end = $o
    while ($end -lt $b.Length -and $b[$end] -ne 0 -and ($end - $o) -lt 64) { $end++ }
    if ($end -eq $o -or $end -ge $b.Length -or $b[$end] -ne 0) { return $null }
    for ($k = $o; $k -lt $end; $k++) {
        if ($b[$k] -lt 0x20 -or $b[$k] -gt 0x7E) { return $null }
    }
    return [Text.Encoding]::ASCII.GetString($b, $o, $end - $o)
}

$text = $secs | Where-Object { $_.Name -eq '.text' }
if (-not $text) { throw 'no .text section' }
$start   = [int]$text.Raw
$stop    = [int]($text.Raw + $text.RawSize)
$textVa  = $text.VA
$textEnd = $text.VA + $text.RawSize

$hits = New-Object System.Collections.ArrayList
for ($i = $start; $i -lt $stop - 0x70; $i++) {
    if ($b[$i] -ne 0x48 -or $b[$i + 1] -ne 0x8D -or $b[$i + 2] -ne 0x15) { continue }

    # lea rdx,[rip+disp32] -> the command name
    $rva  = [uint32]($textVa + ($i - $start))
    $name = ReadStr ([uint32]([int64]$rva + 7 + (I32 ($i + 3))))
    if (-not $name -or $name.Length -lt 4) { continue }

    # the next lea rcx,[rip+code] whose store follows immediately
    $hrva = $null
    for ($j = $i + 7; $j -lt $i + 0x60; $j++) {
        if ($b[$j] -ne 0x48 -or $b[$j + 1] -ne 0x8D -or $b[$j + 2] -ne 0x0D) { continue }
        $rva2 = [uint32]($textVa + ($j - $start))
        $h    = [int64]$rva2 + 7 + (I32 ($j + 3))
        if ($h -lt $textVa -or $h -gt $textEnd) { continue }
        if ($b[$j + 7] -ne 0x48 -or $b[$j + 8] -ne 0x89 -or
            $b[$j + 9] -ne 0x4C -or $b[$j + 10] -ne 0x24) { continue }
        $hrva = [uint32]$h
        break
    }
    if (-not $hrva) { continue }

    [void]$hits.Add([pscustomobject]@{ Name = $name; Handler = ('+0x{0:X}' -f $hrva) })
}

$sorted = $hits | Sort-Object Name -Unique
if ($Table) {
    $arr  = @($sorted)
    $half = [int][Math]::Ceiling($arr.Count / 2)
    for ($i = 0; $i -lt $half; $i++) {
        $l = $arr[$i]
        $rs = ''
        if ($i + $half -lt $arr.Count) {
            $r  = $arr[$i + $half]
            $rs = ('{0,-28}{1}' -f $r.Name, $r.Handler)
        }
        '{0,-28}{1,-12}{2}' -f $l.Name, $l.Handler, $rs
    }
} else {
    $sorted | Format-Table -AutoSize
}
Write-Output ("total name+handler pairs: " + @($sorted).Count)
