# Marker Quality

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file; the full
> reverse-engineering trail is
> [docs/research/marker-pipeline.md](../../docs/research/marker-pipeline.md)
> and the calibration dataset behind the thresholds is
> [docs/research/marker-quality-status-2026-11-09.md](../../docs/research/marker-quality-status-2026-11-09.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Behavior change | tracking markers are painted by observation quality (dark red → yellow → green, green latches) instead of the stock uniform color |
| Two processes | scan side runs in `scanservice.exe` (frame observer), paint side in `EXStar Hub.exe` (recolorer); the host must be injected into both via `[host] sibling_all = 1` / `sibling_modules = marker_quality` |
| Scan-side hook | `HandlePrjFile::putFrameDataIntoFile` detoured **by export name** in scanservice; per-marker accumulation: Welford RMS of world position + ≥20° direction slots |
| Cross-process channel | snapshot file in `C:\Users\Public\Shining3d\EXStar Hub\` (deleted at session start, no cross-session state) |
| Paint-side hook | the vendor's per-frame marker color callback is found by **signature scan** (never patched on disk) and wrapped; recolor goes through the per-child osg uniform `frontInnerColor` |
| Color contract | red must stay dark (`R = 128/255 < 0.55`) so marker_inframe's "vendor red" detector (R ≥ 0.55, G ≤ 0.38, B ≤ 0.38) never fires on it; in-frame tag = quality color with green byte +1/255, exact float4 tuples exported via `ex_mod_inframe_snapshot()` |
| Key source file | `modules/marker_quality/marker_quality.c` |
| Verify at runtime | `verbose = 1` + `[host] log = 1` → calibration reports (`report_ms`) and publish cadence in `%LOCALAPPDATA%\ExStarHubMods\mod.log` |
| Version | 1.0.2 (current), tested on 3.1.49 |

During a scan every tracking marker is painted by how well the scanner
has observed it:

| Color | Meaning |
|---|---|
| dark red | seen at least once (deliberately dark — see the color contract below) |
| yellow | a decent observation set (enough frames, ≥2 directions) |
| green | fully tracked — position stable, seen from many sides. **Green latches**: it never decays during the session and is only reset by opening another project. |

Markers the scanner has never seen keep the vendor color.

| | |
|---|---|
| Module id | `marker_quality` |
| Status | tested on EXStar Hub 3.1.49 (Windows 11 x64) |
| Mechanism | scan side: a detour of `HandlePrjFile::putFrameDataIntoFile` (by export name) in `scanservice.exe` observes every frame, accumulates per-marker coverage (Welford RMS + ≥20° direction slots) and publishes a snapshot file into `C:\Users\Public\Shining3d\EXStar Hub\`; render side: in the hub the vendor's per-frame marker color callback (signature-scanned pointer, never patched on disk) is wrapped — after the vendor paints, visible markers get the quality color through the per-child osg uniform `frontInnerColor`. |
| Session scope | the snapshot file is deleted at session start and holds no state across sessions |

## Configuration

Enable/disable — `modules.ini` in the application directory (the Mod
Manager does this for you):

```ini
[marker_quality]
enabled = 1
```

Behavior — `modules\marker_quality.ini`:

```ini
green_frames     = 20    ; observations needed for green
green_spread_um  = 100   ; max RMS spread of the world position, µm
                         ; (live data: good markers 20..50 µm, poor >380 µm)
green_supported  = 4     ; distinct >=20 deg direction slots with >=3 frames
yellow_frames    = 8
yellow_supported = 2
red_r    = 128 red_g    = 24  red_b    = 24   ; stage colors (0..255),
                         ; red must stay dark: R < 139 keeps the
                         ; marker_inframe "vendor red" detector off it
yellow_r = 255 yellow_g = 200 yellow_b = 25
green_r  = 40  green_g  = 215 green_b  = 60
verbose          = 1     ; 0 = quiet log (no calibration/binding lines)
report_ms        = 5000  ; calibration report cadence (verbose only)
publish_ms       = 300   ; min gap between snapshot writes
scan_side        = 1     ; frame observation (scanservice.exe)
paint_side       = 1     ; recoloring (the hub renderer)
share_inframe    = 1     ; invisible in-frame tag for marker_inframe
```

Restart EXStar Hub after editing. The Mod Manager GUI exposes a
"marker positioning quality" preset row (грубое/среднее/точное/
сверхточное) - one click writes green_spread_um, green_frames and
green_supported together; the yellow thresholds stay ini-only.

## Multi-process note

Frames only arrive inside `scanservice.exe`, so the host must be active
there too. The Mod Manager maintains the whitelist for this:

```ini
[host]
sibling_all = 1
sibling_modules = marker_quality
```

(`dev_all`/`dev_modules` are the legacy developer aliases; when
`dev_all = 1` the host prefers them.)

## Cooperation with Marker In-Frame

[Marker In-Frame](../marker_inframe/README.md) detects "marker is in the
current scan frame" from the vendor's red `frontInnerColor` upload.
Because this module repaints that same uniform, the handshake is:

* read what the vendor just wrote (osg `Uniform::get(Vec4f)`); red
  (`R >= 0.55, G <= 0.38, B <= 0.38` — the shared contract) = in frame;
* in-frame markers get their quality color with the green byte +1/255
  (invisible tag) and the exact float4 tuples are exported from the DLL
  (`ex_mod_inframe_snapshot`); the inframe module matches uploads
  against them and enlarges those markers.

So a marker is enlarged while in frame *and* keeps its quality color.
`share_inframe = 0` disables the tagging (the inframe module then only
sees vendor red and enlargement stops for colored markers — not
recommended when both modules are on).

## Why these signals

The quality model mirrors what the vendor's own bundle adjustment uses:
a marker is trustworthy when its reconstructed world position stops
moving while the scanner views it from genuinely different directions.
Hence the three independent gates — observation count, position RMS and
direction coverage — instead of a single "frames seen" counter, and a
green that latches: once a marker contributed a stable pose to the
solution, downgrading the color afterwards would only fight the user.
