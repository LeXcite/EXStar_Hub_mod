<#
.SYNOPSIS
  Builds the complete EXStar Hub Mods project: bootstrap, host, modules,
  and the single-file Mod Manager.

.DESCRIPTION
  Requires Visual Studio 2022 (or Build Tools) with the
  "Desktop development with C++" workload. Output (x64 only):

    build\winmm.dll                        bootstrap proxy
    build\ExStarRec.dll                    module host
    build\modules\marker_inframe.dll        modules
    build\examples\template.dll            example modules (not deployed)
    build\ExStarModManager.exe             installer/manager (all of the
                                           above embedded as resources)

.USAGE
  powershell -ExecutionPolicy Bypass -File build.ps1
#>
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ---------------------------------------------------------------- MSVC --
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) {
    throw @"
vswhere.exe not found.
Install "Visual Studio Build Tools" with the "Desktop development with C++" workload:
https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
"@
}
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with C++ tools found." }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

# ------------------------------------------------------------- targets --
$compile = '/nologo /O2 /MT /GS- /utf-8 /D_CRT_SECURE_NO_WARNINGS'
$mhInc   = '/I third_party\minhook\include /I third_party\minhook\src'
$mhSrc   = @(
    'third_party\minhook\src\buffer.c',
    'third_party\minhook\src\hook.c',
    'third_party\minhook\src\trampoline.c',
    'third_party\minhook\src\hde\hde32.c',
    'third_party\minhook\src\hde\hde64.c'
) -join ' '
$mhObj   = @(
    'build\obj\buffer.obj', 'build\obj\hook.obj', 'build\obj\trampoline.obj',
    'build\obj\hde32.obj', 'build\obj\hde64.obj'
) -join ' '

# deployable modules: add one line per new module here
$modules = @(
    'modules\marker_inframe\marker_inframe.c',
    'modules\marker_probe\marker_probe.c',   # dev diagnostic, never embedded
    'modules\ui_probe\ui_probe.c',           # dev diagnostic, never embedded
    'modules\marker_quality\marker_quality.c',
    'modules\popup_dim\popup_dim.c',
    'modules\wallpaper\wallpaper.c',
    'modules\laser50\laser50.c',
    'modules\fitfill\fitfill.c',
    'modules\gpu_gate\gpu_gate.c',
    'modules\selective_mesh\selective_mesh.c'
)
# example modules (built for verification, never deployed)
$examples = @('examples\template\template.c')
# modules embedded into the manager: 'id' + sources of binary and ini template
# (must match the registry inside src\manager.c)
$managerMods = @(
    @{ id = 'marker_inframe'; ini = 'modules\marker_inframe\marker_inframe.ini' }
    @{ id = 'marker_quality'; ini = 'modules\marker_quality\marker_quality.ini' }
    @{ id = 'popup_dim';      ini = 'modules\popup_dim\popup_dim.ini' }
    @{ id = 'wallpaper';      ini = 'modules\wallpaper\wallpaper.ini' }
    @{ id = 'laser50';        ini = 'modules\laser50\laser50.ini' }
    @{ id = 'fitfill';        ini = 'modules\fitfill\fitfill.ini' }
    @{ id = 'gpu_gate';       ini = 'modules\gpu_gate\gpu_gate.ini' }
    @{ id = 'selective_mesh'; ini = 'modules\selective_mesh\selective_mesh.ini' }
)

$version = '1.7.1'   # product version; release exe is copied as ExStarModManager_<version>.exe

function New-VersionRes($path, $desc, $origName) {
    $vq = ($version -split '\.').Trim()
    $fv = ($vq + '0') -join ','
@"
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
 FILEVERSION $fv PRODUCTVERSION $fv
 FILEFLAGSMASK 0x3fL FILEFLAGS 0 FILEOS 0x40004 FILETYPE 0x2 FILESUBTYPE 0
BEGIN
  BLOCK "StringFileInfo" {
    BLOCK "040904b0" {
      VALUE "CompanyName",    "EXStar Hub Mods"
      VALUE "FileDescription", "$desc"
      VALUE "FileVersion",     "$version"
      VALUE "LegalCopyright",  "MIT License"
      VALUE "OriginalFilename","$origName"
      VALUE "ProductName",     "EXStar Hub Mods"
      VALUE "ProductVersion",  "$version"
    }
  }
  BLOCK "VarFileInfo" { VALUE "Translation", 0x409, 1200 }
END
"@ | Set-Content -LiteralPath $path -Encoding ASCII
}

foreach ($d in 'build', 'build\obj', 'build\modules', 'build\examples') {
    New-Item -ItemType Directory -Force -Path (Join-Path $root $d) | Out-Null
}

