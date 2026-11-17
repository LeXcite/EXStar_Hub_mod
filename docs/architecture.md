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
   modules\marker_inframe  modules\future_x   modules\...
   (one DLL per module, plain C, SDK ABI)
```

## Deployment layout (application directory)

| File | Role |
|---|---|
| `winmm.dll` | bootstrap proxy (built) |
| `winmm_real.dll` | byte-identical copy of `%SystemRoot%\System32\winmm.dll` |
| `ExStarRec.dll` | host (built) |
| `modules.ini` | `[host] log=0\|1`, `[<module id>] enabled=0\|1`, `[host] sibling_all`/`sibling_modules` (run the host in sibling exes like `scanservice.exe` for multi-process modules; `dev_all`/`dev_modules` are the legacy dev aliases) |
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
* **Runtime:** modules hook GL entry points obtained via `wglGetProcAddress`
  (driver thunks shared process-wide) - see
  [research/marker-pipeline.md](research/marker-pipeline.md) for the
  proof-driven example.

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
| `gl_thunk(name)` | `wglGetProcAddress` → `opengl32.dll` fallback, waits ~5 min for app GL |
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
| `exstar_qrc.h` | rewrite compressed QML resources (`[u32 BE rawSize][zlib]` records) in a mapped vendor DLL: zlib framing, the one safe write path (fits-in-record, rawSize update, verify-by-reinflate, byte-exact restore), section scan, DLL map notification | fps, laser50 `[caption]` |
| `exstar_export.h` | detour an exported function by **unique name** (never RVA/ordinal; ambiguous needle = stay idle) | laser50 `[lines]`, fitfill |

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
