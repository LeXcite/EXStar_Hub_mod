# Architecture

Three layers, strict downward dependencies only.

```
┌───────────────────────────────────────────────────────────┐
│ EXStar Hub.exe (never modified on disk)                   │
│   imports WINMM.dll  ──────────────┐                      │
└─────────────────────────────────── │ ─────────────────────┘
                                     ▼
┌───────────────────────────────────────────────────────────┐
│ winmm.dll - bootstrap proxy            src/winmm_proxy.c  │
│   forwards all winmm exports to winmm_real.dll            │
│   loads ExStarRec.dll from the same directory             │
└──────────────────────────────┬────────────────────────────┘
                               ▼
┌───────────────────────────────────────────────────────────┐
│ ExStarRec.dll - module host              src/host.c       │
│   module discovery + ABI check + enable flags             │
│   private hidden-window OpenGL context                    │
│   MinHook, logging, per-module config                     │
└──────────┬───────────────────┬───────────────────┬────────┘
           ▼                   ▼                   ▼
   modules\marker_quality  modules\future_x   modules\...
   (one DLL per module, plain C, SDK ABI)
```

## Deployment layout (application directory)

| File | Role |
|---|---|
| `winmm.dll` | bootstrap proxy (built) |
| `winmm_real.dll` | byte-identical copy of `%SystemRoot%\System32\winmm.dll` |
| `ExStarRec.dll` | host (built) |
| `modules.ini` | `[host] log=0\|1`, `[<module id>] enabled=0\|1` (what a module card writes), `[host] sibling_all`/`sibling_modules` (run the host in sibling exes like `scanservice.exe`; the Mod Manager recomputes both from the module switches, see "Which process a module may run in"). `dev_all`/`dev_modules` are the legacy developer aliases and always win |
| `modules\<id>.dll` | module binaries |
| `modules\<id>.ini` | free-form module config (`key = value`) |

Uninstall = delete these items. The vendor application is never touched.
`ExStarModManager.exe` (built from `src\manager.c`) automates all of the
above: it embeds every payload as RCDATA resources, records installed files
in `<app dir>\exstar_mods.state` and exposes per-module toggle switches
(ON = install + enable, OFF = remove, keeping `modules\<id>.ini`) plus a
restore-stock action; the CLI (`--install/--remove/--restore`) offers the
same operations for scripting.

## Why these hook points

* **Entry:** `EXStar Hub.exe → Qt5Core.dll → WINMM.dll`. winmm is not a KnownDLL on
  current Windows, so the loader resolves it from the application directory.
  `opengl32.dll` *is* a KnownDLL - an opengl32 proxy would never load.
* **Runtime:** the cheapest place where the vendor's own data is already
  decoded. Today that means exported C++ entry points resolved **by name**
  (`HandlePrjFile::putFrameDataIntoFile`, `setMarkerPoints`,
  `isGenerateCloud`, `osg::StateSet::getUniform`, ...) and signature-
  scanned pointers to vendor callbacks. GL driver thunks
  (`wglGetProcAddress`, shared process-wide) are still available through
  `host->gl_thunk` and are what `marker_probe` uses - but no shipped
  module hooks GL any more: the retired `marker_inframe` did, and the
  marker recoloring it owned moved into `marker_quality` one level up,
  where the scene graph is still readable. See
  [research/marker-pipeline.md](research/marker-pipeline.md) for the
  proof-driven example of both routes.

## Which process a module may run in

Two switches exist and only one of them is a user's:

| switch | who writes it | meaning |
|---|---|---|
| `[<id>] enabled` | the module card (or `--install/--remove`) | does the module load and start |
| `[host] sibling_all` + `sibling_modules` | the manager, recomputed on every install and remove | does the host wake up in the other exes of the app directory, and which modules may load there |

`sibling_modules` is exactly the enabled modules whose `ModDef` says
`siblingCapable` - a capability earned by a proven run inside
`scanservice.exe`, not an assumption (2026-11-24: six modules there
aborted the process, 0xc0000409 in ucrtbase, which the vendor surfaces as
error 0x00000012 on project open). Currently `marker_quality` (scan frames
arrive only there) and `selective_mesh` (the post-mesh data is built only
there) qualify; every other module never leaves the main application.