# version resources: the proxy's OriginalFilename is a private marker the
# manager uses to tell "our" winmm.dll from a vendor file - keep in sync
# with PROXY_MARKER in src\manager.c
New-VersionRes (Join-Path $root 'build\version_winmm.rc') `
    'EXStar Hub Mods bootstrap proxy' 'ExStarMod.winmm.dll'
New-VersionRes (Join-Path $root 'build\version_host.rc') `
    'EXStar Hub Mods module host' 'ExStarRec.dll'
New-VersionRes (Join-Path $root 'build\version_manager.rc') `
    'EXStar Hub Mod Manager' 'ExStarModManager.exe'

# manager.rc: embed every payload as RCDATA -> one-file manager exe
$rcEsc = $root.Replace('\', '\\')
$rc = @(
    "BOOT_WINMM RCDATA `"$rcEsc\\build\\winmm.dll`"",
    "HOST       RCDATA `"$rcEsc\\build\\ExStarRec.dll`""
)
foreach ($m in $managerMods) {
    $upper = $m.id.ToUpper()
    $dllEsc = "$rcEsc\\build\\modules\\$($m.id).dll"
    $iniEsc = $m.ini.Replace('\', '\\')
    $iniEsc = "$rcEsc\\$iniEsc"
    $rc += "MOD_$upper RCDATA `"$dllEsc`""
    $rc += "MOD_${upper}_INI RCDATA `"$iniEsc`""
}
Set-Content -LiteralPath (Join-Path $root 'build\manager.rc') -Value $rc -Encoding ASCII

$steps = [System.Collections.Generic.List[string]]::new()
$steps.Add("cl $compile /c $mhInc /Fobuild\obj\ $mhSrc")
$steps.Add("rc /nologo /fo build\version_winmm.res build\version_winmm.rc")
$steps.Add("rc /nologo /fo build\version_host.res build\version_host.rc")
$steps.Add("rc /nologo /fo build\version_manager.res build\version_manager.rc")
$steps.Add("cl $compile /c $mhInc /I include /Fobuild\obj\ src\host.c")
$steps.Add("cl /nologo /LD /O2 /MT build\obj\host.obj $mhObj build\version_host.res" +
           ' /Fe:build\ExStarRec.dll /link user32.lib gdi32.lib opengl32.lib')
$steps.Add("cl $compile /LD /Fobuild\obj\ src\winmm_proxy.c build\version_winmm.res" +
           ' /Fe:build\winmm.dll')
foreach ($m in $modules) {
    $name = [IO.Path]::GetFileNameWithoutExtension($m)
    $steps.Add("cl $compile /LD /I include /Fobuild\obj\ $m /Fe:build\modules\$name.dll")
}
foreach ($m in $examples) {
    $name = [IO.Path]::GetFileNameWithoutExtension($m)
    $steps.Add("cl $compile /LD /I include /Fobuild\obj\ $m /Fe:build\examples\$name.dll")
}
$steps.Add('rc /nologo /fo build\manager.res build\manager.rc')
$uac = '/MANIFESTUAC:"level=''requireAdministrator'' uiAccess=''false''"'
# manager.c is compiled as C++: the wallpaper converter uses the GDI+
# C++ headers (the only C++ unit in the project; gdiplus.lib via pragma).
# /TP also makes cl treat the .res files as source, so they go to the
# linker explicitly instead.
$steps.Add("cl $compile /TP /Fobuild\obj\ src\manager.c" +
            ' /link build\manager.res' +
            ' build\version_manager.res' +
           ' /OUT:build\ExStarModManager.exe /SUBSYSTEM:WINDOWS' +
           ' /ENTRY:wWinMainCRTStartup /MANIFEST:EMBED' +
           " $uac" +
           ' /MANIFESTINPUT:src\manager.exe.manifest' +
           ' user32.lib gdi32.lib shell32.lib ole32.lib comdlg32.lib comctl32.lib version.lib' +
           ' advapi32.lib dwmapi.lib')

# --------------------------------------------------------------- build --
$bat = Join-Path $env:TEMP 'exstar_mods_build.bat'
$lines = @('@echo off', "call `"$vcvars`" >nul", "cd /d `"$root`"")
foreach ($c in $steps) { $lines += 'if errorlevel 1 exit /b 1'; $lines += $c }
Set-Content -LiteralPath $bat -Value $lines -Encoding ASCII

& cmd.exe /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

'built:'
Get-ChildItem "$root\build" -Recurse -Include *.dll, *.exe |
    ForEach-Object { '  ' + $_.FullName.Substring($root.Length + 1) }

# -- release: keep every version under a versioned file name ----------
$rel = Join-Path $root 'release'
New-Item -ItemType Directory -Force -Path $rel | Out-Null
$relExe = Join-Path $rel "ExStarModManager_$version.exe"
Copy-Item (Join-Path $root 'build\ExStarModManager.exe') $relExe -Force
"release: release\ExStarModManager_$version.exe"
