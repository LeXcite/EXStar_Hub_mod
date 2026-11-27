/*
 * exstar_export.h - locate an exported function by UNIQUE name inside a
 * mapped module, for modules that detour vendor exports (laser50; the
 * retired fitfill was its other user). Shared, header-only (static, one
 * copy per module DLL).
 *
 * Policy: exports are a named, version-stable ABI contract - never hook
 * by RVA/ordinal. The needle is a substring of the (mangled) export
 * name and MUST match exactly one non-forwarded export: zero or several
 * hits return NULL and the caller stays idle (a guess is never allowed
 * to hook the wrong function). All foreign reads run under SEH.
 */
#ifndef EXSTAR_EXPORT_H
#define EXSTAR_EXPORT_H

#include <windows.h>
#include <string.h>

/* Returns the entry address of the ONLY export whose name contains
 * `needle`, else NULL; *hitsOut reports how many matched, *nameOut (if
 * given) receives the name of the first hit for logging. */
static void *ex_findExport(HMODULE mod, const char *needle, char *nameOut,
                           unsigned nameCap, int *hitsOut)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_EXPORT_DIRECTORY exp;
    DWORD rva, size, i;
    void *found = NULL;

    *hitsOut = 0;
    if (nameOut && nameCap)
        nameOut[0] = 0;
    __try {
        DWORD *names, *funcs;
        WORD  *ords;

        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return NULL;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return NULL;
        rva = nt->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        size = nt->OptionalHeader
                   .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!rva || !size)
            return NULL;
        exp = (PIMAGE_EXPORT_DIRECTORY)(base + rva);
        names = (DWORD *)(base + exp->AddressOfNames);
        ords  = (WORD  *)(base + exp->AddressOfNameOrdinals);
        funcs = (DWORD *)(base + exp->AddressOfFunctions);
        for (i = 0; i < exp->NumberOfNames; i++) {
            const char *nm = (const char *)(base + names[i]);
            if (strstr(nm, needle)) {
                (*hitsOut)++;
                if (*hitsOut > 1)
                    return NULL;             /* ambiguous: refuse        */
                if (rva <= funcs[ords[i]]
                    && funcs[ords[i]] < rva + size)
                    return NULL;             /* forwarder: refuse        */
                found = base + funcs[ords[i]];
                if (nameOut && nameCap) {
                    strncpy(nameOut, nm, nameCap - 1);
                    nameOut[nameCap - 1] = 0;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return found;
}

#endif /* EXSTAR_EXPORT_H */
