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
| [marker_quality](modules/marker_quality/README.md) | a tracking marker's **disc** shows how well the scanner observed it (dark red = seen, yellow = decent, green = fully tracked, green latches) and its **outline** shows that the marker is in the scanner's field of view this frame (blue) - the vendor's red flash is gone, so quality stays readable while the marker is still in view | ✅ quality path and outline both live-verified by the owner on 2.x (file stamp 3.1.49) through 2.1.0-14, 2026-11-28 (v1.1.8). The vendor never resolves `frontExternalColor` by name for a marker: its ring comes from a channel one StateSet *below* the markers and shared by them, so each marker gets a channel of its own, armed with `OVERRIDE_VALUES` while it is in view and without it again when the marker leaves. `mod.log` names the case (`outline: made on the marker's own StateSet`, or that the entry belongs to the vendor) and the heartbeat counts the writes |
| [popup_dim](modules/popup_dim/README.md) | grey veils behind popups become light translucent overlays; the solid grey background of busy overlays (project loading / processing / exiting) becomes frosted glass over the live page - redirected at its single shared source | ✅ live-verified on 2.x (file stamp 3.1.x) through 2.1.0-10 (v1.2.8, 2026-11-18); re-hooked on 2.1.0-14 (2026-11-27) with `verified=5 restored=0 fault=0` on every target |
| [wallpaper](modules/wallpaper/README.md) | custom application background: the vendor's embedded JPEG in AppUi.dll is replaced in process memory with `modules\wallpaper.jpg` (placed and converted by the Mod Manager); stock wallpaper returns on *Reset* | ✅ live-verified on 2.x (file stamp 3.1.x, v1.0.0); on 2.1.0-14 it fails open as designed - the update deleted the user's `modules\wallpaper.jpg`, so the module logs `no ... wallpaper.jpg - stock wallpaper kept` and declines until the image is placed again |
| [laser50](modules/laser50/README.md) | the complete 50-line feature, one switch: detours the exported `set_match_plane_id_vulkan` (by name) to widen the plane-id tuple `(3,21,3,21)` → `(0,24,0,24)` so all 25+25 lines reach the cloud, **and** rewrites only the label's argument (`.arg(50)`, translated qsTr source kept) so the Light Source Mode button reads `50 lines`; `7 lines` stays stock | ✅ live-verified on 2.x (file stamp 3.1.x, v1.1.0: 25×25 lines in a real scan + `50 lines` button, EN UI intact); re-hooked on 2.1.0-14 (2026-11-27) - export detour plus `caption ScanPage_E7.dll ... 1 caption(s) rewritten (ok)` |
| [gpu_gate](modules/gpu_gate/README.md) | run EXStar Hub on integrated GPUs (e.g. a Radeon 780M): the vendor startup gate refuses every non-discrete GPU with a "requires NVIDIA RTX 3060" dialog before the pipeline - pure Vulkan behind it - ever runs; one in-memory byte (`jne`→`ja`) found by a unique instruction pattern lets INTEGRATED/OTHER devices reach the vendor's *own* AMD/NVIDIA checks (which pass them), while VIRTUAL/CPU stay refused; behavior-neutral on machines that already pass | ✅ live-verified on 2.1.0-10 (v1.0.0, 2026-11-19 NVIDIA self-test; the relaxed AMD branch field-verified on 2.0.0-45 on a Radeon 780M); on 2.1.0-14 the pattern resolved at the predicted `Sn3DSpeckleNew.dll` RVA 0x1AB6B5 (2026-11-27) |
| [selective_mesh](modules/selective_mesh/README.md) | pick which scans feed the **common mesh**: hide a scan's **eye icon** in the project list and the next merge is built without it - one in-memory detour on the single exported question `HandlePrjFile::isGenerateCloud` (only Sn3DMultiProjects.dll imports it), answered through the vendor's own "no cloud" branch, so stored data, display and per-scan export stay stock. The eye state crosses into the processes that build the mesh as `[uuid] included = 0` in `selective-mesh.ini`, and it lives exactly as long as the run: the hub clears those sections when it starts, because the vendor shows every scan again when a project is re-opened (1.8.0 - the hand-editable `[folder]` sections, the `.map.txt` roster and the manager's roster window were retired with it, the eye is the only interface). Since 1.7.0 a merge this module would leave with **no** scan runs as if all eyes were open (the vendor's post-process never finishes on empty input), reading that merge's own scan list straight out of the caller's registers - trusted only when the list it points at contains the scan being asked about, no Qt call, `tools/merge_list_lab.c` proves the container layout against the shipped Qt. The viewport half (re-asserting a hidden eye after the Mesh Model environment switch) is **off by default**: it calls the vendor's show/hide slot and can hang the hub (1.6.1) | ✅ live-verified on 2.1.0-10 (mesh exclusion + eye binding across processes 1.4.7/2026-11-24; 1.6.2 eye-click fix re-confirmed 2026-11-26; the all-hidden guard live-verified 2026-11-26: every eye hidden -> the merge finishes with all scans instead of stalling at 80 %); 1.8.0 (eye-only interface, session-scoped state) live-verified on 2.1.0-14 (2026-11-27): two of three eyes hidden -> both `-> SKIP`, vendor `meshFinished`, and the all-hidden guard read the merge list with `self at #1, 0 unreadable` on the recompiled `Sn3DMultiProjects.dll`, so the item layout holds; joined as `merge_select` with manager 1.5.0, renamed Selective Mesh in 1.6.0 |

