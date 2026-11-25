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
* **2026-11-24 19:2x (v1.4.7, user-verified twice): eye icons drive the
  merged result.** The user hid two of three scans with the eye icons
  and pressed Mesh Model; the log shows the two QUuids persisted as
  `included = 0` and both merge loops answering with the vendor flag
  untouched but the decision overridden:

  ```
  eye A65FE546-D6D2-44FE-8CBC-3DB3408848C8 -> hidden
  eye B7F2F769-26E8-42A4-ADA1-15E9C1322128 -> hidden
  gate uuid=A65FE546… vendor=1  excluded in .ini  -> SKIP   ra=+0x7E3BB
  gate uuid=14ACC446… vendor=1  identity unreadable -> include
  gate uuid=B7F2F769… vendor=1  excluded in .ini  -> SKIP   ra=+0x7E3BB
  gate uuid=A65FE546… excluded in .ini -> SKIP              ra=+0x83097
  gate uuid=B7F2F769… excluded in .ini -> SKIP              ra=+0x83097
  ```

  The final mesh contained only the scan whose eye stayed visible, and
  the project's folders were untouched. No crash dumps appeared with the
  module loaded in `scanservice.exe` for the whole session.

## 6. The scan-page mesh flow and the eye icons (2026-11-24)

Two facts learned from live diagnostics (module v1.1.1/v1.1.2 probes):

* a **cached** common mesh is re-shown without any work: the
  `isGenerateCloud` gate only runs when the mesh is actually rebuilt
  (new project or after deleting the result);
* the scan page's Mesh flow **does** call `isGenerateCloud`, but only
  from the post-mesh collection in `scanservice.exe` (see section 7).
  The earlier "it never asks the gate" reading came from runs where
  the module was not loaded in that process.

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
`0x00000012`. Config reverted; since then only `merge_select` may enter
a sibling, and the module itself applies a second, stricter gate (see
`gate_exes` below).

## 7. Where the eye decision is actually applied (v1.4.6)

The gate writer and the gate reader live in different processes, which
explains every confusing observation of the day:

* **the eye icons run in the UI process** - `EXStar Hub.exe`, where the
  QML `ScanPage_E7` controllers live and where the `setNodeVisible`
  metacall is watched and written to `merge-select.ini` as
  `[<uuid>] included = 0`;
* **the common mesh is collected in the service process** -
  `scanservice.exe` loads `Sn3DMultiProjects.dll` and its own log shows
  the merge loop:
  `[open-post-mesh] getMeshData ready ... uuidCount=3` followed by
  `Sn3DHandleProjects::getMeshData()`. The gate calls arrive there as
  `ra=Sn3DMultiProjects.dll+0x7E3BB`;
* `Sn3DprocessManager.exe` also runs during the Mesh Model flow but
  never maps `Sn3DMultiProjects.dll`, so it cannot be the merge host
  (that working hypothesis of the day was wrong).

