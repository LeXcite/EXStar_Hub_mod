# Vendor update 2.1.0-14 (file stamp still 3.1.49): offline impact report

Date: 2026-11-27. Vendor release: **2.1.0-14**, installer
`EXStar Hub_v2.1.0-14.exe` (1 686 680 416 B, sha256
`08F3E6B6E5023D560C14E5B47216551D5DA3CE41276A5E20E71ACB15794A0DB2`, valid
Shining 3D signature, `FileVersion 2.1.0-14`, release date 2026-11-25 per its own
`feedback.ini`).

Outcome: **no module and no manager component has to change.** Every vendor
fact the mods anchor to is bit-identical between 2.1.0-10 and 2.1.0-14, proven
offline on the installer payload before the update is installed. Nothing was
installed, nothing in the running application was touched, no vendor file was
modified.

## How this was checked (and why the numbers can be trusted)

The setup is a forked NSIS build: the archive after its first header is not a
7-Zip/zip/cab container and no archiver opens it, so the payload is read by
`tools\nsis_payload.py` (container format documented in that file). The reader
is gated on ground truth, not on a reading of the spec:

```
python tools\nsis_payload.py index  <setup.exe> <index.json>
python tools\nsis_payload.py verify <index.json> "<app dir>"
```

Against the installed 2.1.0-10 tree: **2992 of its 3020 files (2594 MB) are
payload records with the same size and the same sha256**; the 28 that are not are
exactly the files this project puts there (`winmm.dll`, `winmm_real.dll`,
`ExStarRec.dll`, `modules\*.dll|ini`, `modules.ini`, `setting.ini`,
`exstar_mods.state`, `EXStar Hub.url`) plus three user-side leftovers. A reader
that reproduces an installed tree byte for byte is trusted for a tree that is not
installed yet.

Payload size: 2764 records / 3120.7 MB of plaintext in 2.1.0-10, 2765 records /
3122.6 MB in 2.1.0-14; both walks end 36 bytes before the trailer the header
declares, so no record is missed on either side.

Then, per file: extract the record out of both payloads (records are
self-contained, so one DLL is read without touching the rest of 1.6 GB), verify
each against the hash the walk recorded, and compare.

## What the update actually is

| scope | result |
|---|---|
| named payload files | 1779: **1073 identical**, 705 rebuilt, 1 new (`cryptopp.dll`), 0 removed |
| `dll`/`exe` names | 710: 6 identical (installer helpers: `7z.exe`, `uninst.exe`, the four VC runtimes), **703 rebuilt** |
| text configuration | 52 names, **2 changed**: `feedback.ini` (`version=2.1.0-10` -> `2.1.0-14`, `releaseDate=2026-11-11` -> `2026-11-25`) and `config.db` (294 912 B on both sides, no `SQLite format 3` header: an encrypted store, consistent with the new `cryptopp.dll`) |
| scanner side | the OTA image `tx2_1.71_K0001_20261102_200818_update_s.img` (184 270 848 B) is **byte-identical**, so the device image did not change; `app_tx2` itself is not part of the Windows payload |
| product vs stamp | the product number moves to 2.1.0-14, the **file stamp of `EXStar Hub.exe` stays `3, 1, 49`** (File and Product version), as it did across 2.0.0-45 -> 2.1.0-10 |

"Rebuilt" is not the same as "changed": most of the 703 differ only in the Rich
header, the version resource and the Authenticode trailer. Classifying the 25
binaries the mods actually touch by *where* the bytes differ:

| class | files | meaning for an anchor |
|---|---|---|
| header/signature only | `Qt5Core` `Qt5Gui` `Qt5Network` `Qt5Qml` `Qt5Quick` `Qt5Widgets` `zlib1` `zlib` `osg158-osg` `qtquickcontrols2plugin` | content is byte-identical; nothing can have moved |
| `.rdata`/`.rsrc` only (4-518 B), `.text` byte-identical | `AppUi` `ScanPage_E7` `Sn3DFile` `sn3d.digit.scenecontrol` `CompositeComponents` `pluginSn3DNavigation` `Sn3DCloudDisk` `EXStar Hub.exe` `scanservice.exe` `Sn3DprocessManager.exe` | the changed bytes are the compiler's guarded-call target table (4-byte entries) and the version resource; code, strings and resource records did not move |
| code rebuilt | `algorithmLzy` (4.6 MB of `.text`), `librenderkit` (1.57 MB), `librenderkit.extensions.digit` (2.9 KB), `Sn3DMultiProjects` (0.88 MB), `Sn3DSpeckleNew` (3.4 KB) | every signature and layout assumption inside these has to be re-checked by hand - done below |

