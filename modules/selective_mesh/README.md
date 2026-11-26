# Selective Mesh (`selective_mesh`)

**Version 1.8.0 · targets EXStar Hub 2.1.0-10 · memory-only, no disk patches**

Until 1.6.0 this module was named `merge_select` ("Merge Selection"). The
rename is identity only; the manager removes the old `merge_select.dll` on
the next install so the two ids cannot both hook the same gate.

The module map - every hook, anchor, thread, state table, config key and
log line - is in [ARCHITECTURE.md](ARCHITECTURE.md) next to this file.
The evidence chain behind every vendor fact used here is
[docs/research/merge-common-mesh.md](../../docs/research/merge-common-mesh.md).

## What it does

A project can hold one scan or fifty. Stock EXStar Hub feeds **every** scan
that has "generate a point cloud" enabled into the single common mesh; the
only stock way to drop one scan from that merge is to delete it, which also
throws away its stored data.

Selective Mesh adds the missing switch: **the eye icon on the scan's row in
the project list.**

```
eye open    ->  the scan is part of the common mesh   (stock behaviour)
eye hidden  ->  the scan is left out of the next common-mesh build
```

That is the entire interface. Nothing to edit, nothing to tick in another
window, nothing to prepare per project. The scan's own data, its display and
its individual export stay untouched - only the merge input changes, through
the vendor's own "this scan has no cloud" branch.

Since 1.8.0 the eye is the *only* interface. The older hand-edited sections
(`[Project_1\Scan_2] included = 0`), the `.map.txt` roster and the manager's
"Scan roster"/"Open folder" buttons are gone: they existed while an eye
could not be tied to a scan, and afterwards they were just a second way to
say the same thing - and a second way to be wrong, because a forgotten
section quietly outranked what the user could see on screen.

**The choice lives as long as the session.** Hiding an eye is a decision
about what you are looking at *now*, so it is not meant to outlive the
program: when the hub starts, the module drops the exclusions the previous
run left behind and begins from open eyes. Nothing accumulates between
sessions, and there is nothing to clean up.

## How to use it

1. Enable the module in the Mod Manager, start the hub, open a project.
2. Hide the eye of every scan that must not be in the final mesh.
3. Press Mesh Model / generate the common mesh. Only the visible scans are
   merged.

Show an eye again to put that scan back - no restart anywhere in this loop.

What this design rests on - that a re-opened project shows its scans again -
is an observation from 2026-11-24, not something the module enforces; what the
module guarantees by itself is the reset when the hub starts.

*The one case to know about:* inside a single run of the hub, if you hide a
scan, switch to another project and come back, the vendor shows that scan
again while the module still holds the exclusion you made - click the eye
twice (hide, show) and the two agree again. Closing and reopening the hub
always resets it. Whether the vendor re-issues its own "show" for every row
on a project re-open (which would clear this by itself) has not been measured
on the hub UI; it is the open question on this design, and the log line
`eye <uuid> -> VISIBLE` is what would answer it.
The hub UI writes what you hid into
`C:\Users\Public\Shining3d\EXStar Hub\selective-mesh.ini` (one `[<UUID>]`
section per hidden scan) because the mesh is often built in another process
- `scanservice.exe`, `Sn3DprocessManager.exe` - which has no project list and
no eyes of its own. That file is a bridge between processes, not a settings
file, and deleting it while the hub runs is the same as showing every eye.

**Hiding every eye is not a selection, and since 1.7.0 it is not a dead
end either.** Stock EXStar Hub reacts to "no scan takes part" by starting
the merge with an empty input: its post-process step never reports done, so
the Processing bar stops around 80 %, the page buttons stay dead and only
closing the hub clears it (measured 2026-11-26,
[evidence](../../docs/research/merge-common-mesh.md) section 20). So when
a merge would get *nothing* because this module hid every scan of the
project, it hides nothing instead: that one run merges every scan the
vendor itself would have merged, exactly like with all eyes open. Nothing
is written back - the eye icons and the next run stay as they are, and the
vendor's own "generate a point cloud" flag still decides (a scan with that
off stays out, as in stock). Switch `all_hidden = 0` to keep the stock dead
end.

