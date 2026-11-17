# Laser 25×25 (+ the "50 lines" caption)

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file. Framework
> internals: [docs/architecture.md](../../docs/architecture.md). Full RE
> protocol: [docs/research/laser-plane-ids.md](../../docs/research/laser-plane-ids.md)
> (+ [scanpage-qml-resources.md](../../docs/research/scanpage-qml-resources.md)
> for the caption's qrc machinery); batch journal:
> [batch-2026-11-17-status.md](../../docs/research/batch-2026-11-17-status.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Feature | one switch, two coupled effects: (a) all **25+25 laser lines** reach the point cloud instead of the central 19+19 = 38; (b) the Light Source Mode button reads **`50 lines`** (in the UI language) instead of the vendor nominal `39` |
| Root cause - lines | the pipeline matches laser "plane ids" inside a window: the scan platform calls the exported `Sn3DAlgorithm::set_match_plane_id_vulkan(ctx, &startId, &endId, &startIdR, &endIdR)` with `(3, 21, 3, 21)`; the export only logs and tail-calls an internal store (`FUN_1804b24b0` in 3.1.x: four `mov` writing the ints to `ctx+0x25534/38/3C/40`), so the window is fixed by the **values the caller passes**, not by any config file |
| Root cause - caption | the laser settings QML (`ScanSettingModelProviderLaser`, a compressed qrc record inside `ScanPage_E7.dll`) labels the mode with `text: qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)` - the hanzi source string **is** translated (vendor `.qm` catalogs: `"%1线"` → `"%1 lines"` in English), the number comes from the C++ model property (= 39, nominal for the (3,21) window); the twin button uses `parallelLaserLineShowNum` (= 7) |
| Fix - lines | a detour (`host->hook`, MinHook) at the export entry, found by **export name** in `algorithmLzy.dll`; when the four referenced ints equal the configured `from_*` tuple `(3,21,3,21)` the original is called with pointers to our `to_*` copy `(0,24,0,24)` instead; any other call (e.g. the 7-line mode) passes through untouched |
| Fix - caption | map-time text rewrite of that qrc record (the fps module's zlib machinery), replacing **only the formatted number**: `.arg($scanSettingModelLaser.crossLaserLineShowNum)` → `.arg(50)`. The translated `qsTr` source stays byte-identical, so catalogs keep working in every locale; `7 lines` is not matched and stays stock |
| Key source file | `modules/laser50/laser50.c` (`detour`, `tryInstall`, `capRecord`); export lookup via the shared `include\exstar_export.h`, the caption's qrc write path via the shared `include\exstar_qrc.h` |
| Offline check | lines: `dumpbin /exports algorithmLzy.dll` shows exactly one `?set_match_plane_id_vulkan@Sn3DAlgorithm@@YA?AW4RetVal@1@PEAXAEBH111@Z`; Ghidra: log + tail-call to the four-dword store; the only importing module is `SnAlgAppPlat.dll` (loaded dynamically by scan processes - no EXE links it). Caption: `tools\replace_lab.exe "<ScanPage_E7.dll>" "ScanSettingModelProviderLaser" pat_from.bin pat_to.bin` - exactly one record (`@file 0x98C90E raw=67753 lc=9777 pattern=1`), `patched=9587 FITS` (slack 194 B) |
| Runtime proof | `%LOCALAPPDATA%\ExStarHubMods\mod.log`: `laser50: hooked ?set_match_plane_id_vulkan@... strict=1 (3,21,3,21) -> (0,24,0,24)` + per-scan `laser50: call N: (3,21,3,21) -> remap`; `laser50: caption ScanPage_E7.dll stream @0x98DB0E raw 67753->67711, packed 9777<-9587, 1 caption(s) rewritten (ok)` (live VA = file +0x1200, never hardcoded); visually: 25×25 lines in a real scan + `50 lines` / `7 lines` buttons (user screenshots, 2026-11-17) |
| Do not ever | hardcode the export RVA/ordinal (build-specific); rewrite the vendor's caller globals (the friend's disk patch - version-bound, never live-verified); reword the **translated** qsTr source string instead of its argument (v1.0.0 caption bug, see below); use `strict=0` carelessly (rewrites *every* call including other line modes); hook before `algorithmLzy.dll` is mapped (dynamic LoadLibrary - poll for the module) |

| | |
|---|---|
| Module id | `laser50` |
| Version | 1.1.0 (merged: was `laser50` 1.0.0 + a separate `captions` module) |
| Status | ✅ live-verified on 3.1.x (2026-11-17): real scan shows 25×25 lines (user-confirmed); log `caption ... 1 caption(s) rewritten (ok)` byte-exact with the dry-run; screenshot shows `50 lines` / `7 lines` in the English UI |
| Mechanism | worker thread + `LdrRegisterDllNotification`; two independent halves, each idle unless its carrier DLL appears in this process - [lines]: poll `algorithmLzy.dll`, walk exports for the unique `needle`, `host->hook`, SEH-guarded argument read, strict tuple match, counters + first-N-calls logging; [caption]: patch `ScanPage_E7.dll` **inside the map notification** (before DllMain/Qt parses the QML), record predicate `count(anchor)==1 && count(pattern)==cap_expected`, replace-all, level-9 re-deflate into the original compressed length, rawSize update, verify-by-reinflate + byte-exact restore, `QML_DISABLE_DISK_CACHE=1` |
| Requires | runs everywhere: `[host] sibling_modules` kept in sync by the Mod Manager (`needsSiblings = TRUE` - the pipeline lives in scan processes, the caption in the UI process); the app's `zlib1.dll` for the caption half (absent → that half self-disables); no file changes, no offsets |

## Why one module

The caption is a lie without the lines (it would promise 50 while the
pipeline delivers 38), and 25×25 without the caption leaves a stale "39"
on the button. Separate switches only created a broken intermediate
state; `enabled` of this single module is the feature. Sub-switch
`captions = 0` in `laser50.ini` remains for anyone who wants the lines
without the label.

## Why hook the setter, not patch the data (lines)

Three candidate layers existed:

1. **caller globals** (friend's way: rewrite `(3,21,3,21)` in the
   importing module's data section) - dead on the first vendor update,
   and the "importing module" is only known per version;
2. **the algorithm context** (`ctx+0x25534`, discovered via Ghidra) -
   needs the instance pointer and races every fresh `set_...` call;
3. **the exported setter** - a named, version-stable contract
   (`?set_match_plane_id_vulkan@Sn3DAlgorithm@@YA?AW4RetVal@1@PEAXAEBH111@Z`,
   3.1.x exports table), one call site per scan session, trivially
   pass-through-safe for other modes. Chosen.

Export lookup is by substring `set_match_plane_id_vulkan@` (the `@`
excludes `..._opencl`): zero hits or several hits both mean *stay idle*
(logged), never a guess. Forwarded exports are refused as well.

`strict=1` semantics: only the exact tuple `(3,21,3,21)` is rewritten;
a vendor/scan mode with different values passes through and logs the
mismatch once - the app keeps its stock behaviour, the ini is exposed
(`from_*`/`to_*`) so a future value can be adapted without a rebuild.

## Why the caption changes the argument, not the string

The vendor retranslated these strings once already (the friend's
2.0.0-45 build had literal `qsTr("50线")`/`qsTr("7线")`; 3.1.x has the
dynamic `qsTr("%1线").arg(model)` form - discovered by dumping the qrc
record at file offset `0x98C90E`, ≈ the friend's `H=0x98C906`). Content
gating (`anchor`×1 **and** `pattern`×`cap_expected`) makes any further
restructure skip silently instead of patching blind.

**The v1.0.0 `captions` bug (do not reintroduce).** The first version
replaced the literal `qsTr("%1线")` itself. Wrong twice: (1) the hanzi
source **is** translated - the EN catalog maps `%1线` → `%1 lines`, so
the English UI was never showing hanzi; changing the source changes the
*lookup key*, the catalog misses and the raw replacement leaks into
every locale - the EN user saw `38 линий`; (2) the number comes from
the model, so a suffix swap could not produce "50" anyway. Rule:
**never reword a translated source string; change only the value it is
formatted with.** `.arg(50)` keeps the catalog key intact and reads
`50 lines` / `50线` per locale.

The replacement is *shorter* than the pattern (63 → 21 B), so the
re-deflate headroom is guaranteed; framing rules (fixed next-record
boundary, `[u32 BE rawSize]` header, verify-by-reinflate with byte-exact
backup/restore) are the fps module's.

## Configuration

Enable/disable - `modules.ini` in the application directory (the Mod
Manager *Laser 25×25* card also maintains the sibling whitelist):

```ini
[laser50]
enabled = 1
```

Behavior - `modules\laser50.ini`:

```ini
; [lines]
target_module = algorithmLzy.dll
needle        = set_match_plane_id_vulkan@
strict        = 1
from_left_start = 3     from_left_end  = 21
from_right_start = 3    from_right_end = 21
to_left_start = 0       to_left_end    = 24
to_right_start = 0      to_right_end   = 24
log_calls = 6           ; first N calls logged with real values

; [caption]
captions      = 1       ; 0 = keep the stock "39" on the button
cap_target    = ScanPage_E7.dll
cap_anchor    = ScanSettingModelProviderLaser
cap_pattern   = qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)
cap_replacement = qsTr("%1线").arg(50)
cap_expected  = 1

verbose = 1
```

(`cap_*` values are UTF-8 bytes read verbatim; `#`/`;` inside a value
must be wrapped in `"..."` - the host parser honours quotes.)

## Verification log (3.1.x, 2026-11-17)

```
laser50 v1.1.0 armed in pid=...: algorithmLzy.dll "set_match_plane_id_vulkan@", strict=1, (3,21,3,21) -> (0,24,0,24), captions=1
laser50: hooked ?set_match_plane_id_vulkan@Sn3DAlgorithm@@... : strict=1 (3,21,3,21) -> (0,24,0,24)
laser50: call 1: (3,21,3,21) -> remap            (x4 per scan session)
laser50: caption ScanPage_E7.dll stream @0x98DB0E raw 67753->67711, packed 9777<-9587, 1 caption(s) rewritten (ok)
```

- user-confirmed: 25×25 lines in a real scan; `50 lines` / `7 lines`
  buttons, English UI intact;
- caption raw/packed sizes match the offline `replace_lab` prediction
  exactly;
- rollback: `enabled = 0` + restart = stock lines and stock label
  (nothing on disk changes at runtime).

## Known constraints

- which sibling process hosts `SnAlgAppPlat.dll`/`algorithmLzy.dll` is
  discovered at runtime (no EXE statically imports them); the module
  logs its own pid at `armed` and at `hooked`;
- the caption half stops probing its target after 2 min in processes
  that never load `ScanPage_E7.dll` (scan workers stay quiet);
- if the pipeline is ever restructured so the tuple no longer matches,
  the strict guard degrades to vendor behaviour + one log line -
  feature silently off, never wrong lines.
