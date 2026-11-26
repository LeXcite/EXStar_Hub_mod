# Batch status journal 2026-11-17 — fps / laser50 (+captions) / fitfill

Chronicle of a single sitting: deploys, catching the caption bug, merging the
modules, final code revision. RE protocols — in `scanpage-qml-resources.md`,
`laser-plane-ids.md`, `zoom-fit-optimization.md`; product cards —
the modules' READMEs. Everything released here was verified live on
2.x (file stamp 3.1.x) that same evening; into git — upon "put it in git".

---

## 1. Deploy recipe (the repeatable part)

Edits in `C:\Program Files\Shining3d\EXStar Hub\modules\` require elevation:

1. kill `EXStar Hub.exe`, `scanservice.exe`, `scanhub.exe` (otherwise the DLL is locked);
2. write a runner script to temp (UTF-8 BOM), `Start-Process powershell
   -Verb RunAs -Wait` — inside it: copying the DLL/ini, a marker file `OK`/`ERROR`
   to temp for checking the result;
3. **the first UAC attempt may silently fail** (the marker file did not appear
   — happened twice) → simply retry;
4. delete `%LOCALAPPDATA%\ExStarHubMods\mod.log`, start the application,
   read the log after ~50 s.

`tools\new3_deploy.ps1` — the same recipe plus cleanup of leftovers from the
retired standalone module `captions` (dll/ini/ini section/whitelist).

## 2. Laser: lines (laser50 v1.0.0)

- hook by export name `?set_match_plane_id_vulkan@...` (algorithmLzy.dll) —
  in two sibling processes (`Sn3dprocessManager.exe`, plus a second UI process);
- trace `call N: (3,21,3,21) -> remap` (×4 per scan session) + user:
  **"25×25 lines there"** — goal reached, the 7-line mode untouched.

## 3. Captions: the v1.0.0 bug and its investigation

The standalone module `captions` v1.0.0 rewrote the button string by
replacing the translatable source `qsTr("%1线")` itself with the Russian
plural "lines" caption while hard-appending "38". On the user's English
UI, that raw Russian caption appeared verbatim.

Breakdown (from a dump of the qrc record `ScanSettingModelProviderLaser` and a screenshot):

1. the vendor source is Chinese (`%1线`), the EN `.qm` catalog translates it into
   `%1 lines` — **the catalog lookup key = the source text**. Replacing
   the source killed the catalog hit: the raw replacement leaked out;
2. the number came from the model (`crossLaserLineShowNum` = 39), the target suffix "50"
   never worked anyway.

Fix v1.1.0: touch **only the argument** — `.arg($scanSettingModelLaser.
crossLaserLineShowNum)` → `.arg(50)`. The key stays intact, the language is preserved, "7 lines"
untouched. Live: `packed 9777<-9587, 1 caption(s) rewritten (ok)` =
dry-run `replace_lab` byte for byte; user's screenshot: "50 lines /
7 lines". The rule is recorded in the README and `scanpage-qml-resources.md` §5.

## 4. Merging captions → laser50 v1.1.0 (decision)

The "50" caption without a real 50 lines is a promise; lines labeled "39" —
a lie. A separate switch allowed exactly those intermediate states,
so this is one feature: module `laser50`, sub-switch
`captions = 1`. The halves are process-independent ([lines] is fed by
`algorithmLzy.dll`, [caption] by `ScanPage_E7.dll`; in a foreign process
they stay silent; caption stops querying the target after 2 min so that scan
workers don't spam. The standalone module was removed completely: files, manager card,
ini section, whitelist, mentions in the docs (ROADMAP item 4 is a deliberate
"merged" entry with the lessons).

## 5. The rest of the batch's features — the user's live verdict

| feature | confirmation |
|---|---|
| fps v1.0.0 (qrc insert `exFpsLabel`) | log `raw 137735->138526, packed 22753<-22610 (ok)` = dry-run; "**I can see the fps**" |
| fitfill v1.0.0 (forcing `setIsZoomOptimization` = 0) | `hooked ... forcing 0` + forwards ×5; "**Zoom to Fit works**" |
| wallpaper, popup_dim, marker_* | live-verified earlier (14.11 and before), untouched in this batch |

Side fix of the batch: the host's ini parser now understands quotes —
`color = "#8ecbff"` arrives in full (`#` cuts a comment only outside
`"..."`). Without it the fps module got an empty color.

## 6. Final code revision (same day)

Rule: **no edits that could break live-verified behavior for the sake of
style**; the check is the fresh log matching the previous one byte for byte.

- extracted into shared header-only helpers: all the qrc machinery
  (`include\exstar_qrc.h`: zlib framing, the single safe write path,
  section scan, map notification — it had been duplicated in fps and
  laser50, ~220 lines removed) and unique export lookup by name
  (`include\exstar_export.h`; used by laser50 and fitfill). Log line
  formats preserved verbatim — the READMEs/artifacts quote them;
- `popup_dim` deliberately NOT refactored (its own equal-length variant, stable
  v1.2.7 — working code is not up for style edits);
- `tools\fps_lab.c` deleted: `replace_lab` reproduced its prediction for
  fps byte for byte (stock 22392 → patched 22610 ≤ 22753) — one dry-run
  harness for all qrc edits;
- removed intermediate manager releases 1.3.3–1.3.6 and the stale
  `build\captions.dll`; later the same day the decision was made to move all
  EXEs out of git into GitHub Releases (see §7) — `release/` keeps only up to the current
  1.3.7;
- `examples\template.c` brought up to the SDK (it was missing `EX_EXPORT` and `DllMain` —
  the starter wouldn't build into a working module);
- statuses aligned with the facts: fps/fitfill/laser50 → ✅ live-verified;
  `.gitignore` +`Artboard 1.jpg`; removed the duplicate ROADMAP heading and
  the unused counter `g_remaps`.

Control run after the refactoring (application on the new DLLs): `fps ...
22753<-22610 (ok)`, `caption ... 9777<-9587 ... (ok)`, `laser50: hooked ...
strict=1`, `fitfill: ... forwarding 0` ×5 — identical to the pre-revision one.

## 7. Result and release boundaries

Ready to commit: modules fps/laser50/fitfill, `include\*.h`, `src\host.c`
(+ ini quotes), `src\manager.c` (1.3.7, 7 cards), `build.ps1`, docs,
`tools\` (replace_lab, new3_deploy, zrc_lab), `.gitignore`. Not published:
the community patchset (gitignored), screenshots/`Artboard 1.jpg`.
EXEs are no longer committed to the repository at all (including historical ones —
untracked): binaries are published only as GitHub Releases assets,
`release\` in git keeps only one `HOWTO-INSTALL.txt` pointing to the Releases
link.
