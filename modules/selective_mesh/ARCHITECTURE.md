# Selective Mesh - module architecture

`modules/selective_mesh/selective_mesh.c` (single translation unit, plus
`qt_read.h`), version in `MOD_VERSION`. Targets EXStar Hub 2.1.0-10,
x64, built by `build.ps1` into `build\modules\selective_mesh.dll` and
embedded into the manager as a resource.

The user-facing description is [README.md](README.md). This file is the
implementation map: every hook, how it is anchored, what state it owns,
which thread touches it, every switch and every log line. Evidence for
each vendor fact quoted here lives in
[docs/research/merge-common-mesh.md](../../docs/research/merge-common-mesh.md).

---

## 1. Shape of the problem

The product asks one question to decide whether a scan's point cloud
takes part in the common mesh:

```
bool HandlePrjFile::isGenerateCloud()   (export of Sn3DFile.dll)
```

Only `Sn3DMultiProjects.dll` imports it, and that import is the whole
merge loop. The module answers that question with "no cloud" for the
scans the user excluded, and nothing else in the product notices: the
vendor's own skip-branch does the rest. A second, independent half - the
project-list **eye** icon - is watched so the exclusion is one click
instead of an ini edit, and re-asserted after the scene's environment
switch so the viewport agrees with the merge.

```
 eye click (QML)                     common-mesh run
      |                                    |
      v                                    v
 det_meta (ScanPage_E7.dll)          det_isCloud via g_thunk
      |                                    |            (Sn3DFile.dll export)
      v                                    v
 g_eye[] in-RAM map  ----+      +---- selective-mesh.ini  (the shared record)
                         |      |
                    selective-mesh.ini  =  [uuid] / [folder] sections
                                            included = 0
```

The .ini is the only durable state, and it is shared across processes:
the hub writes it, every gated process reads it at the gate.

## 2. Process topology

One DLL, loaded into every process the host injects, but it only ever
goes live in processes named in an allowlist (both keys, module ini;
comma-separated, case-insensitive, matched against the exe basename):

| key | default | why |
|-----|---------|-----|
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | the processes that actually run the `Sn3DMultiProjects` merge loop |
| `eyewatch_exes` | `EXStar Hub.exe` | the QML UI process, i.e. where the eye icons and the scene plugin live |

A process outside `gate_exes` logs one line and stays completely inert
(no hooks, no file writes). The same flag also stops the UI phase of the
worker, so `EXStar Hub.exe` has to appear in **both** lists for the eye
watch to arm - which is exactly what the two defaults say. Evidence for
the allowlists: hooks in an unrelated service process took
`scanservice.exe` down once (`0xC0000005`, then vendor error
`0x00000012` on project open), so "everywhere that does not need it" is
not a style choice here.

## 3. Lifecycle and threads

| what | thread | notes |
|------|--------|-------|
| `ex_get_info()` | loader | returns `g_info` (`"selective_mesh"`, "Selective Mesh", `MOD_VERSION`) |
| `ex_init()` | host's init thread | reads config, resolves the exe basename for the allowlists, computes `g_selFile`/`g_mapFile` (+ migration), creates the wake event, starts the worker. Returns non-zero only if the event or thread cannot be created. |
| `worker()` | the module's single worker thread | everything dangerous happens here, never under the loader lock |
| `det_isCloud` / thunk | any thread of a gated process | the merge loop's own threads |
| `det_meta` | the hub's Qt UI thread | the only thread that may re-issue eyes |
| `eyeRetryTimer` | the same Qt UI thread | `SetTimer(NULL, ...)`, fires 300 ms later in that thread's message loop |

`worker()` in order:

1. `armNotify()` - `ntdll!LdrRegisterDllNotification`; every DLL load
   sets `g_wake`. If unavailable, the loop degrades to polling (logged).
2. **Gate phase**: loop `tryInstall()` until installed or broken, waking
   on DLL loads, 50 ms poll during the first 10 s and 500 ms after,
   giving up after 15 min (`Sn3DFile.dll never mapped in this process`).
3. **UI phase** (only when `eyewatch` and the exe is in `eyewatch_exes`):
   loop `eyeTry()` and, when `scene_watch || scene_filter || eye_reapply`,
   `sceneTry()`, until both report done or 10 min elapse, 1 s poll. The
   `SetCurrentEnv` hook is armed for `eye_reapply` even when the watch
   and the filter are both off, because that hook is where the re-apply
   is triggered.

