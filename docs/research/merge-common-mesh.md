# Common mesh: how scans are selected, and how Selective Mesh steers it

Research + design record for the **Selective Mesh** module (manager
1.5.0 onwards). Since 1.6.0 the module is named `selective_mesh` and its
selection file is `selective-mesh.ini`; older sections below quote the
original names (`merge_select`, `Merge Selection`, `merge-select.ini`)
exactly as they appear in the evidence of the day - same module, same
mechanism. All RVAs are for **EXStar Hub 2.1.0-10** (image base 0x180000000); the
vendor files are unmodified - evidence comes from static analysis of the
installed binaries (Ghidra/REA) plus one reconstructed community
patchset used as a map, not as code. The implementation map of the
module as it stands today:
[modules/selective_mesh/ARCHITECTURE.md](../../modules/selective_mesh/ARCHITECTURE.md).

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

> **Corrected 2026-11-25, section 11.** Pressing **Mesh Model** with a
> hidden row on the same product version produced neither line: one eye
> click produced two `SetModelNodesVisible` commands and no
> `SetModelsVisible`, and the press produced only `SetCurrentEnv`. The
> two lines above were read live from a hooked handler, so they are real
> - but they belong to the project list addressing every model at once,
> not to a single eye and not to the Mesh Model page. Everything else in
> this section describes a filter that, in practice, never had anything
> to withhold.

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
`modules\selective_mesh\qt_read.h` (named `modules\merge_select\qt_read.h`
when this was measured); `tools\qlist_lab.c` includes that very
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

That experiment was run on 2026-11-25 with module 1.5.6 (the build that
logs every command argument it can read). It did not come out the way
this section expected, and what it came out with is the rest of this
document.

## 11. One press of Mesh Model, logged end to end

Build under test: `merge_select` 1.5.6
(sha256 `D01B7BB101305018B810E9AB2A50EB26BD9E37BBB86274A0475FD7A090DDDB77`),
`verbose = scene_watch = scene_filter = eye_gate = 1`, one project with
three scans, the hub and `scanservice.exe` both hosting the module.
Two scans hidden with their eyes, then **Mesh Model** pressed. Trimmed
`mod.log` (`…\ExStarHubMods\mod.log`, timestamps are host-relative ms):

```
[78850937] eye-meta invoke idx=8 via ScanPage_E7.dll+0x3A521
[78850937] eye F7E075F3-… -> hidden
[78850937] scene SetModelNodesVisible (arg4) cmd="SetModelNodesVisible" nodes=1 [type8 …]
[78850953] scene SetModelNodesVisible (arg4) cmd="SetModelNodesVisible" nodes=1 [type8 …]
[78852609] eye 0267F028-… -> hidden
[78852609] scene SetModelNodesVisible (arg4) …
[78852609] scene SetModelNodesVisible (arg4) …
[78854734] scene SetCurrentEnv (arg4) env="(unreadable)" showgroups=list? {ref:2 alloc:2 b:0 size:1} +10/h0=1(0) cmd="SetCurrentEnv"
[78854812] gate uuid=4026A480-… ra=+0x7E3BB  identity unreadable (fail-open)  -> include
[78854812] gate uuid=F7E075F3-… ra=+0x7E3BB  excluded in .ini  -> SKIP
[78854812] gate uuid=0267F028-… ra=+0x7E3BB  excluded in .ini  -> SKIP
[78856562] gate uuid=4026A480-… ra=+0x83097  identity unreadable (fail-open)  -> include
[78856875] gate uuid=F7E075F3-… ra=+0x83097  excluded in .ini  -> SKIP
[78856890] gate uuid=0267F028-… ra=+0x83097  excluded in .ini  -> SKIP
[78860453] scene SetCurrentEnv (arg4) env="(unreadable)" showgroups=list? … cmd="SetCurrentEnv"
```

Read line by line, a press is:

1. `SetCurrentEnv` - the page switches to the mesh environment;
2. the merge itself, i.e. the two collection loops of section 1
   (`+0x7E3BB` and `+0x83097`), where the gate answers per scan;
3. a second `SetCurrentEnv` a few seconds later, when the result lands.

Nothing else. Across the whole session - two eyes hidden, four presses -
`SetModelsVisible` never appeared once, and no command naming a model
appeared anywhere near a press. The gate is the only thing that decided
what went into the mesh, and it excluded both hidden scans on every
press (`excluded in .ini -> SKIP`, both loops).

The `identity unreadable (fail-open)` on the surviving scan is the
known scanservice limitation (section 7): in that process the scan's
folder path cannot be read, so the `[uuid]` section is what carries the
choice. Scans the ini does not mention are included, as stock.

