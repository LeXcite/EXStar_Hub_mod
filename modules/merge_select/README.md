# Merge Selection (`merge_select`)

**Version 1.4.0 · targets EXStar Hub 2.1.0-10 · memory-only, no disk patches**

## What it does

A project can hold several scans. Stock EXStar Hub feeds **every** scan
that has "generate a point cloud" enabled into the single common mesh;
the only stock way to drop one scan from that merge is to delete it,
which also throws away its stored data.

`merge_select` adds the missing switch. For any scan listed in

```
C:\Users\Public\Shining3d\EXStar Hub\merge-select.ini
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
bring it back. Everything below is the manual override for scripts and
power users.

1. Install/enable the module in the Mod Manager and start a common-mesh
   run once so the hub sees each scan.
2. Open `merge-select.map.txt` (same folder): the module records each
   scan with its ready-to-copy section name.
3. Add the scan you want to drop to `merge-select.ini`, e.g.
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
every toggle is also **persisted into `merge-select.ini` as a
`[<UUID>]` section** (`included = 0/1`) - the scan `QUuid` is the one
at `HandlePrjFile+0x38`, the same bytes the community patchset keys its
uuid sections with. The gate consults that section first, so the eye
state follows the scan into every process that runs the merge -
including the fusion engine (`Sn3DprocessManager.exe`), which builds
the scan-page mesh in its own process and has no UI of its own. The
hub is the only writer; the merge processes are readers.
`eye_gate = 0` restores stock behaviour, explicit `[folder]` sections
keep working, and everything stays fail-open. The mesh dialog's own
"show everything" burst is direct vendor C++ (bypasses the meta-call),
so it never writes a section - a hidden scan stays excluded even while
the dialog momentarily re-shows it in the 3D view.

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
unlisted scans are provably unaffected.

## Configuration (`modules\merge_select.ini`)

| key | default | meaning |
|-----|---------|---------|
| `verbose` | `1` | log each scan decision to the mod log (needs `[host] log = 1`) |
| `eyewatch` | `1` | record project-list eye show/hide events (observation) |
| `eye_gate` | `1` | hidden eye = scan left out of the common mesh, persisted as `[uuid]` sections (explicit `.ini` sections still win) |
| `gate_exes` | `EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe` | processes allowed to hook the merge gate; every other host keeps an inert module |
| `eyewatch_exes` | `EXStar Hub.exe` | processes allowed to watch the eye icons (the QML UI) |
| `file` | stock path | full path to the selection `.ini` (the `.map.txt` sits next to it) |

## Relationship to a community patchset

A shared community patchset implements a similar per-scan feature driven
by `mesh-selection.ini` with UUID section names in the same folder. The
two are independent and simply AND together: either mechanism excluding
a scan takes it out of the merge. The eye binding writes its own
`[uuid]` sections into `merge-select.ini`; the community file is never
touched.

## Files created at runtime (user-owned, safe to delete)

- `...\EXStar Hub\merge-select.ini` - the selection (created with a header on first use)
- `...\EXStar Hub\merge-select.map.txt` - the scan list seen during common-mesh runs
