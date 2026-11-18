# Wallpaper

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file. Framework
> internals: [docs/architecture.md](../../docs/architecture.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Feature | user picture as the application background instead of the stock one |
| Root cause (how stock works) | the vendor embeds the wallpaper as an **uncompressed JPEG** in a mapped read-only section of `AppUi.dll` (1920x1080 24bpp, 837 517 B, file offset `0x643F95` on file stamp 3.1.x); QML shows it as `ApplicationWindow.background: Image { source: "qrc:/images/MainBackground.jpg" }` - it is a static resource, never a screenshot |
| Fix | at DLL-map time, find the JPEG **structurally** (largest `FF D8 FF`..first-`FF D9` extent >= `min_slot` in readable non-writable sections) and overwrite it in the mapped image with `modules\wallpaper.jpg`; Qt decodes the replacement on first paint |
| Key source file | `modules/wallpaper/wallpaper.c` (`jpegSlotSize`, `patchImage`, `loadImage`) |
| Offline check | `tools\wp_dryrun.c` - same scan on the DLL file; must print `SOI @0x643F95 slot=837517  MATCH known-good` |
| Runtime proof | `%LOCALAPPDATA%\ExStarHubMods\mod.log`: `wallpaper: AppUi.dll slot +6572949 x 837517 B <- <n> B image (ok)` |
| Do not ever | store the slot by offset/RVA (it moves with vendor updates - the friend's disk patch died exactly this way); write more than the slot size; assume the last `FF D9` ends a JPEG (byte stuffing makes the **first** unescaped `FF D9` the true EOI) |

EXStar Hub always paints the same photo behind the whole UI. The picture
is not a themeable asset - it is raw JPEG bytes inside `AppUi.dll`. This
module replaces those bytes **in process memory** the moment the DLL is
mapped (before its DllMain, long before Qt decodes the image), so the
custom picture is up from the first frame. Nothing on disk changes: the
DLL file is never written; deleting `modules\wallpaper.jpg` (the Mod
Manager's *Reset* button) restores the stock wallpaper on next start.

| | |
|---|---|
| Module id | `wallpaper` |
| Version | 1.0.0 |
| Status | ✅ live-verified on 2.x (file stamp 3.1.x, 2026-11-14), re-verified on 2.1.0-10 (file stamp 3.1.49): gradient test image (55 996 B) replaces the stock background window-wide, log shows slot found at exactly 837 517 B; deleting the file -> module declines cleanly, stock wallpaper back; correctly idle in sibling processes (`scanservice.exe`) via the host sibling whitelist and the `exe = hub` gate. End-to-end through the Mod Manager GUI with a user photo: *Choose file…* converts + installs, *Reset* restores stock |
| Mechanism | `LdrRegisterDllNotification` (patches AppUi.dll at map time) + worker thread for the already-mapped case and as a 5-min polling fallback; one `VirtualProtect` over the slot; SEH around every dereference; silent refusal on any mismatch |
| Requires | a JPEG at `<app>\modules\wallpaper.jpg` (the Mod Manager converts any jpg/png/bmp to 1920x1080 JPEG fitted under the slot budget); no GL hooks |

## Why the slot search is structural

The vendor resource has no exported symbol, its position drifts between
versions, and the qrc tree references it only by name inside compressed
blobs. What *is* stable: it is by far the largest JPEG in the mapped
image. So a pass over every readable, non-writable section collects all
`FF D8 FF` starts and keeps the candidate with the largest
SOI..first-`FF D9` extent that is still >= `min_slot` (300 000 B; UI
icons are all far smaller). The first unescaped `FF D9` is the EOI
because JPEG entropy-coded data byte-stuffs every literal `0xFF` with
`0x00`, so an unescaped `FF` inside scan data is always a real marker.
Verified offline: the extent yields exactly the resource size
(837 517 B) - `tools\wp_dryrun.c` asserts this known-good pair.

The replacement must fit the slot. The Mod Manager guarantees this by
re-encoding with decreasing JPEG quality (95 -> 26) until the file is
under the budget (default = the known stock slot size, overridable with
`max_bytes` in `modules\wallpaper.ini`); the module re-checks and keeps
the stock wallpaper if the check fails.

## Configuration

Enable/disable - `modules.ini` in the application directory:

```ini
[wallpaper]
enabled = 1
```

Behavior - `modules\wallpaper.ini`:

```ini
target   = AppUi.dll     DLL carrying the wallpaper image
file     = wallpaper.jpg image next to the module DLL
exe      = hub           only run when the executable name contains this
min_slot = 300000        smallest JPEG accepted as the wallpaper slot
verbose  = 1             log slot/patch details to mod.log
```

The user picture itself is **not** configured here: the Mod Manager card
*Wallpaper* has two buttons - *Choose file…* (opens a file picker,
converts the chosen jpg/png/bmp to 1920x1080 JPEG via GDI+, aspect
preserved and centered on black, and writes `modules\wallpaper.jpg`) and
*Reset* (deletes it, stock wallpaper returns). Changes apply on the next
EXStar Hub restart; a new version of the app re-finds the slot by
structure, so an update can only cause a silent skip, never a crash.

## Verification log (2026-11-14, file stamp 3.1.x)

```
wallpaper v1.0.0 armed in pid=31740: 55996 B image, target AppUi.dll
wallpaper: AppUi.dll slot +6572949 x 837517 B <- 55996 B image (ok)
```

- visual: screenshot showed the custom gradient across the full window
  (sidebar, canvas, header all painted over the new background);
- reset: after deleting `modules\wallpaper.jpg` the module logs
  `no ...\wallpaper.jpg - stock wallpaper kept` and declines to start;
  stock background confirmed by screenshot;
- siblings: `host: skipping 'wallpaper' (not in sibling whitelist)` in
  every `scanservice.exe` child; the `exe = hub` gate is a second net.

## Known bugs fixed (do not reintroduce)

- candidate pointer computed as `p + off + 3` while ignoring the offset
  returned by the pattern search: produced a garbage "slot" of 2.9 MB at
  `section start + 3` and the SOI re-check refused it
  (`same=-1` in the diagnostic log). The search offset must be added
  (`p + off + o`), mirrored in `tools\wp_dryrun.c`;
- manager `wpEncoderClsid`: `Gdiplus::GetImageEncoders(num, size, buf)`
  does not report the encoder count (it comes from
  `GetImageEncodersSize`). A count left at 0 made the loop empty, the
  JPEG encoder "absent" and every conversion fail with the misleading
  *does not fit the budget* message (2026-11-14, first user test);
- manager card button row needs `cardHeightOf()` extra or the buttons
  overlap the next card.
