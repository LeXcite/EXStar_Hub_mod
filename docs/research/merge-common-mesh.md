# Common mesh: how scans are selected, and how `merge_select` steers it

Research + design record for the `merge_select` module (manager 1.5.0).
All RVAs are for **EXStar Hub 2.1.0-10** (image base 0x180000000); the
vendor files are unmodified - evidence comes from static analysis of the
installed binaries (Ghidra/REA) plus one reconstructed community
patchset used as a map, not as code.

## 1. The vendor merge path (verified statically)

A project holds several scans; each scan has a project-file object
`HandlePrjFile` (class exported by `Sn3DFile.dll`). One exported method
decides whether the scan's point cloud exists for downstream use:

```
?isGenerateCloud@HandlePrjFile@@QEAA_NXZ   ; bool __thiscall(self)
```

Verified facts on 2.1.0-10:

* the import name string appears in exactly two binaries: `Sn3DFile.dll`
  (its export table) and `Sn3DMultiProjects.dll` (its import table) -
  i.e. **only Sn3DMultiProjects.dll ever asks the question**;
* the import thunk/IAT slot: `Sn3DMultiProjects.dll` RVA **0x1A0738**
  (descriptor "Sn3DFile.dll");
* the common-mesh collection is a large function starting at RVA
  **0x7DEF0**; it loops over the project's scans, and calls
  `[rip+…] -> 0x1A0738` per scan at RVA **0x7E3B5** and again at RVA
  **0x83091** (two mesh-generation variants: the second one feeds the
  `sendPrjsMeshDataToFZ` command path with `uuidList`/`meshList`/
  `rotList`/`tranList`/`markersList`/`textureFlags`, reads
  `keepPostMesh`, and publishes through `CommonLib::Sn3DQObject::publish`);
* a false answer jumps over the scan's append block - the scan is
  simply not part of the merge input ("pathlist" in the debug strings);
  the vendor uses this same branch for scans whose "generate a point
  cloud" project option is off.

Consequence: answering `false` for one scan removes it from the common
mesh **only** - stored scan data, display, per-scan export and the rest
of the UI keep the stock behaviour, because none of the other code
queries the flag (it has no other importer).

## 2. Scan identity for a human-editable selection

`HandlePrjFile` additionally exports:

```
?getFolderPath@HandlePrjFile@@QEAA?AVQString@@XZ    ; QString folder path
?getProjFileFullName@HandlePrjFile@@QEAA?AVQString@@XZ
```

On disk a project is `<root>\<Project_N>\Scan_N\…` with the scan's
`.ccf` inside the scan folder, so `getFolderPath()` yields exactly the
name the tree shows (`Project_N` / `Scan_N`). The module therefore uses
the folder path as the selection key (full path first, then
`<parent>\<leaf>`), which needs no UUID knowledge.

Raw UUID note (not used by the module): the object also keeps the scan
UUID as 16 raw bytes at offset **+0x38**, serialized in mixed-endian
order `{3,2,1,0,5,4,7,6,8..15}` (the byte-order permutation and offset
are taken from the community patchset's cave, which decodes them to a
hex section name).

## 3. The community patchset (same feature, different mechanism)

The shared patchset (r6.1, `manifest.json` payload applied to the
**identical 2.1.0-10 binaries** - every variant `sha256` matched the
installed files) patches four sites inside `Sn3DMultiProjects.dll`
inline (5/12-byte `call`/`jmp` stubs into a new `.text` cave):

| patched RVA | original code | what the cave adds |
|---|---|---|
| 0x7E3B5 | `call [isGenerateCloud]` + `test/je` | replaces the call: reads the UUID at `self+0x38`, hex-encodes it, calls `GetPrivateProfileIntW(uuid, "included", 1, mesh-selection.ini)`; `included=0` returns sentinel 2 |
| 0x83091 | same call in the second variant | same extended call + dispatcher cave (AL==2 -> skip block with a debug log, AL==0/1 -> the vendor branches) |
| 0xAFCFC | `saveScanData` loop: `mov rcx,[rsi]; mov eax,[rcx]; test/je` | excluded scans are dropped from `successfulUuids` and the `scannerFlag` publish (RVA 0xDDDE5 site) |
| 0xDD1E5 | `lea rdx, "scannerFlag"` + `call [djz]` | same exclusion decision for the flag publish |

Its UI writes `mesh-selection.ini` from checkboxes it injects into the
Qt pages (PostProcessUI/ScanPage QML patches - disk patching, which
this project deliberately does not do), and its exporter
(`CloudExport.inc`) additionally filters/merges `.asc`/`.p3` files by
the same ini (`[ExportOptions] oneFile`).

Differences that matter here: the patchset is **version-locked byte
patching of vendor files** (dead after any vendor update), keys by
**opaque UUIDs**, and touches three DLLs. The module needs none of
that: one export detour + the folder-path key covers the merge
selection, and the two mechanisms coexist (both can only *remove*
scans, so their effects simply AND together).

