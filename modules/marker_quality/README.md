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
| Behavior change | the disc of a tracking marker shows observation quality (dark red → yellow → green, green latches) and its outline shows whether the scanner sees it right now (blue) — the vendor's red flash is gone |
| Two channels | disc = `frontInnerColor` (ours, quality), outline = `frontExternalColor` (ours, in-view). The vendor only ever writes the disc at runtime, so the outline costs no fighting |
| Two processes | scan side runs in `scanservice.exe` (frame observer), paint side in `EXStar Hub.exe` (recolorer); the Mod Manager writes `[host] sibling_all` / `sibling_modules` from its own toggle — nothing to hand-configure |
| Scan-side hook | `HandlePrjFile::putFrameDataIntoFile` detoured **by export name** in scanservice; per-marker accumulation: Welford RMS of world position + ≥20° direction slots |
| Cross-process channel | snapshot file in `C:\Users\Public\Shining3d\EXStar Hub\` (deleted at session start, no cross-session state) |
| Paint-side hook | the vendor's per-frame marker color callback is found by **signature scan** (never patched on disk) and wrapped; recolor goes through the per-child osg uniforms |
| "In view" detector | read back what the vendor wrote into the disc uniform: `R ≥ 0.55, G ≤ 0.38, B ≤ 0.38` = the node is in the `CURRENT_MARKER` group this frame |
| Works without the scan side | the outline needs no id, no binding, no bridge file — it works from the first rendered frame. Only the quality colors need scanservice |
| Key source file | `modules/marker_quality/marker_quality.c` |
| Verify at runtime | `verbose = 1` + `[host] log = 1` → `%LOCALAPPDATA%\ExStarHubMods\mod.log`: `outline channel found/not available`, `in-view cue armed`, `mq: render hb …` counters, calibration reports |
| Version | 1.1.0 (current), tested on 2.x (file stamp 3.1.49) |

## What the marker says

| Signal | Meaning |
|---|---|
| white disc | the scanner has not observed this marker yet (stock look) |
| dark red disc | seen at least once |
| yellow disc | a decent observation set (enough frames, ≥2 directions) |
| green disc | fully tracked — position stable, seen from many sides. **Green latches**: it never decays during the session and is only reset by opening another project. |
| **blue outline** | the marker is in the scanner's field of view **in this frame** |
| black outline | out of view (stock) |

Before 1.1.0 the in-view state was the vendor's red disc, which the
quality palette also needs — so reading quality meant moving the marker
out of view and back. Splitting the two states over the two color
channels is what made both readable at the same time.

| | |
|---|---|
| Module id | `marker_quality` |
| Status | tested on EXStar Hub 2.x (file stamp 3.1.49), incl. 2.1.0-10 (Windows 11 x64) |
| Mechanism | scan side: a detour of `HandlePrjFile::putFrameDataIntoFile` (by export name) in `scanservice.exe` observes every frame, accumulates per-marker coverage (Welford RMS + ≥20° direction slots) and publishes a snapshot file into `C:\Users\Public\Shining3d\EXStar Hub\`; render side: in the hub the vendor's per-frame marker color callback (signature-scanned pointer, never patched on disk) is wrapped — after the vendor paints, every visible marker gets its disc and outline colors through its own osg `StateSet`. |
| Session scope | the snapshot file is deleted at session start and holds no state across sessions |

## How the outline is written

The callback runs once per rendered frame for the marker group, with
`owner = { ?, osg::Group*, visibility-array* }`. For every visible child
the module reads `frontInnerColor`, decides in-view from the vendor's
red, and writes:

* the **disc** — quality color if the palette has one for that marker id,
  white when the marker is in view but has no data yet (`hide_red`),
  otherwise untouched so the stock white stays;
* the **outline** — the highlight while the marker is in the scanner's view,
  and the value that was already in it the moment the marker leaves.

Both colors come from one place in the vendor's code: `MarkerPoint::createMarkerPoint`,
the vendor's own factory, builds a marker as an `osg::Geometry` and adds
`frontExternalColor` and `frontInnerColor` to its `StateSet`. The ring is
therefore not a global setting of the program, and the vendor never writes it
again at runtime — its in-view flash only ever touches the disc.

Reaching it took a detour worth recording. `StateSet::getUniform(name)` answers
NULL for `frontExternalColor` on every marker (measured live over 35 of them),
so the module reads the StateSet's uniform map itself, with the layout taken
from `osg::StateSet::getUniform` in `osg158-osg.dll` rather than from memory:
map at `StateSet + 0xb0`, heap-allocated header, root at `header + 8`,
`_Isnil` at `node + 0x19`, the key `std::string` at `node + 0x20` and the
`Uniform*` at `node + 0x40`. An earlier build had walked that map with the
offsets of Qt's `QMap`, which this module also uses elsewhere — that is where
the empty dumps and one wrong conclusion came from. Names are compared by hand,
so the `std::string` the vendor API wants is no longer part of the lookup.

Where that channel actually sits in a running scene is the part worth reading.
Measured 2026-11-28: a marker's own map holds **only** `frontInnerColor`, and
the `frontExternalColor` the ring paints with lives one `StateSet` *below* it —
and it is the **same uniform** for different markers (`found on one of the
marker's parts - channel 1871C3C0A70`, logged twice for two markers). So that
channel cannot be written: one write would outline the whole field.

