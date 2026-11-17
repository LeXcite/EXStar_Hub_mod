# EXStar Hub Mods

A small, dependency-free mod framework for **EXStar Hub** (Windows x64):
a `winmm.dll` proxy bootstraps a module host, and modules - plain C DLLs
dropped into a `modules\` folder - extend or change the application's
behavior at runtime. The vendor binaries are never modified; uninstall is
deleting a few files.

> ⚠️ Unofficial research project. "EXStar Hub" and all related marks belong
> to their owner. Use at your own risk; intended for interoperability
> research and personal customization.

## Modules

Each module README is a **self-contained artifact**: symptom, reverse-engineered
vendor mechanism, exact patch/hook inventory, configuration, runtime
verification and failure modes - readable cold, no external context needed.

| Module | Description | Status |
|---|---|---|
| [marker_inframe](modules/marker_inframe/README.md) | in-frame marker grows in size instead of flashing the red "detected in current scan frame" color; the marker keeps its color, so it composes with Marker Quality | ✅ tested on 3.1.49 |
| [marker_quality](modules/marker_quality/README.md) | tracking markers are painted by observation quality: dark red (seen) → yellow (decent) → green (fully tracked, latched) | ✅ tested on 3.1.49 |
| [popup_dim](modules/popup_dim/README.md) | grey veils behind popups become light translucent overlays; the solid grey background of busy overlays (project loading / processing / exiting) becomes frosted glass over the live page - redirected at its single shared source | ✅ live-verified on 3.1.x (v1.2.7, 2026-11-14) |
| [wallpaper](modules/wallpaper/README.md) | custom application background: the vendor's embedded JPEG in AppUi.dll is replaced in process memory with `modules\wallpaper.jpg` (placed and converted by the Mod Manager); stock wallpaper returns on *Сброс* | ✅ live-verified on 3.1.x (v1.0.0, 2026-11-14) |
| [fps](modules/fps/README.md) | live `FPS: N` readout at the bottom right of the scan page: the **real** UI frame rate (frameSwapped counter) inserted into the compressed scan page QML at DLL map time - the vendor "FPS: 80-90" is a constant capture-pipeline statistic, not rendered frames | ✅ live-verified on 3.1.x (v1.0.0, 2026-11-17) |
| [laser50](modules/laser50/README.md) | the complete 50-line feature, one switch: detours the exported `set_match_plane_id_vulkan` (by name) to widen the plane-id tuple `(3,21,3,21)` → `(0,24,0,24)` so all 25+25 lines reach the cloud, **and** rewrites only the label's argument (`.arg(50)`, translated qsTr source kept) so the Light Source Mode button reads `50 lines`; `7 lines` stays stock | ✅ live-verified on 3.1.x (v1.1.0, 2026-11-17: 25×25 lines in a real scan + `50 lines` button, EN UI intact) |
| [fitfill](modules/fitfill/README.md) | Ctrl+D / *Zoom to Fit* frames the model into the viewport: forces the manipulator's `setIsZoomOptimization` export off (the same switch the friend's version-bound byte patch cleared - semantics now RE'd: pinned camera distance mode) | ✅ live-verified on 3.1.x (v1.0.0, 2026-11-17) |

## Mod Manager

`ExStarModManager.exe` is a single self-contained executable (bootstrap,
host and all modules are embedded as resources):

* **GUI** (elevated automatically): pick the application folder, flip the
  switch next to a module - ON installs and enables it, OFF removes it.
  The window scales itself to the screen (system DPI and physical panel
  density are detected at startup; force it with `--zoom 1.5`).
  Modules with tunable behavior show inline controls that write the
  matching ini keys and apply after an EXStar Hub restart:
  *Marker In-Frame*: an *увеличение* slider (in-frame scale). *Marker
  Quality*: a "качество позиционирования маркеров" preset row - one click
  sets the three coupled green thresholds together (грубое 0.25 mm /
  среднее 0.1 mm / точное 0.05 mm / сверхточное 0.02 mm spread, with
  matching frames and viewing directions); the active preset is read
  back from the ini. *Popup Dim*: a *затемнение* slider (how opaque the
  overlay behind popups stays). *Wallpaper*: a *Выбрать файл…* button
  (any jpg/png/bmp is converted to a 1920x1080 JPEG fitted into the
  vendor's wallpaper slot and placed as `modules\wallpaper.jpg`) and a
  *Сброс* button (back to the stock wallpaper). Values are stored in
  the module's ini;
  switch states are read back from disk, so the window always shows what
  is actually installed. *Вернуть программу в сток* removes everything.
* **CLI**:

```text
ExStarModManager.exe --list
ExStarModManager.exe --install --modules marker_inframe[,id2] [--scale 1.5] [--app "D:\...\EXStar Hub"]
ExStarModManager.exe --remove  --modules id1,id2
ExStarModManager.exe --restore
ExStarModManager.exe --zoom 1.5        # GUI only: force UI scale 1.0..4.0
```

Everything the manager places is recorded in `<app>\exstar_mods.state`, so
*restore stock* deletes exactly its own files - and never touches a foreign
`winmm.dll` (a vendor file there is detected by its version resource and
aborts the install).

## How it works

```
EXStar Hub.exe ─ imports WINMM.dll (via Qt5Core)
   └─ winmm.dll            our proxy, forwards every export to
        └─ winmm_real.dll  a copy of the system winmm
        └─ ExStarRec.dll   module host: ABI check, enable flags,
             └─ modules\   one DLL per module, hot-configurable