There is no unhook path: the module is unloaded with the process.

## 4. Component A - the merge gate

| | |
|---|---|
| target | export `?isGenerateCloud@HandlePrjFile@@QEAA_NXZ` of `Sn3DFile.dll` |
| detour | `g_thunk` -> `det_isCloud` -> `decideCloud` |
| installed by | `tryInstall()`, worker thread |
| switches | `gate_exes`, and the master key `[Settings] enabled` inside the selection .ini |

`tryInstall()` waits until `Sn3DFile.dll`, `Qt5Core.dll` and
`Sn3DMultiProjects.dll` are all mapped, then resolves (all under SEH):

* `Sn3DFile!isGenerateCloud` - the hook target,
* `Sn3DFile!getFolderPath@HandlePrjFile` and `getTXProjFullPath@HandlePrjFile`
  - identity channels,
* `Qt5Core!QString::utf16` and `~QString` - to read the returned strings,
* the `Sn3DMultiProjects.dll` image base and `SizeOfImage` (from its own
  `IMAGE_OPTIONAL_HEADER`), which define the caller containment range.

Missing any of the first four: log once, `g_broken = 1`, the app stays
stock. Then `buildThunk()` and `g_host->hook(target, g_thunk, &g_origIsCloud)`.

### Why a hand-built thunk

The detour needs two things the export's own signature does not carry:
the **caller's return address** (to tell merge callers from everybody
else) and a **wrapper candidate** (to find the scan uuid, section 6).
Both are only available at the raw call site, so the hook target is a
65-byte code blob `VirtualAlloc`ed as `PAGE_EXECUTE_READWRITE`:

```
mov  r8, [rsp]          ; the caller's return address
mov  rdx, rax           ; the string-return hidden-return slot MSVC x64 uses
push rax rbx rcx rdx rsi rdi r8 r9 r10 r11 ; every volatile register
pushfq                                      ; and flags
sub  rsp, 0x20                              ; shadow area for the call
mov  r10, det_isCloud
call r10
movzx eax, al                               ; keep only the bool
add  rsp, 0x20
popfq                                         ; flags restored verbatim
pop  r11 r10 r9 r8 rdi rsi rdx rcx rbx
add  rsp, 8                                   ; drop the saved rax
ret                                           ; AL = the answer
```

Nothing is invented and nothing is lost: the callee sees exactly the
registers its real caller left, only `AL` changes.

### The decision (`decideCloud`)

```
if ra outside [Sn3DMultiProjects base, base+SizeOfImage)
        -> probeLog("cloud?", ra, self); return vendor answer   (containment)
inc, why = scanDecision(self)                  (section 5, identity)
uuid     = gateEyeUuid(self, impl, wrapper)    (section 6)
if eye_gate and uuid known and not master-off:
        [uuid] section in the .ini wins outright, else
        the in-RAM eye map (eyeLookup) can only turn include -> exclude
report the scan once to the .map.txt, log the line under budget
return (vendor && inc)                        (can only ever narrow, never widen)
```

`det_isCloud` wraps `decideCloud` in `__try/__except`: any fault returns
the stock answer and logs once. A vendor object that moved between
releases can cost the exclusion, never the process.

## 5. Component B - scan identity

`scanDecision()` produces the section key and the .ini verdict, best
channel first, all reads SEH-boxed:

| order | channel | read | fills |
|------|---------|------|-------|
| 1 | exported folder path | `getFolderPath(self)` -> `QString::utf16()` | `folder` |
| 2 | embedded scan name | `impl = *(self+0x18)`, then the QString at `impl+8` - the same string `HandlePrjFile::getProjInfo` reads; live session scans have this even when both getters return empty | `name` |
| 3 | project-file path | `getTXProjFullPath(self)` | `alt` |

`impl = *(unsigned long long *)(self + 0x18)` is also what section 6
searches for a uuid. If all three channels come back empty, `idDump()`
walks `impl + 0x00 .. +0x48` in 8-byte steps, tries each position as one
embedded QString and logs the printable ones (`idmap impl=... +0x.. =
<text>`), at most six dumps per process - the diagnostic for "the scan
has no name here".

