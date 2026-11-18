# GPU vendor gate and the AMD (Radeon 780M) path

Date: 2026-11-18/19. Target: EXStar Hub **2.1.0-10** (file stamp 3.1.49), all
findings verified against the installed binaries (RE) and the vendor's own
logs. Goal: understand the factory "NVIDIA-only" restriction well enough to
decide how an injection-based mod could lift it on a machine whose only GPU
is an integrated Radeon 780M (vendorID `0x1002`, Vulkan deviceType
`INTEGRATED_GPU=1`).

## 1. Executive summary

- The blocking check is **one function**: `Sn3DSpeckleNew::device_detect()`
  (Vulkan-based), reached only through `Sn3DAlgCheck::checkGpu()` in
  `Sn3DAlgorithmWrapper.dll`, which runs **inside `scanservice.exe`**.
- Its `int` return is republished verbatim as the `errorCode` field of the
  bus message `cudaCheckResponse`; the hub's `PrestartCheck.dll` accepts
  **1 and 2** (splash progresses), shows a dialog demanding
  `NVIDIA GeForce RTX 3060 Laptop GPU` + `Vulkan 1.2` for other values, and
  then **unconditionally publishes `quitApp`**. So on an unpatched AMD/iGPU
  machine the app is hard-blocked at startup.
- The gate itself is a `deviceType == 2 (DISCRETE_GPU)` comparison with a
  6-byte conditional jump (`0F 85`); relaxing it to `ja` (`0F 87`) lets
  OTHER(0)/INTEGRATED(1)/DISCRETE(2) through while still rejecting
  VIRTUAL(3)/CPU(4). Inside, integrated AMD takes the vendor's AMD branch,
  the `RX\s*(\d{3,4})\b` regex does not match "Radeon 780M", and the vendor
  code itself returns success "out of caution". This exact byte was
  independently field-verified on product 2.0.0-45 on a 780M.
- The compute engine behind the gate is **pure Vulkan** - `Sn3DSpeckleNew`
  imports only `vulkan-1.dll` + `commonVulkan.dll` (no CUDA). The vendor
  even ships Vulkan twins of the CUDA mesh stage (`MeshBasicProcessVulkan`
  alongside `MeshBasicProcessCuda`). Lifting the gate is therefore a
  policy bypass, not an engine substitution.
- The natural fit in this framework is a small module that patches the gate
  byte **in the mapped image** of `Sn3DSpeckleNew.dll` by unique pattern
  (same load-image technique as popup_dim/wallpaper), running in every
  injected process but only ever acting where `Sn3DSpeckleNew.dll` is
  mapped (live-observed: `scanservice.exe` + the hub UI process); nothing
  on disk changes; disable + restart reverts.

## 2. The startup chain (verified)

```
EXStar Hub.exe (splash, PrestartCheck.dll)
   |  publishes  cmd="checkCuda"                (bus "Sn3DBox")
   v
scanservice.exe  loads  Sn3DAlgorithmWrapper.dll   (plugin, scanservice-only)
   Sn3DAlgCheck::onSubscribeData("checkCuda")
      -> Sn3DAlgCheck::checkGpu()
           -> import: ?device_detect@Sn3DSpeckleNew@Sn3DAlgorithm@@YAHXZ
              (Sn3DSpeckleNew.dll, export ordinal 59)      <-- THE GATE
      -> publishCheckResponse(ret, 0)
           -> publishes { cmd:"cudaCheckResponse",
                          errorCode:<ret>, cudaErrorCode:0 }   (always 0)
   v
EXStar Hub.exe  PrestartCheckController::onSubscribeData("cudaCheckResponse")
   handler reads errorCode:
      1 or 2  -> splash progress = 50, continue            (pass)
      3       -> dialog: "... minimum Vulkan 1.2 ..."     + quitApp
      other   -> dialog: "... NVIDIA GeForce RTX 3060 Laptop GPU
                           ... Vulkan 1.2 ..."             + quitApp
   (quitApp publish is unconditional after the message box)
```

Evidence (this machine, vendor logs 2026-11-18, NVIDIA GPU present):
scanservice log carries `preHeatGpu` / algorithm-plat lines; hub log carries
`handle cudaCheckResponse begin QMap((cmd cudaCheckResponse)(cudaErrorCode
0)(errorCode 1))`. Glog line `init_speckle  gpu test result:` is emitted by
`checkGpu` in the scanservice process.

A separate, legacy command `checkCuda` also exists that calls
`Sn3DAlgorithm::device_detect` from **`algorithmHlj.dll`** - that one is the
old **CUDA** detector (cudaDeviceProp dump, "not nvidia device", driver
version gate). It is not on the speckle/TX2 startup path observed here; its
`deviceDetectRes` fields (`isDriveVersionOk`, `isComputeCapabilityOk`,
`isGetDeviceSucceed`, `driverVersion`, `runtimeVersion`) match the wrapper's
log strings.