Adding a channel of the module's own to the marker's `StateSet` is not enough
either, and `osg::State::pushUniformList` says why — quoted from OSG, the same
code is in `osg158-osg.dll` at RVA 0x18003b770:

    else if ((us.uniformVec.back().second & StateAttribute::OVERRIDE) &&
             !(aitr->second.second & StateAttribute::PROTECTED))
        // push existing back since override keeps the previous value.

A `StateSet` higher up the path keeps its value against one below it **only**
when its own entry carries `OVERRIDE_VALUES` (2) — and `addUniform(Uniform*,
mask)`, which keys by the uniform's name and stores `mask & 6`, is where that
bit lives. So:

| step | what happens |
|---|---|
| every frame | the marker's own `StateSet` map is read again — nothing about the channel is carried over from the previous frame, because the vendor replaces markers while it scans |
| in view | the entry is what is written; when the marker has none it is made (`getOrCreateUniform`, value type copied from the disc uniform) and armed with `OVERRIDE_VALUES`, since `getOrCreateUniform` inserts it with no override bits at all |
| out of view | the entry is **removed** (`StateSet::removeUniform`) — the vendor's channel below paints the ring again, with whatever value it holds at that moment |
| an entry that is not painted in the highlight color | left alone completely: only this module paints that color, so anything else there belongs to the vendor, and there would be no honest value to put back |

Ownership is read from the object rather than remembered, and nothing at all is
remembered between frames. Two live runs of 2026-11-28 measured the two ways of
getting this wrong:

- a resolved uniform pointer kept across frames points at something the scene
  may no longer draw — `made=163` channels for 49 markers, 207 highlight writes
  going nowhere, the outline visible for exactly one moment;
- a list of "the channels I made" is remembered state too, and it only shrinks
  when a marker is seen leaving the view. Under a scanner a marker is not taken
  out of view, it is replaced by a new node with a new `StateSet`, so the list
  never shrank: `made=1025 live=1024` for 49 markers, and the cue switched
  itself off at its own limit while every counter still looked healthy.

Guards: a fresh uniform holds `0,0,0,0`, so the highlight goes in before the bit
is ever used; the bit is re-asserted whenever the entry's override bits are not
`OVERRIDE_VALUES` again, which is what a rebuilt `StateSet` would leave; every
read and write goes through SEH and a value read is only trusted when it lies in
0..1, because it came out of a memory walk.

What you see also depends on the vendor's own shader, which contains
`if (marker_length_screen < innerDiameterTheshold) externalColor = frontInnerColor;`
— below 4 pixels on screen a marker paints its ring with the disc color, so the
outline cue appears once the marker is large enough to show a ring, which is
also when a cue is worth having.

`mod.log` says what was found, once per run:

| log line | meaning |
|---|---|
| `outline: the vendor's ring color comes from 0x…, below the marker: r g b a alpha x, entry override bits N` | where the stock ring really lives, and its bits — `4` would be `PROTECTED`, which nothing above can beat |
| `outline: made on the marker's own StateSet - channel 0x…, armed with OVERRIDE_VALUES (the marker's map now holds […])` | the module gave this marker a channel of its own; the map contents are printed |
| `outline: a marker carries its own "frontExternalColor", which this module does not own …` | that entry holds a color this module never paints, so the ring is left to the vendor and the cue stays off |
| `render hb … outline=… restored=… noChannel=… made=…` | highlight writes, entries taken back, in-view markers with nothing to write, channels created |

Quality colors do not depend on any of this, and the whole cue is one
`inframe = 0` away from a stock look.

## Configuration

Enable/disable — `modules.ini` in the application directory (the Mod
Manager does this for you; switching the card off leaves no hook anywhere):

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
red_r    = 128 red_g    = 24  red_b    = 24   ; stage colors (0..255)
yellow_r = 255 yellow_g = 200 yellow_b = 25
green_r  = 40  green_g  = 215 green_b  = 60
                         ; keep red_r below 140 with low green/blue: the
                         ; in-view detector reads the vendor red back
                         ; (R >= 0.55, G <= 0.38, B <= 0.38), so a bright
                         ; quality-red would light an outline on a marker
                         ; nobody is looking at

inframe      = 1         ; 0 = the in-view cue is off, markers look stock
inframe_r    = 0         ; highlight color
inframe_g    = 120
inframe_b    = 255
inframe_a    = 255       ; alpha 0..255; 255 = fully opaque
inframe_where = ring     ; ring = the outline (quality stays visible on the
                         ; disc); disc = paint the disc and hide quality
                         ; while the marker is in view
inframe_create = 1       ; 1 = the channel is lent: the vendor has no
                         ; frontExternalColor on a marker, so the module puts
                         ; one on the marker's own StateSet while it is in view
                         ; (value type taken from the disc uniform) and removes
                         ; it again when the marker leaves - out of view the
                         ; StateSet is what the vendor built, so no "not in
                         ; view" color is invented; 0 = the cue stays off
hide_red     = 1         ; 1 = an in-view marker with no quality data shows
                         ; a white disc instead of the vendor's red flash

verbose          = 1     ; 0 = quiet log (no calibration/binding lines)
report_ms        = 5000  ; calibration report cadence (verbose only)
publish_ms       = 300   ; min gap between snapshot writes
scan_side        = 1     ; frame observation (scanservice.exe)
paint_side       = 1     ; recoloring (the hub renderer)
```

Restart EXStar Hub after editing. The Mod Manager GUI exposes a
"marker positioning quality" preset row (Coarse/Medium/Fine/Ultra) — one
click writes `green_spread_um`, `green_frames` and `green_supported`
together; the in-view keys stay ini-only.

## Multi-process note

Frames only arrive inside `scanservice.exe`, so the host has to be active
there too:

```ini
[host]
sibling_all = 1
sibling_modules = marker_quality,selective_mesh
```

The Mod Manager recomputes both keys from its toggles on every install and
remove (manager 1.8.0+, `writeSiblingKeys()` in `src\manager.c`), so the
card is the whole switch: on = armed in the hub and in the scan service,
off = listed nowhere. Which modules may appear in that list at all is a
per-module capability in the manager's module table, earned by a proven
run in that process — not a guess (2026-11-24: six modules inside
`scanservice.exe` aborted it, 0xc0000409 in ucrtbase → vendor error
0x00000012 on project open).

If the render side runs while the scan side is not there, the palette
stays empty: the outline still works, the disc stays stock.

## History: the size module

Through 1.7.x the in-view cue was a separate module,
[Marker In-Frame](../../docs/research/marker-pipeline.md) (retired
2026-11-27): it enlarged the marker via GL driver-thunk detours and needed
a byte-exact float handshake with this module to keep working alongside it.
Two owners of one uniform plus four detours on the application's hottest
GL path bought a cue that hid quality — the outline replaced all of it
with a channel of its own inside this module.

## Why these signals

The quality model mirrors what the vendor's own bundle adjustment uses:
a marker is trustworthy when its reconstructed world position stops
moving while the scanner views it from genuinely different directions.
Hence the three independent gates — observation count, position RMS and
direction coverage — instead of a single "frames seen" counter, and a
green that latches: once a marker contributed a stable pose to the
solution, downgrading the color afterwards would only fight the user.
