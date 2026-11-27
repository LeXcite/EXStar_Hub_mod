# "Red marker" signal protocol — EXStar Hub 2.x (file stamp 3.1.49)

Description of the stock application's mechanism: what the red marker color
means, who sets it, on what trigger and with what dynamics. Section 8 - two
proven ways to intercept this signal from outside (the one in use today: §8.4).

---

## 1. What the signal is

The red marker disc is **not a "marker color" and not a state of its state
machine**, but **the color of the 3D model-group named `CURRENT_MARKER`**.
Every scan frame the algorithm reports which marker codes it saw in that
exact frame; the scene moves those marker nodes into the `CURRENT_MARKER`
group, for which the marker shader uniforms are:

- `frontInnerColor = (1, 0, 0, 1)` — red disc,
- `frontExternalColor = (1, 0, 0, 0.5)` — red semi-transparent ring.

All other markers stay in the "white" group: `frontInnerColor = (1, 1, 1, 1)`.
OSG re-applies all uniforms every frame, so the red "lives" as long as the
marker lands in every new frame, and is cleared at the very next frame
operation where the marker is no longer present.

Live clarification (§8.3): what reaches the driver is only the *changed*
value, and during a scan only the disc changes - a marker in view gets a red
disc and keeps its stock outline.

---

## 2. Trigger chain

```
[L0] Algorithm (registration/recognition of marker-plate codes from a frame)
     Sn3DRegistration.dll: MarkerPtCloud, RangeDataDense::set_markerPts —
     "which markers were detected where in the current frame"
        │  Qt signals
        ▼
[L1] Sn3DScanController.dll
     signals generatePointCloudFinished / registrationFrameMarkerFinished,
     slot onRegistrationFrameMarkerFinished, publishing of generateFrameMarker
     (strings: "publish generateFrameMarker start", "generateFrameMarker.sync")
        │  Qt event queue (posted event → GUI thread)
        ▼
[L2] Post-processing pipeline: PostProcess.dll / PostProcessUI.dll
     pipeline step named "PostProcess.FrameMarker"
     (PostProcessUI FUN_18000e750, RVA 0xE750): matches the step name against
     the "PostProcess.FrameMarker" string, builds osg::TemplateArray<osg::Vec3f,28,3,5126>
     — the marker point array from the algorithm data — and passes the operation
     through SceneControl / SceneControlProxy
        │  call into the scene plugin
        ▼
[L3] sn3d.digit.scenecontrol.dll  (Qt canvas plugin, exports qt_plugin_instance)
     scene-operation dispatcher FUN_18004f9f0;
     handler "PostProcess.FrameMarker" FUN_18005f5c0 reads the payload:
     uuid, markerFids, markerLastFids, markerVids, markerUuid
     (strings at RVA 0x197860..0x197890);
     moves the nodes into the "CURRENT_MARKER" model;
     FUN_180024390: compares the model name against UTF-16 "CURRENT_MARKER" (14 chars) →
        YES: frontInnerColor = (1,0,0,1), frontExternalColor = (1,0,0,0.5)
        NO : frontInnerColor = (1,1,1,1)
     a branch on marker size in the same place → radius pairs 3/1.5, 5/3, 9/6
     (65-byte pos/normal/diameter record)
        │  export Sn3D::RenderKit::ExtDigit::ModelNode::setMarkerPoints(Sn3DMarkerParam)
        ▼
[L4] librenderkit.extensions.digit.dll  (owner of the marker shaders/uniforms)
     setMarkerPoints (RVA +0xAD195, +0xAD52C inside the export) — writes a vec4
     into osg::Uniform "frontInnerColor"/"frontExternalColor"
     via osg::Uniform::set / setElement;
     librenderkit+0x14C447 (createMarkerPoint@MarkerPoint zone) — marker geometry
        │  osg::Uniform::set(Vec4f)/setElement(uint,Vec4f) [export of osg158-osg.dll]
        ▼
[L5] OSG → OpenGL
     StateSet of the marker program; on the render thread osg::OperationQueue::runOperations
     (osg+0x11670B) and the osgViewer graph traversal re-apply the uniform every frame →
     glUniform4fv(location=1, frontInnerColor)
```

---

## 3. Execution stack (live, from the running application)

Writing the red value (GUI thread, Qt event of the frame operation),
via `osg::Uniform::setElement(Vec4f)`:

```
osg158-osg.dll+0x16E0EF                          (Uniform::set… internal)
librenderkit.dll+0x14C447                        (createMarkerPoint@MarkerPoint zone)
librenderkit.extensions.digit.dll+0x1329BA
librenderkit.extensions.digit.dll+0x13684E
librenderkit.extensions.digit.dll+0xAD195        (setMarkerPoints@ModelNode +0x355)
sn3d.digit.scenecontrol.dll+0x52AA9              (scene-operation executor)
sn3d.digit.scenecontrol.dll+0x35DB1              (FUN_180035bc0+0x1F1 — operation call)
sn3d.digit.scenecontrol.dll+0x33D9F              (FUN_180033d00 — Qt event handler)
Qt5Core.dll+0x2052B3                             (QObject::event+0x183)
Qt5Widgets.dll+0x17A1A                           (QApplicationPrivate::notify_helper+0x13A)
```

Variant with the second executor: `ext.digit+0xAD52C (setMarkerPoints+0x6EC) →
scenecontrol+0x518F8 → the same 0x35DB1 / 0x33D9F → Qt`.

Per-frame application (QtQuick/osgViewer render thread),
via `osg::Uniform::set(Vec4f)`:

```
librenderkit.extensions.digit.dll+0x12F1DF
osg158-osg.dll+0x11670B                          (OperationQueue::runOperations+0xBB)
osg158-osgViewer.dll+0x16E97 / +0x49357          (osgViewer frame)
librenderkit.dll+0xFA2D2
Qt5Quick.dll+0x1F6B7E → Qt5Core.dll+0x1FD37D (qt_static_metacall) → Qt5Quick render loop
```

Intercepted value writes:

```
SetElement idx=0 RED   v=(1.00,0.00,0.00,1.00) name=frontInnerColor
Set4       idx=—  RED   v=(1.00,0.00,0.00,1.00) name=frontInnerColor
```

---

## 4. Timing and dynamics

A "marker plate in frame / out of frame" session (4 insertions), red `frontInnerColor` writes:

| # | in-frame duration | red writes |
|---|-------------------|------------|
| 1 | 30.4 s | 7044 |
| 2 | 4.8 s  | 1464 |
| 3 | 5.3 s  | 1230 |
| 4 | 4.0 s  | 752  |

* Rate during the flash ≈ **230 red writes/sec** — every render frame
  (~30–60 fps) × every marker in frame (set + setElement, several markers).
* The red appears at the very first frame operation where the marker plate's
  codes are recognized.
* The red is cleared **at the very next frame operation where the marker is
  absent** (≤1 algorithm frame; pauses between insertions 1.5–2.8 s — only
  white during that time).
* Per-session ratio: RED ≈ 10 490 (set) / 19 031 (setElement) against
  WHITE ≈ 60 419 / 61 810 — white is written constantly, for every marker
  out of frame.