**Shared-state decision.** `merge-select.ini` in
`C:\Users\Public\Shining3d\EXStar Hub\` is the hand-off: the UI process
writes `[<uuid>] included = 0/1`, the service process reads it at every
gate call. An explicit `[folder]` section still wins over a `[uuid]`
one, and every failure mode keeps the stock answer (fail-open).

**Hook allowlists (v1.4.6).** A module that hooks inside a service
process can take the service down, and a dead `scanservice` is exactly
what the vendor reports as error `0x00000012` on project open. Two ini
switches therefore decide where hooks may live; an unmatched process
keeps a completely inert module (no hooks, no files):

| switch | default | meaning |
|---|---|---|
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | processes allowed to hook `isGenerateCloud` |
| `eyewatch_exes` | `EXStar Hub.exe` | processes allowed to hook the QML metacall |

**Boxed gate (v1.4.6).** The decision half of the detour runs inside
`__try/__except`: a vendor object that moved between releases can no
longer take the process down, the call simply keeps the vendor answer
and one line is logged. This was added after `scanservice.exe` died
with `0xC0000005` (execute at address 1) while the module was loaded.

**Real caller address (v1.4.6).** The detour used to compare
`_ReturnAddress()` - but that is the address inside the detour thunk,
never inside `Sn3DMultiProjects.dll`, so every call took the
"caller outside the merge code" branch and the decision was never
applied. The thunk now passes the caller's own return address
(`[rsp]` at entry) as a third argument, which is why live runs finally
show `ra=+0x7E3BB` together with the three scan QUuids of the project.

**Wrapper identity.** The uuid printed by the gate comes from the loop
item the thunk captures in `rax` (`wrapper+0x38`, accepted only when
`[wrapper+0x28] == self`). The member bytes are a **QUuid struct**
(`uint data1` little-endian first), so they are already the canonical
order the eye map uses; converting them to wire order produces uuids
that are not version 4. Live check on `Project_3` made that visible:
`F54E`/`6049`/`1D4D` in the version nibble before the fix, i.e.
`470E64FE-CE12-4EF5-944A-011E29720325`,
`6849861E-6E32-4960-BFEF-6378AF077A00`,
`98396A4E-8563-4D1D-BD42-10469549E656` after it (v1.4.7). The
wire-order layout is only used when it is the one that matches a
recorded eye event.

**Thunk ABI.** The hand-assembled stub is 65 bytes: it grabs the return
address, passes `rax` and that address to the C helper, reserves the
32-byte shadow space the x64 ABI requires, keeps 16-byte stack
alignment at the call, and returns `AL` only (the saved `rax` is
dropped, not popped, because the vendor reads the byte straight after
the call). Versions 1.4.4/1.4.5 got the shadow space and the return
address wrong; neither was visible while the gate was inert.

## 8. The viewport: the scene's own named commands (v1.5.0)

Pressing **Mesh Model** while a row is hidden re-draws the hidden scan
anyway. The reason is that the page never asks the project tree for
anything: the hub log for one press shows the toolbar controller, the
QML environment switch and `SceneControllerPrivate::onRecvSetCurrentEnv`
- and no `setNodeVisible` at all. So the tree is not the path that
shows models, and hiding through it cannot control the viewport.

**The scene is driven by named commands.** `sn3d.digit.scenecontrol.dll`
is a Qt plugin: it exports only `qt_plugin_instance` and
`qt_plugin_query_metadata`, so nothing here is reachable by name - but
its command table is built in one constructor out of repeated blocks:

```
lea rdx,[rip+"SetModelsVisible"]     ; the command name
lea rcx,[rbp+var]  /  call [QString::QString]
lea rcx,[rip+handler]                ; the handler itself
mov [rsp+30],rcx                     ; stored with the {fn, obj} pair
```

Anchoring on the *name string* therefore yields the handler with no RVA
anywhere in the module: find the whole string in initialized data, find
the `lea rdx,[rip+…]` that materialises it, take the next
`lea rcx,[rip+…]` that is followed by that `mov [rsp+disp8],rcx` and
points into an executable section. Checked offline against the live
binary and against what a decompiler reported for the same file:

| command | handler |
|---|---|
| `SetModelsVisible` | `sn3d.digit.scenecontrol.dll+0xE2C90` |
| `SetModelNodesVisible` | `+0xE1A80` |
| `SetCurrentEnv` | `+0xE06D0` |

**What the command carries** (read live, from a hooked handler, using
the vendor's own container layout - see below):

```
scene SetModelsVisible (arg1) names=[Project_2] reset=0 visible=0
scene SetModelsVisible (arg1) names=[Project_1111, Project_2, Project_1000] reset=1 visible=1
```

The first line is one eye click. The second is the Mesh Model page: a
request to show *every* project, sent twice per press. That request is
the whole reason hidden scans came back.

**Why interception is enough.** In the decompiled apply path
(`FUN_1800fcd70`, reached from the handler for both directions) `reset`
only decides the starting set - `reset == 0` starts from an empty list,
`reset != 0` starts from `Sn3DCanvasView::models()`, i.e. "show exactly
these models" - and nothing else in the command has side effects. A
request that is not forwarded therefore leaves the viewport exactly as
it was: models that were visible stay visible, hidden ones stay hidden,
and there is nothing to re-hide afterwards. That is why v1.5.0 filters
the command instead of replaying the hidden state on timers.

**Two identity spaces.** The project tree and its metacall speak
QUuids; the scene commands speak model names (`Project_2`). The merge
gate therefore keeps using `[uuid]` sections in `merge-select.ini`
(§7), while the viewport filter keys on a hash of the UTF-16 name, in
RAM only. Names are never written to a file.

**Implemented rule** (`scene_filter`, UI process only, everything
fail-open): a `visible = 0` command records its names; a `visible = 1`
command naming one model is the user showing it again, so it is passed
on and forgotten; a `visible = 1` command naming several models while
at least one of them is hidden is not forwarded. A command whose map,
list or flag cannot be read completely is always passed through.
`SetModelNodesVisible` carries a nested shape (`model`, `nodes`, `node`,
`visible`, `marker`) and is watched and logged only.

**Qt container reading, for the record.** `QVariantMap` = one pointer to
data, root at `data+0x10`, node `{ parent +0x00, left +0x08, right +0x10,
key QString +0x18, value QVariant +0x20 }`. `QString` = one pointer to
`QArrayData` `{ ref, alloc, size, pad, offset }` with the payload at
`offset` (0 meaning "right behind the header"). `QVariant` = 16 bytes:
**payload at +0x00, type tag in the 30 low bits of the dword at +0x08**.
These are not remembered from documentation, they are *measured*:
`tools\qmap_lab.c` builds a real `QMap<QString,QVariant>` out of
`QJsonDocument::toVariantMap` with nothing but exported functions and
walks it, and `tools\qv_layout.c` builds single QVariants from their
exported constructors and prints the bytes:

```
node "beta"  key@+0x18 -> beta     val@+0x20 userType=10 (QString)
QVariant(true)     01 cd cd ..    01 00 00 00 ..      payload +0x00, tag +0x08
QVariant(false)    00 cd cd ..    01 00 00 00 ..
QVariant(QString)  <d ptr>        0A 00 00 00 ..      tag 10 = QString
QVariant(double)   00 00 00 00 00 00 0c 40   06 00 .. tag 6, 3.5 inline
```

**`QStringList` inside a `QVariant`, measured the same way.** The reader is
`modules\merge_select\qt_read.h`; `tools\qlist_lab.c` includes that very
header, builds a list with the shipped Qt's own `QString::split()` and
asserts what comes back:

```
QString("Project_1111,Project_2,Project_1000").split(',')  -> QVariant tag 11
  QVariant   +0x00 = the list's data pointer
  data       +0x00 ref   +0x04 alloc   +0x08 begin   +0x0C size
             +0x10 ...  the entries, one QString pointer each, inline
  -> 3 entries via size@+0x0C entries@+0x10 hop0
       [0] "Project_1111"   [1] "Project_2"   [2] "Project_1000"