The compressed UI resources (`[u32 BE raw size][zlib]` records - the container
`popup_dim`, `fps` and `laser50` read) are **identical, record for record, at the
same file offsets** in `AppUi.dll` (42 records, 1 159 793 B), `ScanPage_E7.dll`
(53 / 842 178 B), `CompositeComponents.dll` (141 / 2 219 341 B),
`pluginSn3DNavigation.dll` (34 / 411 705 B), `Sn3DCloudDisk.dll`,
`sn3d.digit.scenecontrol.dll`, `Qt5Gui.dll`, `Qt5Quick.dll` and
`Sn3DprocessManager.exe` (hash of the whole inflated text equal). The only qrc
content change anywhere is in `librenderkit.dll`: two GLSL records, +110 and +27
bytes, which after normalizing CRLF to LF are **byte-identical** - the vendor
re-saved shader source with Windows line endings.

## Per module

Anchor state = the same measured value in both builds. "Silent skip" is the only
failure mode a name/pattern anchor has, so each row states what would have to be
true for that skip, and the evidence that it is not.

| module | vendor anchors | 2.1.0-14 state | verdict |
|---|---|---|---|
| popup_dim 1.2.8 | QML literals inside qrc records of `AppUi`, `CompositeComponents`, `ScanPage_E7`, `pluginSn3DNavigation`; re-deflate must still fit | records identical at the same offsets (above); offline replay with the shipped harness `tools\dim_dryrun.c` gives byte-for-byte the same output on both: `AppUi zlib streams=5 blocks=6 too_big=0`, `CompositeComponents 1/1/0`, navigation plugin `2/2/0`, `ScanPage_E7 5/5/0`, `Sn3DCloudDisk 0/0/0` | unaffected |
| wallpaper 1.0.0 | largest embedded JPEG slot in `AppUi.dll` >= `min_slot` 300000 | slot 837 517 B at file offset 10 056 947 on both sides; `AppUi` `.text` identical | unaffected |
| fps 1.0.0 | `/// ScanPage_E7` anchor, `id: root` uniqueness, compressed size must fit | `ScanPage_E7` resources identical at the same offsets, literal counts equal | unaffected |
| laser50 1.1.0 | export name matching `set_match_plane_id_vulkan@` in `algorithmLzy.dll` (must be unique), qrc caption record in `ScanPage_E7` | export present exactly once in both, although `algorithmLzy` was rebuilt; `ScanSettingModelProviderLaser` / `crossLaserLineShowNum` literals equal | unaffected |
| fitfill 1.1.0 | unique exports `?setIsZoomOptimization@CustomizeManipulator@@` (`librenderkit`) and `?setEnableScanScale@RenderKitExtDigitSingleton@` (`librenderkit.extensions.digit`); `sn3d.digit.scenecontrol` must still import the latter and its call-site windows must look the same | all four hold; the import is present in both, and both byte windows are at identical addresses (`mov rcx,rax; xor edx,edx; call [IAT]` 10 hits from RVA 0x49BD2, `mov rcx,rax; mov dl,imm` 14 hits from RVA 0x1BA01) in both builds | unaffected |
| gpu_gate 1.0.0 | one byte pattern `8B 85 00 03 00 00 83 F8 02 0F 85` in `Sn3DSpeckleNew.dll`, exactly one occurrence | exactly one, at RVA 0x1AB6B5 in both; the rebuilt parts of that file's `.text` are two regions around 0x121900 and 0x138700, far from the gate | unaffected |
| marker_quality 1.0.2 | color-slot stub signature `48 83 C1 08 E9 77 ED FF FF`, exactly one occurrence; `?putFrameDataIntoFile@HandlePrjFile@@...` and `?setMarkerPoints@ModelNode@ExtDigit@RenderKit@Sn3D@@...` exports; osg and Qt export names; object layouts (`node+0x150`, `+0x138`, `geom+0x220`, `+0x248`), Qt 5.15.10 `QMap`/`QList`/`QVariant` | signature exactly once, at RVA 0x130320 on both sides - the very address the 2.1.0-10 note records, and since the pattern *contains* the relative jump displacement, an identical match also proves the stub target did not move; both exports unique; the module's second precondition - exactly one `.rdata` qword pointing at the stub - also holds on both (one at `0x28CAA8` in the old build, one at `0x28CAE0` in the new one, the shift being the 57-byte growth of the section ahead of it), so the slot swap cannot land on a stale entry; `osg158-osg.dll` and every Qt DLL are content-identical, so neither the osg API nor the container layouts changed | unaffected |
| marker_inframe 1.1.0 | `frontInnerColor` / `frontExternalColor` literals, osg exports | literal counts equal; osg identical | unaffected |
| selective_mesh 1.8.0 | `?isGenerateCloud@HandlePrjFile@@QEAA_NXZ` export (`Sn3DFile`), QMetaObject strings `ProjectListControllerE7` / `setNodeVisible` (`ScanPage_E7`), scene command names `SetModelsVisible` / `SetModelNodesVisible` / `GetCurrentEnv` (`sn3d.digit.scenecontrol`), and *content*-based reading of the merge list | all present and in unchanged bytes; the all-hidden guard finds the list by testing that one of its items *is* the scan being asked about, so it does not depend on the offsets the decompiler showed (`[param_1+0x70]`) | unaffected, with one item to watch (below) |
| host `ExStarRec.dll` | `zlib1.dll` exports `inflate*`/`deflate*` used to re-pack qrc records | `zlib1.dll` content identical, exports and `FileVersion 1.2.11` equal | unaffected |
| manager 1.7.0 | compatibility stamp `KNOWN_GOOD_STAMP L"3.1."` on `EXStar Hub.exe`; the injection chain (something loaded early must import `winmm`); no vendor file may collide with the mod files | stamp still `3, 1, 49`, so the gate passes unchanged (the product number is only displayed); `Qt5Core.dll` is content-identical and still imports `WINMM.dll` with `timeSetEvent`/`timeKillEvent`; the payload of both builds ships **none** of `winmm.dll`, `winmm_real.dll`, `ExStarRec.dll`, `modules.ini`, `setting.ini`, `exstar_mods.state`, `modules\*` | unaffected |