* Reset at scan completion: a separate path `cleanCurMarker` / `cleanCurCloud`
  (SendDataToClientController.dll: "ready to send cleanCurCloud message to
  scenecontroller") — on stop/clean the red group is cleared and the markers
  turn white.

---

## 5. Color semantics

| Uniform | Meaning |
|---|---|
| `frontInnerColor` = `(1,0,0,1)` red disc | marker recognized **in the current (latest) frame** of the scanner; the node sits in the `CURRENT_MARKER` model; re-confirmed every frame |
| `frontInnerColor` = `(1,1,1,1)` white | any other marker: registered earlier, currently out of frame, or removed from `CURRENT_MARKER` by the latest frame operation |
| `frontExternalColor` (the ring) | **not part of the signal**: a real session logs red writes for the disc only, so a marker in view keeps its stock outline (§8.3). The channel is free for anything else |

---

## 6. Verified exclusions (what is NOT a trigger)

* `setState@MarkerNodeBase`, `setStateParams@MarkerNodeBase`, `updateMarker@Sn3DMarker`
  (exports of librenderkit.extensions.digit.dll) — **0 calls** during a real scan
  session. The node state machine (`enum State`, `QMap<State,Param>`) has nothing
  to do with the "in frame" recoloring.
* Vertex colors, red textures, other uniforms — not involved: the red arrives
  only through `glUniform4fv` (loc=1 disc, loc=0 ring) of the marker program.

---

## 7. Version-stable anchors (for porting to other builds)

Do not use RVAs. Anchors:

1. The string **`frontInnerColor`** (and the vendor typo `innerDiameterTheshold`)
   — marker shader; uniforms of the marker program: u0 `frontExternalColor`
   (ring), u1 `frontInnerColor` (disc), u3 `normal`, u4 `osg_ModelViewMatrix`,
   u5 `osg_ModelViewProjectionMatrix`, u8 `radius` — a SINGLE FLOAT (float, loc 8;
   live measurement via glGetProgramiv/ACTIVE_UNIFORMS, type=0x8B52 — the earlier
   "vec2" note was wrong).
2. OSG exports: `?set@Uniform@osg@@QEAA_NAEBVVec4f@2@@Z`,
   `?setElement@Uniform@osg@@QEAA_NIAEBVVec4f@2@@Z` (the uniform name is read from
   the inline `std::string` of the `osg::Uniform` object).
3. The UTF-16 string **`CURRENT_MARKER`** in `sn3d.digit.scenecontrol.dll` (+ ASCII
   `markerFids/markerLastFids/markerVids/markerUuid`) — the recoloring decision
   point.
4. The operation string **`PostProcess.FrameMarker`** in `PostProcess.dll`/`PostProcessUI.dll`
   (pipeline step) and in `sn3d.digit.scenecontrol.dll` (handler).
5. Signals/strings `registrationFrameMarkerFinished`, `generateFrameMarker` in
   `Sn3DScanController.dll` — the per-frame source.
6. Export `?setMarkerPoints@ModelNode@ExtDigit@RenderKit@Sn3D@@QEAAIUSn3DMarkerParam@234@@Z` —
   the point where the color is applied to nodes.

---

## 8. How to intercept and change the signal

Two routes were proven against this mechanism. The first (the GL driver
thunk) is what originally proved the signal and its dead ends; it is retired
with the module that used it. The second (the scene graph) is what
`marker_quality` uses today.

```
winmm.dll                          bootstrap proxy    (src/winmm_proxy.c)
   └ ExStarRec.dll                 host manager       (src/host.c)
        ├ modules\marker_quality.dll    current owner of the marker's look  (§8.4)
        └ modules\marker_inframe.dll    RETIRED 2026-11-27 (GL route, §8.2-§8.3)
```

### 8.1 Entry point - the winmm.dll proxy

- `EXStar Hub.exe` does not import winmm itself, but does so via
  `Qt5Core.dll → WINMM.dll`; on Win11 winmm is absent from KnownDLLs → the
  loader takes `winmm.dll` from the application directory.
- The proxy forwards all ~180 named exports to `winmm_real.dll` (a copy of the
  system winmm; forwarder generation - `tools/gen_forwards.ps1`; one unnamed
  ordinal is not forwarded - not used by the application) and loads the host
  `ExStarRec.dll` from the same directory.
- `DllMain` of the proxy and of the host always returns TRUE; all work happens
  in the host's worker thread.

### 8.2 The GL route (how the signal was proven; route retired)

- The host creates its own hidden GL window with a context and waits for GL
  initialization by the application; a module obtains the driver thunk
  `wglGetProcAddress("glUniform4fv")` via `host->gl_thunk` (the thunk is
  process-wide and identical between runs - the address is NOT hardcoded).
- Putting our own `opengl32.dll` into the directory is pointless: KnownDLL, loads
  from system32 (verified).
- The "this is the red marker signal" filter needed all three conditions:
  1. `location == 1 && 1 <= count <= 64` (fixed disc location, u1);
  2. the value is red: `v0 >= 0.55 && v1 <= 0.38 && v2 <= 0.38` (§5);
  3. the current program is the marker one: `glGetUniformLocation(prog,"frontInnerColor") == 1`
     (cache keyed by program id; ring - u0 `frontExternalColor`, radius - u8 `radius`).
  Live check: during the session 3341/3341 red writes passed the filter
  (0 false positives) - no dynamic fallback needed.

### 8.3 What the GL route bought, and why it was dropped

- **The ring is a free channel.** The vendor recolors only the disc (u1). The
  log of a real scan shows `RED inner` writes and never a red ring write, so
  the per-marker `frontExternalColor` upload never fires and the outline stays
  its stock color. That single fact is why the "in view" cue could be moved to
  the outline without taking anything from the vendor.
- **Size cannot be changed on upload.** OSG sets a uniform only when the value
  changes, `radius` is shared by all markers, and editing it on upload would
  "leak" into all subsequent draws. The enlargement therefore had to be
  assembled at the boundary of a single draw call (`glDrawArrays` /
  `glDrawElements` of the marker program, with a "the last disc upload was
  red" flag): multiply `radius` by `scale`, scale the 3×3 parts of
  `osg_ModelViewMatrix` and `osg_ModelViewProjectionMatrix` (multiplication on
  the right, so the translation column and the marker position are untouched),
  recolor the ring, and restore everything right after the draw.
- So the cue cost four detours on the application's hottest GL path, and the
  disc uniform ended up owned by two modules at once (the quality palette and
  the red detector had to agree on a float threshold between them). Moving one
  level up, to the scene graph, replaced all four detours with one callback
  hook and gave each state its own uniform.

### 8.4 The scene-graph route (current: `marker_quality` 1.1.8)

- **Interception point:** the vendor's per-frame marker color callback, found
  by signature scan of its pointer (never patched on disk) and wrapped with
  MinHook; the original runs first, so the module always reads what the vendor
  decided before it overwrites.
- **Per-marker state:** every marker is a child `osg::Node` of the group the
  callback receives, with its own `osg::StateSet`. Everything is reached by
  **export name** in `osg158-osg.dll` - `getChild@Group@osg`,
  `getNumChildren@Group@osg`, `getStateSet@Node@osg`, `getUniform@StateSet@osg`,
  `set@Uniform@osg`, `get@Uniform@osg`, `getType@Uniform@osg`,
  `getOrCreateUniform@StateSet@osg`, `addUniform@StateSet@osg`. No RVA, no
  pattern.
- **The `std::string` catch (this one cost a live session):**
  `getUniform@StateSet@osg` takes a `const std::string&`, so the name has to
  be described in MSVC layout - a 16-byte union `{ buffer | pointer }`, then
  `size @16` and `capacity @24`, where the STL reads the buffer when
  capacity <= 15 and the pointer otherwise. `frontInnerColor` is 15
  characters and fits inline; `frontExternalColor` is 18 and must be
  described as a heap string - **pointer plus 8 bytes of padding**, then
  size and a capacity above 15. A 24-byte `{ pointer, size, capacity }`
  puts `size` inside the union and leaves the capacity reading as zero, so
  the STL takes the name from the pointer bytes and *every lookup of a name
  longer than 15 characters misses quietly*. The layout, one hit and the miss
  are reproduced offline by [`tools\str_lab.c`](../../tools/str_lab.c). Both
  strings point at static text, the vendor lookup only reads, nothing is
  destroyed.
  **Measured 2026-11-27: that bug was real, and it was not the whole story.**
  With the corrected description the disc name resolved and painted for all 35
  markers while the outline name still did not - for the reason two points
  below.
- **Both marker colors come from the vendor's own factory:**
  `Sn3D::RenderKit::MarkerPoint::createMarkerPoint` in `librenderkit.dll`
  (export
  `?createMarkerPoint@MarkerPoint@RenderKit@Sn3D@@SAPEAVGeometry@osg@@AEBUMarkerParam@23@_N@Z`)
  builds a marker as an `osg::Geometry` and then does

        StateSet *ss = geometry->getOrCreateStateSet();
        ss->addUniform(new osg::Uniform("frontExternalColor", param + 0x0c));
        ss->addUniform(new osg::Uniform("frontInnerColor",   param + 0x2c));

  so the ring color is not a global setting of the program, and what the vendor
  never does at runtime is write it again: only the disc gets the in-view red.
  (Read out of the binary; the only reference to the text `frontExternalColor`
  anywhere in `librenderkit.dll` is here, at 0x1802d2c70 -> 0x18014d373.)
- **In the running scene that ring channel is not where the marker is**
  (measured 2026-11-28, and it is why two rounds of "no blue" happened): the
  map of a marker's own `StateSet` holds **only** `frontInnerColor`, and the
  `frontExternalColor` the ring paints with sits one `StateSet` *below* it - and
  two different markers walk down to **the same uniform object**
  (`outline: found on one of the marker's parts - channel 1871C3C0A70`, logged
  twice). Writing that single channel would outline every marker in the field,
  so it cannot be used as a per-marker signal.
- **Which of two StateSets wins is decided by two bits, not by distance.**
  `osg::State::pushUniformList` (include/osg/State, and the same code in
  `osg158-osg.dll` at RVA 0x18003b770):

        else if ((us.uniformVec.back().second & StateAttribute::OVERRIDE) &&
                 !(aitr->second.second & StateAttribute::PROTECTED))
            // push existing back since override keeps the previous value.

  The value from higher up the path survives one below it **only** when its own
  entry carries `OVERRIDE_VALUES` (2); a deeper entry marked `PROTECTED_VALUES`
  (4) beats even that. The bits live in the map entry, written by
  `addUniform@StateSet@osg` (RVA 0x14c2f0), which keys by `uniform->getName()`
  and stores `mask & 6`. Note what that means for a channel the module makes:
  `getOrCreateUniform@StateSet@osg` (RVA 0x14dd40) ends in
  `addUniform(this, uniform, 1)`, and `1 & 6` is **0** - a fresh channel has no
  override, so it loses to the shared one below until `addUniform(u, 2)` says
  otherwise.
- **The uniform map, walked by hand.** `getUniform@StateSet@osg` (RVA 0x14e310)
  and `addUniform` (0x14c2f0) describe the container exactly:

        StateSet + 0xb0        the std::map; the header node is heap allocated
        header   + 0x08        the root node (the header's _Parent)
        node     + 0x00/08/10  left / parent / right
        node     + 0x19        _Isnil, so the header is also the sentinel
        node     + 0x20        the key: an MSVC std::string - union{buffer |
                               pointer} at 0x20, size at 0x30, capacity at 0x38,
                               inline buffer while capacity <= 15
        node     + 0x40        the value: ref_ptr<Uniform>
        node     + 0x48        the entry's override bits (`mask & 6`)

  Names are compared by hand, which takes the hand-built `std::string` out of
  the lookup path, and the same walk feeds the log, so `mod.log` reports what a
  StateSet really holds.
  **The trap that cost two live rounds:** this module also walks Qt's `QMap` on
  the scan side, whose node offsets are 8 / 16 / 24 / 32. Reusing them on a
  `std::map` finds nothing - and once produced a confident "one uniform shared
  by all 35 markers" that was pure garbage. Two containers, two layouts; the
  offsets must not cross over.
- **What the module does with it - and what it must not remember.** Two live runs
  of 2026-11-28 measured the two wrong ways:

  | what was remembered | what the scene did to it |
  |---|---|
  | the resolved uniform pointer, kept per marker | the vendor replaces markers while it scans, so the writes went into objects the scene no longer drew: `made=163` channels for 49 markers and 207 highlight writes, the outline visible for one moment |
  | a list of "the channels I made", so a vendor entry is never mistaken for one of ours | it only shrinks when a marker is seen leaving the view. Under a scanner a marker is not taken out of view, it is replaced by a new node with a new `StateSet`, so the list never shrank: `made=1025 live=1024` for 49 markers, then the cue switched itself off at its own limit while every counter still looked healthy |

  So the marker's own map is read again every frame and acted on as it is now:

  | what that map holds now | what happens |
  |---|---|
  | no `frontExternalColor` - the stock case, measured | while the marker is in view the entry is made on the marker's own `StateSet` (`getOrCreateUniform`, `Type` copied from the disc uniform via `getType@Uniform@osg`) and armed with `addUniform(u, OVERRIDE_VALUES)`, because `getOrCreateUniform` inserts with `1 & 6 == 0`; then painted |
  | an entry holding the highlight color | only this module paints that color, so it is ours: painted while the marker is in view, **removed** with `StateSet::removeUniform` when it leaves, which hands the ring back to the vendor's channel below with whatever value it holds then - nothing frozen, nothing invented |
  | an entry holding any other color | the vendor's own: left alone entirely - there is no color of ours that would be honest to put back - and the cue stays off, with a log line saying so |
  | `inframe_create = 0` | nothing is ever made, so a marker without its own entry stays stock |

  Ordering and guards: a fresh uniform holds `0,0,0,0`, so the highlight goes in
  before the bit is ever used, and the bit is re-asserted whenever the entry's
  override bits are not `OVERRIDE_VALUES` any more, which is what a rebuilt
  `StateSet` leaves behind. Both directions of every read and write go through
  SEH with the value range-checked (the pointer came out of a memory walk), and
  the deeper entry's override bits are logged - `PROTECTED` there would mean no
  override from above can ever win, and the log says so instead of leaving a
  silent "nothing happened".
- **Detection:** read back the disc value the vendor just wrote -
  `R >= 0.55, G <= 0.38, B <= 0.38` means the node is in `CURRENT_MARKER` this
  frame (see §5). The same contract the GL filter used, one level up and
  without touching the driver.
- **Rollback:** `inframe = 0` in `modules\marker_quality.ini` restores the
  vendor's look for this cue; switching the module off removes every hook.

### 8.5 Rules and dead ends

- Third-party DLLs are never patched on disk: proxy + in-memory inline hooks
  only.
- Verified dead ends (do not repeat): the `setState/setStateParams/updateMarker`
  exports are never called (§6); vertex colors and textures do not carry the
  red; a custom `opengl32.dll` is never loaded.
- At startup the application briefly duplicates its process (the second
  instance exits after ~3 s) - normal, the hooks are armed in both.
