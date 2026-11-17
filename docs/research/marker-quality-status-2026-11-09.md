# marker_quality v1 - status & morning experiment (2026-11-09 night)

## 2026-11-09 day4: marker_inframe <-> marker_quality conflict resolved

Reported: with both modules on, colored markers stopped enlarging (the
quality repaint hid the vendor red from the inframe detector), and the
bright quality red would in turn have falsely triggered enlargement.

Resolution (user chose "enlarge only, keep colors"):
- marker_inframe 1.1.0: green outline removed entirely (no custom rim
  color anymore); enlargement is the only in-frame cue; a quality-colored
  disc passes through the upload hook untouched;
- invisible tag handshake inside the render process: marker_quality
  reads the vendor `frontInnerColor` (osg `Uniform::get(Vec4f)`, export
  confirmed on osg158-osg.dll), and for a vendor-red (in-frame) marker
  paints the tier color with G+1/255; the exact float4 tuples are
  exported (`ex_mod_inframe_snapshot`) and matched exactly at
  glUniform4fv time - same GL thread, no locking, degrades to the plain
  red detector when quality is off;
- quality red stage is now dark red 128/24/24 (R=0.50 < the 0.55 red
  threshold, can never be read as "in frame"); ini documents R < 139.
Both modules verified loading together (armed + color slot hooked);
live scan confirmed by the user: colored markers still enlarge in frame.

Follow-ups the same day:
- a brightness-throb (`pulse`) experiment for extra in-frame pop was
  built, found too flashy for the user and fully reverted (module back
  to v1.1.0 semantics, released as manager 1.2.1; 1.2.2 discarded);
- the Mod Manager now has generic per-module slider rows
  (`SliderDef`): Marker In-Frame keeps its scale slider, Marker Quality
  got five threshold sliders (green frames/supported/spread_um, yellow
  frames/supported) that write modules\marker_quality.ini directly;
  card height and window height now derive from the slider count;
- usability iterations on the user's request: units shown with every
  value ("100 µm", "1.6×"), factory default annotated when moved off
  ("0.29 mm (умолч. 0.10)"), spread switched to millimetres (the ini
  still stores µm via `iniMul`); finally the five quality sliders were
  replaced by four coupled preset pills «качество позиционирования
  маркеров» (грубое 0.25 mm/10/2, среднее 0.1 mm/20/4, точное
  0.05 mm/30/5, сверхточное 0.02 mm/45/7) - one click writes all three
  green_* keys, selection is read back as the pill whose spread is
  nearest to the ini value (verified by an injected click test).
- tried a "▶ Запустить" button on the version row that would start
  EXStar Hub with the normal non-elevated token (manager itself is
  requireAdministrator; explorer.exe hand-off silently no-ops and
  explorer-token duplication is blocked on this UAC setup - CPAU 1314 /
  CPWT 5; a self-deregistering one-shot Task Scheduler task with
  RunLevel Limited does yield a MEDIUM-IL child, verified with an HKLM
  write probe). The user vetoed the feature ("запуска не происходит")
  and it was removed again before publishing; the manager ships as
  1.2.1 without a launch button.
- field note: around this time the hub began self-exiting ~1 s after
  every launch (even vanilla, proxy winmm.dll removed, exit code 0,
  no WER events, no vendor logs written). Root cause: a local LLM
  server holding 92.8 GB of 95.7 GB RAM; only ~3 GB free - the app
  refuses to start under that memory pressure. Not a mod issue
  (button launches were still reaching the app - host lines appeared
  in mod.log at press time).

## 2026-11-09 day3: hardening pass + Mod Manager integration (v1.2.0)

Robustness (marker_quality 1.0.1):
- shared bridge folder `C:\Users\Public\Shining3d\EXStar Hub` is now
  created recursively (fresh machines) and re-checked before every write;
- bridge file deleted by the process that actually hooked the color slot
  (the painter), not at module start - sibling helpers spawning mid-session
  can no longer wipe a live palette; a vanished file is recreated on the
  next publish cycle;