Section lookup: `GetPrivateProfileIntW(cand, "included", -1, g_selFile)`
for up to two candidates (primary then secondary), each tried first
verbatim then as `<parent folder>\<leaf folder>` (`shortKey()`); the
first hit decides. Verdict codes `WHY_*`:

| code | meaning | effect |
|------|---------|--------|
| `WHY_INI_EXC` | `included = 0` matched | excluded |
| `WHY_INI_INC` | `included = 1` matched | included (explicitly) |
| `WHY_UNLISTED` | no section matched | included (stock) |
| `WHY_MASTER` | `[Settings] enabled = 0` in the selection .ini | included, eye binding skipped |
| `WHY_NOREAD` | identity unreadable | included, logged as such |
| `WHY_EYE` | the in-RAM eye map excluded it | excluded |

## 6. Component C - binding a scan to its uuid

The eye state is keyed by the scan `QUuid`, so the gate must find that
uuid inside the `HandlePrjFile` it is being asked about. Four channels,
best first (`gateEyeUuid`):

1. **`wrapperUuid`** - the thunk passes the hidden-return slot as a
   wrapper candidate. Accepted only when `*(wrapper+0x28) == self`; the
   uuid is then the 16 bytes at `wrapper+0x38`. `QUuid`'s first three
   members are little-endian in memory, so the raw bytes are already the
   canonical struct order; the byte-swapped variant is used only if it
   is the one the eye map knows. This is the same-transaction channel:
   the state recorded in this very call.
2. **`g_uLoc`** - once a uuid has been found by scanning, its position
   is cached as `{ level, offset, layout }` (`level` 0 = in `self`, 1 =
   in `impl`; layout 0 = raw, 1 = byte-swapped, 2 = 36-char UTF-16 text)
   and the next calls read it directly. A failed cached read invalidates
   the cache and falls through, so a wrong cache cannot persist.
3. **`uuidScan`** - while new eye uuids keep appearing (or within its
   budget) the module searches `self` and `impl` windows
   (`USCAN_WIN` = 0x200 bytes per object) for any of up to `USCAN_MAX`
   = 32 known eye uuids, in raw, byte-swapped and UTF-16-text form, and
   caches what it finds. This is what makes the binding work on builds
   where the offset is not 0x38.
4. **`self+0x38`** - the last, documented guess (verified offset,
   `docs/research/merge-common-mesh.md`).

No uuid at all simply means: no eye input to this decision, the .ini
sections still work.

## 7. Component D - the eye watch

| | |
|---|---|
| anchor | `metaFind(ScanPage_E7.dll, "ProjectListControllerE7", "setNodeVisible")` |
| detour | the class's `qt_static_metacall` (`det_meta`) |
| state | `g_eye[]` (`EYE_MAX` = 64 uuid+visible+seq), `g_eyeIdx`, `g_eyeCtrl`, `g_eyeCtrlTid` |
| switches | `eyewatch`, `eye_gate`, `eyewatch_exes` |

`metaFind()` walks the image's initialized-data sections for a Qt5
`QMetaObject` shape `{ superdata, stringdata, data, static_metacall }`,
checks the `className` string, then walks the method table (this moc's
layout: 5 uints per method, table at `data[data[5]]`) for
`setNodeVisible`, and reports its index (`g_eyeIdx`, 8 on 2.1.0-10 - the
**method index**, not an offset into `data`). No RVAs anywhere: the
anchor is a class name, a method name and a verified structure. The
offline equivalent is `tools/qmeta_dump.ps1`.

`det_meta` then, per call:

* remembers `obj` + thread as `g_eyeCtrl` / `g_eyeCtrlTid` (component E);
* logs each new `(call, idx)` pair once, with the calling module +
  offset - the traffic survey that found the eye path;
* for `call == InvokeMetacall` and `idx == g_eyeIdx`, reads the
  `QUuid` at `a[1]` and the bool at `a[2]` under SEH and calls
  `eyeRecord()`;
* always forwards to the original metacall.

`eyeRecord()` updates `g_eye[]` and, with `eye_gate`, persists the
state: hidden writes `[<UUID>] included = 0` into the selection .ini,
shown deletes the section (so the file stays a clean list of
exclusions). The vendor resets the tree to "all visible" when a project
re-opens, so these sections describe the current session - exactly like
the icons themselves.

