# GPU Gate

| | |
|---|---|
| Version | 1.0.0 |
| Status | ✅ pattern anchor + gate semantics static-verified on 2.1.0-10 (2026-11-19); the relaxed branch is field-verified on product 2.0.0-45 on a Radeon 780M; live self-test on NVIDIA RTX 4090: pattern unique, gate relaxed in every loading process, vendor logs byte-identical (`errorCode 1`) |
| Mechanism | load-image notification + a **one-byte** in-memory edit (`jne`→`ja`) in the process-private copy of `Sn3DSpeckleNew.dll`, located by a unique 11-byte instruction pattern |
| Requires | vendor UI/algorithm stack where `Sn3DSpeckleNew::device_detect` runs (scanservice.exe); any Vulkan 1.2+ driver |

EXStar Hub refuses to start on machines whose only GPU is integrated.
The check is a policy gate, not a technical limit - which is worth
stating plainly, because the whole module is one byte.

## The factory behavior

During the splash screen (all evidence from the shipped 2.1.0-10
binaries, chain verified end to end):

```
EXStar Hub.exe  (PrestartCheck.dll)  -- cmd "checkCuda" -->  scanservice.exe
scanservice: Sn3DAlgorithmWrapper -> Sn3DSpeckleNew::device_detect()
    Vulkan device check, first policy test:
        mov  eax, [rbp+0x300]   ; VkPhysicalDeviceProperties.deviceType
        cmp  eax, 2             ; DISCRETE_GPU
        jne  out_of_scope       ; everything else -> "not in design scope"
    result republished as cudaCheckResponse{ errorCode }
EXStar Hub.exe: errorCode 1/2 -> continue
                otherwise     -> dialog "requires NVIDIA GeForce RTX 3060
                                   Laptop GPU / Vulkan 1.2" -> quitApp
```

A Radeon 780M (or any iGPU) is deviceType `INTEGRATED_GPU = 1` and never
reaches the vendor's own model detection - behind that gate the vendor
ships full AMD support: the speckle engine is **pure Vulkan** (the DLL
imports only `vulkan-1.dll`/`commonVulkan.dll`), the mesh stage carries a
Vulkan twin (`MeshBasicProcessVulkan.dll`), and the device stream decoder
falls back NVIDIA→AMD ([AMDJPEG]/AMF) on its own.

## The fix

Flip the comparison from "not equal" to "above":

```
mov eax,[rbp+300h]  cmp eax,2  0F 85 (jne)  ->  0F 87 (ja)
```

| deviceType | stock | with GPU Gate |
|---|---|---|
| 0 OTHER | refused | reaches vendor checks |
| 1 INTEGRATED (780M...) | refused | reaches vendor checks |
| 2 DISCRETE | passes | passes (unchanged) |
| 3 VIRTUAL / 4 CPU | refused | refused (unchanged) |

Inside, an integrated AMD hits the vendor's AMD branch; the model regex
`RX\s*(\d{3,4})\b` does not match a name like "Radeon 780M", and the
vendor's own code concludes *"not in the scope, but set to success out of
caution"* - the same `return 1` a discrete GPU produces. Nothing is
spoofed: the hub receives a normal, vendor-computed success.

## How the byte is found (no hardcoded offsets)

The module locates the gate by the full instruction sequence

```
8B 85 00 03 00 00 83 F8 02 0F 85    ; mov eax,[rbp+300h]; cmp eax,2; jne
```

which is **unique** across the whole 3.6 MB `Sn3DSpeckleNew.dll`. At
map time (a `LdrRegisterDllNotification` callback, i.e. before anyone can
call the function) the module counts both the strict and the already
relaxed form across read-only image sections:

| finding | action |
|---|---|
| exactly one strict form | flip `85`→`87`, log the RVA |
| relaxed form present | already done (idempotent) |
| absent | vendor changed the code - stay out, app stays stock |
| ambiguous (>1 strict, or mixed) | refuse, log, app stays stock |

The write is one byte via `VirtualProtect` (copy-on-write private page):
files on disk, signatures and vendor updates are untouched, a restart
restores stock behavior, and disabling the module + restart reverts.

## Why this is safe on machines that already pass

`deviceType == 2` passes both `jne` and `ja`, so on any discrete NVIDIA/AMD
machine the module provably changes nothing (this is also the local
regression test: same `errorCode 1` in the hub, same splash flow). It only
ever acts where `Sn3DSpeckleNew.dll` is mapped. Live observation on 2.1.0-10
shows **two** processes mapping it - `scanservice.exe` (where the gate is
called) and the hub UI process - and the module relaxes the gate in both;
each patch is equally neutral on discrete GPUs. Everywhere else the module
idles and exits its watch loop.

Self-test evidence (NVIDIA RTX 4090 Laptop, product 2.1.0-10, 2026-11-19):

```
mod.log     gpu_gate: Sn3DSpeckleNew.dll gate relaxed at RVA 0x1AB6B5 ...   (x2 procs)
scanservice init_speckle  gpu test result: 1                                (unchanged)
hub         handle cudaCheckResponse ... (cudaErrorCode 0)(errorCode 1)     (unchanged)
```

## Tuning

```ini
modules = sn3dspecklenew.dll   ; DLL basenames carrying the gate
verbose = 1                    ; log the patched RVA (needs [host] log = 1)
```

Expected log lines ( `%LOCALAPPDATA%\ExStarHubMods\mod.log` ) on a
working machine:

```
gpu_gate: Sn3DSpeckleNew.dll gate relaxed at RVA 0x1AB6B5 (jne -> ja: ...)
host: module 'gpu_gate' v1.0.0 (integrated GPUs pass the vendor startup GPU check ...)
```

(The logged value is the mapped-image RVA; the file offset of the same byte
is `0x1AAAB5` - the `.text` VA/file delta on 2.1.0-10 is `0xC00`.)

and in the vendor hub log: `cudaCheckResponse ... errorCode 1` with no
GPU dialog.

## Limitations / open items

* Anchored to this instruction form; a vendor rebuild that changes the
  addressing (`rbp+0x300`) is detected (pattern absent) and leaves the
  app stock rather than guessing.
* Proves the app *runs* on an iGPU; scan throughput/quality on 780M
  hardware is user-reported territory (field-verified working on
  2.0.0-45).
* Does not touch the informational system-check page
  (`libSn3DNavigation` NVIDIA model list) - it is not a startup gate.