```

So the count is the dword at `+0x0C` (`+0x08` is `begin`, which a freshly
built list reports as zero), and entries of pointer size - a QString is one -
lie inline. A list of larger values keeps a node array one hop further out;
that hop is searched too and named in the log (`hop0`/`hop1`), so a future Qt
that moves the items says so instead of deciding anything. Three shapes are
refused rather than guessed, each verified by the same tool: the shared empty
list, a value that is not a list at all, and uninitialised memory.

An entry that is a valid but *empty* QString is kept as an entry and simply
names nothing - the count of entries that do name something is carried
separately. That is not a hypothesis: the hub's own startup `SetCurrentEnv`
arrives with `showgroups` as a one-entry list whose `QArrayData` reports
`size = 0`, and a rule that demanded a non-empty string from every entry
would have thrown the list away before the filter ever saw a real name.

## 9. The 1.5.0 startup regression (what not to do inside a hook)

1.5.0 shipped with the hub dying during start-up: five crash dumps, all
`0xC0000005` at `Qt5Core.dll+0xBE5DF` with a read of `0x30`, all with
`modules\merge_select.dll` loaded, and all of them stopping the moment the
scene hooks were switched off. Two separate faults, both found again
offline:

* **Calling vendor code with a self-derived pointer.** The probe asked
  `QVariant::typeName` / `QVariant::toBool` what a command value was,
  passing an address this module had picked out of a tree it was guessing
  at. Qt walked off that pointer. A guard around the call is not enough:
  the fault lands inside Qt, several frames away, and whatever SEH does
  with it, vendor code has already run over memory it did not own. Since
  1.5.1 the values are read as bytes (section 8) and no Qt function is
  called anywhere in the scene path.
* **Forwarding four arguments.** The detour declared four parameters and
  called the original with four. The Microsoft x64 convention keeps the
  fifth argument and beyond in the caller's shadow area, so a handler that
  reads one got the detour's own frame instead - and `SetCurrentEnv`,
  which fires while the hub is still starting, does exactly that: its
  probe line shows `a1..a3` pointing into the stack, with the map beyond
  the four registers. The detour now takes eight parameters and hands on
  eight, which is the entire convention, so the handler sees the bytes its
  real caller left.

Verified after the fix, same machine, same three projects: the hub starts,
`SetCurrentEnv` fires twice during start-up through the hook, and no crash
dump is written.

## 10. Measuring a list, 1.5.2 - 1.5.5

The command handlers were filtering correctly in shape but the filter had
never actually fired, because nothing had yet decoded a *non-empty*
`names` list: at start-up the only list in flight is `SetCurrentEnv`'s empty
`showgroups`, and the element offsets used since 1.5.0 (`data + 0x18`) came
from the size of a container, not from a measurement.

Rather than wait for a click, the layout was taken offline.
`tools\qlist_lab.c` loads the product's own `Qt5Core.dll`, builds a real
`QStringList` with `QString::split()`, puts it in a real `QVariant` with
`QVariant(QStringList)` and runs the shipping reader (`qt_read.h`) over it -
the module and the tool include the same file, so the tool cannot drift away
from the code it is testing. Exit code 0 means the expected names came out.
`tools\qlist_one.c` does the same for one-entry and empty-entry lists, which
is what the hub actually sends at start-up.

What that settled:

| question | answer (this Qt build) |
|---|---|
| where is a list value | the payload qword at `QVariant+0x00`, tag 11 at `+0x08` |
| where is the count | `data+0x0C` - *not* `+0x08`, which is `begin` |
| where are the entries | `data+0x10`, inline, one pointer each (`hop0`) |
| larger item types | a node array, the value one hop further (`hop1`) |
| a valid empty entry | accepted, counts as an entry, names nothing |
| shared empty list, non-list, garbage | refused, and the filter does nothing |

The reader still searches the plausible offsets instead of trusting one, and
it now reports which combination decoded - the log line carries
`via size@+0x0C entries@+0x10 hop0` the first time a list is read, so a
vendor update that moves something is visible in an ordinary start log
instead of being discovered as a missing filter.

Still open: one press of **Mesh Model** with a hidden row, on this build, to
see the filter fire on a live `names` list of real project names. The
watching hooks and the eye recorder are already in place for that; the
expected log lines are `model hidden: <name>` followed by `kept N of M
models hidden (...)`, with the hidden project staying hidden and the others
untouched.

