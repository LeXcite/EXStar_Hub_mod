# "Red marker" signal protocol — EXStar Hub 2.x (file stamp 3.1.49)

Description of the stock application's mechanism: what the red marker color
means, who sets it, on what trigger and with what dynamics. Section 8 — a
proven way to intercept and substitute this signal from outside.

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

| `frontInnerColor` | Meaning |
|---|---|
| `(1,0,0,1)` red disc (+ ring `(1,0,0,0.5)`) | marker recognized **in the current (latest) frame** of the scanner; the node sits in the `CURRENT_MARKER` model; re-confirmed every frame |
| `(1,1,1,1)` white | any other marker: registered earlier, currently out of frame, or removed from `CURRENT_MARKER` by the latest frame operation |

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

## 8. How to intercept and change the signal (proven)

Goal: substitute the "marker in frame" signal (§1) without patching third-party
DLLs on disk. Implementation — this repository's three-layer framework:

```
winmm.dll                          bootstrap proxy    (src/winmm_proxy.c)
  └─ ExStarRec.dll                 host manager       (src/host.c)
       └─ modules\marker_inframe.dll   behavior module  (modules/marker_inframe/)
```

### 8.1 Entry point — the winmm.dll proxy

- `EXStar Hub.exe` does not import winmm itself, but does so via
  `Qt5Core.dll → WINMM.dll`; on Win11 winmm is absent from KnownDLLs ⇒ the
  loader takes `winmm.dll` from the application directory.
- The proxy forwards all ~180 named exports to `winmm_real.dll` (a copy of the
  system winmm; forwarder generation — `tools/gen_forwards.ps1`; one unnamed
  ordinal is not forwarded — not used by the application) and loads the host
  `ExStarRec.dll` from the same directory.
- `DllMain` of the proxy and of the host always returns TRUE; all work happens
  in the host's worker thread.

### 8.2 Interception point — glUniform4fv

- The host creates its own hidden GL window with a context and waits for GL
  initialization by the application; the module obtains the driver thunk
  `wglGetProcAddress("glUniform4fv")` via `host->gl_thunk` (the thunk is
  process-wide and identical between runs — the address is NOT hardcoded).
- Putting our own `opengl32.dll` into the directory is pointless: KnownDLL, loads
  from system32 (verified).
- `osg::Uniform::set/setElement(Vec4f)` would also work (exports of
  osg158-osg.dll), but OSG re-applies the uniform every frame (§2, L5) ⇒
  hooking the upload is enough.
- The detour is installed with MinHook via the host wrapper (`host->hook`); the
  detour body is under SEH; no hardcoded addresses/RVAs — import names and
  strings only.

### 8.3 The "this is the red marker signal" filter (all three conditions)

1. `location == 1 && 1 <= count <= 64` (fixed disc location, u1);
2. the value is red: `v0 >= 0.55 && v1 <= 0.38 && v2 <= 0.38` (§5);
3. the current program is the marker one: `glGetUniformLocation(prog,"frontInnerColor") == 1`
   (cache keyed by program id; ring — u0 `frontExternalColor`, radius — u8 `radius`).

Live check: during the session 3341/3341 red writes passed the filter (0 false
positives) ⇒ no dynamic fallback needed.

### 8.4 Substitution and configuration

- Only the RGB of red elements is substituted, the vendor alpha is preserved.
  Live clarification: the vendor recolors only the disc (u1); the ring (u0) is
  never set to red (the log shows only `RED inner`), so for the ring the color
  substitution effectively never fires and the black outline stays black.
- The "in frame" enlargement is NOT done in the upload hook: OSG sets the
  uniform only when the value changes, `radius` is shared by all markers, and
  editing it on upload would "leak" into all subsequent draws. The size is
  assembled at the boundary of a single draw call (`glDrawArrays`/`glDrawElements`
  of the marker program, flag "the last disc upload was red"): for the duration
  of the real call `radius` is multiplied by `scale`, the 3×3 parts of
  `osg_ModelViewMatrix` and `osg_ModelViewProjectionMatrix` are scaled
  (multiplication on the right — the translation column is untouched, the
  marker stays in place), and `radius` + `frontExternalColor` are recolored to
  `outline_color`; everything is restored immediately after the draw. No
  foreign draw ever sees the modified values.
- Thus a marker "in frame" grows by a factor of `scale` and gets a colored
  (green by default) outline instead of the black one; out of frame — the
  original size and outline.
- Enabling/disabling the module: `<app>\modules.ini`, section `[marker_inframe]
   enabled = 0|1` (applied at application start; a disabled module is not loaded).
- Settings: `<app>\modules\marker_inframe.ini` → `scale = F` (1.0..4.0,
  default 1.6), `outline_color = R G B` (outline color of the enlarged marker,
  default `0 1 0`), `color_inner`/`color_ring = R G B` (float 0..1),
  `probe = 0|1` (diagnostics: uniforms and shader source of the marker
  program, grow/outline events). In the manager GUI `scale` is available as a
  slider (1.0..2.0), in the CLI — via the `--scale` key; both write the value
  into this ini (applied from the next application start).
- Diagnostics: `[host] log = 1` in modules.ini →
  `%LOCALAPPDATA%\ExStarHubMods\mod.log` (`armed`, `calls/recolored` counters).
- Full rollback: delete `winmm.dll`, `winmm_real.dll`, `ExStarRec.dll`,
  `modules.ini` and the `modules\` directory from the application directory.

### 8.5 Rules and dead ends

- Third-party DLLs are never patched on disk: proxy + in-memory inline hooks
  only.
- Verified dead ends (do not repeat): the `setState/setStateParams/updateMarker`
  exports are never called (§6); vertex colors and textures do not carry the
  red; a custom `opengl32.dll` is never loaded.
- At startup the application briefly duplicates its process (the second
  instance exits after ~3 s) — normal, the hooks are armed in both.