## 12. The command surface, enumerated

Because "does a show-everything command exist at all" is a question about
the *whole* surface and not about the three names the module watches, the
registration blocks were enumerated offline. `tools\scmd_enum.ps1` walks
`.text` for the same pair the module anchors on
(`lea rdx,[rip+name]` … `lea rcx,[rip+code]` + `mov [rsp+d8],rcx`) and
prints every pair. No RVA is hardcoded, the file is only read.

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\scmd_enum.ps1 `
    -Dll 'C:\Program Files\Shining3d\EXStar Hub\sn3d.digit.scenecontrol.dll'
```

On 2.1.0-10 (`sn3d.digit.scenecontrol.dll`,
sha256 `4c1a6e12e8ec6264ce4ebdbbd46d0fe751d5909e46f55530bce91bff3cf7b961`)
that is 43 commands:

```
addModuleObject             +0xB99A0    QueryClipplaneState         +0xDB270
adjustMeshTexture           +0xBA050    registrationFinished        +0xD82F0
ApplyManualAlign            +0xCDC60    reloadClipPlane             +0xDD3F0
cancelPlaneAlign            +0xCF230    removeModelHoeScrap         +0xDE4D0
changeDisplayType           +0xBB970    RotationCenter              +0xDED50
checkedManualAlignType      +0xD5EE0    sceneMouse                  +0xDF080
clearManualAlign            +0xCF8F0    sendUUid                    +0xE5640
createCutPlane              +0xCFA70    setAllModelMarkerVisible    +0xDF2B0
createOrLoadSolutionFinished+0x8E800    SetCameraAlign              +0xDF4F0
createPlaneByDrawLine       +0xCFE30    SetCurrentEnv               +0xE06D0
curPrjSliceNames            +0xE3FB0    SetModelNodesVisible        +0xE1A80
CutFaceSet                  +0xE5760    SetModelsSelectState        +0xE2630
DeleteAll                   +0xD03B0    SetModelsVisible            +0xE2C90
deleteSceneBasePlane        +0xC9CB0    setProjectionType           +0xFE4F0
downSamples                 +0xD1390    setTextureVisible           +0xE3690
finishAlign                 +0xD9300    showEncodeMarker            +0xD1A10
finishFillHoles             +0xD2F40    showFrameMarker             +0xD3FC0
finishHoles                 +0xD5550    showManualAlignModel        +0xCCED0
finishPostProcessPointCloud +0x88370    showOriginAxis              +0xE3D60
getDeleteObjDataFinish      +0xD1240    updateModelColorShowType    +0xE4250
manualAlignUndoRedo         +0xC9FE0    updateModelMarker           +0xE4710
model_matrix_change         +0xD6070
```

(`-Table` prints exactly this block, so the list above can be diffed
against a vendor update with one command.)

Two earlier attempts at the same question are kept for the record and did
not work: `tools\scmd_lab.c` looked for the registration blocks as *data*
and found one hit out of 43 (the pattern is too strict there), and
`tools\qmeta_lab.c` tried to resolve Qt type ids through an exported
`QMetaType::typeName` and stops with `QMetaType::typeName not found` on
this Qt build. `tools\func_str.c` (strings and calls referenced inside
one function) was written before the decompiler path was available and
was not needed.

Model visibility is changed by exactly two of them - `SetModelsVisible`
and `SetModelNodesVisible`. `setTextureVisible`,
`setAllModelMarkerVisible` and `showManualAlignModel` are a texture, the
markers and the alignment preview. There is no third door, and no
command that says "keep the hidden ones hidden".

## 13. `keep_hide` is a cut-plane flag

The string `keep_hide` (0x1801ab770) looked like the native switch worth
finding, and it is not. It has one code reference in the plugin,
`0x1800e83f7`, which sits inside the handler for `CutFaceSet`
(`+0xE5760`, body `+0xE5760..+0xE8F15`), and it is read the way every
other command key is read:

```c
local_be8 = QString::fromAscii_helper("keep_hide", 9);
lVar27    = FUN_18001ade0(*in_stack_00000028, &local_be8);  /* map lookup */
…
*(bool *)((longlong)param_1 + 0x3b9) = QVariant::toBool(…);  /* private field */
```

So it is the cut/`delete below` option ("leave the cut-away part hidden")
stored as a field of the controller, one of the same family as
`belowHideDataState`, `white_plane` and `editable` in that metaobject
string table. Model eyes are not involved.

## 14. What "hidden" means to the scene

Two findings from the decompiled handlers explain why the name-keyed
filter could never fire:

* **The eye's command is node-level, not model-level.** The handler at
  `+0xE1A80` reads `nodes` and, per entry, the keys `model`, `type`,
  `node`, `marker` and `visible` (`QVariant::toBool` for the flag), and
  turns them into `ModelGeoNodeKey`s for that view
  (`ModelNode::markerGeometryNode`, `ModelGeoNodeKey::ModelGeoNodeKey`).
  There is no "model is hidden" bit to flip - only geometry nodes on a
  specific view, which is also why an environment switch can redraw them
  as visible without any command contradicting the eye.
* **What the scene says about visibility is outbound.** `+0xCB360`
  iterates `Sn3DCanvasView::models()`, splits them by a per-model
  predicate into two `QStringList`s and publishes
  `{cmd: "mainview_model_visible_change", view: <view>, show: […], hide: […]}`
  through `CommonLib::Sn3DQObject::publish`. That is a status report of
  what the main view currently draws, not a switch anyone can set. It is
  also not in the command registry of section 12, so it cannot be hooked
  as a command.

The only place the eye's intent exists in a readable form is the
project-list metacall `setNodeVisible(QUuid, bool)` in
`ScanPage_E7.dll`, which the module already watches (section 7). That is
a uuid, and the scene commands never carry a uuid - so the two halves
still do not share a key.

## 15. Two side findings worth keeping

* **The environment switch defers its own work.** `onRecvSetCurrentEnv`
  (`+0xE06D0`, ~4.9 KB, the handler the module hooks as `SetCurrentEnv`)
  reads `showgroups` with `QVariant::toStringList`, writes the
  `Environment` property, logs ` environment value newenv = ` /
  ` environment value is scan newenv = ` / `PostProcess.Mesh`, and
  imports `QTimer::singleShotImpl`. Anything that wants to "fix the view
  after the switch" therefore has to be later than the handler return,
  which is exactly the timing we refused to guess at.
* **Visible point clouds travel through shared memory.** `+0x110960` is
  the only code that calls
  `Sn3DViewSelector::getVisibleStatesObjects` (`QMap<QString, ModelStateObject>`).
  It keeps the geo nodes whose flag byte at `+0x28` matches `& 0x16`,
  sizes a block from their keys and matrices and writes a counted list
  into a `QSharedMemory` segment named `ConnectionDomain_PointCloud_FromClient<number>`.
  The decompiler resolves no callers (it is reached indirectly), so this
  is recorded as a mechanism, not as part of the merge input.

## 16. Consequences, and the open decision

Settled by the above:

* the merge side is done and verified live: eyes write `[uuid] included`
  to the selection .ini once (`merge-select.ini` then,
  `selective-mesh.ini` since 1.6.0), it survives restarts, and every press of
  **Mesh Model** excludes those scans in both collection loops;
* the viewport side has nothing to intercept: the re-show is the
  environment's own redraw, not a command, and the plugin exposes no
  "keep hidden" state (sections 12 to 15);
* therefore a name-keyed viewport filter (`scene_filter`,
  `sceneHideDecide`, `sceneNodeDecide`) cannot work as designed. It is
  inert today: the hidden-by-name set is never populated, because the
  eye's command carries node keys the walk does not read and the press
  carries no names at all.

Two ways forward, decided on 2026-11-25 in favour of **C**:

* **A - stop at the merge.** Remove the inert filter, keep `scene_watch`
  as an opt-in diagnostic, ship the workflow the evidence supports.
* **C - re-apply the eyes after the switch.** The module already holds
  the controller, the metacall and the uuids, so re-hiding is one
  `setNodeVisible(uuid, false)` per scan on the hub's own object - but
  it is a vendor call from inside a hook during an environment apply,
  and section 15 says the apply is deferred, so it needs the deferred
  path understood first. This is the same class of call that produced
  the five start-up crashes of section 9.

What C took, and what it finally needed, is section 18.

## 17. Reproducing this evidence

Nothing from the vendor's pseudocode is committed; the addresses and one
digest reproduce it.

```
tools\scmd_enum.ps1 -Dll 'C:\Program Files\Shining3d\EXStar Hub\sn3d.digit.scenecontrol.dll'
```

For the pseudocode, open the same file (sha256 above, image base
0x180000000) with any decompiler - the REA toolchain was used here
(`open_binary`, then `decompile_all` over one range at a time) - and read
these ranges:

| range | what it is |
|---|---|
| `0x1800cb360..0x1800cbd08` | publishes `mainview_model_visible_change` |
| `0x1800e06d0..0x1800e1a2d` | `onRecvSetCurrentEnv` (`SetCurrentEnv`) |
| `0x1800e1a80..0x1800e262f` | `SetModelNodesVisible`, the eye's command |
| `0x1800e2c90..0x1800e338f` | `SetModelsVisible` |
| `0x1800e5760..0x1800e8f15` | `CutFaceSet`, where `keep_hide` is read (`0x1800e83f7`) |
| `0x180110960..0x18011244e` | the `ConnectionDomain_PointCloud_FromClient` export |

## 18. Option C: re-issuing the eyes (1.5.7, then 1.5.8)

The decision from section 16 was C, and it took two builds. What made
the difference is the difference between *calling the vendor slot* and
*making the vendor state change*.

**1.5.7 - the call, no effect.** The `SetCurrentEnv` detour passed the
command through untouched and, once the vendor handler had returned,
re-ran the eye slot for every hidden scan:

```
[100043421] scene SetCurrentEnv (arg4) cmd="SetCurrentEnv"
[100043421] re-applied 2 hidden eye(s) after the environment switch
```

Nothing else. The scans stayed on screen. Compare the same log at an
actual eye click, where the slot is followed by the scene commands the
viewport acts on:

```
[100040812] eye-meta invoke idx=8 via ScanPage_E7.dll+0x3A521
[100040812] eye 6966B795-... -> hidden
[100040812] scene SetModelNodesVisible (arg4) nodes=1 [type8 ... ]
[100040828] scene SetModelNodesVisible (arg4) nodes=1 [type8 ... ]
```

So the slot ran and emitted nothing. It was not a logging artifact: the
`SetModelNodesVisible` log has its own budget of 400 lines, still
unspent at that point (four of its lines appear 100 ms above the press).

**Why.** The call was `setNodeVisible(uuid, false)` - the value the node
already carried after the last eye click. QML and the scene act on a
*transition*; a write of the same value never reaches the viewport, so
the re-apply was a perfectly correct no-op. Section 14 already said the
scene keeps its own visibility state: that state is what the environment
switch resets, and the project list is the only place that still knows
the user's intent. Re-asserting intent therefore has to look like a
change.

**1.5.8 - the change.** Same slot, same object, same thread - but each
hidden scan is *toggled*: `true`, then `false`, through the same
metacall. Both writes are transitions, so both emit, and the second
leaves the scan hidden:

```
[123575765] reapply toggled 77B2D1F5-CDB3-42E8-80FF-4CEFD9EC23E4 (show+hide)
[123575765] reapply toggled 6EF095DF-7ACF-4F9A-A471-13F7F10C1855 (show+hide)
[123575765] re-applied 2 hidden eye(s) after the environment switch
[123575796] scene SetModelNodesVisible (arg4) nodes=1 [type8 ... ]   x8
```

Plus one deferred pass 300 ms later. Section 15 showed the vendor's own
apply runs through `QTimer::singleShotImpl`, so the first toggle can land
before the new environment has finished building its node set; the retry
runs in the message loop of the same Qt thread (a `SetTimer(NULL, …)`
timer, armed once per press burst) and repeats the toggle after the work
has settled:

```
[123576078] reapply retry after the env work settled
[123576078] reapply toggled 77B2D1F5-... (show+hide)
[123576078] reapply toggled 6EF095DF-... (show+hide)
```

Verified live on 2026-11-26 (1.5.8): after **Mesh Model** the viewport
keeps only the scan that was left visible, the merge still logs
`excluded in .ini -> SKIP` for both hidden scans, no crash dump.

**Correction, same day (1.6.1).** That verification stopped too early.
A minute after the Mesh Model press the hub stopped responding and
Windows logged an Application Hang (`EXStar Hub.exe` 3.1.49, 14:31;
the module's last lines are the toggle and the vendor's own
`SetModelNodesVisible`). The show half of the toggle is also what
re-appears on screen before the hub dies. The user confirmed the same
hang with the 1.5.7-era build, so it belongs to the re-apply design,
not to the 1.6.0 rename: the metacall runs while the post-process
environment switch still owns the scene. `eye_reapply` therefore
defaults to 0 since 1.6.1; a quiescence-checked re-apply (act only
when no scene command has been seen for a while, skip while
post-process is busy) is the open design item.

Safety notes worth repeating: the retry never re-arms itself (one pass
per press), the whole vendor call is SEH-boxed, and `det_meta` is
deliberately bypassed by calling the original trampoline - a re-apply
must not rewrite the `.ini`, whose job is only to remember what the user
clicked.

**The metacall, proven from disk.** The eye slot has no export, so
`tools\qmeta_dump.ps1` walks the image's initialized-data sections for a
QMetaObject (four image pointers: `superdata, stringdata, data,
static_metacall`) and prints the method table. On this `ScanPage_E7.dll`
it yields, for `ProjectListControllerE7`, exactly the values the module
resolves at run time:

```
class        : ProjectListControllerE7   (moc revision 8)
metaobject   : RVA 0xA38210
static meta  : RVA 0x42DF0
  idx  argc  type                 name
    6     0  Signal               totalModelChanged
    8     2  Slot                 setNodeVisible
```

Three layout facts this tool had to get right, each of which silently
returned nothing before it was fixed: `superdata` points at the base
class' metaobject *in another module*, so on disk it reads 0 and is
filled by a relocation - it cannot take part in the candidate filter;
`SizeOfImage` lives at optional-header `+0x38`, not `+0x10`; and Qt's
`data[]` section offsets (method table, property table) are counted in
**uints**, not bytes. The method type is not a 3-bit field either: Qt5
stores the `MethodType` enum (`Method 0x0`, `Signal 0x6`, `Slot 0xa`),
which is why every entry read as `Method` until the mask was widened.

For the senders of the scene command itself, `tools\xref_str.ps1` finds
`lea reg,[rip+disp32]` references to a literal - the four places in
`ScanPage_E7.dll` (RVAs `0x8E5D5`, `0x97543`, `0x9E253`, `0xA516B`, all
inside `ProjectInfoUiManagerE7`, 110 methods, `static meta 0x41880`) that
build a `SetModelNodesVisible` request. That is the fallback path if a
future vendor build ever stops routing the eye through the metacall.

```
tools\qmeta_dump.ps1 -File 'C:\Program Files\Shining3d\EXStar Hub\ScanPage_E7.dll' -Class ProjectListControllerE7
tools\xref_str.ps1   -File 'C:\Program Files\Shining3d\EXStar Hub\ScanPage_E7.dll' -Text 'SetModelNodesVisible'
```

## 19. The eye-click hang of 1.6.1: a self-referencing probe walk (1.6.2)

After the 1.6.1 deploy (`eye_reapply = 0`, `scene_filter = 0`,
`scene_watch = 1`, `verbose = 1`) the hub still hung on eye clicks, twice,
minutes apart, with nothing else touched. Turning the module off in
`modules.ini` (`[selective_mesh] enabled = 0` - the host logs `module
'selective_mesh' is disabled`) and repeating the same clicks produced no
hang at all, so the defect was ours, even though WER also lists the media
stack (`calib_quik.mp4`, MF/EVR/msmpeg2vdec) that the dashboard keeps
playing while the UI is stuck.

A full-memory minidump of the still-hung process (taken with
`MiniDumpWriteDump` from an elevated PowerShell, 6.36 GB) names the exact
place. One thread is burning a whole core (+3.09 s CPU in 3 s) with
`RIP = selective_mesh.dll+0x4460`, which is the first instruction of
`readQName5()`, and `[RSP] = selective_mesh.dll+0x36B9`, which is
`qmapDumpD+0x169` - the key read of the map walk. Because RIP is at the
function's entry the caller's frame starts at `RSP+8`; it can be found in
the stack by its `roots[2] = {0x10, 0x18}` marker and it reads
`sp = 1, cnt = 0, used = 0`, with the spilled skip counter at ~2.2e9.
One pending node, no key accepted, billions of iterations.

Why the walk never ended: `qmapDumpD()` bounded the *pending stack* but
not the number of nodes visited. An argument that is not a command map -
scene commands carry up to eight - read at the wrong root offset yields a
pointer graph whose children point back into it; each step pops one node
and pushes one back, so `sp` stays at 1-2 forever, no key parses (so the
`cnt < 16` key limit never trips) and the thread spins. The same function
compared its push guard against `sizeof stack` *bytes* (511) for a
64-element array, so a wider bogus graph would also have written pointers
over the saved registers and the return address - the array sits flush
against them in the frame.

Fix in 1.6.2, three independent bounds inside `qmapDumpD()`:
`sp < WALK_SLOTS` (64, counted in elements), `visits < WALK_VISITS` (256,
the whole walk, not just what is pending), and a root gate - a walk only
starts when the root node carries a readable key, which every real
command map has. The probe is diagnostic only, so all three fail open:
no walk means no log line, never a changed command.

Two conclusions for the record. First, the 14:31 hang of 1.6.0 was
attributed to `eye_reapply`, but these later hangs happened with re-apply
off; the re-apply switch is still correctly off by default, yet it was
not the cause of the eye-click hang. Second, `scene_watch` is not free:
it runs the walk on the UI thread for every watched command, so every
bound in it has to be a hard one.

