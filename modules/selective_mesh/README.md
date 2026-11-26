# Selective Mesh (`selective_mesh`)

**Version 1.7.2 · targets EXStar Hub 2.1.0-10 · memory-only, no disk patches**

Until 1.6.0 this module was named `merge_select` ("Merge Selection"). The
rename is identity only; the selection file moved with it
(`merge-select.ini` → `selective-mesh.ini`, copied once automatically,
old file kept as a backup) and the manager removes the old
`merge_select.dll` on the next install so the two ids cannot both hook
the same gate.

The module map - every hook, anchor, thread, state table, config key and
log line - is in [ARCHITECTURE.md](ARCHITECTURE.md) next to this file.
The evidence chain behind every vendor fact used here is
[docs/research/merge-common-mesh.md](../../docs/research/merge-common-mesh.md).

## What it does

A project can hold several scans. Stock EXStar Hub feeds **every** scan
that has "generate a point cloud" enabled into the single common mesh;
the only stock way to drop one scan from that merge is to delete it,
which also throws away its stored data.

Selective Mesh adds the missing switch. For any scan listed in

```
C:\Users\Public\Shining3d\EXStar Hub\selective-mesh.ini
```

with

```ini
[Project_1\Scan_2]
included = 0
```

the hub is told that this one scan has no cloud, so the vendor's own
merge code skips it - the same path a scan with cloud generation turned
off already takes. Everything else about that scan (its data, display,
individual export) is untouched. Scans that are not listed behave
exactly like stock.

## How to use it

The everyday way: **hide the scan's row in the project list (eye
icon)** - the next common-mesh build leaves it out. Show it again to
bring it back. Once is enough: the choice is written into
`selective-mesh.ini` and survives restarts, so later merges need no
preparation at all. Since 1.5.8 the scan also *stays* hidden on screen
after the Mesh Model page redraws (that part is off by default since
1.6.1, see below). Everything else below is the manual override for
scripts and power users.

**Hiding every eye is not a selection, and since 1.7.0 it is not a dead
end either.** Stock EXStar Hub reacts to "no scan takes part" by starting
the merge with an empty input: its post-process step never reports done,
so the Processing bar stops around 80 %, the page buttons stay dead and
only closing the hub clears it (measured 2026-11-26,
[evidence](../../docs/research/merge-common-mesh.md) section 20). So when
a merge would get *nothing* because this module hid every scan of the
project, it hides nothing instead: that one run merges every scan the
vendor itself would have merged, exactly like with all eyes open. Nothing
is written back - the eye icons, `selective-mesh.ini` and the next run
stay as they are, and the vendor's own "generate a point cloud" flag
still decides (a scan with that off stays out, as in stock). Switch
`all_hidden = 0` to keep the stock dead end.

*Status, 2026-11-26: live-verified on the installed build - with all
three eyes hidden the merge finished (no 80 % stall) and the log read
`all 3 scans of this merge are hidden - exclusions ignored, every scan
goes in`; with one eye shown it read `at least one stays in` and merged
only the visible scans, as before. 1.7.1 dropped the precondition that
made 1.7.0 refuse to act; 1.7.2 corrects only names and records. The
container layout itself is proven outside the product by
`tools\merge_list_lab.c` against the shipped Qt.*

1. Install/enable the module in the Mod Manager and start a common-mesh
   run once so the hub sees each scan.
2. Open `selective-mesh.map.txt` (same folder): the module records each
   scan with its ready-to-copy section name.
3. Add the scan you want to drop to `selective-mesh.ini`, e.g.
   `[Project_1\Scan_2]` / `included = 0`.
4. Re-generate the common mesh. The scan is left out. Delete the section
   (or the file) to bring it back.

Section names are matched as `<parent folder>\<leaf folder>`; a scan's
full folder path also works and is checked first. Since 1.1.4 the scan
name is also read the way the vendor itself reads it inside
`HandlePrjFile::getProjInfo` - an embedded string in the object behind
`self+0x18` - so live session scans get an identity even while the
exported folder/name getters return empty (a one-shot `idmap` object
dump lands in the mod log if even that ever goes blank).