73 individual anchor checks (exports, imports, byte patterns with their
addresses, literals in raw and inflated form, the JPEG slot, zlib exports and
version resources) were run over the two builds: **73 equal, 0 differing.**

The 5 genuinely rebuilt binaries also say what the vendor worked on, which is the
other half of "will this break a behaviour a mod leans on":
`Sn3DMultiProjects` gained `projectRenamed` and an `IRSpeckleRebuildProcessController`;
`librenderkit` now calls `QPainter`/`QBrush` where it used `osgText`;
`Sn3DSpeckleNew` changed two small code regions; nothing in that list touches a
hook point of this project.

## The one thing to watch: selective_mesh object offsets

Two of the layouts `selective_mesh` reads live in a file that really was
recompiled: the merge item it is handed (`self+0x18` -> implementation,
item `+0x28` -> `HandlePrjFile*`, item `+0x38` -> the scan `QUuid`) belongs to
`Sn3DMultiProjects.dll`, whose `.text` changed by 875 KB (and whose `.qtmetad`
changed too, i.e. a meta object was edited). An object layout cannot be proven
from a file the way a byte pattern can - it is only proven by a live dump.

The failure mode is safe by construction: the guard box-reads at most 11
candidate registers and `ML_MAX` items, requires `item+0x28 == self` before it
trusts anything, and treats an unreadable id as *visible*, i.e. stock behaviour.
Worst case, an "all eyes hidden" merge behaves like stock again (the vendor's own
stall at ~80 % of the Processing bar, documented in
`merge-common-mesh.md` section 20) - no wrong mesh, no hang, nothing written.

Action after installing: one merge with `verbose=1` and a single eye open. The
log lines that must appear are the same ones the 1.7.2 live run produced
(`merge list: reg ... n ... idx ...`). If they appear, the layout survived; if
the guard reports "nothing readable", the offsets moved and `merge_list.h` needs
a fresh dump - nothing else in the module.

## Deploy steps for the update

1. Install 2.1.0-14 the normal way (the installer is signed; the payload was
   verified offline, running it is not needed for anything in this report).
2. The payload ships none of the mod files, so they survive an in-place update -
   but the previous update wiped a user file inside the app directory
   (`modules\wallpaper.jpg`), so re-check that file and re-run the Mod Manager
   afterwards to make sure every wanted module is still enabled.
3. Verification pass (10 minutes, all read-only for the vendor):
   - start a scan, confirm the popup blur/dim and the wallpaper (popup_dim,
     wallpaper), the FPS readout (fps) and the enlarged in-frame marker
     (marker_inframe);
   - one scan with markers to see quality coloring (marker_quality) - remember
     that only one module may own the color slot, so keep `marker_probe` off;
   - `50 lines` caption and 25+25 lines (laser50);
   - Ctrl+D re-fills the frame (fitfill - note this is still the open item from
     2026-11-19: the anchor is in place, a live confirmation is owed);
   - start on an iGPU machine (gpu_gate), and one merge with a single eye open
     (selective_mesh, see above).
4. Nothing to rebuild. If a live check does fail, the module rebuild is enough:
   the anchors themselves were proven equal, so a failure would mean a layout,
   not a pattern.

## Notes on tooling

- `tools\nsis_payload.py` is new: the reader described above, with `index`,
  `list`, `extract`, `verify`. Without it the documented "unpack the update"
  step is impossible for anybody but the vendor.
- `tools\wp_dryrun.c` did not compile (it used `DWORD` without `windows.h`) and
  its known-good constant still pointed at the pre-2.1.0-10 slot
  (`0x643F95`). Both fixed; run against both payloads it now reports the same
  single candidate on each side: `SOI @0x9974F3 slot=837517  MATCH known-good`.
- The analysis artifacts (record indexes, per-file classification, anchor
  reports) were kept out of the repository on purpose: they are ~3 GB of vendor
  material. The commands to regenerate them are in this file.

## Conclusion

2.1.0-14 is a re-sign plus a small feature increment: the vendor touched five
binaries' code, and none of the five contains a byte this project's anchors rest
on that moved. The name/pattern/content anchor policy again needed no code
change - the honest residual risk is the one class of assumption that is only
provable against a running process (the merge item layout in
`Sn3DMultiProjects.dll`), and that path is bounded, read-only and fails open to
stock behaviour.