## 3. The gate itself (`Sn3DSpeckleNew.dll`, sha256 7f74e924..., 2.1.0-10)

Source file baked into the binary:
`Sn3DSpeckleNew\SnCommonGpu\vk\deviceDetect.cpp`, function
`Sn3DGPGPUSpeckle::device_detect`.

Behavior (strings + control flow, all present in the shipping file):

1. `Running GENERAL version device_detect() func !` - Vulkan instance init;
   crash path blames "AMD+NVIDIA hybrid GPU Vulkan incompatibility".
2. Driver API version < desired -> fail; `Device is too old to use` -> fail
   (these land on the `Vulkan 1.2` dialog, errorCode 3).
3. **Device-type gate**:
   `mov eax,[rbp+0x300]` (`VkPhysicalDeviceProperties.deviceType`),
   `cmp eax,2`, `jne out_of_scope`.  Only DISCRETE_GPU reaches the vendor
   branches; INTEGRATED (780M!), VIRTUAL, CPU and OTHER fall to
   `Error: current GPU type is not in design scope ...` -> **return 5**.
4. NVIDIA branch: regex `RTX\s*(\d{3,4})\b`, `GTX\s*(\d{3,4})(?!\d)`;
   in-range -> OK; below desire -> `Warning ... lower than desire`; unmatched
   model name -> `Warning ... not in the scope, but set to success out of
   caution` (success).
5. AMD branch: regex `RX\s*(\d{3,4})\b`; same warning ladder. "Radeon 780M"
   matches neither pattern -> falls into *set to success out of caution* ->
   **return 1**. (Same trick covers any MX/Mobile/7xxM/8xxM naming.)
6. Neither vendor -> `Error: current GPU type is neither AMD nor NVIDIA`
   (Intel rejected); Apple branch `Running on apple M series GPU !` ->
   out-of-scope.

### Patch point (verified in our 2.1.0-10 file)

| item | value |
|---|---|
| pattern | `8B 85 00 03 00 00 83 F8 02 0F 85` |
| occurrences in file | **1** (file offset `0x1AAAB5`) |
| the byte to flip | `0F 85` (jne) -> `0F 87` (ja) at file offset `0x1AAABF` |
| effect | deviceType 0/1/2 enter vendor detection; 3/4 still rejected |
| instruction length | unchanged (6 bytes), no relocations involved |

Our installed DLL is stock (`7F74E924...`, byte = `0x85`). The same byte was
already field-verified to make 2.0.0-45 fully usable on a Radeon 780M
(integrated) box; for 2.1.0-10 the byte/pattern identity is static-verified
here, live verification pending (that is exactly what the user's new
Radeon-only PC will provide).

## 4. Which engines actually matter on AMD

Static import evidence (REA `inspect_pe_imports`):

- `Sn3DSpeckleNew.dll` (speckle depth recon, the gated core):
  imports **only `commonVulkan.dll` + `vulkan-1.dll`** - no CUDA anywhere.
- `MeshBasicDistributeProcess.dll` (mesh post-processing dispatcher) imports
  **both** `MeshBasicProcessCuda.dll` (`mesh_qem_*_cuda`,
  `sn3BiNormalFilterCuda`, denoise) **and** `MeshBasicProcessVulkan.dll`
  (`qem_simp_vulkan::precomputaion/get_device_name/...`) - the vendor ships
  CUDA and Vulkan variants of the same stage and selects between them at
  runtime. `commonVulkan.dll`, `MeshBasicProcessVulkan.dll`,
  `Sn3DVulkanWrapper.dll` are all present in the install.
- CUDA/TensorRT in the box is mostly **other product lines**: dental/face AI
  (`Sn3D*Mono/Sn3DAiSegment`, TensorRT `nvinfer_10`), structured-light
  recon (`Sn3DPhaseBuild`, `Sn3DGratingRecons`), `Sn3DLineCode` (AI line
  extraction), OpenCV CUDA modules. Not part of the TX2 speckle core.
- One genuine NVIDIA dependency on the TX2 path resolved in the vendor's
  favor: `syncservice\device\TX2Plugin*.dll` contains a full **NVIDIA→AMD
  decode fallback chain**. `jpegDecoder` logs show `cudaGetDeviceCount
  failed:` -> `AMD Decoder init ...`; a dedicated `[AMDJPEG]` component
  dynamically loads the AMD **AMF runtime** (`amfrt64.dll`, `AMFInit`,
  `AMFQueryVersion`, DX11 context "AMF will create device internally") -
  AMD UVD JPEG decode. Additionally **libjpeg-turbo is statically linked**
  into the plugin (Copyright 1991-2025 string block), so a CPU decoder is
  present too. Hardware check on the 780M box: AMD drivers with AMF
  (standard Adrenalin install provides `amfrt64.dll`).

