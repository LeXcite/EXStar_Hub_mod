# "Zoom to Fit" and the zoom-optimization mode — RE protocol

The knowledge behind the `fitfill` module. Status journal: `batch-2026-11-17-status.md`.

---

## 1. Symptom

After Ctrl+D / "Zoom to Fit" the part takes up about 35 % of the window's width and about
45 % of its height, the center is shifted down-right; pressing again changes nothing
(the friend's screenshots, confirmed by the user).

## 2. False lead (documented so we don't step on it again)

The QML chain `fitView()` → `Sn3DCanvasView::fitView` → `FitOperationAuto`
was verified live by the friend and is NOT the multiplier for the menu item:

- the constant `3.5` @ RVA 0x25F8C0 (librenderkit.extensions.digit,
  `SceneCamera::fit`): 3.5→1.6 gave +4 pp of height;
- `3.5` @ RVA 0x293524 (librenderkit, the distance function 0x180050990 from
  `FitOperationAuto`): 3.5→1.3 — visually no change.

His final patch was a single byte: `librenderkit.extensions.digit.dll`,
file offset `0x171D68`, `01`→`00` (reference r8; a byte-by-byte comparison of
the clean and final files — no differences left). He did not explain the semantics,
and the offset is version-dependent.

## 3. Byte semantics (2.x, file stamp 3.1.x; our recon)

The byte in the same spot is live: the code in the file —

```
... 48 85 C0 0F 84 05 01 00 00  B2 01 48 8B C8  FF 15 96 FA 0C 00 ...
                                  ^^
                     mov r10b, 1   (4th DL argument = bool true)
                     mov rcx, rax  (this = the manipulator)
                     call [rip+0xFA96]  → IAT slot
```

The breakdown required two checks:

- **the file↔RVA delta**. The export `?fitView@Sn3DCanvasView@...` has RVA
  `0x171560`; its prologue (`40 57 48 83 EC 50 ...`) lies in the file at
  `0x170960` — the delta is **.text: RVA = file + 0xC00** (a naive "browse the file
  by RVA" points at a foreign byte and gives a bogus target near `FF 15`).
  So the code byte at file `0x171D68` = RVA `0x172968`, inside
  `Sn3DCanvasView::init` (Ghidra: the function starts around `0x172070`).
- **the RIP-relative target of `FF 15`**. disp32 `0x0CFA96` from the RVA following
  the call gives slot `0x180242408` (RVA `0x242408`). `dumpbin /imports` does not print
  thunks by address — the import table was parsed by hand from DataDirectory[1]
  (ILD/INT, by name):

```
0x242400  librenderkit.dll  ?getRotateCenter@CustomizeManipulator@@...
0x242408  librenderkit.dll  ?setIsZoomOptimization@CustomizeManipulator@@QEAAX_N@Z
0x242418  librenderkit.dll  ?isMouseInGizemoFitRegion@CustomizeManipulator@@...
```

Bottom line: `init` turns "zoom optimization" on for the manipulator — a fixed camera
distance mode, which is exactly why fit fails to fit the part's extents. Setter/getter
(Ghidra, librenderkit.dll, exports 0x56820/0x51930):

```c
void CustomizeManipulator::setIsZoomOptimization(bool v) { this[0x138] = v; }
bool CustomizeManipulator::getIsZoomOptimization()       { return this[0x138]; }
```

The only call found is that same `init`. The friend's byte patch =
`mov r10b, 0` at one site; our equivalent — the same effect for **all** callers.

## 4. Implementation (`modules/fitfill`)

Detour via `host->hook` on the exported setter (name
`?setIsZoomOptimization@CustomizeManipulator@@`, uniqueness checked by
`ex_findExport`), forwarding a fixed `force` (ini, default 0 = mode off).
By name, not RVA; the `this[0x138]` field is never written by hand — all writes
go through the regular setter. `needsSiblings`: 3D views are also rendered in
sibling processes.

Live (2026-11-17): `fitfill: hooked ... forcing value 0`,
`setIsZoomOptimization(1) -> forwarding 0` ×5 (view creations), the user
confirmed: Ctrl+D fits the model.

## 5. Accepted risk and rollback

The optimization mode is the vendor default; disabling it globally may change how
interactive zoom feels. In the friend's final build this lived, no complaints; the log
shows every force. Rollback: `force = 1` (the vendor value) or
`enabled = 0`, nothing on disk.
