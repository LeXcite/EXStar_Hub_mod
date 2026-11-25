<#
.SYNOPSIS
    Dump the Qt5 meta-object of a class inside a shipped binary: class
    name, methods with their metacall index, argument count and method
    type, plus properties.

.DESCRIPTION
    Vendor modules keep their moc data in the image: a QMetaObject is
    { superdata, stringdata, data, static_metacall, related, extradata },
    the stringdata entries are 24-byte descriptors { ref, size, alloc,
    pad, offset } whose text lives at (descriptor + offset) - only that
    offset is static, ref/size/alloc are runtime fields, so the text is
    read up to its NUL - and data[] holds the counts and the section
    offsets (data[5] is the method table: 5 uints per entry in this Qt,
    name, argc, parameters, tag, flags, with the method type in flags
    bits 0..2).

    The index a QML or C++ caller passes to qt_static_metacall is the
    position in that table, so this dump is what you need to invoke a
    non-exported slot by name at runtime - the same lookup
    modules\merge_select\merge_select.c does in metaFind().

.PARAMETER File
    Binary to read (read-only).

.PARAMETER Class
    Class to look for, e.g. ProjectListControllerE7.

.PARAMETER MetacallRva
    Look instead for the metaobject whose static_metacall is this RVA,
    e.g. 0x42DF0.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qmeta_dump.ps1 `
        -File 'C:\Program Files\Shining3d\EXStar Hub\ScanPage_E7.dll' `
        -Class ProjectListControllerE7

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\qmeta_dump.ps1 `
        -File 'C:\Program Files\Shining3d\EXStar Hub\ScanPage_E7.dll' `
        -MetacallRva 0x41880
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [string]$Class = '',
    [string]$MetacallRva = ''
)

$b = [IO.File]::ReadAllBytes($File)
function I32([int]$o) { [BitConverter]::ToInt32($b, $o) }

$pe        = I32 0x3C
$numSec    = [BitConverter]::ToUInt16($b, $pe + 6)
$optSize   = [BitConverter]::ToUInt16($b, $pe + 20)
$secBase   = $pe + 24 + $optSize
$imageBase = [BitConverter]::ToUInt64($b, $pe + 48)          # opt +0x18
$sizeOfImage = [BitConverter]::ToUInt32($b, $pe + 24 + 0x38) # opt +0x38

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
function RvaToOff([uint32]$rva) {
    foreach ($s in $secs) {
        if ($rva -ge $s.VA -and $rva -lt ($s.VA + [Math]::Max($s.VSize, $s.RawSize))) {
            return [int]($s.Raw + ($rva - $s.VA))
        }
    }
    return -1
}
function InImage([int64]$v) {
    return ($v -ge [int64]$imageBase) -and ($v -lt ([int64]$imageBase + $sizeOfImage))
}

# stringdata descriptor -> text (offset is the only static field)
function MetaStr([int]$sdStart, [int]$idx) {
    $doff = $sdStart + $idx * 24
    if ($idx -lt 0 -or $doff + 24 -gt $b.Length) { return $null }
    $textOff = $doff + [BitConverter]::ToInt64($b, $doff + 16)
    if ($textOff -lt 0 -or $textOff -ge $b.Length) { return $null }
    $end = $textOff
    while ($end -lt $b.Length -and $b[$end] -ne 0 -and ($end - $textOff) -lt 96) { $end++ }
    if ($end -eq $textOff) { return $null }
    for ($q = $textOff; $q -lt $end; $q++) {
        if ($b[$q] -lt 0x20 -or $b[$q] -gt 0x7E) { return $null }
    }
    return [Text.Encoding]::ASCII.GetString($b, $textOff, $end - $textOff)
}

$mcWant = -1
if ($MetacallRva) {
    $mcWant = [Convert]::ToUInt64($MetacallRva -replace '^0x', '', 16)
}

# Qt5 MethodType enum, as stored: Method 0x0, Signal 0x6, Slot 0xa
# (private slots carry the Private access bit 0x2, so 0x2 = Slot|Private)
$typeNames = @{ 0 = 'Method'; 2 = 'Slot'; 6 = 'Signal'; 10 = 'Slot'
                16 = 'Constructor'; 20 = 'MethodWithVariant' }
$found = 0
$cands = 0

# metaobjects live in initialized data (.data here, next to the objects
# they describe); every pointer of this build reads 0x1_8xxx_xxxx, so
# bytes 4..7 of each qword are 01 00 00 00
foreach ($ds in ($secs | Where-Object { $_.Flags -band 0x00000040 })) {
    $start = [int]$ds.Raw
    $stop  = [int]($ds.Raw + $ds.RawSize)
    for ($i = $start; $i + 32 -le $stop; $i += 8) {
        # Prefilter on stringdata/data/static_metacall only: superdata (the
        # base class metaobject) lives in ANOTHER module when the direct
        # base is QObject or a plugin class, so on disk it reads 0 and is
        # filled by a relocation at load - it can never take part here.
        # Pointers into this image read 0x1_8xxx_xxxx, byte 4 = 01.
        if ($b[$i + 12] -ne 0x01 -or $b[$i + 20] -ne 0x01 -or
            $b[$i + 28] -ne 0x01) { continue }
        $stringdata = [BitConverter]::ToInt64($b, $i + 8)
        $dataPtr    = [BitConverter]::ToInt64($b, $i + 16)
        $metacall   = [BitConverter]::ToInt64($b, $i + 24)
        if (-not (InImage $stringdata) -or -not (InImage $dataPtr) -or
            -not (InImage $metacall)) { continue }
        $sdOff = RvaToOff ([uint32]($stringdata - $imageBase))
        $dOff  = RvaToOff ([uint32]($dataPtr - $imageBase))
        if ($sdOff -lt 0 -or $dOff -lt 0) { continue }
        $rev = I32 $dOff
        if ($rev -lt 7 -or $rev -gt 200) { continue }
        $cands++

        $mcRva = [uint32]($metacall - $imageBase)
        $cls   = MetaStr $sdOff (I32 ($dOff + 4))
        if ((-not $Class -or $cls -ne $Class) -and
            ($mcWant -lt 0 -or $mcRva -ne $mcWant)) { continue }

        $methodCount = I32 ($dOff + 16)
        # NOTE the parens: `I32 X * 4` would call I32 with (X*4), since a
        # PowerShell command call swallows every following token as argument
        $methodData  = $dOff + ((I32 ($dOff + 20)) * 4)   # offsets are uints
        $propCount   = I32 ($dOff + 24)
        $propData    = $dOff + ((I32 ($dOff + 28)) * 4)

        "class        : $cls   (moc revision $rev)"
        "metaobject   : RVA 0x{0:X}" -f [uint32]($i - $ds.Raw + $ds.VA)
        "static meta  : RVA 0x{0:X}" -f $mcRva
        "methods      : $methodCount"
        ""
        "  idx  argc  type                 name"
        for ($m = 0; $m -lt $methodCount; $m++) {
            $e  = $methodData + $m * 20
            $nm = MetaStr $sdOff (I32 $e)
            $ac = I32 ($e + 4)
            $fl = I32 ($e + 16)
            "  {0,3}  {1,4}  {2,-19}  {3}" -f $m, $ac, $typeNames[[int]($fl -band 0xf)], $nm
        }
        ""
        "  properties ($propCount):"
        for ($p = 0; $p -lt $propCount; $p++) {
            "    " + (MetaStr $sdOff (I32 ($propData + $p * 8)))
        }
        ""
        $found++
    }
}
"matched $found metaobject(s) out of $cands candidates in $(Split-Path $File -Leaf)"
