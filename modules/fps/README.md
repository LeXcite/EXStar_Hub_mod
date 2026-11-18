# Real FPS

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file. Framework
> internals: [docs/architecture.md](../../docs/architecture.md). Full RE
> protocol: [docs/research/scanpage-qml-resources.md](../../docs/research/scanpage-qml-resources.md);
> batch journal: [batch-2026-11-17-status.md](../../docs/research/batch-2026-11-17-status.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Feature | live `FPS: N` readout at the bottom right of the scan page showing the **real UI frame rate** |
| Root cause (how stock works) | the vendor "FPS: 80-90" is a scan statistics value: the `FPS: %1` string lives in `AppCommon_E7.dll` and the `real_time_info` C++ model feeds it the **capture pipeline** rate (~constant 80-90), nothing to do with how many frames the UI actually presents; there is no `FPS` text anywhere in the scan page QML |
| Fix | at ScanPage_E7.dll map time, inflate the qrc record whose text starts `/// ScanPage_E7`, insert one `Text { id: exFpsLabel }` block after the root Item's `id: root` line (frame counter on `Connections { target: window; function onFrameSwapped() }` + 1 s sampler Timer), re-deflate level 9 **into the original compressed length**, update the stored rawSize |
| Key source file | `modules/fps/fps.c` (`fpsRecord`); the shared qrc write path lives in `include\exstar_qrc.h` (also used by laser50's caption half) |
| Offline check | `tools\replace_lab.exe "<ScanPage_E7.dll>" "/// ScanPage_E7" fps_from.bin fps_to.bin` - `from` = `id: root\r\n`, `to` = same + the rendered snippet; must show `record @file 0x97D318 raw=137735 lc=22753 anchor=1 pattern=1 ... stock=22392 (slack 361) -> patched=22610 FITS` |
| Runtime proof | `%LOCALAPPDATA%\ExStarHubMods\mod.log`: `fps: ScanPage_E7.dll stream @0x97E518 raw 137735->138526, packed 22753<-22610 (ok)` |
| Do not ever | move the next record's stream boundary (write > `lc` corrupts the neighbour); forget the `[u32 BE rawSize]` header update; feed the write back unchecked (this module re-inflates the patched record from the image and byte-compares, restoring a byte-exact backup on any mismatch); reuse the friend's `ScreenFpsProbe.inc` (IAT hook on `QString::number` + fixed caller RVAs `0x1620cd/0x16218b` - version-bound, died in an update); put a bare `#` value in an ini (see bugs) |

The scan page UI lives in **compressed QML resources inside the vendor
DLL**: `[u32 BE rawSize][zlib stream]` records in a read-only section
(Qt resource blobs, inflated by the app's own `zlib1.dll` at engine
startup). This module rewrites one such record **in the mapped image** the
moment `LdrRegisterDllNotification` reports the DLL - before its DllMain,
before Qt parses the QML, so the very first instantiation of the page
already carries the label. Files on disk are never touched; a plain
restart restores stock behaviour.

| | |
|---|---|
| Module id | `fps` |
| Version | 1.0.0 |
| Status | ✅ live-verified on 2.x (file stamp 3.1.x) through 2.1.0-10 (file stamp 3.1.49) (2026-11-17): log `(ok)` repeatedly and the user confirmed the `FPS: N` readout on the scan page; label hidden at 0 fps by design (idle UI presents no frames) |
| Mechanism | `LdrRegisterDllNotification` + worker fallback (5 min polling); one anchor pass on the inflated text; level-9 re-deflate must fit the original compressed length; `VirtualProtect` over header+stream; verify-by-reinflate from the image with byte-exact backup/restore; SEH around every foreign dereference; `InterlockedCompareExchange` guard between loader-thread and worker; silent refusal on any mismatch |
| Requires | zlib1.dll in the process (the app always loads it); no GL hooks; `QML_DISABLE_DISK_CACHE=1` is set by the module at init |

## Why the insertion point is textual

The record is identified structurally, not by offset: the live VA offset
(`0x97E518`) differs from the file offset (`0x97D318`) and both drift with
vendor updates. What is stable is the source text itself: the scan page
file is the only inflated record containing the comment line
`/// ScanPage_E7` (uniqueness is checked - more than one hit means skip),
and the snippet lands after the first `id: root` line following it, i.e.
inside the root `Item {` body. Anchor, insert marker and all label
geometry are configurable in `modules\fps.ini`; a vendor restructure can
only cause a silent skip, never damage.

The QML uses the engine context property `window` (the frameless app
window): the same file calls `window.openMessageBox(...)` and siblings
use `window.width/height`, so `window` resolves in this scope and
exposes QQuickWindow's `frameSwapped`. If a future build ever breaks
that assumption, `Connections { ignoreUnknownSignals: true }` keeps the
counter at 0 and the label stays hidden - degraded, never broken.

Idle UI presents no frames at all (scene-graph renders on demand), so
`frameSwapped` stops and the label hides itself; it appears exactly when
something is animating (scanning, rotating the view). That is the same
final behaviour the external helper converged on (right 62 / bottom 56,
no "(screen)" suffix).

## Configuration

Enable/disable - `modules.ini` in the application directory:

```ini
[fps]
enabled = 1
```

Behavior - `modules\fps.ini`:

```ini
target       = ScanPage_E7.dll  DLL carrying the scan page QML
anchor       = /// ScanPage_E7  identifies the record (must be unique)
insert_after = id: root         snippet is placed after this line
right  = 62                      right margin, px
bottom = 56                      bottom margin, px
size   = 16                      font pixel size
color  = "#8ecbff"               label colour (quotes required)
verbose = 1                      log patch details to mod.log
```

The Mod Manager card *Real FPS* has no controls on purpose (UI-only
module, like the stock "FPS" panel it supplements).

## Verification log (file stamp 3.1.x)

```
fps v1.0.0 armed in pid=11380: target ScanPage_E7.dll, margins 62/56, font 16 #8ecbff
fps: ScanPage_E7.dll stream @0x97E518 raw 137735->138526, packed 22753<-22610 (ok)
```

- raw grew by exactly +791 B = the snippet size, and the packed size
  matches the offline `replace_lab` prediction (22610) byte for byte -
  the dry-run harness models the module exactly;
- the "(ok)" suffix means the module re-inflated the patched record
  **from the live image** and compared it to the intended text - a
  mismatch would have restored the byte-exact backup and logged
  `VERIFY FAILED ... restored, left stock`;
- siblings: `host: skipping 'fps' (not in sibling whitelist)` in every
  `scanservice.exe` child (expected - UI-only module).

Release gate: closed on 2026-11-17 - user confirmed `FPS: N` on the
scan page, moving when the view redraws (rotate the model; idle = hidden).

## Known bugs fixed (do not reintroduce)

- `color = #8ecbff` in `fps.ini` arrived at the module as an empty
  string: the host's `iniGet` cut every line at the first `#` (comment
  syntax). Symptoms: armed log printed `font 16 ` (no colour) and raw
  grew 784 instead of 791 B. Fixed in three layers: the parser now
  honours `"..."` (comment markers inside quotes are literal, a fully
  quoted value has the quotes stripped), the ini ships the value as
  `"#8ecbff"`, and the module ignores empty colour strings. A `#` that
  is *not* inside quotes still starts a comment for plain values;
- the first draft of the record write path planned to "disable" a record on
  verification failure by zeroing rawSize (Qt would then skip the whole
  resource - blank scan page). Rejected: the write path now takes a
  byte-exact backup of `4 + lc` bytes before `VirtualProtect` and
  restores it verbatim when the re-inflate check fails;
- the friend's FPS approach (`ScreenFpsProbe.inc`): IAT hook
  `QString::number`, formatting call sites selected by **fixed caller
  RVAs** - the first thing a vendor update invalidates. The QML-level
  insertion above is version-agnostic by construction.