So "enable the needed libraries instead of nvidia cuda" is largely already
true by design: the speckle pipeline is Vulkan and the mesh stage has a
Vulkan twin. The factory blocker is the deviceType policy in the gate, not
missing AMD support.

## 5. Second, independent NVIDIA list (not startup-blocking)

`libSn3DNavigation.dll` (`RunCheck::getGPUInfo`) enumerates adapters via
DXGI + WMI `Win32_VideoController`, keeps a hard-coded model whitelist that
is NVIDIA-only (RTX 3060..5090 incl. Laptop variants, RTX A/Ada/PRO
Blackwell), logs `Found GPU: / Exact GPU model matched: / No supported GPU
model found`, and uses `nvidia-smi` for clock/memory polling. This belongs
to the on-demand system-check page (same RunCheck family as disk/RAM
checks), not to the splash gate; treated as informational, revisit if the
new box shows any prompt from it.

## 6. Integration options for this framework

| option | what | verdict |
|---|---|---|
| **A. pattern memory-patch (recommended, built as `modules/gpu_gate`)** | load-image notify on `Sn3DSpeckleNew.dll` in every injected process; find the unique 11-byte pattern in the mapped image, flip `0F 85`->`0F 87` in the private page copy, log the RVA (popup_dim/wallpaper architecture; no hooks, no disk writes, Authenticode intact; disable+restart = stock) | 1:1 with the field-verified file patch; pattern-anchored = survives vendor rebuilds as long as the code is unchanged. Live 2.1.0-10 observation: the DLL is mapped by `scanservice.exe` **and** the hub UI process, so the module must run everywhere the host injects (it does); the patch is neutral in every process on discrete GPUs. NVIDIA self-test passed: unique hit, gate relaxed in both processes at RVA `0x1AB6B5`, vendor flow byte-identical (`init_speckle gpu test result: 1`, hub `errorCode 1`) |
| B. hook the `device_detect` export, return 1 | skips the whole vendor branch | vendor's own caution path already returns 1 legitimately; a hook would skip side effects (kernel-variant globals) - riskier, no benefit |
| C. spoof `vkGetPhysicalDeviceProperties` (deviceType=2 + discrete-sounding name) | gate never sees INTEGRATED | broadest blast radius (every Vulkan user in-process), can mislead real device selection; last resort |
| D. patch files on disk | friend-style single-byte installer | contradicts this project's injection-only principle |

Hub-side changes are **not** needed for the 780M case: the hub only consumes
`errorCode`, and with option A the value arrives as 1 (caution path).

## 7. Open questions (remaining, to close on the Radeon box)

1. End-to-end scan on an iGPU-only machine: throughput/quality, and which
   scanservice stages actually load during a TX2 scan (plugin manifest +
   runtime observation).
2. Mesh decimation router inputs are known (`auto_qem_simp_distribute::
   get_vulkan_device_name_for_step0 -> UsingMeshBasicProcessVulkan`,
   params `is_discrete_gpu`, `gpu_device_name`, `current_available_memrory`,
   `input_mesh_vn`), exact thresholds are not. It carries `_cpu` router
   implementations for denoise, so worst case is slower mesh post, not a
   dead feature.
3. Live log confirmation on the new PC: scanservice should show
   `init_speckle  gpu test result: 1` (now via the AMD "out of caution"
   branch, not the NVIDIA regex) and the hub `errorCode 1` with the module
   enabled. The NVIDIA-side half of this is already done (2026-11-19
   self-test, see §6A): same two values, patch neutral as predicted.
4. TX2 decode path actually taken on 780M: `[AMDJPEG] AMF ... initialized
   successfully` (syncservice log) vs. nvJPEG failures / CPU turbo decode.

Resolved since the first draft (static evidence above): AMF/JPEG fallback
exists in `TX2Plugin`; CUDA-vs-Vulkan mesh dispatch exists as an explicit
`auto_qem_simp_distribute` router (name/memory/discrete driven), not a
hidden CUDA requirement; `libSn3DNavigation` confirmed informational
(system-check page only).

Local-machine note: on an NVIDIA box the patch byte is behavior-neutral
(`deviceType==2` passes both `jne` and `ja`), so local testing can only
prove "module applies cleanly + nothing regresses", never the AMD branch
itself.

## 8. Reproduction tools used

- `tools/zrc_lab.exe` / `tools/dim_dryrun.exe` (unrelated pipeline, same
  lab); byte-pattern scans: ad-hoc PowerShell (`gpu_scan_fast.ps1`).
- REA `inspect_pe_imports`, `search_strings`, `xrefs`, `analyze_function`
  (Ghidra) on `PrestartCheck.dll`, `Sn3DAlgorithmWrapper.dll`,
  `Sn3DSpeckleNew.dll`.
- Vendor logs: `C:\Users\Public\Shining3d\EXStar Hub\log\*` (hub +
  scanservice).