Since 1.2.0 the module additionally **watches the eye icons** of the
project list (the vendor slot `ProjectListControllerE7::setNodeVisible`,
located at runtime through its Qt meta-object - no RVAs). Since 1.4.0
every toggle is also **persisted into `selective-mesh.ini` as a
`[<UUID>]` section** (`included = 0/1`) - the scan `QUuid` is the one
at `HandlePrjFile+0x38`, the same bytes the community patchset keys its
uuid sections with. The gate consults that section first, so the eye
state follows the scan into every process that runs the merge -
including the fusion engine (`Sn3DprocessManager.exe`), which builds
the scan-page mesh in its own process and has no UI of its own. The
hub is the only writer; the merge processes are readers.
`eye_gate = 0` restores stock behaviour, explicit `[folder]` sections
keep working, and everything stays fail-open.

Since 1.5.0 the module can also watch what the viewport is asked to
show. The intent was to keep a hidden row hidden: opening the Mesh Model
page appears to redraw every scan of the project, so 1.5.0 resolved the
scene's `SetModelsVisible` handler at runtime from the command's *name*
inside the scene plugin (no RVAs), read the request the way the vendor's
own handler reads it, and did not pass on a request to show several
models while one of them was hidden. Nothing re-hidden afterwards, no
timers.

**Live evidence for 1.5.6 says that request is not what the Mesh Model
page sends** (`docs/research/merge-common-mesh.md`, sections 11 to 16):
one eye click sends `SetModelNodesVisible` (node keys of one model on one
view, no uuid), the press sends only `SetCurrentEnv` twice around the
merge, and a full offline enumeration of the plugin's command registry
(`tools\scmd_enum.ps1`, 43 commands) shows no command that keeps hidden
models hidden. So on 2.1.0-10 `scene_filter` has nothing to withhold and
never fires: the viewport coming back after a merge is the vendor's own
environment redraw and does not change what ends up in the mesh.
`scene_watch` stays useful as a diagnostic - it is what produced that
evidence.