The metacall sees QML traffic only: direct C++ calls bypass it, which is
useful signal, and it is why the mesh dialog's own show burst is not
mistaken for a user click.

## 8. Component E - keeping hidden scans hidden on screen

Verified behaviour: pressing **Mesh Model** switches the scene
environment (`SetCurrentEnv`), and the new environment does not carry
the eye's node-level hide - the scan is visible again while the merged
mesh is still correct (the gate excluded it). Option C is the fix, and
1.5.8 is the working version of it:

* `sceneCall(idx == 2)` calls `eyeReapply(1)` **after** the vendor
  handler returned.
* `eyeReapply()` runs only on `g_eyeCtrlTid` (the Qt thread that last
  entered the metacall), takes `g_reapplyBusy`, and for every hidden
  entry in `g_eye[]` **toggles** through the same slot:
  `setNodeVisible(uuid, true)` then `(uuid, false)` - two real state
  changes, each of which emits, which is precisely the sequence a hand
  click performs. Re-asserting `false` alone is a no-op: 1.5.7 proved
  live that an unchanged value produces no command anywhere on the way
  to the scene.
* The call goes through `g_origMeta` - the original metacall trampoline
  - so `det_meta` and with it the .ini write are deliberately bypassed;
  only the final state is "hidden".
* The vendor `setNodeVisible` body ignores its `this` (it goes through a
  file-scope model singleton, verified in the decompiled body), so the
  borrowed `g_eyeCtrl` pointer is passed through but never dereferenced
  by us.
* Each toggle is SEH-boxed; a fault stops the loop and logs, the
  viewport stays as the vendor left it.
* One deferred second pass: `SetTimer(NULL, RETRY_TIMER_ID /* 0x5CA7 */,
  300, eyeRetryTimer)` on the Qt thread, guarded by `g_retryPending`, for the
  environment switch's own `singleShot` redraw work that can settle
  after the handler returned. `eyeRetryTimer` kills the timer, clears
  the flag and calls `eyeReapply(0)` (no further retry).

Switch: `eye_reapply` (needs `eyewatch`). **Default off since 1.6.1**: live evidence
2026-11-26 shows the metacall landing while the post-process environment switch
still owns the scene hangs the hub (Application Hang of `EXStar Hub.exe`), and the
show half of the toggle flashes every hidden scan first. Opt in at your own risk.

## 9. Component F - the scene command watch and filter

| | |
|---|---|
| plugin | `sn3d.digit.scenecontrol.dll` |
| commands | `SetModelsVisible`, `SetModelNodesVisible`, `SetCurrentEnv` (`SCENE_N` = 3, indices 0/1/2) |
| anchor | `sceneAnchor()` - by command **name**, no RVAs |
| detours | `det_scene0/1/2` -> `sceneCall(idx, ...)` |
| switches | `scene_watch` (log), `scene_filter` (withhold), `eye_reapply` (uses index 2) |

`sceneAnchor()` finds the handler a plugin registered for a named
command: locate the command string in an initialized-data section, find
the `lea rdx,[rip+string]` that materialises it in a code section, then
the next `lea rcx,[rip+handler]` that is stored with the same pattern
(`mov [rsp+disp8],rcx`) and points into a code section. The plugin wires
all of its commands in one constructor, so the shape is stable; the
offline enumeration is `tools/scmd_enum.ps1` (43 commands on this build).

`sceneCall()` per call:

1. `scene_watch` (and `verbose`, and budget left) -> `sceneProbe()`:
   finds the QVariantMap argument and describes it as memory only.
2. `scene_filter`: `sceneHideDecide()` for `SetModelsVisible`,
   `sceneNodeDecide()` for `SetModelNodesVisible`; a non-zero answer
   means the command is **not** passed on and `0` is returned.