*Status, 2026-11-26: live-verified on the installed build - with all three
eyes hidden the merge finished (no 80 % stall) and the log read `all 3 scans
of this merge are hidden - exclusions ignored, every scan goes in`; with one
eye shown it read `at least one stays in` and merged only the visible scans,
as before. 1.7.1 dropped the precondition that made 1.7.0 refuse to act;
1.7.2 corrected names and records only. The container layout itself is
proven outside the product by `tools\merge_list_lab.c` against the shipped
Qt.*

## How it works

The whole product asks one exported question to decide whether a scan's
cloud participates in the merge: `HandlePrjFile::isGenerateCloud`
(exported by `Sn3DFile.dll`, imported only by `Sn3DMultiProjects.dll`).
The module places one in-memory detour on that export and answers "no
cloud" for a scan whose eye is hidden.

To keep the change contained, the detour only answers differently when the
**caller** is inside `Sn3DMultiProjects.dll` (a return-address check); any
other caller, including `Sn3DFile.dll` itself, gets the pure vendor answer
back. If an export cannot be resolved in some future build the module logs
that once and stays out of the way.

**How the eye is watched.** The eye icon is the vendor QML slot
`ProjectListControllerE7::setNodeVisible(QUuid, bool)`. The class is not
exported, so the module finds its Qt meta-object inside the plugin by
matching the class-name string, checks that method 8 of its own method table
is really called `setNodeVisible`, and only then detours the class's
`qt_static_metacall`. QML eye toggles flow through that call; direct C++
calls (the mesh dialog's force-show burst) do not, which is itself useful
information.

The scan's `QUuid` is the key on both sides: it is what the slot receives,
and at the gate it is found in the scan object (offset located at runtime,
in both QUuid byte orders and as text). The eye state therefore follows the
scan into the process that runs the merge, and the hub is the only writer.

**Why a file, and not shared memory:** the decision is made once per scan at
the moment the merge payload is built, in a process that starts on demand. A
one-line-per-hidden-scan `.ini` is readable by every process without any
synchronisation object, it survives nothing longer than the session by
design, and it is the one artefact a user can open and read when a merge did
not take the shape they expected.

**What the decision reads.** Since 1.8.0 it is memory only: the scan's uuid,
the in-process eye map, the session `.ini`. No vendor getter is called, no Qt
function is called. That is not cosmetics - the version that asked
`QString` getters for a folder name faulted inside `Qt5Core.dll` (1.5.0), and
the rule since then is that a hook never hands vendor code a pointer it
derived itself. Everything in the decision runs under SEH, and any fault
keeps the vendor's own answer. The module can only turn "yes" into "no" for
an eye the user hid; the single documented exception is the all-hidden guard,
which turns this module's own "no" back into "yes" when nothing would be
left, and never overrides the vendor's "no".

**How the all-hidden guard knows the scan set.** The answer has to be
given on the *first* scan of the merge, and only the vendor knows how
many scans that merge holds. It also has them right there in the registers
saved by the call the module is already hooked: both merge
senders in `Sn3DMultiProjects.dll` (+`0x7E3BB`, +`0x83097`) build a
`QList` of scan items for the loop, and the live block dump shows that
list in **`r9`** at the moment they ask about each item. The hand-built
thunk therefore hands the helper the caller's saved registers as well, and
[merge_list.h](merge_list.h) reads that list as plain memory - no Qt
call, the mistake 1.5.0 made. Its header layout (`ref`, `alloc`,
`begin`, `end`, slots right behind) was measured against the shipped
`Qt5Core.dll` (5.15.10) and is re-checked by `tools\merge_list_lab.c`,
which builds a real `QStringList` with Qt's own code, drops fake scan
items into its slots and runs the shipping reader on it.

Nothing is trusted on sight - including which register holds what. A
candidate register counts as the merge list only when one of its items
**is** the scan the vendor called for (`item+0x28 == self`), and the first
decision of every process logs the raw block. That rule, not a register
name, is what makes the guard correct: measured live on 2026-11-26, the
list arrives in `r9`, the scan object `self` is in *none* of the saved
registers, and the slot an early revision called `rcx` holds a code
address of the merge DLL. Any doubt and the guard stays out of the way.

## Keeping a hidden scan hidden on screen

An eye hide lives on scene nodes, and the Mesh Model page switches the scene
environment - the redraw does not carry that state, so a hidden scan can show
up on the viewport again. It does not change the merged result: the gate
excluded that scan anyway.

1.5.8 fixed the viewport by *toggling* the hidden eye (show, then hide)
through the very slot a click uses once the vendor's `SetCurrentEnv` handler
returns - re-asserting the same value does nothing, because neither QML nor
the scene acts unless the value changes (proved live in 1.5.7: the re-issued
slot emitted no command at all). **`eye_reapply` has been off by default
since 1.6.1:** in the 2026-11-26 verification the viewport did hold only the
visible scan, but a minute later the hub stopped responding (Application
Hang) - the toggle metacall lands while the post-process switch still owns
the scene. Until a quiescence-checked design replaces it, a hidden scan may
re-appear on the viewport after a Mesh Model press, and the merged mesh still
excludes it (*evidence: docs/research/merge-common-mesh.md section 18*).

`scene_watch` (default on) logs the scene's named commands -
`SetModelsVisible`, `SetModelNodesVisible`, `SetCurrentEnv` - and is purely
diagnostic: it is what proved that an eye click sends node keys of one model,
that a Mesh Model press sends only `SetCurrentEnv` twice around the merge,
and that none of the plugin's 43 commands keeps hidden models hidden
(`tools\scmd_enum.ps1`, evidence sections 11-17). Reading a command is memory
inspection with hard bounds (64 pending nodes, 256 visited, walk starts only
on a readable key): an unbounded walk hung the hub once in 1.6.1, because a
non-map argument whose pointers refer back to itself is walked forever
(section 19).

`scene_filter` (default off) would withhold a `SetModelsVisible` that shows a
model recorded as hidden. On 2.1.0-10 nothing sends that command while showing
models, so it never fires; it stays because a different vendor build might,
and because it fails open.

Neither `scene_*` switch changes what ends up in the merged mesh - that is
`eye_gate`.

## Configuration (`modules\selective_mesh.ini`)

| key | default | meaning |
|-----|---------|---------|
| `verbose` | `1` | log each scan decision to the mod log (needs `[host] log = 1`) |
| `eyewatch` | `1` | watch the project-list eye icons - without it the module cannot know what you hid |
| `eye_gate` | `1` | the eyes decide participation: a hidden scan is left out of the next common mesh; `0` = watch and log only |
| `all_hidden` | `1` | if this module would hide *every* scan of a merge, ignore the exclusions for that run instead of letting the vendor stall on an empty input (the eye icons are left as they are) |
| `eye_reapply` | `0` | re-assert hidden eyes (toggle show+hide) after the scene's environment switch; **on, this can hang the hub** (metacall during the switch, evidence 2026-11-26) - off by default since 1.6.1 |
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | processes allowed to hook the merge gate; every other host keeps an inert module |
| `eyewatch_exes` | `EXStar Hub.exe` | processes allowed to watch the eye icons (the QML UI) |
| `scene_watch` | `1` | log the scene's named commands (diagnostic). Memory-only and hard-bounded: an unbounded walk hung the hub on 1.6.1 |
| `scene_filter` | `0` | withhold a `SetModelsVisible` that would show a hidden model again; on 2.1.0-10 nothing sends that command, so today it is inert |
| `file` | stock path | full path of the session `.ini` the hub writes; a lab switch, not a user setting |

## Relationship to a community patchset

A shared community patchset implements a similar per-scan feature driven
by `mesh-selection.ini` with UUID section names in the same folder. The
two are independent and simply AND together: either mechanism excluding
a scan takes it out of the merge. The eye binding writes its own
`[uuid]` sections into `selective-mesh.ini`; the community file is never
touched.

## Files created at runtime (user-owned, safe to delete)

- `...\EXStar Hub\selective-mesh.ini` - the exclusions of the current run of
  EXStar Hub, written on each eye click (created with a readable header on
  first use, `[uuid]` sections dropped when the hub starts, deleting it is
  the same as showing every eye)

Nothing else. The roster file the module used to keep
(`selective-mesh.map.txt`) is not written anymore; a leftover copy can be
deleted.
