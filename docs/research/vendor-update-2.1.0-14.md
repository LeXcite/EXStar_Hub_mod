# Vendor update 2.1.0-14 (file stamp still 3.1.49): impact report

Date: 2026-11-27. Vendor release: **2.1.0-14**, installer
`EXStar Hub_v2.1.0-14.exe` (1 686 680 416 B, sha256
`08F3E6B6E5023D560C14E5B47216551D5DA3CE41276A5E20E71ACB15794A0DB2`, valid
Shining 3D signature, `FileVersion 2.1.0-14`, release date 2026-11-25 per its own
`feedback.ini`).

Outcome (offline): **no module and no manager component has to change.** Every
vendor fact the mods anchor to is bit-identical between 2.1.0-10 and 2.1.0-14,
proven offline on the installer payload before the update is installed. Nothing
was installed, nothing in the running application was touched, no vendor file
was modified.

Outcome (live, same day, after the update was installed): the prediction held
for the eight surviving modules - all armed and hooked on the first start, no
code change. It did **not** hold for `fps`: the same insert that works on
2.1.0-10 makes the new hub close itself a few seconds into the scan page, and
the module was retired with manager 1.7.1. The live pass, the bisect that
pinned it, and two facts an offline pass cannot see are at the end of this
file.

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
| fps 1.0.0 | `/// ScanPage_E7` anchor, `id: root` uniqueness, compressed size must fit | `ScanPage_E7` resources identical at the same offsets, literal counts equal; the patch itself still applied `(ok)` in the live pass | **retired**: with only this module enabled the hub closed itself a few seconds into the scan page (see the live pass below) |
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

**Result: the layout survived.** That merge happened on 2026-11-27 14:53 in the
first session after the update (`self at #1`, `0 unreadable` of 3 items, two
eyes hidden → two `SKIP`, mesh finished). See the live pass below.

## Deploy steps for the update

1. Install 2.1.0-14 the normal way (the installer is signed; the payload was
   verified offline, running it is not needed for anything in this report).
