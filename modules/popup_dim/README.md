# Popup Dim

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file. Framework
> internals: [docs/architecture.md](../../docs/architecture.md). The shared
> qrc resource model this module predates (and deliberately keeps its own
> equal-length variant of): [docs/research/scanpage-qml-resources.md](../../docs/research/scanpage-qml-resources.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Symptom fixed | opaque grey field behind modal popups; hard grey slab behind busy overlays («Project loading....», «Processing....», «Exiting...») |
| Root cause (type 1) | QML: `RecursiveBlur { source: backgroundImg }` - `backgroundImg` is the *static wallpaper* (`ApplicationWindow.background: Image`, `qrc:/images/MainBackground.jpg` inside AppUi.dll), not a screenshot. Blur of a grey vignette = uniform grey on every page |
| Root cause (legacy) | `Overlay.modal` items hard-coded to ~50-65 % black rectangles or fully opaque blur backdrops |
| Fix | in-memory rewrite of vendor QML text at DLL-map time: veil alphas → `dim_alpha`, curtain blur sources re-pointed to the live content (`$mainbody`), dialog frosted glass byte-identical |
| Key source file | `modules/popup_dim/popup_dim.c` (`dimBlurBlocks`, `dimGaussBlocks`, `gaussVeilKind`, `swapSourceToMainbody`, `applyZrc`) |
| Offline check | `tools\dim_dryrun.exe <dll> <app zlib1.dll>` - prints every byte edit without running the app |
| Runtime proof | `%LOCALAPPDATA%\ExStarHubMods\mod.log`: per-DLL `blocks=N verified=N restored=0 fault=0` |
| Do not ever | edit `radius`/`samples`/`deviation` of a `GaussianBlur` (shader degenerates to a solid grey slab) |

EXStar Hub paints a full-window curtain behind every modal popup -
notifications, progress bars, the memory-usage warning. Historically it was
a solid ~65 % black rectangle or a *fully opaque blurred snapshot* of the
window, so the whole scene disappeared for a two-second "saved" toast.
This module rewrites those QML sources **in process memory** the moment the
UI libraries are mapped: heavy veils become light translucent overlays and
the solid grey busy-overlay background becomes proper frosted glass over
the live page. Nothing on disk changes; a plain restart restores the stock
behaviour.

| | |
|---|---|
| Module id | `popup_dim` |
| Version | 1.2.7 |
| Status | ✅ live-verified on 3.1.x (2026-11-14): no grey slab behind «Project loading....»/«Processing....»/«Exiting...» (frosted glass over the real page), modal dialogs keep their frosted glass, home page visible behind popups; every rewritten compressed stream re-inflates from the patched memory (`verified=N restored=0` in mod.log) |
| Mechanism | load-image notification + literal byte-patches + a compressed-qrc pass (inflate → edit → re-deflate, rollback-verified) in the mapped (read-only) images of the vendor UI DLLs, plus `QML_DISABLE_DISK_CACHE=1` so the patched QML is recompiled |
| Requires | Qt Quick Controls 2 popups of the stock 3.1.x UI; no GL hooks |

## Configuration

Enable/disable - `modules.ini` in the application directory:

```ini
[popup_dim]
enabled = 1
```

Behavior - `modules\popup_dim.ini`:

```ini
dim_alpha = 0     # curtain opacity for modal overlays, percent (0..100)
soft_blur = 1     # also reduce blur radius/loops (0/1)
modules   = AppUi.dll,CompositeComponents.dll,Sn3DCloudDisk.dll
verbose   = 1     # log every patch hit to mod.log (needs [host] log = 1)
```

`dim_alpha` can also be set from the Mod Manager GUI (*затемнение*
slider); it applies after an EXStar Hub restart. The QML overlay literals
only offer 0.1 steps, so the value is rounded to the nearest tenth
(`0` see-through, `100` stock-like solid).

## The two overlay families

The vendor UI builds modal backgrounds in exactly two ways, and they look
fundamentally different. The whole module logic is a classification into
these two families - one shared root cause per family, never per-window
whack-a-mole:

**Type 2 - frosted glass (keep).** Dialogs (Tips, leave-confirm, save,
cloud login) use

```qml
Overlay.modal: Item {
    GaussianBlur  { anchors.fill: parent; source: $mainbody; radius: 16; samples: 16; deviation: 4 }
    Rectangle     { anchors.fill: parent; color: "#000000"; opacity: 0.5 }
}
```

`$mainbody` is the window's alias for the live page container
(`windowPageContainer`), so the blur samples the *actual content* - that is
the pretty frosted glass. The module leaves these blocks byte-identical.

**Type 1 - solid grey curtain (fix at its single source).** Busy overlays
(«Project loading....», «Processing....», «Exiting...», mode switch,
firmware update) use

```qml
Item { z: -1; MouseArea { anchors.fill: parent }   // whole-window input blocker
    RecursiveBlur { anchors.fill: parent
        source: backgroundImg; radius: 7.5; loops: 4; transparentBorder: false }
}
```

and here lies the root cause of the grey slab: **`backgroundImg` is not a
screenshot of the window** - it is `ApplicationWindow.background: Image {
source: "qrc:/images/MainBackground.jpg" }`, the *static wallpaper* baked
into AppUi.dll (1920×1080, mean RGB ≈ 80,80,87 - a grey vignette, lighter
center, darker corners, pixel-for-pixel the veil in the screenshots).
`RecursiveBlur` at `radius 7.5 / loops 4..10` averages that wallpaper into
one flat grey field - identical on every page because the wallpaper does
not depend on the page. All «loading/processing/exiting» screens are one
component (`ProgressBar_E7`, objectName `Sn3DProgressBarUI`); the veil just
gets different `title` text from C++, which is why one fix covers them all.

## Classification rules

Every rule is a structural property of the QML block text, so a new vendor
build either matches cleanly or is left alone:

| Kind | Shape | Action |
|---|---|---|
| GaussianBlur + companion `Rectangle { color: "#AA000000" }` (black-alpha veil, right after the block or `background: Rectangle` right before it) | busy curtain (ScanPage_E7 quick-action overlays, Sn3DCloudDisk login) | rewrite **only the two veil-alpha hex digits** to `dim_alpha`; blur props untouched |
| GaussianBlur + companion `Rectangle { color: "#000000"; opacity: X }`, or a veil inside the popup's own `background:` | frosted dialog glass | **never touched** |
| `RecursiveBlur` with unqualified `source: backgroundImg` and **no `opacity:` property** | solid grey curtain (the type-1 family) | **source redirect** (below) + soften `radius:`/`loops:` |
| `RecursiveBlur` with `source: $backgroundImg` and an `opacity:` property | dialog backdrop blur (Sn3DDialog and friends) | soften `radius:`/`loops:`, scale `opacity:` to `dim_alpha` |
| any block whose line starts with `//` | vendor's commented-out copies | skip (`lineCommented` guard) |

The `opacity:` property is what separates curtains from dialogs in the
RecursiveBlur family: the vendor gives every dialog backdrop an explicit
`opacity:`, the busy curtains have none. The `$`-qualification of the
source reference is the second, independent tell.

## The curtain source redirect (1.2.7)

The decisive fix. `RecursiveBlur` cannot simply be "switched off": its
implementation (Qt Graphical Effects 5.15) renders the *raw* source when
`loops: 0`, and the effect Item must stay in place - it is the
input-blocking `MouseArea` parent that keeps the UI unclickable during a
busy state. So the module does not fight the blur, it **re-points it at
the live content**:

```
source: backgroundImg   →   source: $mainbody␣␣␣
         (13 bytes)                (12 + 1 blank, length-preserving)
```

* `$mainbody` is the same global context alias the *kept* frosted dialogs
  already consume in production (their `GaussianBlur { source: $mainbody }`
  works in exactly the same overlay position), so the reference is proven
  to resolve at runtime;
* `RecursiveBlur` never hides its source (`SourceProxy` runs with
  `hideSource: false`), so the page keeps rendering underneath;
* QML ignores the trailing blank inside the binding, and the edit changes
  the decompressed stream size by **zero bytes** - the same
  length-preserving discipline as every other edit, so the re-deflated
  stream still fits its original slot;
* the result: instead of a flat grey field, busy overlays get the same
  frosted glass as the dialogs - the real page stays readable behind the
  progress card.

Applies to exactly two blocks in AppUi.dll (`ProgressBar_E7` and the
firmware-update veil); all 24 `$backgroundImg`+`opacity` dialog blocks stay
byte-identical (dry-run counters).

## Editing rules that must not be broken

* **Never touch `radius`/`samples`/`deviation` of a `GaussianBlur`.**
  With `radius: 0`/`samples: 1` its shader degenerates into a *solid grey
  slab* instead of the source image - this was the 1.2.5 regression that
  made the background "more grey than before". For GaussianBlur curtains
  only the veil alpha (`"#AA000000"` digits) may change.
* `RecursiveBlur` is safe to soften (`radius: 7.5 → 0.1`, `loops: N → 1`)
  - it re-renders from the source every frame.
* Every edit is length-preserving, and the re-deflated stream must fit the
  original compressed slot or the whole stream is skipped (never partially
  written).
* All writes happen on private copy-on-write pages of the mapped image
  under `VirtualProtect`; files on disk are never modified.

## Where the sources live

The whole UI is QML; the sources are embedded in the Qt resource blobs of
the vendor DLLs - partly as plain text, partly **zlib-compressed** per
file (`[u32 BE rawSize][zlib stream]` records). Verified inventory on the
3.1.x build (block counts = what the walkers match, dry-run and live
agree):

| DLL | where | what | handled by |
|---|---|---|---|
| AppUi.dll | `.text` (plain) | generic dialog base `Rectangle { color: "#a7000000" }` (~65 % black) | v1 ARGB pass |
| AppUi.dll | qrc, compressed, 5 streams | 6 `RecursiveBlur` blocks: `ProgressBar_E7` busy overlay (redirected), firmware-update veil (redirected), `$backgroundImg`+`opacity` dialog backdrops (dimmed) | `applyZrc` + `dimBlurBlocks` |
| AppUi.dll | qrc, compressed | 2 `GaussianBlur` dialog frosts (`#000000`+opacity) | classified dialog → untouched |
| ScanPage_E7.dll | qrc, compressed, 5 streams | 3 `RecursiveBlur` dialog backdrops; 2 `GaussianBlur` + `#94000000` busy veils (alpha → `dim_alpha`) | `applyZrc` + both walkers |
| CompositeComponents.dll | plain text | `Sn3DDialogWithOverLay.qml` (`RecursiveBlur $backgroundImg ... opacity: 1.0`), `BluredCoverEx.qml` (`loops1: 50`/`opacity1: 0.6`), `BluredCover { bluredTarget: backgroundImg }` | literal table |
| CompositeComponents.dll | qrc, compressed | 1 more `RecursiveBlur` curtain | `applyZrc` |
| Sn3DCloudDisk.dll | `.rdata`, **uncompressed** qrc | cloud-login modal `GaussianBlur radius 16/samples 16/deviation 4` + `#000000` 0.5 veil | literal table + `dimRawGauss` |
| pluginSn3DNavigation.dll | qrc, compressed | 2 veil copies (one at `opacity: 0.85`) | `applyZrc` |
| qtquickcontrols2plugin.dll | plain text | stock Controls 2 templates: `Color.transparent(palette.shadow, 0.5)` behind every modal (×5) | literal table |

`tools\zrc_lab.c` inflates every qrc record of a PE file and prints what
the block walkers see (inventory tool); `tools\qmap_lab.c` maps QML
component text inside the blobs (used to find `backgroundImg`'s
definition). `tools\dim_dryrun.c` (below) is the regression harness.

## Failure modes worth remembering

Found the hard way on this vendor; re-verify all of them before writing a
new QML patch:

1. **QML disk cache** (`%LOCALAPPDATA%\EXStar Hub\cache\qmlcache`) replays
   precompiled sources - every byte patch is invisible while enabled. The
   module exports `QML_DISABLE_DISK_CACHE=1` before Qt starts.
2. **Never hook hot Qt methods** (an inline detour of
   `QQuickItem::setVisible` once made the hub abort with the vendor
   "Software error code: 0x00000011"). QML *text* rewriting replaced that
   whole class of approaches.
3. **Polling for the DLL loses the race** - dialog QML compiles within
   milliseconds of the DLL mapping (dialogs are pre-created during the
   splash); only `LdrRegisterDllNotification` (patch at map time, before
   DllMain) wins reliably.
4. **Header-first writes fault the whole pass** - the qrc pages are
   PAGE_READONLY; writing the 4 big-endian `rawSize` bytes before
   `VirtualProtect` raised an AV that a function-wide `__except` silently
   swallowed (the ≤1.2.1 "rewritten=0 forever" bug). Header **and** body
   must go under one protection change, and the pass must count faults.
5. **Section scanning needs `min(VirtualSize, SizeOfRawData)` semantics**:
   ScanPage_E7 `.rdata` has a 4 KB raw tail past the virtual end; an
   unconditional "virtual ≥ raw" filter skips the whole qrc area.
6. QML sources in these blobs are **CRLF**; line-spanning patterns must
   include `\r\n`.
7. A fullscreen topmost Qt window can be invisible to `CopyFromScreen`
   (overlay/MPO plane) - when screenshot-verifying, capture the window
   after a plain un-maximize, not after toggling iconic state.
8. The DLL load notification fires **twice per mapping** - without the
   per-image guard the heavy pass runs twice (wasted work, double edits
   were avoided only because every edit is idempotent).

## How the patch works

1. `ex_init` converts `dim_alpha` to the ARGB alpha byte (v1 pass) and to
   the QML opacity string `0.0..1.0`, then exports
   `QML_DISABLE_DISK_CACHE=1` - the QML engine keeps an on-disk byte-code
   cache (`%LOCALAPPDATA%\EXStar Hub\cache\qmlcache`) that otherwise
   replays the *unpatched* popup. Night testing showed this cache is the
   single biggest gotcha: every patch "fails" while it is enabled;
2. a worker thread registers an `LdrRegisterDllNotification` callback and
   then polls (20 ms, 5 min cap) as a safety net;
3. the notification fires **when a DLL is mapped, before its DllMain
   runs**. This is what makes the patch win the race: dialog QML is
   compiled within milliseconds of the DLL being mapped (dialog windows
   are pre-created during the splash), so polling-based patching loses.
   For each mapped target the module walks read-only, non-writable
   sections and applies a context-anchored literal table:

   | DLL | pattern | replacement | hits |
   |---|---|---|---|
   | CompositeComponents | `radius: 7.5` | `radius: 0.1` | 4 |
   | CompositeComponents | `loops: 10` | `loops: 1 ` | 2 |
   | CompositeComponents | `loops1: 50` | `loops1: 1 ` | 1 |
   | CompositeComponents | `opacity1: 0.6` | `opacity1: 0.0` | 1 |
   | CompositeComponents | `transparentBorder: true\r\n...opacity: 1.0` | same, `opacity:` from `dim_alpha` | 2 |
   | Sn3DCloudDisk | `color: "#000000"\r\n...opacity: 0.5` | `opacity:` from `dim_alpha` | 1 |

   plus the v1 pass: `Overlay.modal` anchors followed (within 400 bytes)
   by a pure-black `color: "#AARRGGBB"` literal get their alpha rewritten
   to `dim_alpha` (this is what covers the AppUi dialog base);
4. the compressed-qrc pass (`applyZrc`): every `[u32 BE rawSize][zlib
   stream]` record of the mapped image is inflated with the application's
   own `zlib1.dll`; streams containing `backgroundImg`/`GaussianBlur {`
   are walked block-by-block (`dimBlurBlocks` + `dimGaussBlocks` apply the
   classification table above), edited in place, re-deflated (level 9,
   fallback 6) and written back into the original record slot - never
   larger than the original compressed size, so record boundaries and the
   following entry stay untouched. The header **and** body go in under one
   `VirtualProtect` (writing the read-only header bytes first faults the
   whole pass - the bug that kept this pass silently dead in ≤ 1.2.1).
   The original bytes are backed up before the write and the patched
   record is re-inflated from memory right after: if it no longer decodes,
   the record is restored (mod.log `verified=`/`restored=`);
5. every write is `VirtualProtect` → byte-copy → restore, on private
   copy-on-write pages of the mapping. The file on disk is never touched
   and other processes keep their stock view. All dereferences are
   SEH-guarded: a target that changes shape simply does not match and the
   app keeps its stock curtain. The load notification fires twice per
   mapping, so each image is patched exactly once (per-handle guard).

Note the QML sources use **CRLF** line endings - patterns that span lines
must match `\r\n`, plain `\n` finds nothing.

## Offline verification (dim_dryrun)

`tools\dim_dryrun.c` compiles `popup_dim.c` into a console tool and
replays the exact production walkers (`dimBlurBlocks`, `dimGaussBlocks`,
re-deflate sizing) over the zlib records of a DLL **file**, printing every
byte edit and whether the re-deflated stream still fits the slot:

```powershell
cl /W3 /I . /I include /Fe:build\dim_dryrun.exe tools\dim_dryrun.c
build\dim_dryrun.exe "C:\Program Files\Shining3d\EXStar Hub\AppUi.dll" `
                     "C:\Program Files\Shining3d\EXStar Hub\zlib1.dll"
```

Its counters match the live `mod.log` counters 1:1 (AppUi 6 blocks /
ScanPage 5 / CC 1 / Nav 2), so walker changes are verified offline before
an app restart. This is how the 1.2.7 redirect was proven
(`source: backgroundImg → source: $mainbody␣␣␣` in exactly the two curtain
blocks, 24 dialog blocks untouched) - and how a logic bug in the first
draft of the redirect was caught without ever touching the app.

With `[host] log = 1` the mod log shows what happened (1.2.7 live run):

```text
popup_dim v1.2.7 armed in pid=4044: overlay opacity 0.0 (0%), blur soft on, 6 target module(s), qmlcache disabled
popup_dim[27664]: AppUi.dll zrc zlib=1 cand=49 qml=11 rewritten=5 blocks=6 verified=5 restored=0 fault=0
popup_dim[27664]: ScanPage_E7.dll zrc zlib=1 cand=74 qml=6 rewritten=5 blocks=5 verified=5 restored=0 fault=0
popup_dim[27664]: CompositeComponents.dll zrc zlib=1 cand=142 qml=12 rewritten=1 blocks=1 verified=1 restored=0 fault=0
popup_dim[27664]: pluginSn3DNavigation.dll zrc zlib=1 cand=34 qml=2 rewritten=2 blocks=2 verified=2 restored=0 fault=0
```

## Version history

* **1.2.7** - curtain source redirect: type-1 curtains blur `$mainbody`
  (live page) instead of the `backgroundImg` wallpaper → frosted glass
  replaces the grey slab on every busy overlay at once; block
  classification by `opacity:`/`$`-qualification; length-preserving
  13-byte edit; `dim_dryrun` offline harness made mandatory for walker
  changes.
* **1.2.6** - GaussianBlur blocks classified via `gaussVeilKind`
  (curtain vs dialog vs none); curtains dim by veil alpha only;
  `//`-commented copies skipped.
* **1.2.5** - first GaussianBlur pass (regressed: radius/samples edits
  rendered a solid grey slab - lesson learned, see editing rules).
* **1.2.4** - popup_dim synced into all sibling processes
  (`sibling_modules = marker_quality,popup_dim`); pid tags in log lines.
* **1.2.3** - per-image once-guard for the heavy zrc pass.
* **1.2.2** - single-`VirtualProtect` record writes (the ≤1.2.1 silent-fail
  bug), re-inflate verification + rollback.
* **1.2.1 and below** - literal table, `QML_DISABLE_DISK_CACHE`, v1
  Overlay.modal alpha pass, compressed-qrc pass.

## Known trade-off

`QML_DISABLE_DISK_CACHE=1` slows the first QML compile of a session
(startup gets a bit slower, roughly the same as after a UI update). The
vendor update `softwareUpgrade.exe` can replace the DLLs at any time; the
module scans by pattern, not by offset, so a new build keeps working as
long as the QML text is recognizable - and silently does nothing when it
is not.
