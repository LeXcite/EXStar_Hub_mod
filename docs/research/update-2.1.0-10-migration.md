# Vendor update 2.1.0-10 (file stamp 3.1.49): migration verification

Date: 2026-11-18. Outcome: **all seven modules re-verified live on the
updated application; six needed zero code changes, one (popup_dim) needed
v1.2.8** - the vendor moved a curtain opacity into a QML *binding*, which
byte-literal edits cannot reach (see the popup_dim row below).

## Version notation

The vendor keeps two independent version numbers:

* the **product number** from the uninstall-registry key (this update:
  `2.0.0-45` → `2.1.0-10`);
* the **file stamp** - the `ProductVersion` of `EXStar Hub.exe`, which the
  vendor does not bump per release (still `3.1.49` after this update).

Research notes written before 2026-11-18 say "3.1.x/3.1.49" - that is the
file stamp. Module anchors are names/patterns/content, never version
numbers, so the notation never mattered functionally.

## Method: offline validation before the first launch

The update was installed but not started until every intervention point
had been validated offline (hash inventory of 815 app binaries, export /
import tables, offline dry-run replay harnesses). The dry-run ↔ live-log
byte-parity rule held again: every number predicted offline matched the
live log on the first run exactly.

What moved and what did not:

| area | change in 2.1.0-10 | consequence |
|---|---|---|
| injection | `EXStar Hub.exe` no longer imports WINMM directly; Qt5Core.dll still imports `timeSetEvent`/`timeKillEvent` | proxy still loads through the app directory - unchanged |
| fps | scan-page qrc record moved (`0x97D318`→`0x98D088`), raw size 137735→138401 (+19 QML lines); `id: root` still exactly one | dry-run 22504→22723 ≤ lc 22875 FITS; live `raw 138401->139192, packed 22875<-22723 (ok)` |
| laser caption | laser provider record byte-identical to the pre-update baseline | live `packed 9777<-9587, 1 caption(s) rewritten (ok)` |
| laser lines | export `?set_match_plane_id_vulkan@…` byte-identical mangled name | name-based detour unchanged |
| fitfill | setter/getter exports unique; call-site pattern in `librenderkit.extensions.digit.dll` intact, immediate still `1` (the vendor did not fix their fit bug) | unchanged |
| wallpaper | embedded JPEG slot same size (837 517 B), new location `0x9974F3` | structural search is location-independent; the updater only wiped the user's `modules\wallpaper.jpg` (it lives in the app directory) - re-place it |
| popup_dim | walker green everywhere (AppUi 6, CompositeComponents 1, ScanPage 5, navigation plugin - moved to `Sn3D\Navigation\` - 2 blocks) | block matching unchanged, but the vendor wrapped the shared busy/firmware curtain in an `opacity: $appUiController.isFirmwareUpdting ? 0.6 : 1.0` **binding** the literal edits could not reach: `dim_alpha` looked dead on busy overlays and the redirected curtain blurred the (now custom) wallpaper image. **v1.2.8** keeps the vendor's blur parameters on redirected blocks and scales both literals of the vendor's own binding (digit-preserving `? 0.d : 0.d`), plus 1% slider granularity; the one cloud veil stays vendor-owned (qrc entries removed from `Sn3DCloudDisk.dll` - not a mod bug) |
| markers, scan side | `?putFrameDataIntoFile@HandlePrjFile@@…` moved **with the same mangled name** from `scanservice.exe` (shrunk to a 69 KB stub) into `Sn3DFile.dll`; the frame pipeline reaches it through that DLL | the module resolves the export by name in the loaded modules of pipeline processes - no change needed |
| markers, render side | `librenderkit.extensions.digit.dll` rebuilt, but the color-slot stub signature is unique (`renderer+0x130320`) and the slot qword is present in `.rdata` (`renderer+0x28CAA8`); osg exports intact; binding chain offsets (`node+0x150` → mix, `+0x138` → geometry, `+0x220`/`+0x248`) unchanged | live: `color slot hooked`, bindings resolve, quality coloring confirmed in a real scan |

## A self-inflicted failure worth documenting (deploy hygiene)

Right after the update the render-side recolor looked broken:
`color slot candidates=0 (need exactly 1)`. The slot was fine - a dev
diagnostic probe module (built into `build\modules\`, carried into the app
by a wildcard "copy everything" deploy) detours the **same anchors**: the
color-slot pointer and `setMarkerPoints`. Whichever module loads first
wins; the other one degrades exactly as designed. Production deploys (the
Mod Manager, the recipes under `tools\`) never bundle probes. The lesson
is recorded here and in `tools\probe_on.ps1`: keep deploy lists explicit,
and never leave a probe next to an enabled `marker_quality`.

## Scanner firmware

The updated software also offers a scanner firmware upgrade (TX2 image
`tx2_1.63_…` → `tx2_1.71_…`, shipped under `fabu_UPDATE\Configure\` in the
app directory). The mods are firmware-agnostic: after the device rebooted
with V1.71 every module behaved identically. Firmware images are not part
of this repository.

## Conclusion

The name/pattern/content anchor policy paid off again: a large vendor
rebuild (7 changed binaries, hundreds of shifted offsets) required **no
code change in six of the seven modules**. The exception is the honest
limit of the pattern approach: popup_dim's edits are anchored to QML
*literals*, and the vendor moved the curtain opacity into a runtime
binding - invisible to dry-run matching (the walker stayed green) yet
behaviorally dead. v1.2.8 anchors to the new binding instead. The only
manual step remains re-placing the user's wallpaper file, which the
updater wipes by design (it lives inside the app directory).