```

Modules receive a tiny C service table (`include/exstar_sdk.h`): logging,
per-module config, GL entry-point resolution, MinHook-backed detour
install. Details: [docs/architecture.md](docs/architecture.md).

Reverse-engineering protocols behind the features
([docs/research/](docs/research/), рус.):

* [marker-pipeline.md](docs/research/marker-pipeline.md) — the red-marker
  signal and why `glUniform4fv` hooking beats patching (marker_quality);
* [scanpage-qml-resources.md](docs/research/scanpage-qml-resources.md) —
  the vendor's compressed QML (Qt zrc) format, the safe in-memory write
  protocol, `.qm` catalog pitfalls (popup_dim, fps, laser50 `[caption]`);
* [laser-plane-ids.md](docs/research/laser-plane-ids.md) — the
  `set_match_plane_id_vulkan` window and the "50 lines" caption (laser50);
* [zoom-fit-optimization.md](docs/research/zoom-fit-optimization.md) — the
  zoom-optimization mode behind "Zoom to Fit" (fitfill);
* [batch-2026-11-17-status.md](docs/research/batch-2026-11-17-status.md) —
  dated journal of the fps/laser50/fitfill batch (deploys, the caption
  v1.0.0 catalog bug and its fix, module merge, code revision).

What is shipped and what is planned next (the borrowed-capabilities port
batch): [docs/ROADMAP.md](docs/ROADMAP.md) (рус.).

## Requirements

* Windows x64, EXStar Hub 3.1.x (see the compatibility table below);
* build: Visual Studio 2022 Build Tools, "Desktop development with C++".

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output:

```
build\winmm.dll                   bootstrap proxy
build\ExStarRec.dll               module host
build\modules\marker_inframe.dll  modules
build\examples\template.dll       example module (not deployed)
build\ExStarModManager.exe        one-file manager/installer
```

`tools\dim_dryrun.c` is an offline harness for the popup_dim QML walkers:
it compiles the module into a console tool and replays every byte edit on
DLL files (inflate → edit → re-deflate fit check), so walker changes are
verified without launching the app - see the module README.

## Install

Easiest: download the latest `ExStarModManager_<version>.exe` from
[Releases](https://github.com/LeXcite/EXStar_Hub_mod/releases) (single
file, no install, asks for admin once), run it and flip the switch of the
module you want, restart EXStar Hub. That is it. Building from source
works the same: run `build\ExStarModManager.exe`.

<details>
<summary>Manual alternative (no manager)</summary>

1. Close EXStar Hub completely.
2. In an elevated PowerShell in the application directory
   (e.g. `C:\Program Files\Shining3d\EXStar Hub\`):

```powershell
$src = '<repo>\build'
$dst = 'C:\Program Files\Shining3d\EXStar Hub'
Copy-Item "$src\winmm.dll","$src\ExStarRec.dll" $dst
Copy-Item "$src\modules" $dst -Recurse -Force
Copy-Item "$env:SystemRoot\System32\winmm.dll" "$dst\winmm_real.dll"
Copy-Item '<repo>\config\modules.ini' $dst
```

</details>

### Uninstall

Delete `winmm.dll`, `winmm_real.dll`, `ExStarRec.dll`, `modules.ini` and
the `modules\` folder from the application directory. Nothing else was
touched.

## Configuration

`<app dir>\modules.ini`:

```ini
[host]
log = 0              ; 1 -> %LOCALAPPDATA%\ExStarHubMods\mod.log

[marker_inframe]
enabled = 1
```

`<app dir>\modules\<id>.ini` - per-module options (see module READMEs).
All changes apply at application start.

## Compatibility

| EXStar Hub | Tested | Notes |
|---|---|---|
| 3.1.49 | ✅ | Windows 11 x64, NVIDIA driver |
| other 3.1.x | ? | re-check research anchors before deploying |

## Writing a module

Copy [`examples/template`](examples/template/template.c) - it is a
compilable skeleton; the ABI is documented in
[`include/exstar_sdk.h`](include/exstar_sdk.h), the lifecycle and safety
rules in [docs/architecture.md](docs/architecture.md).

## Safety design

* vendor DLLs are never patched on disk - proxy + in-memory detours only;
* `DllMain` never fails, risky callbacks are SEH-guarded and pass calls
  through on any error;
* no hardcoded addresses/RVAs - name/string anchors only;
* disabled modules are not loaded at all; a failing module disables
  itself without affecting others.

## Third-party

* [MinHook](https://github.com/TsudaKageyu/minhook) (MIT), vendored under
  `third_party/minhook/`.

## License

MIT - see [LICENSE](LICENSE).
