# Roadmap

Standing work-plan document. Lifecycle: feature card → status
`planned → implementing → tested → shipped`. After a feature is closed, its card
collapses into one line under "Done" (details live in the module's README).
Porting knowledge: private external RE-base (not published,
not included in git).

## Rules (reminder, always in force)

- Never push to GitHub without live verification and an explicit "put it in git".
- Injection only: winmm proxy → host → modules; vendor files on disk
  are never patched; rollback = delete the added files; DllMain always TRUE;
  SEH around foreign pointers; no hardcoded addresses/RVAs —
  patterns/names only; a module stays silent when an anchor fails, it does not crash.
- Changes to QML walkers — only through `tools\dim_dryrun.c` (offline
  run over the DLL before launching the application).
- Vendor-update deadline procedure: first offline anchor validation
  (`zrc_lab`/`dim_dryrun` over the update files), then install.
- Deploy into Program Files only via `Start-Process -Verb RunAs`.
- Versioning is `MAJOR.MINOR.PATCH` (same for the manager and every
  module): MAJOR is bumped only on the owner's explicit decision and is
  `0` until the first proven release; MINOR = a new feature/capability;
  PATCH = a fix. Full rule in README § Versioning.

## Shipped

| Module | Version | What it does |
|---|---|---|
| popup_dim | 1.2.8 | gray veil → frosted glass, dims the veils |
| marker_quality | 1.0.2 | marker color by observation quality (latch-green) |
| marker_inframe | 1.1.0 | in-frame marker grows instead of a red flash |
| wallpaper | 1.0.0 | custom app background (in-memory image swap) |
| fps | 1.0.0 | real scan-page UI frame rate readout |
| laser50 | 1.1.0 | 25+25 laser lines (plane-id remap) + `50 lines` caption |
| fitfill | 1.0.0 | Zoom to Fit frames the whole model (zoom-optimization off) |
| gpu_gate | 1.0.0 | integrated GPUs pass the startup GPU check (one memory byte) |
| ExStarModManager | 1.3.9 | manager: enabling modules, installing/updating payload |

## Batch "porting borrowed features" (2026-11-14) — CLOSED

All five cards shipped 2026-11-17 and re-verified live on vendor update
2.1.0-10 on 2026-11-18 (manager 1.3.8/1.3.9). Cards kept below as the
historical record.

Knowledge source: private external RE-base (`STATE.md`, a research
journal, `patches-history.md` — his patches are version-locked and died
in the update —
we port the logic, not the bytes). Work order: 1 → 2 → 3 → 4 → 5.

### 1. wallpaper — custom application background (module + button in the manager)

**Goal.** The user clicks "App background" in the manager, drops in any
jpg/png — the application gets its own background instead of the stock one. "Reset" — factory default.

**Known (verified 2026-11-14 on 2.x (file stamp 3.1.x)):** the image sits uncompressed in
`.rdata` of AppUi.dll, `FF D8 FF` @file offset 0x643F95, size 837,517 B,
1920×1080 baseline JPEG 24bpp. Resource name `MainBackground.jpg`
(the plain string is absent from the DLL — the reference lives inside the compressed qrc).

**Mechanism.** Manager: pick a file → convert to 1920×1080 JPEG → auto-
tune quality to fit the 837,517 B budget → write `modules\wallpaper.jpg`
(+ `.meta` with the original resolution). Module (a popup_dim extension or a
separate `wallpaper`): on `LdrRegisterDllNotification` for AppUi.dll
find the SOI…EOI block the same way the dry-run harness does and substitute
the bytes in memory under VirtualProtect; if there is no user file —
we do nothing. Budget: the new image ≤ the original (guaranteed by
the manager's converter).

**Verification.** dry-run mode (prints offset/size without launching) →
live run with a screenshot → "Reset" restores the factory look.

Status: ✅ done and verified live (2026-11-14): module `wallpaper`
v1.0.0 (`modules/wallpaper/`, README artifact written), dry-run
`tools\wp_dryrun.c` → `MATCH known-good`; live run: `slot +6572949
x 837517 B <- 55996 B image (ok)` + screenshot with the gradient; deleting
the file → the module declines, the stock background comes back; idles
in sibling processes (whitelist + gate `exe = hub`). Manager 1.3.1:
"Wallpaper" card with "Choose file…"/"Reset" buttons (GDI+ conversion,
quality 95→26 to fit the budget). `.meta` dropped — not needed. Remaining:
a human run-through of the manager GUI (install/choose file in one click).

### 2. fps — real FPS in ScanPage

**Goal.** The stock "FPS: 80-90" figure is not truthful: it is capture-pipeline
statistics from the C++ model `real_time_info` (the string `FPS: %1` in AppCommon_E7.dll),
not the UI frame rate.

**Friend's knowledge.** His `displayFps` timer overrode the stock label;
later — a `QString::number` hook with fixed call-site RVAs; final form —
a single line `FPS: N` at the bottom right (right 62, bottom 56). The RVA approach
is version-locked, we do not repeat it.

**Implementation.** `modules/fps` (v1.0.0): at ScanPage_E7.dll map time,
unpack the qrc record anchored on `/// ScanPage_E7`, insert `Text { id: exFpsLabel }`
after the `id: root` line — a frame counter on
`Connections { target: window; function onFrameSwapped() }` (window is an
engine context property, confirmed: `window.openMessageBox` in the same file) + a 1 s Timer. Re-deflate level 9 must fit into the original
stream length (the next stream's boundary does not move), rawSize
is rewritten, the result is control-unpacked from the image and
compared byte for byte; if it does not match — the byte copy of the stream
is restored. Anchors are text-only.

**Verification.** Dry run `tools\replace_lab.exe <dll> "/// ScanPage_E7"
"id: root\r\n".bin <same text + snippet>.bin` (FITS: stock 22392, patched
22610 ≤ 22753) matched the live log byte for byte: `raw 137735->138526,
packed 22753<-22610 (ok)` (2.x (file stamp 3.1.x), 2026-11-14/17). As a side effect: the host's
ini parser learned to handle quotes (`color = "#8ecbff"`), otherwise `#` cut
the value as a comment.

Status: ✅ verified live (2026-11-17, user: "I can see the fps").
The qrc-editing machinery was extracted into `include\exstar_qrc.h` (shared with
laser50 [caption]). Into git — upon "put it in git".

### 3. laser50 — 25×25 laser lines + "50 lines" caption (merged with item 4)

**Goal.** Widen the line range: `(3,21,3,21)` → `(0,24,0,24)`;
the cloud includes all 25+25 lines.

**Friend's knowledge.** `algorithmLzy.dll`, function `set_match_plane_id_vulkan`;
his was a static edit of values in the calling module's data, never
verified live, tied to a version.

**RE (2.x (file stamp 3.1.x), algorithmLzy 2.1.908.63).** The export
`?set_match_plane_id_vulkan@Sn3DAlgorithm@@YA?AW4RetVal@1@PEAXAEBH111@Z`
is unique (there is also `..._opencl`, imported by nobody); the body — logging and a
tail call to `FUN_1804b24b0`, which writes four ints to
`ctx+0x25534/38/3C/40`. The range is set by **the caller**: the only
importer in the directory — `SnAlgAppPlat.dll`, loaded dynamically (no
EXE links it) — the process host is identified via the module's log.

**Implementation.** `modules/laser50` (v1.1.0, caption merged in): a detour
via `host->hook` on the export (found by export NAME through the shared
`include\exstar_export.h`, no patterns/RVAs needed); on an exact
match of the arguments with `from_* = (3,21,3,21)` the original is called with
pointers to our copy `to_* = (0,24,0,24)`; other calls
(7-line mode) pass through transparently. strict + a log of the first N calls =
the verification trace. `needsSiblings=TRUE`: the module lives in the pipeline's
processes, the manager keeps the sibling whitelist in sync; the second
half [caption] (see item 4) lives in the UI process.

**Verification.** Offline: the export found exactly once, the value flow
confirmed by decompilation (Ghidra). Live (2026-11-17, scanner): log
`laser50: call N: (3,21,3,21) -> remap` + the user confirmed
"25×25 lines there". Rollback: `enabled=0`.

Status: ✅ verified live (v1.1.0 = lines + caption, 2.x (file stamp 3.1.x), 2026-11-17).
Into git — upon "put it in git".

### 4. captions — laser mode captions → MERGED with item 3 (laser50 v1.1.0)

There is no separate module anymore: the "50" caption is meaningless without 25×25 and
vice versa, so the feature was folded into `modules/laser50` as
the [caption] half (sub-switch `captions = 1` in laser50.ini).
The knowledge and lessons were moved into `modules/laser50/README.md`:

- **RE (2.x (file stamp 3.1.x)).** The qrc record `ScanSettingModelProviderLaser` in
  ScanPage_E7.dll (@file 0x98C90E): `qsTr("%1线").arg(
  $scanSettingModelLaser.crossLaserLineShowNum)` (=39) and its twin
  `parallelLaserLineShowNum` (=7); the Chinese characters are the translatable SOURCE
  (the .qm catalog yields "%1 lines" in EN).
- **Implementation.** Only the argument is edited: `.arg(...crossLaser...Num)`
  → `.arg(50)`; the catalog key stays intact → the UI language is preserved. Content gate:
  anchor×1 && pattern×cap_expected; re-deflate ≤ original length
  (9587 ≤ 9777).
- **Lesson from v1.0.0 (do not repeat).** Replacing the `qsTr("%1线")` literal itself
  breaks the catalog lookup and dumps raw text into all locales
  (the English UI showed the Russian "lines" caption). Rule: do not reword the
  translatable source — change only the value being formatted.
- **Verification.** Live (2026-11-17): `laser50: caption ... 1
  caption(s) rewritten (ok)` + screenshot `50 lines` / `7 lines`.

Status: ✅ verified live inside laser50 v1.1.0 (2026-11-17).

### 5. fitfill — Zoom to Fit filling the whole frame

**Goal.** Ctrl+D / "Zoom to Fit" fills the frame by width/height,
a repeat press correctly re-fits.

**Friend's knowledge.** The chain QML → `Sn3DCanvasView::fitView` →
`FitOperationAuto` — a false lead: the constants 3.5/0.25 had no effect on the
menu item (verified live). His final edit — the byte `01→00` in
`librenderkit.extensions.digit.dll` (file offset 0x171D68);
he never determined its semantics, and the offset is version-dependent.

**RE (2.x (file stamp 3.1.x)) — semantics found.** In the current file the same byte sits at the
same place (pattern `B2 01 48 8B C8 FF 15 96 FA 0C 00`, file↔RVA delta
0xC00, RVA 0x172967, inside `Sn3DCanvasView::init` @0x172070):
`mov r10b, 1` — an argument of the call to
`CustomizeManipulator::setIsZoomOptimization(bool)`, an export of
`librenderkit.dll` (`?setIsZoomOptimization@CustomizeManipulator@@QEAAX_N@Z`,
the import disassembled from the import table). The setter = `this[0x138] = flag`
(Ghidra). So the vendor turns on "zoom optimization" (a fixed camera
distance) when creating the view — when disabled, fit fits the model
entirely (which is exactly what the friend got with his byte).

**Implementation.** `modules/fitfill` (v1.0.0): a detour via `host->hook` on
the exported setter (found by export name, no RVAs needed),
forwards `force` from the ini (default 0 = mode off), a log of the first N
calls = the trace. Covers all call sites (not a single one), version-stable
by name. `needsSiblings=TRUE` (3D views are also rendered in
sibling processes).

**Risk.** The optimization mode is the vendor default; globally turning it
off may change how interactive zooming feels (it lived in the friend's final
edit, no complaints). Rollback `force = 1` / `enabled = 0`.

**Verification.** Live (2026-11-17): `fitfill: hooked ... forcing value 0`
+ `setIsZoomOptimization(1) -> forwarding 0` in the log; the user
confirmed: Ctrl+D fits the model into the frame.

Status: ✅ verified live (v1.0.0, 2026-11-17). Into git — upon "put it in
git".

## Batch "GPU independence" (2026-11-18)

### 7. gpu_gate — integrated GPUs pass the startup check

Status: ✅ shipped 2026-11-19 (module `gpu_gate` v1.0.0, manager 1.3.9).
Mechanism, pattern anchor and safety matrix: `modules/gpu_gate/README.md`;
full startup-gate research: `docs/research/gpu-gate-amd-support.md`.
Verification: unique pattern hit + gate relaxed in both loading processes
on 2.1.0-10 (NVIDIA self-test 2026-11-19, vendor flow byte-identical:
`init_speckle gpu test result: 1` / hub `errorCode 1`); the relaxed AMD
branch is field-verified on 2.0.0-45 on a Radeon 780M. Final hardware
validation on the iGPU-only box (planned Radeon 780M PC) is scheduled for
that hardware - logs to check: `gate relaxed`, `gpu test result: 1`,
`[AMDJPEG] ... initialized` vs nvJPEG fallback.

Rejected inside this batch:
- Intel iGPU / Qualcomm Adreno unlock: would require forcing the vendor's
  second-stage "neither AMD nor NVIDIA" error into a fabricated success;
  scan quality on such devices unknown - refused without demand + research.
- VIRTUAL_GPU / CPU Vulkan unlock: same fabricated-success category; the
  speckle pipeline on a CPU Vulkan device is untested and pointlessly slow.

## Batch "scanner hub" (2026-11-19) — STARTING

A new **Scanner tab in the Mod Manager** for everything that talks to the
device itself (fan, home-network, live status, logs). Full research, the
vendor command surface, the feature matrix and a live-verification
checklist: [scanner-hub-module.md](research/scanner-hub-module.md).

### 8. Fan control — quiet the scanner

Status: research complete (vendor telemetry path + device-side control
surface understood), **no code**. Pending from the collaborator: the
pseudo-PID recipe (anchors in `app_tx2`, the ARM64 blob, hook points,
Kp/Ki/Kd defaults, thresholds) — we will not guess into a running
process.

Agreed shape: a status card (duty / RPM / CPU+projector temperature,
`FANBLOCKED` alert) plus an **in-memory** patch of the running `app_tx2`
- version gate by device-binary hash before applying, refuse while a scan
is active, reboot = rollback, zero writes to the device filesystem.
Read-only or coarse-duty variants first, since both are already available
through the vendor telemetry channel (`RWFanActualSpeed`, `threshold`,
`RWFanSpeed`) with no patching at all.

### 9. Home-network (STA) assistant + dashboard + log collector

Status: research complete, **no code**. Key facts (details in
[scanner-hub-module.md](research/scanner-hub-module.md)):

- the scanner's AP→STA capability is vendor firmware (key daemon +
  vendor AP daemon + vendor `WAPControl`/`APReboot`/`SelectWIFIChannel`/
  `SEGIP1..4` commands); a collaborator only added the missing
  PC-side entry points;
- **the stock hub does NOT see a STA scanner**: `TX2Plugin` dials a
  hardcoded `192.168.42.3` and its Wi-Fi UI makes the *PC* join the
  *scanner's* hotspot. The vendor's own model keeps the scanner address
  constant (`SEGIP`), i.e. STA is meant for networks where that address
  can be assigned — which is why a router with a wider mask makes it work
  with nothing patched anywhere.
- Dashboard is mostly a display problem: the hub already receives
  `temp`/`batteryPower`/`bCharge`/disk info plus `ZCPUTEMP`/`ZGIRDERTEMP`/
  `ZBATTERYCUR`/`ZBATTERYVTG`/`WIFIQUALITY` on the PC.
- Next input needed: netstat capture of hub→scanner ports during a Wi-Fi
  session (only for the PC-bridge option), and one live device pass over
  the verification checklist.

## Open regressions

| Item | Since | Status |
|---|---|---|
| fitfill has no effect on 2.1.0-10 | vendor update 2.1.0-10 | diagnosed 2026-11-19: the flag it forces is dead code on the new fit pipeline (analysis: [zoom-fit-optimization.md §6](research/zoom-fit-optimization.md)). Module stays enabled (harmless no-op). TODO: decompile the 2.1.0-10 fit chain (`Sn3DCanvasView::fitView` / `UpdateCameraOperation` / `FitOperationAuto`) and find the new distance clamp - the old `3.5` constants are the first landmarks. |
| manager window was fixed-size | before 1.4.0 | fixed in manager 1.4.0: `WS_OVERLAPPEDWINDOW` (resizable, minimize, maximize), `WM_SIZE` re-layout of width-dependent controls, `WM_GETMINMAXINFO` floor. Needs user visual confirmation. |

## Rejected

- **6. English keyboard layout on the pre-start screen** (PrestartCheck.dll) —
  the user does not need it (decision 2026-11-14).
- Contour during supplemental scanning (the unstable setFrameMatrix chain, the friend
  himself rolled it back) — not taken without an explicit need.
- Bypassing the resolution check on import, forcing Mesh Editing's isEnabled
  (breaks the left panel), edition/terminology cosmetics.
- The hardware part of the TX2 (Wi-Fi/IP/MAC/buttons/web portal) — out of the framework's
  scope, remains knowledge in the private base.

## External circumstances

- **A vendor update is available, installation postponed** (decision
  2026-11-14). Before installing: unpack the update, run
  `zrc_lab`/`dim_dryrun` over the new DLLs, fix the anchors offline.
- All batch anchors are pattern/name-based; breakage on update is ruled out
  by the architecture, only a silent patch skip is possible.
