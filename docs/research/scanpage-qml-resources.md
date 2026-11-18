# Vendor compressed QML resources (Qt zrc) — mechanism protocol

The general knowledge behind the `popup_dim`, `fps` and `laser50 [caption]` modules: where the
scan page's QML lives, how it is packed, how it is patched in process memory before the first
render, and which traps stand in the way. The feature-batch status journal is
`batch-2026-11-17-status.md`; product details are in the modules' READMEs.

---

## 1. Where the QML lives

The vendor's UI pages are NOT external `.qml` files. They are compiled into the Qt resources
(`qrc`) of the DLLs themselves (`ScanPage_E7.dll`, `AppUi.dll`,
`pluginSn3DNavigation.dll`, `CompositeComponents.dll`, ...). In the DLL image they are
read-only sections holding a sequence of records:

```
[u32 BE rawSize][zlib stream][u32 BE rawSize][zlib stream]...
```

- `rawSize` — declaring decompressed size, big-endian;
- the stream is plain zlib (`78` + flags, `(cmf<<8|flg) % 31 == 0`);
- the application engine unpacks them itself with its own `zlib1.dll` at Qt startup;
- records run back to back, the next `rawSize` is read immediately after the end of the previous
  stream — therefore **the boundary of the next stream is an inviolable constant**.

Signs used for recon (implemented in `tools\zrc_lab.exe`, scan/dump modes):
`cmf==0x78`, FCIV checksum check, a plausible `rawSize` before the stream, and after
decompression — readable QML text. dump mode: `zrc_lab.exe <dll> dump <fileoff>
<out>` — emits the inflated text of the record for viewing/searching.

## 2. Live coordinates (2.x, file stamp 3.1.x, ScanPage_E7.dll; NOT a constant — example only)

| record | file offset | raw | packed (lc) | live VA | VA−file |
|---|---|---|---|---|---|
| scan page (`/// ScanPage_E7`) | `0x97D318` | 137 735 | 22 753 | `0x97E518` | +0x1200 |
| `ScanSettingModelProviderLaser` | `0x98C90E` | 67 753 | 9 777 | `0x98DB0E` | +0x1200 |

The file↔VA delta depends on the sections and drifts with updates — the modules work only with
content (text anchor), offsets are never hardcoded.
On the friend's build (2.0.0-45) the laser provider lay at `H=0x98C906` — the record
structure survives between versions, the content changed (see §5).

## 3. Injection window: before DllMain

`LdrRegisterDllNotification` (ntdll) invokes the callback at the moment the module is mapped —
before its DllMain and before Qt has a chance to parse the QML. We patch the record in the
mapped image (`VirtualProtect` read-write for the duration of the write), the files on disk are
left alone; a restart restores vendor behaviour. A worker polling
`GetModuleHandleA` is the fallback (the notification doesn't always register,
plus already-loaded cases).

The QML disk catalog (`%LOCALAPPDATA%\QML\...`) compiles bytecode keyed by the source
signature — mandatory `QML_DISABLE_DISK_CACHE=1` (the popup_dim lesson,
otherwise the patch gets "swallowed" by the cache on the second launch).

## 4. Safe-write protocol (shared code: `include\exstar_qrc.h`)

1. Inflate the record from the image; gate on content (`anchor`×N && `pattern`×M),
   not on offset.
2. Text transformation (snippet insertion or pattern replace-all).
3. re-deflate at **level 9** must fit into the original `lc` — otherwise skip
   (measuring "how much is needed" only under verbose);
4. byte-exact copy of `4+lc` before the write;
5. rawSize(u32 BE) + stream — under a single `VirtualProtect` pair; the remainder after
   the new stream is zeroed;
6. verification inflate **from the image** and byte-by-byte comparison with the intended
   result; a mismatch — restore the copy, log it, stay vendor-original.

All foreign reads/writes are under SEH; the loader-thread vs worker race — an atomic
flag. Log line formats are pinned in the modules' READMEs (artifacts quote them).

## 5. Translations: `.qm` catalogs and how not to break them

The vendor's QML strings are Chinese by default (`qsTr("%1线")`), but the English UI shows
"lines" — because `qsTr` looks up the **key = source text** in the loaded translation catalog:
the `.qm` catalog gives for EN `"%1线" → "%1 lines"`. Consequences:

- what must change is NOT the translatable source (changing the source changes the key —
  the catalog misses, and the raw replacement leaks into all locales; that is how
  captions v1.0.0 showed an English-speaking user the Russian "lines" caption);
- the right way is to change **the format argument** (`.arg(50)`): the key stays intact,
  the language is preserved, only the number changes;
- if a string genuinely isn't translated (no key in the catalog) — it shows hanzi even in
  EN; verify with a screenshot of the actual localized UI, not a QML dump.

This also explains the "different shape" of the strings on the friend's build (2.0.0-45:
literals `qsTr("50线")`) versus ours, 2.x (file stamp 3.1.x) (`qsTr("%1线").arg(<model field>)`):
between versions the vendor moved the captions to dynamic numbers from the C++ model.

## 6. Dry runs (mandatory before live)

`tools\replace_lab.exe "<dll>" "<anchor>" <from.bin> <to.bin>`:
finds every record with anchor/pattern, counts occurrences, re-deflates the intended
result and prints `stock=<...> (slack ...) -> patched=<...> FITS|DOES NOT FIT`.
The dry-run numbers must match the live log byte for byte — the module and the
harness use one and the same machinery:

| module | from | to | expectation | live log |
|---|---|---|---|---|
| fps | `id: root\r\n` | same + 791 B snippet | `@0x97D318 stock=22392 → patched=22610 FITS` (lc 22753) | `raw 137735->138526, packed 22753<-22610 (ok)` |
| laser50 [caption] | `qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)` | `qsTr("%1线").arg(50)` | `@0x98C90E pattern=1, stock=9583 → patched=9587 FITS` (lc 9777) | `raw 67753->67711, packed 9777<-9587, 1 caption(s) rewritten (ok)` |

(`fps_lab.c` was a separate harness for insertions — deleted 2026-11-17 after
confirming that replace_lab reproduces its numbers exactly.)

## 7. Known traps (we stepped on all of them)

- **`#` in ini values** was cut off as a comment (`color = #8ecbff` → empty).
  Fix in the host parser: `#`/`;` outside `"..."`, quotes are consumed; values with
  `#` are written in quotes.
- **QML disk cache** — see §3.
- **Record boundary**: a patch that inflated the stream beyond `lc` would have shifted the
  next record → white screen. Hence "it doesn't fit = we don't patch".
- **Zeroing the rawSize** as "disable the record" — forbidden (Qt would skip the resource
  entirely → empty page); byte-level rollback only.
- **"Already patched" check** by the snippet's presence string (`exFpsLabel`) or the
  presence of the replacement — idempotency against a repeated pass.