## 4. The module's design decisions

* **Detour target = the export, not a call site.** `host->hook()` on
  `?isGenerateCloud@HandlePrjFile@@QEAA_NXZ` (resolved by name) covers
  both vendor call sites and any future one without RVAs anywhere.
* **Caller containment.** The detour answers differently only when the
  return address is inside the `Sn3DMultiProjects.dll` image; every
  other caller (including Sn3DFile internals during save/load) gets the
  untouched vendor answer.
* **Fail-open everywhere.** The `QString` folder query runs under SEH;
  any fault, missing export, refused hook or unreadable ini keeps the
  vendor decision. The module can only turn an explicit `included = 0`
  into a skip; an unlisted scan is provably untouched.
* **One thread owns installation.** The DLL-load notification only sets
  an event; the worker performs all MinHook work, so no hooking ever
  happens while the loader lock is held.
* **`merge-select.map.txt`** records every scan seen during mesh runs
  (timestamp, ready-to-copy section, decision) so exclusions are copy-
  paste, not path guessing.

## 5. Verification log

* 2026-11-23 03:57 (pre-refactor build): hub start in pid 34404 -
  `host: module 'merge_select' v1.0.0 … started`,
  `merge_select: hooked HandlePrjFile::isGenerateCloud` observed in the
  mod log; a second, racing install attempt (loader-notification thread)
  logged a benign `detour refused` - fixed by the single-installer
  design above;
* post-refactor restart: single `hooked …` line, no `refused`
  (see mod.log after `host: startup complete` in the same run);
* pending (needs the user): open a saved multi-scan project, run a
  common-mesh generation, add `included = 0` for one scan from
  `merge-select.map.txt`, re-generate - the excluded scan must vanish
  from the merged result while its folder stays intact.

## 6. The scan-page mesh flow and the eye icons (2026-11-24)

Two facts learned from live diagnostics (module v1.1.1/v1.1.2 probes):

* a **cached** common mesh is re-shown without any work: the
  `isGenerateCloud` gate only runs when the mesh is actually rebuilt
  (new project or after deleting the result);
* the **"Меш" flow on the scan page does not call `isGenerateCloud` at
  all** - the visible selection is applied by the scan page itself.
  Its "force-show" of every scan right after pressing Mesh also resets
  the eye icons in the tree, which is stock behaviour.

The eye icon of a scan row is a QML-callable slot in `ScanPage_E7.dll`:

```
ProjectListControllerE7::setNodeVisible(const QUuid&, bool)
```

The class is not exported, but its moc `QMetaObject` was located and
fully decoded in the 2.1.0-10 binary (verified statically with REA and
live by the module's own scan, which agrees with every pinned value):

| item | value |
|---|---|
| `staticMetaObject` | RVA **0xA38210** (Qt5 flat layout) |
| `qt_static_metacall` | RVA **0x42DF0**, at `metaobject+0x18` |
| method count / index of `setNodeVisible` | 16 / **8** (`Call=InvokeMetaCall`, `_a[1]`=`QUuid*`, `_a[2]`=`bool`) |
| slot body | RVA 0xBD380 |
| stringdata / data arrays | RVA 0x236700 (34 strings) / 0x236C10 (revision 8) |
| other slots | `onSubscribeData`(7), `tryRenameProject`(9), `delteProjects`(10), `selectableNodeClicked`(11..13), `clearSelection`(14), `nodeDoubleClicked`(15) |

Direct C++ calls to `setNodeVisible` (the mesh dialog's force-show)
bypass `qt_static_metacall`; QML eye toggles always go through it - so
hooking the metacall separates the user's real show/hide intent from
the dialog noise for free.

Scan identity for the same objects came from the vendor's own
`HandlePrjFile::getProjInfo` (Sn3DFile.dll RVA 0xDFB30): it reads the
scan NAME as an **embedded QString at `*(self+0x18)+8`** (`NAME` map
key) even while the folder/name getters return empty on live session
scans. v1.1.4+ reads that directly for the gate identity; a one-shot
`idmap` object dump backs it up if a vendor build ever moves it.

Module v1.2.0 adds the **eye watch**: a worker thread finds the
controller's QMetaObject by scanning `ScanPage_E7.dll` for a Qt5
metaobject whose className string matches (no hardcoded RVAs), verifies
the method table contains `setNodeVisible`, and detours the metacall
purely to record `uuid -> visible` plus first-seen `(Call, method)`
traffic. It never modifies anything (`eyewatch` ini switch).

Sibling-process incident (same night): enabling six modules inside
`scanservice.exe` via `sibling_all`/whitelist made it abort
(0xc0000409 in ucrtbase) - project open then failed with vendor error
`0x00000012`. Config reverted, and manager 1.6.1+ hard-disables
sibling writing (`g_siblingsProven = 0`) until each module is proven
there; `merge_select` needs the UI process only.
