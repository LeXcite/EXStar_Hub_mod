# Roadmap

Standing work-plan document. Lifecycle: feature card → status
`planned → implementing → tested → shipped`. After a feature is closed, its card
collapses into one line under "Done" (details live in the module's README).
Porting knowledge: a community patchset (not published,
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
| selective_mesh | 1.8.0 | eye-icon driven common-mesh selection (session ini keyed by scan uuid and shared across processes, meta-object eye watch, hook allowlists); 1.5.1+ reads command arguments as pure memory (no Qt calls in the hook), 1.5.2-1.5.5 use the `QStringList` layout measured against the shipped Qt (`tools/qlist_lab.c`); 1.5.6 reads nested command values and proved the viewport half has nothing to intercept (see 2026-11-25 below); 1.5.8 kept a hidden scan hidden on screen across the environment switch - that re-apply hangs the hub, so it is off by default since 1.6.1; 1.6.0 = renamed Selective Mesh; 1.6.2 = the map walk of a watched scene command is hard-bounded (it could spin forever on a non-map argument and hang the hub) - live-verified 2026-11-26; 1.7.0 = a merge this module would leave empty runs as if all eyes were open, reading the merge's own scan list out of the caller's registers (`merge_list.h`, proven by `tools/merge_list_lab.c`); 1.7.1 = that guard accepts the register block on its content, not on an assumed register name (the first live run falsified `rcx == self`); 1.7.2 = `RegCtx` field names match the measured block order - live-verified 2026-11-26 (all eyes hidden -> full mesh, one eye shown -> only visible scans); 1.8.0 = the eye is the only interface: folder/name identity, the `.map.txt` roster, the `merge-select.ini` migration and the vendor `QString` getters are gone, exclusions are dropped when the hub starts (a fresh session shows every scan, like the vendor itself) |
| ExStarModManager | 1.7.0 | manager: enabling modules, installing/updating payload, resizable window; 1.4.0 = redesigned owner-drawn dark UI (Interface/Scanner/Settings pages, custom dark scrollbar, no native chrome); 1.6.4+ never rewrites the sibling whitelist (one exception: 1.6.9 renames the `merge_select` token of a hand-made list, never adds one); 1.6.9 removes the legacy `merge_select` files and carries its settings to `selective_mesh`; 1.7.0 drops the Selective Mesh "Scan roster" / "Open folder" buttons and the roster window (the eye in the hub is the module's only interface) |

## Batch "porting borrowed features" (2026-11-14) — CLOSED

All five cards shipped 2026-11-17 and re-verified live on vendor update
2.1.0-10 on 2026-11-18 (manager 1.3.8/1.3.9). Cards kept below as the
historical record.

Knowledge source: a community patchset (`STATE.md`, a research
journal, `patches-history.md` — its patches are version-locked and died
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

Knowledge from the community patchset. Its `displayFps` timer overrode the stock label;
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

Knowledge from the community patchset. `algorithmLzy.dll`, function `set_match_plane_id_vulkan`;
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

Knowledge from the community patchset. The chain QML → `Sn3DCanvasView::fitView` →
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
entirely (which is exactly what the patchset author got with that byte).

**Implementation.** `modules/fitfill` (v1.0.0): a detour via `host->hook` on
the exported setter (found by export name, no RVAs needed),
forwards `force` from the ini (default 0 = mode off), a log of the first N
calls = the trace. Covers all call sites (not a single one), version-stable
by name. `needsSiblings=TRUE` (3D views are also rendered in
sibling processes).

**Risk.** The optimization mode is the vendor default; globally turning it
off may change how interactive zooming feels (it lived in the community patchset's final
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
  build again (an earlier CP1251 round-trip had baked mojibake into the
  Laser title).
* CLI (`--install/--remove/--restore/--scanner-*`) behavior unchanged;
  version resources carry 1.4.0.

## Batch "merge selection" (2026-11-23) — CLOSED

Feature: choose which scans of a project participate in the **common
mesh** without deleting anything. Shipped as module `merge_select`
v1.0.0 + manager 1.5.0 (since 1.6.0 this module is named
`selective_mesh`, Selective Mesh - see the rename note at the end of the
next batch; the names below are what shipped at the time).

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

## Batch "eye-bound mesh selection" (2026-11-24) — CLOSED BOTH HALVES (2026-11-26)

Goal (user decision): the selection UI is the hub's own **eye icons** -
a hidden scan row must not enter the common mesh. No manager clicks, no
ini editing during work.

Status: the merge half is live and user-verified repeatedly (1.4.7, and
again with 1.5.6 on 2026-11-25): hidden eye rows are persisted as
`[uuid] included = 0` and both merge loops in `scanservice.exe` answer
`SKIP`, so the final mesh contains only the visible scans, on every press
and across restarts. The viewport half was closed out as "nothing to
intercept" on 2026-11-25 (1.5.6 evidence, see below) and then **solved
differently on 2026-11-26**: option C - re-issue the eyes after the
environment switch - is live-verified in 1.5.8, so a hidden scan now also
stays hidden on screen. Evidence:
[merge-common-mesh.md §§11-18](research/merge-common-mesh.md).

* Live diagnosis: the scan-page "Mesh" flow never queries
  `isGenerateCloud` (it applies visibility itself), a cached mesh is
  re-shown without any gate calls, and live gate objects return empty
  from *both* exported name getters. Full analysis:
  [merge-common-mesh.md §6](research/merge-common-mesh.md).
  **Corrected 1.4.6:** the Mesh flow does query the gate, but from the
  post-mesh collection in `scanservice.exe`
  (`getMeshData`, `ra=Sn3DMultiProjects+0x7E3BB`); the earlier negative
  reading came from runs without the module in that process.
* selective_mesh (then named merge_select) 1.1.x/1.2.0: identity now also read as the embedded
  QString the vendor itself uses in `getProjInfo` (`**self+0x18 → +8`),
  one-shot `idmap` fallback dump; caller probes mapped who drives the
  mesh path; **eye watch** finds `ProjectListControllerE7`'s
  QMetaObject at runtime (no RVAs), verifies method 8 =
  `setNodeVisible(QUuid, bool)`, records `uuid -> visible` - the exact
  signal the future gate will consume.
* Stability: the scanservice abort (vendor error `0x00000012`) was
  traced to running six modules inside sibling processes; reverted, and
  manager 1.6.1 refuses to enable sibling mode until modules are proven
  there one by one. `selective_mesh` needs the UI process only (the eye
  watch does; the gate half runs in the siblings).
* Shipped in 1.3.0/1.4.0: the gate reads the scan `QUuid` straight from
  the `HandlePrjFile` object (offset +0x38, the exact bytes the
  community patchset keys its uuid sections with). Live diagnosis on
  2026-11-24 showed the scan-page "Mesh Model" never reaches the gate
  from the UI process - that merge runs in the **fusion engine**
  (`Sn3DprocessManager.exe`), so eye toggles are now persisted by the
  hub into `merge-select.ini` (the same file, named
  `selective-mesh.ini` since 1.6.0) as `[uuid] included = 0/1` sections
  (1.4.0) and consulted by the gate in every process: hidden rows are
  left out of both the saved-project and the engine-built mesh, and
  the state survives restarts. `eye_gate = 0` restores stock
  behaviour.
* Sibling mode: only this module (then `merge_select`, now
  `selective_mesh`) is proven inside the sibling
  processes (it survived a full night there; the six-module config
  killed `scanservice.exe`). Since manager 1.6.4 the manager never
  rewrites `sibling_all`/`sibling_modules` at all - the proven pair
  `sibling_all = 1` / `sibling_modules = selective_mesh` is set by hand
  and left alone (manager 1.6.9 renames an existing `merge_select` token
  to the new id and nothing else), and every future sibling module needs
  its own live proof first.
* 1.4.6 (gate finally live): three defects of the 1.4.3-1.4.5 chain were
  found and fixed - the detour compared `_ReturnAddress()` (the address
  inside the hand-built thunk, never inside the vendor image), so every
  call took the "outside the merge code" branch and no decision was ever
  applied; the thunk did not reserve the x64 shadow space before calling
  the C helper; and the wrapper capture now receives the caller's real
  return address. Hooks are restricted by `gate_exes`/`eyewatch_exes`
  (unmatched hosts keep an inert module) and the decision half runs
  under `__try/__except` after `scanservice.exe` died executing address
  1 with an earlier build.
* 1.4.7 (user-verified twice): the wrapper QUuid member is already the
  canonical struct order - converting it to wire order produced non-v4
  uuids (`F54E`/`6049`/`1D4D`), so neither the ini section nor the eye
  map could ever match. Live: `eye … -> hidden` writes
  `[uuid] included = 0`, and the next Mesh Model run logs
  `gate … excluded in .ini -> SKIP` for both merge loops
  (`ra=+0x7E3BB` collection, `ra=+0x83097` processing). The final mesh
  contained only the scan left visible; project folders untouched.
* 1.5.0 (viewport): the force-show is not a tree call at all. The scene
  is driven by **named commands** registered inside
  `sn3d.digit.scenecontrol.dll`; a hooked handler logged
  `SetModelsVisible names=[Project_2] reset=0 visible=0` and, with
  several projects open, `names=[Project_1111, Project_2, Project_1000]
  reset=1 visible=1` - "show every project again". The module locates
  that handler by anchoring on the command *name* in the plugin (no
  RVAs), reads the `QVariantMap` with the vendor's own container layout,
  and does not pass on a show-several-at-once request while one of the
  named models is hidden - so the viewport is never touched and nothing
  has to be re-hidden (no timers: that approach was rejected by the
  user). Hidden models are remembered by a hash of the model name, in
  RAM; the scene names models while the project tree uses QUuids, and
  the merge gate keeps using the `[uuid]` sections. Analysis:
  [merge-common-mesh.md §8](research/merge-common-mesh.md). Switches:
  `scene_watch` (log), `scene_filter` (act).
  **Those two log lines do not belong to an eye click or to a Mesh Model
  press** - see the 2026-11-25 entry; the filter built on them has never
  had anything to withhold.
* 1.5.1-1.5.6 (hardening the read path): the argument readers became
  pure memory inspection (1.5.0 handed a self-derived pointer to
  `QVariant::typeName`/`toBool` and the hub faulted in `Qt5Core.dll` at
  start-up - five dumps), the detour forwards all eight Microsoft x64
  slots instead of four (`SetCurrentEnv` keeps its map beyond the
  registers), the `QStringList` layout was measured against the shipped
  Qt with the shipping reader (`qt_read.h`, `tools/qlist_lab.c`,
  `tools/qlist_one.c`), and 1.5.6 walks nested values and reports the
  layout it used. No crash dump since.
* **2026-11-25, viewport closed out.** The live check (1.5.6, one
  project, three scans, eyes hidden, four presses) showed neither
  `SetModelsVisible` nor any model-naming command near a press: an eye
  click sends two `SetModelNodesVisible` (node keys of one model on one
  view), a press sends `SetCurrentEnv` twice around the merge, and the
  gate excluded both hidden scans every time. An offline enumeration of
  the plugin's registration blocks (`tools/scmd_enum.ps1`) lists **43
  commands**, of which exactly two touch model visibility and none keeps
  hidden models hidden; `keep_hide` turned out to be a
  cut-plane/`CutFaceSet` flag written to a private field, and what the
  scene says about visibility (`mainview_model_visible_change
  {cmd, view, show[], hide[]}`) is an outbound status report, not a
  switch. `onRecvSetCurrentEnv` even defers its own apply through
  `QTimer::singleShotImpl`. Conclusion: the scans coming back on screen
  are the vendor's environment redraw, not a request we can decline.
  Full evidence, ranges and reproduction recipe:
  [merge-common-mesh.md §§11-17](research/merge-common-mesh.md).
  The 1.5.6 source additionally walks a command map past an unreadable
  node (Qt's sentinel used to end the walk on the first one, which is
  what hid the keys behind `nodes=1 [type8 …]` in the log); it builds
  clean and is **not deployed** - it ships with whichever of A/C below is
  chosen, so `mod.log` and the installed dll keep agreeing meanwhile.
* **2026-11-26, viewport solved by option C - 1.5.7 then 1.5.8, both
  live.** The open choice of 2026-11-25 (A: keep the merge behaviour and
  drop the inert filter, C: re-issue the eyes through the controller's own
  slot) went to **C**. 1.5.7 called `setNodeVisible(uuid, false)` after
  the vendor's `SetCurrentEnv` handler returned; the live log proved it
  sent **nothing** - the node was already flagged hidden and an unchanged
  value produces no command anywhere between the model and the view (a
  real click always produces two `SetModelNodesVisible` within 0..16 ms).
  1.5.8 therefore *toggles*: show, then hide, through the same slot - the
  exact sequence a hand click performs - plus one deferred pass 300 ms
  later (`SetTimer(NULL, ...)` in the hub's own Qt thread) for the
  environment switch's `singleShot` redraw. Guards: it runs only on the
  thread that last entered the metacall, the borrowed controller pointer
  is passed through and never dereferenced (the vendor slot goes through a
  file-scope singleton), every vendor call is SEH-boxed, and it calls the
  *original* metacall so the .ini write is bypassed - only the final state
  is "hidden". Reported as user-verified on 2026-11-26; that report was
  later contradicted - the same viewport hold re-issued through the vendor
  slot hangs the hub (see 1.6.1 below), so the switch is off by default.
  Evidence: [merge-common-mesh.md §18](research/merge-common-mesh.md).
* **1.6.0 (2026-11-26): renamed Selective Mesh.** `merge_select` →
  `selective_mesh`, "Merge Selection" → "Selective Mesh", data files
  `merge-select.ini`/`.map.txt` → `selective-mesh.ini`/`.map.txt`.
  Behaviour is exactly 1.5.8. The rename is not cosmetic at the deploy
  layer: the host derives the module id from the DLL basename and defaults
  every `modules.ini` section to enabled, so a leftover `merge_select.dll`
  would hook the same gate twice - manager 1.6.9 therefore carries
  `merge_select` in `g_legacy[]` (deletes its dll/ini and section), copies
  its switch file to the new name first, renames the `merge_select` token
  of a hand-written `sibling_modules` list, and the module copies the
  user's selection file to the new name on first start (old file left as a
  backup). Module map for anyone reading the source cold:
  [ARCHITECTURE.md](../modules/selective_mesh/ARCHITECTURE.md).
* **1.6.1 (2026-11-26): `eye_reapply` defaults to 0.** The re-apply was
  shipped "verified" in 1.5.8 and it is not: with it on, the hub stopped
  responding after a Mesh Model press and Windows logged an Application
  Hang. The show/hide slot is a vendor metacall, and calling it while the
  post-process environment switch still owns the scene is what stalls the
  Qt thread; the show half also flashes every hidden scan on screen first.
  The user confirmed the same hang on a 1.5.7-era build, so it belongs to
  the re-apply design rather than to the 1.6.0 rename. Default off,
  everywhere (code, ini template, docs); the viewport may therefore show a
  hidden scan again after Mesh Model, while the merged mesh still excludes
  it (`eye_gate`, which never calls vendor code). Open design item: a
  re-apply that waits for scene quiescence instead.
* **1.6.2 (2026-11-26): the eye-click hang, found in a dump.** With 1.6.1
  deployed the hub still hung on eye clicks, so the module was disabled in
  `modules.ini` and the identical clicks produced no hang - ours, not the
  vendor's. A full-memory minidump of the hung process put a 100%-CPU
  thread at `readQName5()` called from `qmapDumpD()`, with the walk's own
  frame reading `sp = 1, cnt = 0` and a skip counter near 2^31: the map
  walk bounded its *pending stack* but not the number of nodes visited, so
  a non-map argument whose pointers refer back to itself is walked forever
  (no key ever parses, so the key limit never trips). The same guard
  compared `sizeof stack` in *bytes* (511) against a 64-element array, so a
  wider bogus graph would also have written over the saved registers and
  the return address. Three hard bounds now: 64 pending nodes (counted in
  elements), 256 visited nodes, and a root gate that starts a walk only
  when the root node carries a readable key. Diagnostic path only, all
  three fail open.
  Evidence: [merge-common-mesh.md §19](research/merge-common-mesh.md).

 * **2026-11-26: 1.6.2 live-verified by the user.** Eyes toggle without
   any hang, and the common mesh is built only from the scans whose eye is
   open - both halves of the module's purpose confirmed on the installed
   build (sha256 `D62DE7A2...E5FA`).
 * **1.7.0 (2026-11-26): "no eye open" no longer dead-ends the merge.**
   Hiding all three eyes and pressing Mesh Model reproduced the vendor's
   empty-input dead end: Processing stopped at ~80 %, page buttons dead,
   viewport still live, nothing lost. The receiving side
   (`PostProcess.dll!LaserMeshGenerateController::receiveProjectsData`,
   RVA `0xFF850`) skips its whole input loop when `meshList` is empty and
   leaves `hasSophyMesh` false, so the step is never reported done. The
   user's rule: if this module would hide *every* scan of a merge, that run
   must behave as if every eye were open. The scan set of the current merge
   is in the vendor's own registers at the hooked call (both senders keep
   the `QList` at `param_1+0x70` in `rdi`), so the thunk now also hands the
   helper the caller's saved registers, and `merge_list.h` reads that list
   as plain memory - no Qt call. The first version of that reader used the
   *string* header layout, and `tools/merge_list_lab.c` caught it against
   the shipped `Qt5Core.dll` (5.15.10): a `QList` header is `ref`, `alloc`,
   `begin`, `end`, slots at `+0x10`, count = `end - begin`. All lab checks
   pass, including a real `QStringList`, a container whose `begin` is 2,
   and the 11-candidate register scan accepting exactly one register. The
   guard only ever forces "include", the vendor's own cloud flag still
   decides, nothing is written back, and any doubt (list not found,
   unreadable item, id-less scan, mismatched `rcx`) means "do not
   interfere". Switch `all_hidden = 0` to keep the stock behaviour.
   **Built and lab-proven, not yet deployed** - needs the user's UAC click
   and a hub restart. Evidence:
   [merge-common-mesh.md §20](research/merge-common-mesh.md).
 * **1.7.1 (2026-11-26): the guard's precondition was wrong, and the hub
   said so.** 1.7.0 went live at 18:30 (`ah=1` in the armed line) and on
   the user's all-eyes-hidden press the guard refused to act three times
   per sender: its first check demanded the saved `rcx` equal `self`, and
   both rejected values (`0x…76E3BB`, `0x…773097`) are code addresses
   *inside the merge DLL*. Failing open, the merge stalled exactly as
   stock does, so nothing broke; but a rule of the form "register N equals
   X" is a claim about one compiler's allocation, not a fact. The
   acceptance rule is now self-validating - a candidate register is the
   list only when that list holds an item whose `+0x28` is this call's
   `self` - which is also what proves the `RegCtx` decode is aligned, and
   the lab runs it with `rcx` deliberately holding a code address. The
   first decision of every process additionally dumps the raw block, so
   the layout is measured on every vendor build instead of assumed.
 * **1.7.2 (2026-11-26): live-verified; names and records only.** With the
   fix installed the user confirmed the whole flow, and the log shows both
   verdicts from the same path: all three eyes hidden -> `all 3 scans of
   this merge are hidden - exclusions ignored, every scan goes in` and the
   merge finished (no 80 % stall); one eye shown -> `3 scans in the merge
   list (…, reg#9, self at #1, 0 unreadable) - at least one stays in` and
   only the visible scans merged. The block dump also corrected the last
   assumption: exactly one of the eleven slots reads as a list header
   (`hdrs=1`, so the decode is aligned), the list itself arrives in **`r9`**
   (not the `rdi` the decompiler's loop suggested), `self` is in **no**
   saved register, and the slot logged as `rcx` is the vendor's `rbx` -
   `RegCtx` had been laid out the other way round (the first pushed `rax`
   is at the *lowest* address). 1.7.2 renames those fields to match the
   measured order: no behaviour change.
 * **1.8.0 (2026-11-26): the eye becomes the only interface, and the
   manager loses two buttons.** The user's question - what are "Scan roster"
   and "Open folder" for? - was the right one. The product has no stock way
   to exclude a scan, so the eye-to-merge path *is* the feature; everything
   else dated from before the eye could be tied to a scan. Removed: the
   folder/name identity channels and the vendor string code they needed
   (`getFolderPath`, `getTXProjFullPath`, `QString::utf16`, `~QString`), the
   `idDump` object map, the `selective-mesh.map.txt` roster with its dedupe
   table, the `merge-select.ini` migration, and with them the roster window
   and `fbtnNames` in the manager (~500 lines, manager 1.7.0 - the card's
   file-button pair now belongs to the wallpaper module alone). Kept: the eye
   watch, the uuid binding, the all-hidden guard, the scene diagnostics.
   State is session-scoped, in line with how the product behaves (a
   re-opened project shows its scans again - observed 2026-11-24, not
   something the module can enforce). So
   `clearStaleEyeState()` drops every `[uuid]` section when the hub starts
   (hub process only - the siblings only read the file; `[Settings] enabled`
   is the user's own switch and stays), and showing an eye deletes its
   section. That also closes a real trap: 2.7 KB of sections from months of
   projects had piled up, some `included = 0` for scans that no longer exist,
   and a stale section outranked what the user could see on screen.
   The gate's decision is now pure memory inspection - master switch, uuid,
   session `.ini`, in-RAM eye map, guard. No vendor function is called there,
   which is the rule 1.5.0 taught the hard way.
   Deployed 21:17 (elevated `tools\deploy_module.ps1`, note `1.8.0_eye_only`,
   switches `verbose=1 eye_gate=1 scene_watch=1 all_hidden=1 scene_filter=0
   eye_reapply=0`): installed `selective_mesh.dll` sha256 `21EE0558...400E`,
   armed `v1.8.0` in `EXStar Hub.exe` (pid 27920) and `scanservice.exe`,
   gate hooked in both, every other host inert. The session reset ran on that
   first start and logged exactly the number measured before it: `dropped 41
   exclusion(s) a previous run left in ... - the eyes start open`. The now
   stale pre-1.8.0 header text in that file was removed with the file itself,
   so the next start writes the new one. Obsolete files deleted (backed up):
   `merge-select.ini` (pre-1.6.0 name, no reader since the migration went)
   and an orphan `marker-quality-lab.bin` from an old lab run; the
   community patchset's `mesh-selection.ini` was left alone. Module 1.8.0,
   manager 1.7.0.
   Known open question, recorded rather than papered over: a *project*
   re-open inside one run is not measured - if the vendor rebuilds the tree
   without emitting a show event per row, an exclusion made before the switch
   outlives the icon that produced it (the eye is open, the module still says
   hidden). Showing the eye or restarting the hub settles it; the `eye <uuid>
   -> VISIBLE` log line is what would prove the vendor resets it by itself.
 * **2026-11-26, pre-release sweep (same manager 1.7.0 build).** A last pass
   for leftovers found one thing a user could read wrong: the card for Zoom to
   Fit Fill showed `1.0.0` while `fitfill.c` and the arm log say `1.1.0` -
   corrected. Six defines nothing referenced are gone (`KNOWN_GOOD_PRODUCT`,
   `CKIND_NAV`, `IDC_STATUS`, `C_BADG`, `C_INK`, `LMARG`, `CD_GAPX`; the
   compile is the proof they were unreferenced), `WIN32_LEAN_AND_MEAN` stays
   because the headers read it themselves. Recorded rather than deleted on a
   release cut: thirteen `static` functions in `src/manager.c` that no code
   calls (`iniGetKeyS`, `cardHover`, `mkStatic`, `rr`, `textEllW`,
   `cardCollapsedH`, `a64Cmp`, `a64CmpR`, `a64SubR`, `a64AddR`, `a64MovR`,
   `fanGetLE64`, `fanMap2Va`) - the linker drops them, so they cost nothing
   at run time; they go out with the next UI change, where a rebuild is
   already under review anyway. The module itself has nothing left: no
   unreferenced define, no debug print, no TODO, ASCII only, and at `/W4`
   only the two C4057 annotation notices around `CreateThread` that every
   module here carries. Docs now name the borrowed source one way
   everywhere: community patchset, never a person, never a private base.

## Open regressions

| Item | Since | Status |
|---|---|---|
| fitfill has no effect on 2.1.0-10 | vendor update 2.1.0-10 | diagnosed 2026-11-19: the flag it forces is dead code on the new fit pipeline (analysis: [zoom-fit-optimization.md §6](research/zoom-fit-optimization.md)); replacement anchor found the same day - the `setEnableScanScale(false)` call site in `sn3d.digit.scenecontrol.dll` ([§7](research/zoom-fit-optimization.md)), implemented in fitfill 1.1.0 (import-name-anchored in-memory byte patch). TODO: live confirmation on the hub (log line `scan scale forced to 1 at sn3d.digit.scenecontrol.dll+0x...` + Ctrl+D re-fills the frame). |
| manager window was fixed-size | before 1.3.10 | fixed in manager 1.3.10: `WS_OVERLAPPEDWINDOW` (resizable, minimize, maximize), `WM_SIZE` re-layout of width-dependent controls, `WM_GETMINMAXINFO` floor. Visually confirmed 2026-11-22 during the 1.4.0 UI redesign (any height re-flows the grid; the stock white scrollbar that surfaced at short heights is gone - the only scroll thumb is the custom dark overlay). |

## Rejected

- **6. English keyboard layout on the pre-start screen** (PrestartCheck.dll) —
  the user does not need it (decision 2026-11-14).
- Contour during supplemental scanning (the unstable setFrameMatrix chain, the patchset author
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