- install thread: deterministic failures (export renamed, Qt helpers
  missing, hook failing) give up after 10 tries; loop capped at 5 min;
- pose source auto-probe (`mode=1` = 12 doubles at `*cdata`, verified live)
  with orthonormality gate and raw-bytes diagnostics;
- `verbose = 0` in the module ini mutes calibration/binding/heartbeat logs
  (binding log additionally throttled to 10 s);
- slot swap via InterlockedExchangePointer; misc dead code removed.

Multi-process goes production (host 1.2.0):
- new keys `[host] sibling_all` / `sibling_modules` activate the host in
  sibling exes (scanservice.exe); `dev_all`/`dev_modules` remain honored
  and win when dev_all=1 (developer override, current dev machines keep
  working unchanged);
- the mod manager owns the sibling keys: `syncSiblings()` rewrites them on
  every install/remove from modules declaring `needsSiblings`
  (marker_quality only), so the whitelist can never drift.

Manager v1.2.0: `marker_quality` registered (no slider/color controls),
DLL + ini embedded as RCDATA (`MOD_MARKER_QUALITY[_INI]`), `--list` shows
both modules. Release: `release\ExStarModManager_1.2.0.exe`.
Docs: `modules/marker_quality/README.md`, README module table,
architecture.md config table.

## 2026-11-09 day: first live scan - root cause of "all red" FOUND & FIXED

## 2026-11-09 day: first live scan - root cause of "all red" FOUND & FIXED

Live scan proved the whole wiring works (frames ok=4077, bindings captured,
`writes=54645` uniform overrides, palette painted=72). The colors were all
red because the world positions were garbage (~1e38):