3. forward all eight argument slots to the original handler (Microsoft
   x64: four registers plus the caller's stack slots - forwarding four
   only once killed the hub in `Qt5Core` because a handler that reads
   its fifth argument got the detour's frame).
4. index 2 additionally triggers `eyeReapply(1)` afterwards.

Reading a command is **pure memory inspection**, and that is a hard
rule, not an optimisation: a hook may never call vendor code with a
pointer the module derived itself. The Qt layouts used here are measured
by `tools/qlist_lab.c` and shared with the module through `qt_read.h`
(`readQName5()`, `plausiblePtr()`, `qlistRead()`), the QVariant tag and
bool readers are `qvTag5()`/`qvBool5()`, the map is `qmapFind()`
(root at `data+0x10`, node `{ left +0x8, right +0x10, key +0x18,
value +0x20 }`). Anything not fully readable is passed through.

The map walk (`qmapDumpD()`, the probe only) is bounded three ways, and
1.6.1 showed each bound is load-bearing: `sp < WALK_SLOTS` (64 pointers,
counted in **elements** - compared in bytes it allowed 511 pushes into a
64-slot array, straight over the saved registers and the return address),
`visits < WALK_VISITS` (256 nodes for the whole walk - bounding only the
pending stack is not enough, because a non-map argument read at the wrong
root offset yields a graph whose children point back into it: the walk
then pops one and pushes one forever), and a root gate (a walk starts
only when the root node has a readable key, which every real command map
does). The 1.6.1 symptom was the hub hung at 100% of one core on the
first eye click, one thread inside `readQName5()` called from
`qmapDumpD()`; evidence and the dump reading are in
`docs/research/merge-common-mesh.md` section 19.

The filter rule, both commands: a name set `g_hid[]`
(`HID_MAX` = 64, `quHash`-keyed, SRW-locked, built by
`hidAdd()` from `visible=false` commands, `hidDel()` on a single-name
show = "the user clicked this eye again"). A show request is withheld
only when it names several models and at least one of them is in the
set. On 2.1.0-10 **nothing sends a command that names models while
showing them** (measured, docs sections 11-17), so `scene_filter` has
nothing to intercept today and stays off in the shipped ini - it is kept
because a vendor build that does send one would be filtered correctly,
and because it fails open.

## 10. Data files

| file | role | written by |
|------|------|-----------|
| `C:\Users\Public\Shining3d\EXStar Hub\selective-mesh.ini` | the selection: `[folder]` and `[uuid]` sections with `included = 0|1`, plus `[Settings] enabled` master key | the hub (eye), the manager's roster window, the user |
| `...\selective-mesh.map.txt` | discovery roster: one line per scan seen at the gate, with the ready-to-copy section name and the decision | the gate (`reportScan`, `SEEN_MAX` dedupe) |
| `...\merge-select.ini` | name used until 1.6.0; copied once to the new name, never deleted (backup) | older module versions |

Path resolution (`ex_init`): the `file` switch wins; otherwise
`SEL_FILE`, with the .map.txt derived as `<selFile without extension>.map.txt`.
`migrateSelFile()` runs **only** on the default path: if the new file is
missing and the old one exists, `CopyFileW(..., TRUE)` (race-safe -
several gated processes start at once, first writer wins, the others
fail with "already exists" and do nothing). The roster is not migrated;
it rebuilds itself on the next mesh run. `ensureSelFile()` creates the
file with a readable header on first use.

## 11. Configuration (`modules\selective_mesh.ini`)

Read once in `ex_init` through the host's `cfg_int`/`cfg_string`, so a
change needs a process restart. Defaults below are the code defaults;
the shipped ini states them explicitly and keeps `scene_filter = 0`.

| key | default | consumed by |
|-----|---------|-------------|
| `verbose` | 1 | every log line (the gate decision log and the scene probe also need budget) |
| `eyewatch` | 1 | worker's UI phase at all |
| `eye_gate` | 1 | eye -> .ini persistence, the gate's uuid consultation |
| `eye_reapply` | 0 (since 1.6.1 - on, the toggle can hang the hub during the env switch) | component E, and arming `SetCurrentEnv` for it |
| `scene_watch` | 1 | `sceneProbe` |
| `scene_filter` | 1 | `sceneHideDecide` / `sceneNodeDecide` |
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | `tryInstall` |
| `eyewatch_exes` | `EXStar Hub.exe` | worker's UI phase |
| `file` | unset -> `SEL_FILE` | `g_selFile` (+ derived map file, migration) |

The host also gates the log itself: `[host] log = 1` in `modules.ini`
must be on for anything here to reach `mod.log`.

## 12. Log catalogue

All lines are prefixed `selective_mesh: ` (`MOD_LOG`) and land in the
host's `mod.log`. "budget" = the one-shot counter behind the line
(`logBudget()`); when it runs out the line stops, so a hot path can
never flood the log.

| line | where | meaning / budget |
|------|-------|------------------|
| `migrated <old> -> <new> (the old file stays as a backup)` | `migrateSelFile` | first run of 1.6.0 with an older selection file |
| `<exe> is not in gate_exes - module stays inert here (no hooks, no files)` | `tryInstall` | expected once per non-gated process |
| `vendor exports not found (isGenerateCloud/getFolderPath/QString) - staying out of it` | `tryInstall` | build changed: fail-open, investigate |
| `thunk alloc failed - app stays stock` / `detour refused - app stays stock` | `tryInstall` | hook infrastructure refused: fail-open |
| `hooked HandlePrjFile::isGenerateCloud (active for Sn3DMultiProjects.dll callers) - see <file>` | `tryInstall` | gate armed |
| `gate <key> uuid=<uuid> eye=<-1|0|1> vendor=<0|1> <why> -> include|SKIP` | `decideCloud` | the per-scan decision (200) |
| `gate <no id> uuid=... ra=+0x... impl=... vendor=... <why> -> ...` | `decideCloud` | same, for a scan whose identity is unreadable (same budget) |
| `gate fault while reading the vendor object - this call keeps the stock answer` | `det_isCloud` | SEH caught something; logged once |
| `probe cloud? <module>+0x<X> self=... impl=...` | `probeLog` | a caller outside the merge code asked the question (60, per-site dedupe) |
| `idmap impl=<p> +0x<off> = <text>` | `idDump` | object map for an unnamed scan (6 dumps) |
| `uuidscan: <uuid> at self|impl+0x<X> [raw|swap|text]` | `uuidScan` | where the uuid actually lives (8) |
| `uuidscan: no hit (targets=<n>) ...` | `uuidScan` | the known-uuid search missed (3) |
| `eye <uuid> -> VISIBLE|hidden` | `eyeRecord` | an eye toggled (40) |
| `eye-meta <Invoke|Query|...> idx=<n> via <module>+0x<X>` | `det_meta` | metacall traffic survey (32 distinct pairs) |
| `eye controller found: metaobject=+0x... metacall=+0x... setNodeVisible idx=<n>` | `eyeTry` | anchor hit; index must be 8 on 2.1.0-10 |
| `eye watch armed (recording show/hide events, re-apply after env switch on)` | `eyeTry` | metacall detour in place |
| `eye controller not found in ScanPage_E7.dll - watch disabled` / `eye metacall hook refused` | `eyeTry` | fail-open, UI half inactive |
| `eye watch not needed in <exe> (eyewatch_exes)` | `worker` | expected in sibling processes |
| `re-applied <n> hidden eye(s) after the environment switch` | `eyeReapply` | toggle pass ran (20) |
| `reapply toggled <uuid> (show+hide)` | `eyeReapply` | per scan (40) |
| `reapply skipped (controller=... thread=..., here=...)` | `eyeReapply` | wrong thread or no controller yet - by design, never from the worker (20) |
| `reapply faulted after <n> eye(s) - stopped, viewport stays stock` | `eyeReapply` | SEH caught the vendor call |
| `reapply retry after the env work settled` | `eyeRetryTimer` | the 300 ms deferred pass ran (20) |
| `scene <command> (arg<n>) <description>` | `sceneProbe` | what the viewport was asked, as memory (60 / 400 / 60 per command) |
| `scene <command> - no command map among args[...]` | `sceneProbe` | argument shape not recognised (same budgets) |
| `model hidden: <name>` / `<n> models hidden (first: <name>)` | `sceneHideDecide` | recorded in `g_hid[]` (80) |
| `model shown again: <name>` | `sceneHideDecide` | removed from `g_hid[]` (same budget) |
| `kept <n> of <m> models hidden (<names>) - a show-everything request was not passed on...` | `sceneHideDecide` | filter acted (200) |
| `kept <name> hidden - the mesh page asked to show its nodes...` | `sceneNodeDecide` | filter acted (200) |
| `watching scene command <name> (log|filter|both) at +0x<X>` | `sceneTry` | anchor hit and armed |
| `scene command <name> hook refused` | `sceneTry` | that one command stays unhooked |
| `no scene command handler found - viewport watch stays off` | `sceneTry` | anchor missed; the gate is unaffected |
| `dll notification unavailable, polling only` | `worker` | `LdrRegisterDllNotification` missing |
| `Sn3DFile.dll never mapped in this process - nothing to do here` | `worker` | 15 min timeout, gated allowlist but no merge code |
| `meta scan of <class> faulted` | `metaFind` | SEH during the offline-style scan |

## 13. In-memory state

| state | cap | owner / locking |
|-------|-----|-----------------|
| `g_eye[]` uuid+visible+seq | 64 (`EYE_MAX`) | written by the Qt thread in `eyeRecord`, read at the gate on merge threads; entries are 16-byte uuids + int + seq, torn reads can only mis-date an entry, never address memory |
| `g_hid[]` hash+text | 64 (`HID_MAX`) | SRW lock (`g_hidLock`), shared for lookups |
| `g_uLoc` cached uuid location | 1 | `{ level, off, lay }`, invalidated on a bad read |
| `g_probe[]` seen caller sites | 40 (`SITE_MAX`) | `g_probeBusy` guards the survey |
| `g_metaSeen[]` metacall traffic | 32 | `g_eyeBusy` |
| `g_seen[]` roster/log dedupe | 64 (`SEEN_MAX`) | `g_reportBusy` |
| counters: `g_installed`, `g_broken`, `g_eyeDone`, `g_sceneDone`, `g_retryPending`, `g_reapplyBusy` | - | interlocked, all fail-open on the safe side |

## 14. Failure modes

Every branch below leaves the app **stock** (the vendor answer, the
vendor command) - there is no path where an unrecognised situation
changes behaviour:

| situation | result |
|-----------|--------|
| any vendor object read faults | SEH, stock answer, one log line |
| exports/anchors not found | module idles, logs once |
| process not allowlisted | inert module, logs once |
| selection .ini missing / unreadable | everything is unlisted = stock |
| `[Settings] enabled = 0` | master off, gate answers the vendor's value |
| uuid unknown | eye input skipped, `[folder]` sections still work |
| command argument not fully readable | passed through untouched |
| command argument is not a map at all | probe walk never starts (root gate), bounded to 256 nodes if it does |
| hook refused by the host | stock, logs |
| re-apply on the wrong thread | skipped by design |
| deleting the selection .ini | behaviour returns to stock exactly |

## 15. Tooling around this module

| tool | what it proves |
|------|----------------|
| `tools/qmeta_dump.ps1` | the moc data of `ProjectListControllerE7` (slot index, `static_metacall` RVA) - the same layout the module reads; `qmeta_lab.c` proves the two match |
| `tools/xref_str.ps1` | every code site that references a string (e.g. the four `SetModelNodesVisible` senders) |
| `tools/scmd_enum.ps1` / `scmd_lab.c` | the scene plugin's whole command registry |
| `tools/qlist_lab.c` + `qt_read.h` | the `QStringList`/`QString` readers, tested against the shipped `Qt5Core.dll` |
| `tools/qv_layout.c`, `qmeta_lab.c` | QVariant layout and metatype tags, measured |
| `tools/func_str.c` | strings referenced inside a function's prologue->`ret` range |
| `tools/deploy_module.ps1` | install this module into the product folder (keeps `winmm.dll`/`ExStarRec.dll`, migrates the ini names) |

## 16. Naming (1.6.0 rename)

| thing | until 1.5.8 | from 1.6.0 |
|-------|-------------|------------|
| module id (dll basename) | `merge_select` | `selective_mesh` |
| display name | Merge Selection | Selective Mesh |
| folder / sources | `modules\merge_select\merge_select.c` | `modules\selective_mesh\selective_mesh.c` |
| host config | `modules\merge_select.ini` | `modules\selective_mesh.ini` |
| selection file | `merge-select.ini` | `selective-mesh.ini` (auto-copied, old kept) |
| roster file | `merge-select.map.txt` | `selective-mesh.map.txt` (regenerated) |
| log tag | `merge_select: ` | `selective_mesh: ` |

The id is the dll basename (`src/host.c`), and every section in
`modules.ini` defaults to enabled, so the old dll must not survive a
rename: `src/manager.c` keeps `merge_select` in `g_legacy[]`, which
deletes `modules\merge_select.dll`, its ini and its `modules.ini`
section on install, rewrites `sibling_modules`, and copies the old
switch file to the new name first.