2. The update **replaces the application directory**: after installing 2.1.0-14,
   every file of this framework was gone (`winmm.dll`, `winmm_real.dll`,
   `ExStarRec.dll`, `modules\`, `modules.ini`, `exstar_mods.state`), while the
   application re-created its own `setting.ini` and desktop `.url` on the first
   start. "The payload ships none of our files" therefore only means nothing
   *overwrites* them - nothing **survives** either. Copy `modules\*.ini` (the
   tuning) if it matters, then re-run the Mod Manager and re-place
   `modules\wallpaper.jpg` through its *Choose file…* button.
3. Verification pass (10 minutes, all read-only for the vendor):
   - start a scan, confirm the popup blur/dim and the wallpaper (popup_dim,
     wallpaper), and the enlarged in-frame marker
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

## Live pass (2026-11-27, 2.1.0-14 installed)

The update was installed over 2.1.0-10 into `C:\Program Files\Shining3D\EXStar
Hub`. Everything below was measured after it; the offline half of this file was
not edited to fit the results, only annotated.

**The installed tree is the payload.** `tools\nsis_payload.py verify` against
the index built from the installer: **2993 of 2997** installed files are payload
records (3122.6 MB). The four that are not: `setting.ini` and `EXStar Hub.url`
(not shipped, the application writes them), plus two payload entries the reader
could not pair here (`QtQuick\Controls\Styles\Flat\plugins.qmltypes`,
`syncservice\putty\nrfutil\nrfutil`) - the same two that did not pair on the
2.1.0-10 baseline either. Spot-checking the 18 binaries that carry an anchor of
this project (`AppUi`, `ScanPage_E7`, `Sn3DMultiProjects`, `Sn3DSpeckleNew`,
`algorithmLzy`, `librenderkit`, `librenderkit.extensions.digit`,
`sn3d.digit.scenecontrol`, `Sn3D\Navigation\pluginSn3DNavigation`,
`Sn3DCloudDisk`, `Sn3DFile`, `Qt5Core`, `Qt5Gui`, `Qt5Quick`, `zlib1`,
`osg158-osg`, `CompositeComponents`, `EXStar Hub.exe`) by size + sha256 against
the payload: **18 of 18 identical**, so the offline anchor table applies to the
files that are actually loaded.

**An update deletes the framework.** After the setup finished, `winmm.dll`,
`winmm_real.dll`, `ExStarRec.dll`, the whole `modules\` folder, `modules.ini`
and `exstar_mods.state` were gone, and the application had already written its
own `setting.ini`. The offline statement "the payload ships none of the mod
files" is true and useless in the same breath: nothing overwrites our files
because the whole directory is replaced. Nothing of the framework survives an
update - a re-run of the Mod Manager is part of updating, and the tuning lives
in `modules\*.ini`, which the manager recreates from templates, so a copy of
those nine small files is worth keeping (the backup taken before this install
made the whole restore a one-minute job: the module DLLs come from the manager,
the ini files came back byte-identical).

**Every surviving module anchored on the first start** (`mod.log`, pid 4584):

| module | the line that proves it |
|---|---|
| popup_dim | `ScanPage_E7.dll zrc zlib=1 cand=73 qml=6 rewritten=5 blocks=5 verified=5 restored=0 fault=0`, `AppUi.dll ... rewritten=5 blocks=6 verified=5`, `pluginSn3DNavigation.dll ... rewritten=2 verified=2`, `CompositeComponents.dll ... rewritten=1 verified=1` |
| gpu_gate | `Sn3DSpeckleNew.dll gate relaxed at RVA 0x1AB6B5 (jne -> ja ...)` - the RVA the offline check predicted |
| laser50 | `hooked ?set_match_plane_id_vulkan@Sn3DAlgorithm@@... (target algorithmLzy.dll): strict=1 (3,21,3,21) -> (0,24,0,24)` |
| fitfill | `hooked ?setIsZoomOptimization@CustomizeManipulator@@QEAAX_N@Z (target librenderkit.dll), forcing value 0`, and it fires: `fitfill: setIsZoomOptimization(1) -> forwarding 0` |
| marker_quality | `mq: color stub at renderer+0x130320`, `mq: color slot at renderer+0x28CAE0 -> stub`, `mq: color slot hooked`, `mq: install done ... qt=1 put=1 smp=1 slot=1` - the stub RVA and the `.rdata` slot are exactly the pair the offline check measured on the new build |
| marker_inframe | `armed, scale=1.20 ...`, then `calls=294553 ... draws=885208` - the color path is being walked |
| selective_mesh | `hooked HandlePrjFile::isGenerateCloud (active for Sn3DMultiProjects.dll callers)` in the hub, the process manager, the scan service and the net server; `eye watch armed` |
| wallpaper | `no ...\modules\wallpaper.jpg - stock wallpaper kept` → `module 'wallpaper' declined to start`: correct fail-open, the image is a user file and the update removed it. *Choose file…* puts it back |

`host: startup complete, 8 module(s) loaded, 7 started` - the one that did not
start is wallpaper, for that reason.

### The merge item layout survived - the last unknown is closed

The one thing the offline pass could not prove (see "The one thing to watch"
above) answered itself during the same session, in a real merge at
14:53:14 → 14:53:28, with `verbose = 1` already on. The user hid two of three
eyes, entered *Post-process*, and the module's decisions were:

```
eye-meta invoke idx=8 via ScanPage_E7.dll+0x3A521
eye C991B984-DEDC-4D9D-90E6-1D5DCED63611 -> hidden          (14:53:14)
eye 9927D724-5E51-4D0F-82F4-6D580312DCFC -> hidden          (14:53:16)
gate uuid=6D894BEF-...  ra=+0x7E3BB  vendor=1  ...  -> include
all-hidden guard: 3 scans in the merge list (00000146D996BDB0, reg#9, self at #1, 0 unreadable) - at least one stays in
gate uuid=C991B984-...  ra=+0x7E3BB  vendor=1  eye icon hidden  -> SKIP
gate uuid=9927D724-...  ra=+0x7E3BB  vendor=1  eye icon hidden  -> SKIP
gate ... ra=+0x83097 ...  (second pass: same three answers)
```

and the vendor log of that merge says `setBuilding building true` at 14:53:20.8
and `onStateChanged state "meshFinished"` at 14:53:28.3: the post-process ran to
completion with exactly the two hidden scans left out.

What this proves, in the module's own terms: `walkMergeList` accepted register
**r9** as this merge's list only because it found the scan being asked about
inside it (`item+0x28 == self`, reported as **`self at #1`**), and it read all
**3 items with 0 unreadable scan ids**. So all three offsets of the recompiled
`Sn3DMultiProjects.dll` - `self+0x18`, `item+0x28`, `item+0x38` - are unchanged.
`merge_list.h` needs no fresh dump; the guard's decode half works on 2.1.0-14.
The guard's *fire* branch (every eye hidden → ignore the exclusions) was already
live-verified on 2.1.0-10 on 2026-11-26 and was correctly **not** taken here,
because the third scan stayed in. This was also the first live run of the 1.8.0
eye-only interface: eye click → `[uuid] included = 0` in the shared session ini
→ the merge caller's `isGenerateCloud` answer.

One honest caveat about the log line quoted above: for the scan that stayed in,
the printed reason was `scan id unreadable (fail-open)` although its uuid was
readable (`gate uuid=6D894BEF-...` prints it). That is a wording defect in the
reason table - `why` starts at `WHY_NOID` and is only overwritten for a hidden
scan, so "a scan nothing said anything about" is reported with the same label as
"a scan whose id could not be read". The decision is right in both cases
(`-> include`), only the label is wrong; recorded in `docs/ROADMAP.md`.

### fps closed the hub, and only fps

The full nine-module set made the hub vanish ~10-20 s after launch, with no WER
crash record and no error line in the vendor log, so it was bisected by
flipping `modules.ini` and launching once per configuration:

| enabled | hub alive after 75 s |
|---|---|
| (none) | yes |
| all nine | **no** |
| marker_inframe, marker_quality, popup_dim, fps, gpu_gate | **no** |
| wallpaper, laser50, fitfill, selective_mesh | yes |
| popup_dim | yes |
| gpu_gate | yes |
| marker_quality | yes |
| marker_inframe | yes |
| fps | **no** |

The failing runs all end at the same place in the vendor log:
`ScanGuideController.qml:94 unblockNav [postProcessGuide][unblock] SKIP(not
active)` at ~1207 messages; a healthy run continues from that exact message
with `ResolutionCompA.qml:30`, `CutFaceCreationPage.qml:50`, `ClipPlane.qml:405`
and `ScanSceneView_E7.qml:71 setCurrentEnvironment begin Scan` - the scan page
is being instantiated.

The module's own work looks perfect right up to that point:
`fps: ScanPage_E7.dll stream @0x98DC88 raw 138401->139192, packed 22875<-22723
(ok)` - verify-by-reinflate passed. And the target did not move: **all 70 zlib
records inside `ScanPage_E7.dll` are byte-identical between 2.1.0-10 and
2.1.0-14**, including the scan page record with the `/// ScanPage_E7` anchor at
the same file offset, the anchor at the same inflated offset (455) and `id:
root` at 484. The insert is therefore the same 791 bytes in the same place that
rendered correctly on 2.1.0-10 and on 2.0.0-45; what changed is how the rebuilt
page consumes it, and that is a runtime property no offline comparison can see.

The module was retired rather than debugged (owner's decision: the readout is
not worth a hub that closes itself). Manager 1.7.1 carries `fps` in its legacy
list, so an existing installation loses `modules\fps.dll`, `modules\fps.ini`
and the `[fps]` section on the next install - the host enables every DLL it
finds by default, so leaving the file behind would have kept the failure. The
mechanism it proved stays documented in
[scanpage-qml-resources.md](scanpage-qml-resources.md), which laser50's caption
half still uses; the sources and the module README are in the git history
(manager ≤ 1.7.0).

## Conclusion

Offline, 2.1.0-14 is a re-sign plus a small feature increment: the vendor
touched five binaries' code, and none of the five contains a byte this project's
anchors rest on that moved. The name/pattern/content anchor policy needed no
code change - eight of nine modules armed and hooked on the first live start,
including the fitfill anchor that has been waiting for a live confirmation
since 2026-11-19.

The update taught two things the offline pass cannot teach: a vendor update
**replaces the application directory**, so redeploying is part of updating; and
an in-memory **QML insert is a runtime contract** - byte-identical resources do
not guarantee the patched page still loads, which is how fps went.

Both assumptions that were only provable against a running process came back
resolved: fitfill's new anchor fires, and the merge item layout in
`Sn3DMultiProjects.dll` decodes exactly as before (3 items, `self at #1`,
0 unreadable, two hidden eyes honoured, mesh finished). Nothing about this
update is left unverified except the two things a machine cannot decide for
itself: whether *Zoom to Fit* now visibly fills the frame, and whether the
wallpaper is back in place after the update deleted it.