All modules except selective_mesh (and gpu_gate) were re-verified live against the
vendor update **2.1.0-10 (file stamp 3.1.49)** on 2026-11-18 with zero code
changes - migration evidence:
[update-2.1.0-10-migration.md](docs/research/update-2.1.0-10-migration.md).

**Real FPS** left the set on 2026-11-27 (manager 1.7.1): on 2.1.0-14 the hub
closes itself a few seconds into the scan page whenever its QML insert is
applied. Bisected on the installed app - the module alone reproduces the exit,
each of the other eight survives with no symptom. The insert itself still
lands byte-identically (the `/// ScanPage_E7` record is unchanged between
2.1.0-10 and 2.1.0-14), so what broke is how the new build consumes the
patched page. A frame-rate readout is not worth that, so the module is
retired; a leftover `modules\fps.dll` is cleaned up by the manager's legacy
list. Sources and its README are intact in the git history (manager ≤ 1.7.0),
and the compressed-QML mechanism it pioneered stays documented in
[scanpage-qml-resources.md](docs/research/scanpage-qml-resources.md) -
laser50's caption half still uses it.

**Zoom to Fit Fill** left the set the same day (manager 1.7.2), on the owner's
decision rather than by necessity. Its story is unusual and worth keeping: on
2.0.0-45 it worked (Ctrl+D filled the frame), then 2.1.0-10 moved the vendor's
fit onto a new pipeline that ignores the flag it forces; research found the
replacement anchor - the single `setEnableScanScale(false)` call site - and 1.1.0
shipped it, and on 2.1.0-14 both of its hooks fired in a real session
(`scan scale forced to 1 at sn3d.digit.scenecontrol.dll+0xC4865`,
`setIsZoomOptimization(1) -> forwarding 0`), but the visible re-fill was never
confirmed on any current build. A fit nobody can see is not a feature, so the
module is retired; a leftover `modules\fitfill.dll` is cleaned up by the
manager's legacy list. The reverse-engineering stays where it belongs -
[zoom-fit-optimization.md](docs/research/zoom-fit-optimization.md) is the
document about the vendor's fit pipeline (§6 why the old flag went dead, §7 the
call site that replaced it), and the sources are in the git history
(manager ≤ 1.7.1).

**gpu_gate** joined the set with manager 1.3.9 (2026-11-19); its full
startup-gate research:
[gpu-gate-amd-support.md](docs/research/gpu-gate-amd-support.md).
**selective_mesh** joined with manager 1.5.0 (2026-11-23, then named
`merge_select`); the common-mesh
selection research (vendor call sites, folder-key design, community
patchset comparison):
[merge-common-mesh.md](docs/research/merge-common-mesh.md).