**Since 1.5.8 the redraw no longer wins either: a hidden scan stays
hidden on screen.** The environment switch drops the node-level hide,
and re-asserting `setNodeVisible(uuid, false)` does nothing - neither
QML nor the scene act until the value actually *changes*, which 1.5.7
proved live (the re-issued slot emitted no command at all). So once the
vendor's `SetCurrentEnv` handler returns, the module *toggles* every
hidden eye - show, then hide - through the very slot a click uses, and
the UI re-sends its own hide command for the new environment; one retry
pass 300 ms later (a timer in the hub's own Qt thread) covers the
vendor's deferred apply. Switch `eye_reapply` - **default off since 1.6.1**:
in the 2026-11-26 verification the viewport did hold only the visible scan,
but a minute later the hub stopped responding (Application Hang): the toggle
metacall lands while the post-process switch still owns the scene. Until a
quiescence-checked design replaces it, a hidden scan may re-appear on the
viewport after a Mesh Model press - the merged mesh still excludes it
(`evidence: docs/research/merge-common-mesh.md section 18`).

Version 1.5.1 is the startup fix for that feature. Reading a command has
become **pure memory inspection**: the Qt type tag of every value and the
bytes behind it are read where they are stored, and the module calls no
vendor function at all while a scene command is in flight (1.5.0 asked
`QVariant::typeName`/`toBool` what a value was - an address the module
had derived itself, handed to Qt, and the hub faulted in `Qt5Core.dll`
during start-up). The detour also hands the handler all eight argument
slots of the Microsoft x64 convention instead of four, so a handler that
reads a fifth argument from the shadow area gets the caller's value
rather than the detour's frame. Anything the module cannot read
completely is passed through untouched.

Versions 1.5.2 to 1.5.5 made the reading of a command's `names` list match
*this* Qt build rather than a remembered one. The layout is measured, not
recalled: `tools\qlist_lab.c` builds a real `QStringList` with the shipped
`Qt5Core.dll`'s own `QString::split()` and runs the very reader the module
uses (`qt_read.h`, shared so the tool cannot drift from the shipping code).
In this build the payload qword of the value is the list's data pointer, the
count is the dword at `+0x0C`, and pointer-sized items - a QString is one -
lie inline from `+0x10`; a list of larger values keeps a node array one hop
further out, which is read too. An empty entry is a valid entry and simply
names nothing; anything that is not fully readable is passed through, as
everything else here is.

## How it works

The whole product asks one exported question to decide whether a scan's
cloud participates in the merge: `HandlePrjFile::isGenerateCloud`
(exported by `Sn3DFile.dll`, imported only by `Sn3DMultiProjects.dll`).
The module places one in-memory detour on that export and answers "no
cloud" for scans listed as excluded.

To keep the change contained, the detour only answers differently when
the **caller** is inside `Sn3DMultiProjects.dll` (a return-address
check); any other caller, including `Sn3DFile.dll` itself, gets the pure
vendor answer back. If an export cannot be resolved in some future build
the module logs that once and stays out of the way.

Every risky step (folder query, `QString` reads) runs under SEH and
falls back to the vendor's own answer; the module can only turn "yes"
into "no" for an explicitly excluded scan, so live scanning and
unlisted scans are provably unaffected. The one documented exception is
the all-hidden guard above: it turns this module's own "no" back into
"yes" when nothing would be left, and it never overrides the vendor's
"no" - a scan whose own cloud flag is off stays out either way.

**How the all-hidden guard knows the scan set.** The answer has to be
given on the *first* scan of the merge, and only the vendor knows how
many scans that merge holds. It also has them right there in the
registers saved by the call the module is already hooked: both merge
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

## Configuration (`modules\selective_mesh.ini`)

| key | default | meaning |
|-----|---------|---------|
| `verbose` | `1` | log each scan decision to the mod log (needs `[host] log = 1`) |
| `eyewatch` | `1` | record project-list eye show/hide events (observation) |
| `eye_gate` | `1` | hidden eye = scan left out of the common mesh, persisted as `[uuid]` sections (explicit `.ini` sections still win) |
| `all_hidden` | `1` | if this module would hide *every* scan of a merge, ignore the exclusions for that run instead of letting the vendor stall on an empty input (the eye icons and the `.ini` are left as they are) |
| `eye_reapply` | `0` | re-assert hidden eyes (toggle show+hide) after the scene's environment switch; **on, this can hang the hub** (metacall during the switch, evidence 2026-11-26) - off by default since 1.6.1 |
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | processes allowed to hook the merge gate; every other host keeps an inert module |
| `eyewatch_exes` | `EXStar Hub.exe` | processes allowed to watch the eye icons (the QML UI) |
| `scene_watch` | `1` | log the scene's named commands (diagnostic: what the viewport is asked to show). Reading a command is memory-only and hard-bounded (64 pending nodes, 256 visited, walk only starts on a real map): an unbounded walk hung the hub on 1.6.1 |
| `scene_filter` | `0` | withhold a `SetModelsVisible` that would show a hidden model again; on 2.1.0-10 nothing sends that command, so today it is inert (see the note above) |
| `file` | stock path | full path to the selection `.ini` (the `.map.txt` sits next to it); the automatic migration from `merge-select.ini` only runs when this key is absent |

Neither `scene_*` switch changes what ends up in the merged mesh - that
is `eye_gate` and the `.ini` sections.

## Relationship to a community patchset

A shared community patchset implements a similar per-scan feature driven
by `mesh-selection.ini` with UUID section names in the same folder. The
two are independent and simply AND together: either mechanism excluding
a scan takes it out of the merge. The eye binding writes its own
`[uuid]` sections into `selective-mesh.ini`; the community file is never
touched.

## Files created at runtime (user-owned, safe to delete)

- `...\EXStar Hub\selective-mesh.ini` - the selection (created with a header on first use; copied from `merge-select.ini` once if that older file exists)
- `...\EXStar Hub\selective-mesh.map.txt` - the scan list seen during common-mesh runs
