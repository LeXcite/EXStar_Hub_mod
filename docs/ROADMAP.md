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
| fitfill | 1.1.0 | Zoom to Fit frames the whole model (zoom-optimization off; on 2.1.0-10 forces scan scale on at its call site) |
| gpu_gate | 1.0.0 | integrated GPUs pass the startup GPU check (one memory byte) |
| merge_select | 1.2.0 | per-scan include switch for the common mesh (gate identity + eye-icon watch, record-only) |
| ExStarModManager | 1.5.0 | manager: enabling modules, installing/updating payload, resizable window; 1.4.0 = redesigned owner-drawn dark UI (Interface/Scanner/Settings pages, custom dark scrollbar, no native chrome) |

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

Status: **implemented CLI-side 2026-11-21** (`--scanner-fan
status|apply|stop|restore|probe`), awaiting live tests on the device.
The collaborator's recipe was recovered by decompiling his r6.1
installer (class `FanBinary`): full pseudo-PID analysis in the private
research artifacts. His modes patch the on-disk `app_tx2` (writable
rootfs + reboot) — **not** our policy; ours is memory-only:

- `shell` (default): PID loop as a /tmp shell script — reads
  `thermal_zone0/temp`, writes `/dev/hwmon/pwm1`; works on every build,
  `stop`/reboot = rollback.
- `memhook`: the A64 in-process loop (hook + code cave) applied via
  `/proc/<pid>/mem` only, gated by the exact factory profile (size + 7
  anchor words + PT_LOAD coverage + writable-segment check), RAM
  snapshot before writing, read-back verify + auto-rollback, `restore`
  replays the snapshot. Refuses any build that does not match.
- `probe`: read-only profile report for deriving gates on new firmware
  (ELF layout, anchor window, candidate hook words, segment perms).

User docs: [scanner-fan-control.md](scanner-fan-control.md).
Remaining before "tested": live run of `probe`, calibration of the
temperature scale / reload register questions noted in the research
artifact, then `apply` on the device; UI card comes after the CLI is
verified (manager Scanner tab, card 10).

### 8a. Scanner Wi-Fi visibility fix — `--scanner-wifi`