The next vendor update, **2.1.0-14**, was first checked **offline against the
signed installer payload** (2026-11-27): all 73 anchors the modules use are
equal, the manager stamp gate (`3.1.`) and the `winmm` injection chain are
intact, and the payload ships no file that collides with the mod files. It is
now installed and **live-verified**: every module armed and hooked on the
first start with no code change, and the hub runs normally
([vendor-update-2.1.0-14.md](docs/research/vendor-update-2.1.0-14.md)).
Two things the offline pass could not see, both recorded there: the setup
**removes the mod files from the application folder** (re-run the Mod Manager
after every vendor update - `exstar_mods.state` and a file backup make that
routine), and the fps QML insert is no longer survivable on this build, which
retired the module.

## Mod Manager

`ExStarModManager.exe` is a single self-contained executable (bootstrap,
host and all modules are embedded as resources):

* **GUI** (elevated automatically) - since 1.4.0 a redesigned dark
  interface: every page is owner-drawn (GDI+ shapes, ClearType text),
  animated, no framework dependencies. Three pages: *Interface*
  (status tiles, the module cards, a live activity feed), *Scanner*
  (reserved for the scanner-hub dashboard, roadmap item 9) and
  *Settings* (application folder, *Manager behavior* - activity
  journal and UI zoom -, About, *Restore stock*). Pick the
  application folder, flip the switch on a module card - ON installs
  and enables it, OFF removes it.
  The window scales itself to the screen (system DPI and physical panel
  density are detected at startup; force it with `--zoom 1.5`).
  Modules with tunable behavior show inline controls that write the
  matching ini keys and apply after an EXStar Hub restart:
  *Marker Quality*: a "marker positioning quality" preset row - one
  click
  sets the three coupled green thresholds together (Coarse 0.25 mm /
  Medium 0.1 mm / Fine 0.05 mm / Ultra 0.02 mm spread, with
  matching frames and viewing directions); the active preset is read
  back from the ini. *Popup Dim*: a *dim* slider (how opaque the
  overlay behind popups stays). *Wallpaper*: a *Choose file…* button
  (any jpg/png/bmp is converted to a 1920x1080 JPEG fitted into the
  vendor's wallpaper slot and placed as `modules\wallpaper.jpg`) and a
  *Reset* button (back to the stock wallpaper). Values are stored in
  the module's ini;
  switch states are read back from disk, so the window always shows what
  is actually installed. *Restore stock* removes everything.
* **Manager preferences**: zoom and journal switches persist in
  `%APPDATA%\ExStar\exstar_modmanager.ini`; the activity journal itself
  is `%APPDATA%\ExStar\journal.log` (UTF-8).
* **CLI**:

```text
ExStarModManager.exe --list
ExStarModManager.exe --install --modules marker_quality[,id2] [--app "D:\...\EXStar Hub"]
ExStarModManager.exe --remove  --modules id1,id2
ExStarModManager.exe --restore
ExStarModManager.exe --zoom 1.5        # GUI only: force UI scale 1.0..4.0
ExStarModManager.exe --scanner-wifi  status|fix|rollback [--scanner-ip 192.168.42.3] [--scanner-channels 36,40,44,48] [--scanner-pass ...]
ExStarModManager.exe --scanner-fan   status|apply|stop|restore|probe [--scanner-ip ...]
```

Everything the manager places is recorded in `<app>\exstar_mods.state`, so
*restore stock* deletes exactly its own files - and never touches a foreign
`winmm.dll` (a vendor file there is detected by its version resource and
aborts the install). The `--scanner-*` commands are unrelated to local
module files: they talk to the scanner over its Wi-Fi link
([scanner-wifi-channel-fix.md](docs/scanner-wifi-channel-fix.md),
[scanner-fan-control.md](docs/scanner-fan-control.md)).

## Versioning

`MAJOR.MINOR.PATCH`, the same scheme for the manager and for every module:

* **MAJOR** (first digit) - bumped only on the project owner's explicit
  decision. The line is `0.x` until the first proven release; the first
  proven release is `1.x`. The owner alone decides when to move 1 → 2 → 3.
* **MINOR** (second digit) - a new feature: a new module, a new user-facing
  capability of the manager, or a new capability of an existing module.
* **PATCH** (third digit) - fixes and behavior adjustments only.

Modules follow the same scheme: `0.x` while experimental, `1.0.0` at the
first live-verified ship, then MINOR for new capabilities and PATCH for
fixes. Windows requires a 4-part version resource, so shipped binaries
carry a technical `.0` fourth component (e.g. file `…1.3.10.exe`, resource
`1.3.10.0`); it is not part of the version and never conveys meaning.

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
([docs/research/](docs/research/)):

* [marker-pipeline.md](docs/research/marker-pipeline.md) - the red-marker
  signal and the two interception routes it proved: the `glUniform4fv` hook
  (retired with marker_inframe) and the osg uniforms marker_quality writes
  today;
* [scanpage-qml-resources.md](docs/research/scanpage-qml-resources.md) —
  the vendor's compressed QML (Qt zrc) format, the safe in-memory write
  protocol, `.qm` catalog pitfalls (popup_dim, laser50 `[caption]`; its
  first user was the retired fps);
* [laser-plane-ids.md](docs/research/laser-plane-ids.md) — the
  `set_match_plane_id_vulkan` window and the "50 lines" caption (laser50);
* [zoom-fit-optimization.md](docs/research/zoom-fit-optimization.md) — the
  zoom-optimization mode behind "Zoom to Fit" - the research behind the
  retired fitfill, and with it a map of the vendor's fit pipeline;
* [batch-2026-11-17-status.md](docs/research/batch-2026-11-17-status.md) —
  dated journal of the fps/laser50/fitfill batch (deploys, the caption
  v1.0.0 catalog bug and its fix, module merge, code revision);
* [update-2.1.0-10-migration.md](docs/research/update-2.1.0-10-migration.md)
  — offline-validation + byte-parity methodology for vendor updates, and
  the 2.1.0-10 migration record (zero code changes, one deploy-hygiene
  lesson).

What is shipped and what is planned next (the borrowed-capabilities port
batch): [docs/ROADMAP.md](docs/ROADMAP.md).

## Requirements

* Windows x64, EXStar Hub 2.x (file stamp 3.1.x - see the compatibility
  table below);
* build: Visual Studio 2022 Build Tools, "Desktop development with C++".

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output:

```
build\winmm.dll                   bootstrap proxy
build\ExStarRec.dll               module host
build\modules\marker_quality.dll  modules
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

; Multi-process injection is not a second switch: the Mod Manager
; recomputes [host] sibling_all / sibling_modules from the module
; switches on every install and remove, so a module that is off is
; listed nowhere.  What may appear there at all is a per-module
; capability in the manager table (currently marker_quality and
; selective_mesh, both proven inside scanservice.exe):
; sibling_all = 1
; sibling_modules = marker_quality,selective_mesh

[marker_quality]
enabled = 1
```

`<app dir>\modules\<id>.ini` - per-module options (see module READMEs).
All changes apply at application start.

## Compatibility

The vendor keeps two version numbers: the **product number** from the
uninstall registry key (`DisplayVersion`, shown by the Mod Manager) and
the **file stamp** - the `ProductVersion` of `EXStar Hub.exe`, which the
vendor does not bump per release. The anchors bind to binaries, so the
file stamp is what compatibility actually follows.

| EXStar Hub (product) | file stamp | Tested | Notes |
|---|---|---|---|
| 2.1.0-14 | 3.1.49 | ✅ | installed and live-verified 2026-11-27 (manager 1.7.2): every module armed and hooked on the first start, hub stable, and the one layout only a live merge can prove (`Sn3DMultiProjects` merge item) held; the setup **deletes the mod files** from the app folder, so the Mod Manager has to be run again after an update; **fps** retired on this build (it closed the hub on the scan page) and **fitfill** retired the same day at the owner's request |
| 2.1.0-10 | 3.1.49 | ✅ | Windows 11 x64, NVIDIA driver; full re-verification 2026-11-18; gpu_gate self-test 2026-11-19 |
| 2.0.0-45 | 3.1.49 | ✅ | the original research target (same stamp family); the gpu_gate relaxed AMD branch was field-verified here on a Radeon 780M |
| other 2.x / other stamps | - | ? | re-check research anchors before deploying |

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
