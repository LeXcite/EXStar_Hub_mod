# Laser plane range and mode caption — RE protocol

The knowledge behind the `laser50` module (lines + caption). Status journal:
`batch-2026-11-17-status.md`; the qrc machinery for the caption:
`scanpage-qml-resources.md`.

---

## 1. Symptom and goal

The scanner projects 25+25 laser lines, but only 19+19 = 38 reach the point cloud (meanwhile
the button lies "39 lines" — the model's nominal value). Goal: all 50 lines +
an honest "50 lines" caption on the button.

## 2. Control point: the `set_match_plane_id_vulkan` export

Recon on 2.x (file stamp 3.1.x), algorithmLzy.dll 2.1.908.63, sha256 `8c70bd0e…`.
`dumpbin /exports` yields exactly two relevant exports
(ordinals/addresses are values of this build, not a constant):

| # | ord | RVA | export |
|---|-----|-----|---------|
| 561 | 0x230 | 0x1A2880 | `?set_match_plane_id_opencl@Sn3DAlgorithm@@YA?AW4RetVal@1@PEAXAEBH111@Z` |
| 562 | 0x231 | 0x1A2890 | `?set_match_plane_id_vulkan@Sn3DAlgorithm@@YA?AW4RetVal@1@PEAXAEBH111@Z` |

Ghidra: the body of `set_match_plane_id_vulkan(ctx, &a,&b,&c,&d)` is logging
(`"set_match_plane_id_vulkan satrtId = "`, `"   endId = "`, `"  satrtIdR = "`,
`" endIdR = "` — vendor typos preserved) plus a tail call to
`FUN_1804b24b0`, which performs exactly four writes:

```c
*(ctx + 0x25534) = a;  *(ctx + 0x25538) = b;
*(ctx + 0x2553c) = c;  *(ctx + 0x25540) = d;
```

That is, the window `(start..end, startR..endR)` is set by **the caller** with its
by-reference arguments; it does not exist in the configs/QML.

## 3. Who calls it

Signature search for `set_match_plane_id_vulkan` across all DLLs of the application directory:

```
algorithmLzy.dll: the export itself
SnAlgAppPlat.dll: import name (the only importer)
```

No EXE links `SnAlgAppPlat.dll` statically (search across EXEs — zero) — the
pipeline is loaded dynamically. So the host process is identified only by the
module's runtime log (`armed in pid=` / `hooked`), which is what is implemented; the
module lives in all sibling processes (`needsSiblings`), and the hook half starts
once `algorithmLzy.dll` appears.

## 4. Three layers of intervention (why the export was chosen)

1. **Caller globals** (the community patchset's path, its `STATE.md`: patching
   `(3,21,3,21)`→`(0,24,0,24)` in the importer's data, target `cbcd3f42…`) —
   dead at the very first update, the victim process/module is known only for a
   specific version; never verified live by the patchset author.
2. **Algorithm context** `ctx+0x25534` — requires the instance pointer and a race
   with every new `set_...`.
3. **The exported setter** — a named ABI contract; a single point; other calls
   (the 7-line mode) are skipped by a strict tuple comparison.
   Chosen. Implementation: `host->hook` (MinHook) at the export entry, lookup by
   name (`ex_findExport`, an ambiguous needle = downtime), arguments are read
   under SEH, and on a `(3,21,3,21)` match the original is called with pointers to
   our copy `(0,24,0,24)` — the vendor's globals are never written.

Live confirmation (2026-11-17): `laser50: hooked ...`,
`call N: (3,21,3,21) -> remap` (×4 per scan session), user: "there are 25x25 lines".

## 5. Button caption: `ScanSettingModelProviderLaser` (ScanPage_E7.dll)

The QML record (qrc, details: `scanpage-qml-resources.md`), relevant lines:

```qml
// cross mode (line 556 of the dump):
text: qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)
// parallel mode (586):
text: qsTr("%1线").arg($scanSettingModelLaser.parallelLaserLineShowNum)
```

- the numbers (39/7) are properties of the C++ model `crossLaserLineShowNum` /
  `parallelLaserLineShowNum` (nominal values; in the DLL dump the name `crossLaserLineShowNum`
  appears 2× — the model's meta-object, not QML text: the text is stored packed);
- `qsTr("%1线")` — the translatable source: the EN catalog gives "%1 lines"
  (EN UI screenshot: "39 lines", "7 lines").

The patch: only the argument — `.arg($scanSettingModelLaser.crossLaserLineShowNum)`
→ `.arg(50)` (63 B → 21 B, re-deflate 9587 ≤ 9777). The UI language is
preserved, "7 lines" is untouched. Live: `1 caption(s) rewritten (ok)`, screenshot
"50 lines / 7 lines".

### The failed v1.0.0 variant (investigating the Russian-caption leak)

Symptom: after the patch the English UI showed the Russian word for
"lines" after the number (user report). Causes established by the teardown:

1. the patch changed the **translation key** (`qsTr("%1线")` → a hardcoded
   Russian plural "lines" form) — the EN
   catalog stopped matching, and the raw replacement came through;
2. the number was taken from the model (39), the suffix did not influence it — the goal
   ("50") was still not reached.

The rule is pinned in the module's README: never reword a translatable source.

## 6. Why lines and caption are one module (v1.1.0)

A caption without lines promises 50 while there are 38; lines without a caption leave "39".
A separate toggle = a broken intermediate state, so `captions` was merged into
`laser50` (the `captions = 1` sub-toggle remained).
Process model: one module, the halves are independent — [lines] is driven by
`algorithmLzy.dll` (pipeline processes), [caption] by `ScanPage_E7.dll`
(the UI process); in a foreign process a half simply stays silent (the caption stops
polling the target after 2 minutes so the scan workers don't get noisy).