Status: **implemented CLI-side 2026-11-20/21**. One-click repair of
"phone does not see the scanner AP" (vendor regulatory table ships
5.8 GHz channels for KZ/RU/CN while local phones may not scan there —
real incident: AP on ch153). `status|fix|rollback`: idempotent
narrowing of the device's own country row (channels ≤ 64), mandatory
factory backup under `%LOCALAPPDATA%\ExStarHubMods\scanner\`, AP
restart through the vendor's own init script, verified afterwards.
User docs: [scanner-wifi-channel-fix.md](scanner-wifi-channel-fix.md);
standalone equivalent `tools\scanner-wifi-fix.ps1`. The user's device
was fixed under a one-time approved exception; the tool keeps it
repairable after firmware updates.

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

## Batch "manager UI" (2026-11-21 → 22) — CLOSED

Redesigned the whole manager GUI to the approved HTML mockup, shipped as
manager 1.4.0. Pure C/Win32: every page is owner-drawn (GDI+ shapes,
GDI ClearType text, double-buffered, per-frame alpha fades), the only
real child control is the app-folder EDIT.

* Pages: **Interface** (status tiles, module cards, live activity feed),
  **Scanner** (placeholder for item 9), **Settings** (folder, *Manager
  behavior*: journal toggle + zoom slider persisted in
  `%APPDATA%\ExStar\exstar_modmanager.ini`; journal log
  `%APPDATA%\ExStar\journal.log`; About; Restore stock).
* No icon fonts/tiles anywhere (typographic wordmark and card heads),
  no hover displacement (hover only warms border/shadow), custom dark
  overlay scroll thumb and no native scrollbar at any window height.
* Fixes found live: switch activation dead (`ReleaseCapture()` ran
  before the hit test and cleared the press state); a NULL deref in the
  preset band (redesign dropped child-window creation, so control
  descriptors were never seeded - now `uiSeedControls()`); worker-thread
  `MessageBoxW` could hang a job behind an invisible dialog (warnings
  now travel through the window queue and are shown by the UI thread).
* `src/manager.c` is pure ASCII: all UI glyphs (`× → « » µ …`) are
  `\uXXXX` escapes - the file cannot be mojibake'd by an editor or a
  build again (an earlier CP1251 round-trip had baked `25Г—25` into the
  Laser title).
* CLI (`--install/--remove/--restore/--scanner-*`) behavior unchanged;
  version resources carry 1.4.0.

## Batch "merge selection" (2026-11-23) — CLOSED

Feature: choose which scans of a project participate in the **common
mesh** without deleting anything. Shipped as module `merge_select`
v1.0.0 + manager 1.5.0.

* Reverse-engineered the vendor selection: the whole product asks the
  single exported question `Sn3DFile!HandlePrjFile::isGenerateCloud`,
  imported only by `Sn3DMultiProjects.dll`; its common-mesh collection
  asks once per scan and a false answer takes the stock "no cloud"
  skip branch. Full evidence chain (call-site RVAs, folder-key design,
  community-patchset comparison):
  [merge-common-mesh.md](research/merge-common-mesh.md).
* The module is one export detour (no RVAs, no disk patches): folder
  path via the exported `getFolderPath()`, per-scan `included = 0`
  sections in a plain `merge-select.ini`, an auto-written
  `merge-select.map.txt` roster, answers steered **only** for
  Sn3DMultiProjects callers and **only** towards exclusion; every
  risky step SEH-guarded and fail-open.
* Verified 2026-11-23: module arms and hooks `isGenerateCloud` in the
  running hub (mod log); install made single-threaded after a startup
  race was caught in the self-test. Pending: one live multi-scan mesh
  run to confirm the excluded scan is gone from the merged result.

## Batch "eye-bound mesh selection" (2026-11-24) — IN PROGRESS

Goal (user decision): the selection UI is the hub's own **eye icons** -
a hidden scan row must not enter the common mesh. No manager clicks, no
ini editing during work.

* Live diagnosis: the scan-page "Меш" flow never queries
  `isGenerateCloud` (it applies visibility itself), a cached mesh is
  re-shown without any gate calls, and live gate objects return empty
  from *both* exported name getters. Full analysis:
  [merge-common-mesh.md §6](research/merge-common-mesh.md).
* merge_select 1.1.x/1.2.0: identity now also read as the embedded
  QString the vendor itself uses in `getProjInfo` (`**self+0x18 → +8`),
  one-shot `idmap` fallback dump; caller probes mapped who drives the
  mesh path; **eye watch** finds `ProjectListControllerE7`'s
  QMetaObject at runtime (no RVAs), verifies method 8 =
  `setNodeVisible(QUuid, bool)`, records `uuid -> visible` - the exact
  signal the future gate will consume.
* Stability: the scanservice abort (vendor error `0x00000012`) was
  traced to running six modules inside sibling processes; reverted, and
  manager 1.6.1 refuses to enable sibling mode until modules are proven
  there one by one. `merge_select` needs the UI process only.
* Next (needs live sessions): correlate eye uuids with gate scan names
  (`Sn3DMultiProjects!Sn3DProjectsManager::getProjUuidList` etc.), then
  answer `false` for hidden scans on mesh rebuilds.

## Open regressions

| Item | Since | Status |
|---|---|---|
| fitfill has no effect on 2.1.0-10 | vendor update 2.1.0-10 | diagnosed 2026-11-19: the flag it forces is dead code on the new fit pipeline (analysis: [zoom-fit-optimization.md §6](research/zoom-fit-optimization.md)); replacement anchor found the same day - the `setEnableScanScale(false)` call site in `sn3d.digit.scenecontrol.dll` ([§7](research/zoom-fit-optimization.md)), implemented in fitfill 1.1.0 (import-name-anchored in-memory byte patch). TODO: live confirmation on the hub (log line `scan scale forced to 1 at sn3d.digit.scenecontrol.dll+0x...` + Ctrl+D re-fills the frame). |
| manager window was fixed-size | before 1.3.10 | fixed in manager 1.3.10: `WS_OVERLAPPEDWINDOW` (resizable, minimize, maximize), `WM_SIZE` re-layout of width-dependent controls, `WM_GETMINMAXINFO` floor. Visually confirmed 2026-11-22 during the 1.4.0 UI redesign (any height re-flows the grid; the stock white scrollbar that surfaced at short heights is gone - the only scroll thumb is the custom dark overlay). |

## Rejected

- **6. English keyboard layout on the pre-start screen** (PrestartCheck.dll) —
  the user does not need it (decision 2026-11-14).
- Contour during supplemental scanning (the unstable setFrameMatrix chain, the friend
  himself rolled it back) — not taken without an explicit need.
- Bypassing the resolution check on import, forcing Mesh Editing's isEnabled
  (breaks the left panel), edition/terminology cosmetics.
- The hardware part of the TX2 (Wi-Fi/IP/MAC/buttons/web portal) — was
  out of the framework's scope; scope revised 2026-11-20/21: device
  tools now exist as manager CLI (`--scanner-wifi`, `--scanner-fan`),
  still injection/memory-only with reboot rollback, no vendor-file
  patching (the one country-table exception was a user-approved
  one-time repair).

## External circumstances

- **A vendor update is available, installation postponed** (decision
  2026-11-14). Before installing: unpack the update, run
  `zrc_lab`/`dim_dryrun` over the new DLLs, fix the anchors offline.
- All batch anchors are pattern/name-based; breakage on update is ruled out
  by the architecture, only a silent patch skip is possible.