Before 1.8.0 these two keys were hand-maintained after that crash, and the
combination "card ON, sibling slot missing" was silent - `marker_quality`
painted nothing for days while its card said enabled. The recomputation is
what makes the toggle honest: ON arms a module everywhere it needs to be,
OFF leaves it in no process at all.

## Module lifecycle

1. Host worker thread: `MH_Initialize` → create private GL context.
2. Scan `modules\*.dll` (next to the host DLL): `LoadLibraryEx` →
   `ex_get_info()` → reject on `abi != EX_ABI_VERSION` → reject if
   `modules.ini` disables the id → otherwise keep.
3. For each kept module: `ex_init(&ExHost)` on the worker thread.
   Non-zero return = module disables itself, host logs and continues.
4. Detours run on whatever application thread calls the hooked function:
   keep detour bodies allocation-free, fast, SEH-guarded.
5. `DLL_PROCESS_DETACH`: host calls `ex_shutdown()` of started modules
   under SEH. Process teardown otherwise does nothing fancy.

## The ExHost service table (see `include/exstar_sdk.h`)

| Service | Notes |
|---|---|
| `log(fmt,...)` | no-op unless `[host] log=1`; thread-safe |
| `cfg_int/cfg_string` | reads `modules\<id>.ini` bound to the calling module |
| `gl_thunk(name)` | `wglGetProcAddress` → `opengl32.dll` fallback, waits ~5 min for app GL. Used by the `marker_probe` diagnostic only - no shipped module needs it since the size module was retired |
| `hook(target, detour, tramp)` | MinHook wrapper, MinHook lives only in the host |

Forward compatibility: `ExHost.abi`/`size`. Additive growth bumps
`EX_ABI_VERSION`; the host refuses mismatched modules and says why in
the log - a stale module can never corrupt a new host.

## Writing a module

1. Copy `examples\template\` to `modules\<id>\`.
2. Rename id in `ExModuleInfo::id` (and keep it equal to the DLL name).
3. Add one line to `$modules` in `build.ps1` (and a card + RCDATA pair in
   `src\manager.c` if it should appear in the Mod Manager).
4. `build.ps1` → deploy `build\modules\<id>.dll` (+ optional
   `modules\<id>.ini`) into the app's `modules\` directory.

Reusable pieces (header-only, `include\`, one static copy per module DLL):

| Header | For modules that… | Used by |
|---|---|---|
| `exstar_qrc.h` | rewrite compressed QML resources (`[u32 BE rawSize][zlib]` records) in a mapped vendor DLL: zlib framing, the one safe write path (fits-in-record, rawSize update, verify-by-reinflate, byte-exact restore), section scan, DLL map notification | laser50 `[caption]` (retired fps was its first user) |
| `exstar_export.h` | detour an exported function by **unique name** (never RVA/ordinal; ambiguous needle = stay idle) | laser50 `[lines]` (the retired fitfill was its other user) |

(`popup_dim` predates `exstar_qrc.h` and keeps its own equal-length
variant - deliberately not refactored: it is live-verified, and style
must not risk working code.)

## Failure isolation rules (enforced by review)

* `DllMain` of proxy/host/modules: always TRUE, no heavy work;
* all risky callbacks under SEH, detours fall back to the original call;
* no hardcoded addresses/RVAs - names, strings and patterns only;
* a module that cannot initialize returns non-zero and is skipped;
* the application process must never crash because of this project -
  when in doubt: pass the call through untouched.

## Known limitations

* Only one GUI process (`EXStar Hub.exe`) is injected; the hub briefly
  spawns a second instance at launch that exits after ~3 s - both get
  hooked, which is normal.
* Module enable/disable is applied at application start (no hot reload -
  GL detours must not be yanked under a live driver).
* Vendor updates can change the pipeline; the compatibility matrix lives
  in `README.md`.
