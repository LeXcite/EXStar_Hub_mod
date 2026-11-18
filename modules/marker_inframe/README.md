# Marker In-Frame

> **Artifact, self-contained.** Everything needed to understand, verify or
> continue this module without external context is in this file; the full
> reverse-engineering trail of the marker pipeline is
> [docs/research/marker-pipeline.md](../../docs/research/marker-pipeline.md).

**TL;DR for a cold reader**

| | |
|---|---|
| Behavior change | an in-frame marker no longer flashes **red** - it grows by `scale` and keeps its color |
| Injection | GL driver-thunk detours (`glUniform4fv`/`glUniform4f`, `glDrawArrays`/`glDrawElements`) obtained via `wglGetProcAddress`, marker shader program only |
| Marker program tell | uniform dump: `u0 frontExternalColor`, `u1 frontInnerColor`, `u8 radius` (a single shared float, mm) |
| Vendor "in frame" tell | disc color upload with `R ≥ 0.55, G ≤ 0.38, B ≤ 0.38` (the shared red contract), or an exact match against marker_quality's tagged colors |
| Critical rule | **never scale `radius` at upload time** - OSG only re-uploads changed values, `radius` is shared by all markers: scale at upload leaks into every later marker draw. Scale per-draw-call and restore immediately |
| Key source file | `modules/marker_inframe/marker_inframe.c` |
| Verify at runtime | `probe = 1` in `modules\marker_inframe.ini` + `[host] log = 1` → marker events in `%LOCALAPPDATA%\ExStarHubMods\mod.log` |
| Version | 1.1.0 (current), tested on 2.x (file stamp 3.1.49) |

EXStar Hub paints a marker disc **red** whenever the marker is detected
in the current scanner frame. This module removes that color signal: an
in-frame marker simply **grows** by a configurable factor and shrinks
back the moment it leaves the frame. Size is the only cue — the disc
keeps whatever color it has, so the module composes cleanly with
[Marker Quality](../marker_quality/README.md): a green marker that is in
frame is just a bigger green marker.

| | |
|---|---|
| Module id | `marker_inframe` |
| Status | tested on EXStar Hub 2.x (file stamp 3.1.49), incl. 2.1.0-10 (Windows 11 x64) |
| Mechanism | detours of the driver `glUniform4fv`/`glUniform4f` thunks (red suppression, in-frame latch) and `glDrawArrays`/`glDrawElements` (per-draw radius scoping) for the marker program only |
| Research | [docs/research/marker-pipeline.md](../../docs/research/marker-pipeline.md) |

## Configuration

Enable/disable — `modules.ini` in the application directory:

```ini
[marker_inframe]
enabled = 1
```

Behavior — `modules\marker_inframe.ini`:

```ini
scale       = 1.6    # size multiplier while in frame (1.0..4.0)
color_inner = 1 1 1  # RGB replacing a red disc upload (0..1, alpha kept)
color_ring  = 1 1 1  # RGB replacing a red ring upload (0..1, alpha kept)
probe       = 0      # 1 = log marker events (needs [host] log = 1)
```

`scale` can also be set from the Mod Manager GUI; it applies after an
EXStar Hub restart. The replacement colors default to the vendor
"not in frame" white, so the in-frame marker looks completely standard,
only larger. Restart EXStar Hub after editing.

## How it decides "this is the marker signal"

The marker shader program is identified by `frontInnerColor` living at
fixed location 1 (live uniform dump: `u0 frontExternalColor`,
`u1 frontInnerColor`, `u8 radius` — a single **float**, the outer marker
radius in mm).

1. `glUniform4fv(location = 1)` = disc color upload. The value is
   "in frame" when it is **vendor red** (`R >= 0.55, G <= 0.38,
   B <= 0.38`) or when it **exactly matches a tagged color exported by
   marker_quality** (see below). A red RGB is replaced by `color_inner`;
   tagged quality colors pass through untouched, and the program
   remembers "the marker currently being set up is in frame" (array
   uploads are tracked per element);
2. `glUniform4fv(location = 0)` with a red value = its ring — RGB
   replaced by `color_ring` as well;
3. when the marker program reaches an actual draw call
   (`glDrawArrays`/`glDrawElements`) and that flag is set, the current
   `radius` is read, replaced by `radius * scale` **for this one draw**
   and restored immediately afterwards, together with the 3×3 parts of
   the two model-view matrices (the translation column is kept, the
   marker stays in place).

Why the radius cannot be scaled at upload time: OSG only re-uploads
uniforms whose value *changed* — `radius` is shared by all markers and
uploaded rarely, so an upload-time scale leaks into all subsequent
marker draws (wrong sizes, "circle in a square", dropped markers). The
draw-time scope keeps vertex and fragment stages consistent and cannot
leak: every other draw call sees exactly the vendor value.

Color needs no restore: the module only ever writes non-red values,
and the vendor re-uploads colors on every change (per-frame for the
in-frame marker), so the suppression is self-reversing.

## Cooperation with Marker Quality

Both modules drive the same osg uniform `frontInnerColor`. When Marker
Quality is enabled it repaints the disc with the quality color *before*
the upload, so the vendor red would never reach this module and the
enlargement would silently die (v1.0.x behavior). The fix is a
lightweight handshake inside the render process:

* marker_quality reads what the vendor just wrote; when it is red
  (= marker in frame) it paints the quality color with the green byte
  **+1/255 — invisible to the eye**;
* it exports those exact float4 tuples via `ex_mod_inframe_snapshot()`
  from `marker_quality.dll`;
* this module compares uploaded disc colors against the snapshot — an
  exact float match, on the same GL thread, race-free — and treats the
  marker as in frame.

Result: quality color is always visible, enlargement always correct,
regardless of which module painted the disc. Without marker_quality (or
with `share_inframe = 0` on its side) detection degrades gracefully to
the vendor-red rule alone. The quality "red" stage is deliberately dark
(R = 128/255 = 0.50 < 0.55) so it can never be mistaken for vendor red.
