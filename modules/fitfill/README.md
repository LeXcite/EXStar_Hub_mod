# Zoom to Fit Fill

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file. Framework
> internals: [docs/architecture.md](../../docs/architecture.md). Full RE
> protocol: [docs/research/zoom-fit-optimization.md](../../docs/research/zoom-fit-optimization.md);
> batch journal: [batch-2026-11-17-status.md](../../docs/research/batch-2026-11-17-status.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Feature | Ctrl+D / «Zoom to Fit» frames the model into the viewport instead of leaving it at ~35% of the window |
| Root cause (how stock works) | the viewport's `CustomizeManipulator` runs in a "zoom optimization" mode (pinned camera distance); the vendor turns it **on** at view creation: `Sn3DCanvasView::init` (librenderkit.extensions.digit.dll, RVA 0x172070) executes `mov r10b, 1; mov rcx, rax; call [CustomizeManipulator::setIsZoomOptimization]` — the friend's version-bound one-byte file patch (`01`→`00` at file offset `0x171D68`, verified byte-identical spot on file stamp 3.1.x) simply cleared that immediate |
| Fix | **A (≤2.0.0-45):** a detour (`host->hook`, MinHook) on the **exported** setter `?setIsZoomOptimization@CustomizeManipulator@@QEAAX_N@Z` in `librenderkit.dll`, found by export name; every call is forwarded with the configured `force` value (default `0` = mode off). The setter is just `this[0x138] = flag`, so this is exactly equivalent to clearing the immediate — for every caller, not one. On 2.1.0-10 the flag is dead code (§6 of the RE doc) — the hook stays armed as a harmless no-op |
| Fix (2.1.0-10) | **B:** the new pipeline gates the fitted view on the digit singleton's **scan scale**; `sn3d.digit.scenecontrol.dll` turns it off at exactly one call site (`mov rcx,rax; xor edx,edx; call [setEnableScanScale]`, file offset `0xC3C65`). The module resolves the IAT slot of `?setEnableScanScale@RenderKitExtDigitSingleton@` from the import table, finds the unique `FF 15 <that slot>` shaped call site and rewrites the two bool-setup bytes to `mov dl,1` — in memory only, anchored by NAME, refuse-on-ambiguity |
| Key source file | `modules/fitfill/fitfill.c` (`detour`, `tryInstall`, `ex_iatSlot`, `tryScanScale`); export lookup via the shared `include\exstar_export.h` |
| Offline check | `dumpbin /exports librenderkit.dll` → unique `?setIsZoomOptimization@...` (file stamp 3.1.x RVA `0x56820`, value not used); Ghidra: setter/getter both touch `this[0x138]`; the init call site in extensions.digit still reads `B2 01 48 8B C8 FF 15 96 FA 0C 00` and `FF 15` resolves (via the import table) to exactly that setter |
| Runtime proof | `%LOCALAPPDATA%\ExStarHubMods\mod.log`: `fitfill: hooked ?setIsZoomOptimization@... , forcing value 0` then `fitfill: setIsZoomOptimization(1) -> forwarding 0`; on 2.1.0-10 additionally `fitfill: scan scale forced to 1 at sn3d.digit.scenecontrol.dll+0x...`; visually: Ctrl+D fills the frame, repeat presses re-fit |
| Do not ever | patch the immediate by file offset (friend's approach - died per update, semantics unknown to him); write `this[0x138]` directly from the module (the setter is the single documented write path - keep it that way); assume the 3.5-float constants matter (both candidates were live-tested by the friend to NOT affect the menu fit — the false trail is documented in his STATE.md and was not repeated here) |

The symptom (screenshots from the friend's investigation): after Ctrl+D
the part occupies ~35% width / ~45% height with the center shifted, and
a second press changes nothing. The whole `FitOperationAuto` /
`SceneCamera::fit` margin chase produced nothing live (3.5 → 1.6 gave
+4pp at most) — the real switch is the manipulator's zoom-optimization
flag, discovered as the semantics of the friend's mysterious byte
(`01 48 8B C8 FF 15 …` = `mov r10b,1` before a call to
`setIsZoomOptimization`; the import resolved via the PE import table).
The vendor passes `true` at view init and never touches it again in the
offline-discovered call set.

| | |
|---|---|
| Module id | `fitfill` |
| Version | 1.1.0 |
| Status | ✅ A live-verified on 2.0.0-45 (2026-11-17): `fitfill: hooked ... forcing value 0` + `setIsZoomOptimization(1) -> forwarding 0` in mod.log at view init, and the user confirmed Ctrl+D now fills the frame; RE chain verified offline end-to-end (call site → import → exported setter → field `0x138`). ⚠️ B (2.1.0-10 scan-scale anchor) implemented 2026-11-19, offline-verified against the live binaries (unique call site, import resolves to `setEnableScanScale`), **live test pending** |
| Mechanism | worker polls for both targets; A: `librenderkit.dll` → unique export by name (`ex_findExport`) → `host->hook`, detour forwards `force` (ini), logs first N calls; B: `sn3d.digit.scenecontrol.dll` → IAT slot by imported NAME (`ex_iatSlot`) → unique matching call site in read-only sections → 2-byte `VirtualProtect` rewrite under SEH + `FlushInstructionCache`; idempotent; silent idle in processes without the DLLs. `needsSiblings = TRUE`: 3D views render in sibling processes too |
| Requires | nothing beyond the renderer DLLs; no file changes, restart = stock |

## Why the setter, not the byte

The friend's r5-era patch flipped the `1` to `0` in
`librenderkit.extensions.digit.dll` at a fixed offset — his own plan
admitted the semantics were unexplained and the offset version-bound.
Here the same behaviour is expressed against the named C++ contract
(`setIsZoomOptimization`), which is:

- version-stable (the export name is the ABI; the build currently shipped
  (file stamp 3.1.x)
  even still has the identical call-site bytes);
- total: any *other* caller (another view kind, a future caller inside
  librenderkit) is also forced, instead of exactly one call site;
- reversible at runtime: `force = 1` restores the vendor value, `force
  = 0` applies the fix; `[fitfill] enabled = 0` disables everything.

Risk note (accepted): the optimization mode is the vendor default, so
disabling it globally *could* change interactive zoom feel somewhere;
the friend's final edition shipped this state and the user has run it.
The log shows every forced call, so any future surprise is traceable.

## Configuration

Enable/disable - `modules.ini`:

```ini
[fitfill]
enabled = 1
```

Behavior - `modules\fitfill.ini`:

```ini
target     = librenderkit.dll
needle     = ?setIsZoomOptimization@CustomizeManipulator@@
force      = 0        ; 0 = zoom optimization off -> fit fills the frame
log_calls  = 6
scan_scale = 1        ; 0 = mechanism B off
ss_target  = sn3d.digit.scenecontrol.dll
ss_import  = ?setEnableScanScale@RenderKitExtDigitSingleton@
ss_dll     = extensions.digit
ss_force   = 1        ; 1 = scan scale on -> fit fills the frame (2.1.0-10)
verbose    = 1
```

## Verification plan

1. start the app, open any 3D view: `fitfill: hooked ... forcing value 0`
   + at least one `setIsZoomOptimization(1) -> forwarding 0` (view init)
   + on 2.1.0-10 `fitfill: scan scale forced to 1 at sn3d...+0x...`;
2. Ctrl+D with a model: fills the frame (friend's symptom solved);
   press again after zooming: re-fits;
3. interactive zoom/pan sanity pass (see risk note) **and one real scan
   session** (scan scale is a vendor feature active during capture);
4. rollback: `enabled = 0` + restart.
