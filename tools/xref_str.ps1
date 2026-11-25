<#
.SYNOPSIS
    Find code references to a literal in a PE image (read-only).

.DESCRIPTION
    Prints where the literal sits (file offset and RVA) and every
    rip-relative `lea reg,[rip+disp32]` in an executable section that
    points at it, as RVAs you can hand to a decompiler.  This is how the
    four senders of the vendor's `SetModelNodesVisible` command were
    found in ScanPage_E7.dll: the literal has no import and no export,
    only code that materialises it.

.PARAMETER File
    Binary to read.

.PARAMETER Text
    Literal to look for (a NUL is appended, so it matches whole strings).

.PARAMETER Wide
    Look for UTF-16 text instead of ASCII.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\xref_str.ps1 `
        -File 'C:\Program Files\Shining3d\EXStar Hub\ScanPage_E7.dll' `
        -Text 'SetModelNodesVisible'
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$Text,
    [switch]$Wide
)

$b = [IO.File]::ReadAllBytes($File)
function I32([int]$o) { [BitConverter]::ToInt32($b, $o) }

$pe      = I32 0x3C
$numSec  = [BitConverter]::ToUInt16($b, $pe + 6)
$optSize = [BitConverter]::ToUInt16($b, $pe + 20)
$secBase = $pe + 24 + $optSize

$secs = @()
for ($n = 0; $n -lt $numSec; $n++) {
    $o = $secBase + $n * 40
    $secs += [pscustomobject]@{
        Name    = [Text.Encoding]::ASCII.GetString($b, $o, 8).Trim([char]0)
        VSize   = [BitConverter]::ToUInt32($b, $o + 8)
        VA      = [BitConverter]::ToUInt32($b, $o + 12)
        RawSize = [BitConverter]::ToUInt32($b, $o + 16)
        Raw     = [BitConverter]::ToUInt32($b, $o + 20)
        Flags   = [BitConverter]::ToUInt32($b, $o + 36)
    }
}
function OffToRva([int]$off) {
    foreach ($s in $secs) {
        if ($off -ge $s.Raw -and $off -lt ($s.Raw + [Math]::Max($s.VSize, $s.RawSize))) {
            return [uint32]($s.VA + ($off - $s.Raw))
        }
    }
    return 0
}

# the literal, NUL terminated
$needle = if ($Wide) { [Text.Encoding]::Unicode.GetBytes($Text + [char]0) }
          else       { [Text.Encoding]::ASCII.GetBytes($Text + [char]0) }

# a Latin-1 view of the image: one char per byte, so an IndexOf hit is a
# byte offset (a byte-by-byte search of a 10 MB image in script is minutes)
$flat = [Text.Encoding]::GetEncoding(28591).GetString($b)
$rv   = @()
$at   = 0
while (($at = $flat.IndexOf($needle, $at, [StringComparison]::Ordinal)) -ge 0) {
    $rv += OffToRva $at
    "occurrence: file 0x{0:X}  RVA 0x{1:X}" -f $at, (OffToRva $at)
    $at += $needle.Length
}
if ($rv.Count -eq 0) { return }

# executable sections: flags bit 0x20000000 (MEM_EXECUTE)
$found = 0
foreach ($t in ($secs | Where-Object { $_.Flags -band 0x20000000 })) {
    $start = [int]$t.Raw
    $stop  = [int]($t.Raw + $t.RawSize)
    for ($i = $start; $i -lt $stop - 7; $i++) {
        if ($b[$i] -ne 0x48 -or $b[$i + 1] -ne 0x8D) { continue }
        $modrm = $b[$i + 2]
        if (($modrm -band 0xC7) -ne 0x05) { continue }   # [rip+disp32]
        $rva = [uint32]($t.VA + ($i - $start))
        $tgt = [int64]$rva + 7 + (I32 ($i + 3))
        foreach ($r in $rv) {
            if ($tgt -eq [int64]$r) {
                "  lea ref at RVA 0x{0:X}  (reg {1}, {2})" -f $rva,
                    (($modrm -shr 3) -band 7), $t.Name
                $found++
            }
        }
    }
}
"references found: $found"