1. **Frame records store the camera-local position as `double[3]`, NOT
   `float[3]`** (the companion `marker_world_position` memcpy's `double[3]`; its
   comment: "The archived record is packed differently from the live MSVC
   vector element"). v1 read floats -> garbage locals.
2. **`rt` behind the shared-pointer wrapper deref is not 12 doubles on this
   build** (same wrapper path is correct for `curMarker`, so rt's metatype
   differs). v1 copied 96 raw bytes from the wrong object -> garbage pose,
   which dominated every world coordinate.

Fix (deployed 12:25): pose source auto-probed among three candidate layouts
(`*cdata`, `cdata`, `*cdata+8`), locked on whichever yields an orthonormal
rotation (column norms in 0.98..1.02, |t|<1e7); raw `*cdata`/`cdata` doubles
dumped (`mq: pose DIAG ...`) if nothing validates; records now read
`double[3]`; one-shot `mq: first sample id=... world=(...) poseMode=N` log.

## 2026-11-09 day2: calibration numbers + green-unit bug

Second scan (12:30): everything sane. `pose source locked to mode=1`
(`*cdata` holds the 12 doubles), world centers stable to 0.02-0.05 mm.
Green never appeared: the um->mm conversion divided by 1e6 (metres) while
positions are in mm -> fixed to /1000 and deployed 12:56.

Live calibration (short ~1 min scan, good markers):
  spread 0.023..0.052 mm, devAvg 0.02..0.05 mm, devMax < 0.5 mm,
  frames 300..1500, dirs/supported 4..8, maxAng 40..97 deg.
Poor markers: spread 0.38..0.65 mm (clear separation at ~0.1 mm).
Simulated tiers with fixed thresholds: 32 green / 26 yellow / 12 red.

## (night notes, kept for history)

## What is deployed RIGHT NOW in `C:\Program Files\Shining3d\EXStar Hub`

- host 1.1.1 + winmm proxy (unchanged since probe v0.3 deploy).
- `modules\marker_quality.dll` v1.0 + `modules\marker_quality.ini` (NEW module,
  source `modules\marker_quality\marker_quality.c`, builds clean).
- `modules.ini`: `log=1 dev_all=1 dev_modules=marker_quality`,
  `[marker_probe] enabled=0`, `[marker_quality] enabled=1`.
  The probe is OFF on purpose: probe and marker_quality must not MinHook the
  same `putFrameDataIntoFile` export simultaneously.

Night smoke test (no scan): hub pid alive, `install done pid=8116 qt=1 put=1
smp=1 slot=1`; `setMarkerPoints` detoured, `color stub at renderer+0x130320`,
`color slot at renderer+0x28CAA8 -> stub`, `color slot hooked`, put-frame
detoured in both hub and scanservice. No crashes, no faults. Bridge file
correctly absent until first frame.

## Morning experiment (~2 minutes, scanner hardware required)

1. Start `EXStar Hub.exe` from `C:\Program Files\Shining3d\EXStar Hub`
   (normal launch is fine - winmm proxy injects).
2. Open/start a project with markers, do a normal ~2 min scan, wave the
   scanner so markers are seen from several sides. Watch the markers: red ->
   yellow -> green progression should appear.
3. Close the app.
4. Analyze `%LOCALAPPDATA%\ExStarHubMods\mod.log`, `mq:` lines:
   - scanservice pid: `mq: putFrameDataIntoFile detoured`, `mq: new project`,
     periodic `mq: === pid=... frames=... stats=[... ok=N] ===` (ok must be >0)
     and per-marker `mq: id=... frames=... spread=... dirs=... supported=...
     tier=...` calibration lines (these set the final ini thresholds);
   - hub pid: `mq: binding N: flags=... count=...` (id<->child mapping),
     `mq: colorCb first call`, `mq: palette seq=... painted=N`,
     `mq: render hb ... cb= writes= noUniform= noBinding=` (writes>0 means
     colors are actually being applied);
   - `mq: colorCb no binding for flags=...` -> binding identity mismatch;
   - `mq: render chain unresolved` -> setMarkerPoints chain offsets broke.
5. Bridge file: `C:\Users\Public\Shining3d\EXStar Hub\exstar_marker_quality.dat`
   exists during the scan (16400 B, magic 'MQB1'), deleted by scanservice at
   its next start (session-only by design).

## Tuning knobs (modules\marker_quality.ini)

`green_frames=20 green_spread_um=100 green_supported=4`,
`yellow_frames=8 yellow_supported=2`, colors `red_/yellow_/green_ rgb`,
`report_ms`, `publish_ms`, `scan_side`, `paint_side`.
Defaults follow the companion reference model (Welford RMS spread; 8 direction slots
>=20 deg, slot supported >=3 frames; green latches, resets only on new
project). Calibrate `green_spread_um` from the live `spread=` values.

## Facts this build relies on (all proven, see code comments)

- Qt5Core 5.15.10 (GCC/MinGW MS-ABI) QMap layout proven OFFLINE via
  `tools\qmap_lab.c`: root at `d+0x10`, sentinel `d+0x8`, node
  parent@0 tagged (`&~3`), left@8, right@16, key@24, value@32.
- Color slot: single stub signature `48 83 C1 08 E9 77 ED FF FF` at
  renderer+0x130320 (signature-scanned, not hardcoded), single qword ref at
  renderer+0x28CAA8 (.rdata, swapped via VirtualProtect, in-memory only).
- Frame ABI: stride 0xB0, i32 id @0x50, float3 local @0; world = R*local + t
  from `rt` (12 doubles). Keys `frameData/curMarker/rt/frameCount`.
- Render chain: node+0x150 -> mix, +0x138 -> geom, geom+0x220 -> keys(u64),
  geom+0x248 -> state -> +8 visibility obj (i8, <0 hidden).

## Repo state (all UNCOMMITTED, push only with explicit owner approval)

HEAD `125553f` pushed/clean. Uncommitted since then: host dev mode
(dev_all/dev_modules), probe v0.2/v0.3, `tools\qmap_lab.c`,
`tools\mq_deploy.ps1`, `tools\set_ini.ps1` update, `build.ps1` module entry,
entire `modules\marker_quality\`. Rollback = delete `marker_quality.*` files
and remove the build.ps1 line.

## Next after a successful scan

Tune thresholds from calibration lines -> re-deploy ini only -> optional
second scan to confirm green behavior -> finalize -> ask owner for commit.
