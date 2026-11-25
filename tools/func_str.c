/* func_str.c - what one function of a shipped DLL actually names.
 *
 * merge_select decides on the keys inside a scene command map, and guessing a
 * key name is exactly how a filter starts withholding the wrong model.  This
 * tool walks the bytes of a function and prints every string it materialises
 * with a RIP-relative `lea`, plus the calls it makes, so the vocabulary of a
 * handler ("visible", "model", "nodes", "marker"...) is read out of the
 * binary instead of remembered from documentation.
 *
 * Nothing is executed and nothing is disassembled in full: only the
 * `48 8D <modrm> rel32` (lea) encodings are decoded, which is how MSVC
 * produces every string reference.
 *
 *   cl /nologo /O2 /MT /utf-8 func_str.c
 *   func_str.exe "<dll>" <startRVA> <length>
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *g_img;
static size_t         g_size;
static IMAGE_SECTION_HEADER *g_sec;
static int            g_nSec;

static int inImg(const void *p)
{
    return (unsigned char *)p >= g_img && (unsigned char *)p < g_img + g_size;
}

static int rvaToOff(unsigned long rva, size_t *offOut)
{
    int i;

    for (i = 0; i < g_nSec; i++) {
        unsigned long va = g_sec[i].VirtualAddress;
        if (rva >= va && rva < va + g_sec[i].Misc.VirtualSize) {
            *offOut = g_sec[i].PointerToRawData + (rva - va);
            return 1;
        }
    }
    return 0;
}

static unsigned long offToRva(size_t off)
{
    int i;

    for (i = 0; i < g_nSec; i++) {
        size_t s = g_sec[i].PointerToRawData;
        if (off >= s && off < s + g_sec[i].SizeOfRawData)
            return g_sec[i].VirtualAddress + (unsigned long)(off - s);
    }
    return 0xFFFFFFFFu;
}

static const char *secName(size_t off)
{
    int i;

    for (i = 0; i < g_nSec; i++)
        if (off >= g_sec[i].PointerToRawData
            && off < g_sec[i].PointerToRawData + g_sec[i].SizeOfRawData)
            return (const char *)g_sec[i].Name;
    return "?";
}

static int isStr(const unsigned char *p, char *out, int cap)
{
    int n = 0;

    if (!inImg(p) || *p == 0)
        return 0;
    while (n < cap - 1 && p[n]) {
        if (p[n] < 0x20 || p[n] >= 0x7F)
            return 0;
        out[n++] = (char)p[n++];
    }
    out[n] = 0;
    return n >= 2;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : NULL;
    unsigned long start, len;
    HANDLE hFile, hMap;
    IMAGE_DOS_HEADER *dh;
    IMAGE_NT_HEADERS *nt;
    size_t fo, a, str = 0, calls = 0;

    if (!path || argc < 4) {
        printf("usage: func_str <dll> <startRVA-hex> <length-hex>\n");
        return 2;
    }
    start = strtoul(argv[2], NULL, 16);
    len   = strtoul(argv[3], NULL, 16);

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("cannot open %s (err %u)\n", path, (unsigned)GetLastError());
        return 1;
    }
    hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    g_img = (unsigned char *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    g_size = GetFileSize(hFile, NULL);
    dh = (IMAGE_DOS_HEADER *)g_img;
    nt = (IMAGE_NT_HEADERS *)(g_img + dh->e_lfanew);
    g_sec = IMAGE_FIRST_SECTION(nt);
    g_nSec = nt->FileHeader.NumberOfSections;

    if (!rvaToOff(start, &fo)) { printf("RVA not on disk\n"); return 1; }
    printf("%s\n  function RVA 0x%X (%llu bytes), section %s\n",
           path, start, (unsigned long long)len, secName(fo));

    for (a = 0; a + 7 < len && fo + a + 7 < g_size; a++) {
        const unsigned char *p = g_img + fo + a;

        /* 48/4C 8D <modrm> [sib] rel32 : lea reg,[rip+disp] */
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D
            && ((p[2] & 0xC7) == 0x05)) {
            int rel = *(const int *)(p + 3);
            size_t t = fo + a + 7 + rel;
            char s[96];

            if (isStr(g_img + t, s, (int)sizeof s)) {
                printf("  +0x%04X  str  %-40s  RVA 0x%08X [%s]\n",
                       (unsigned)a, s, offToRva(t), secName(t));
                str++;
            }
            continue;
        }
        /* E8 rel32 : call */
        if (p[0] == 0xE8) {
            int rel = *(const int *)(p + 1);
            size_t t = fo + a + 5 + rel;

            if (t < g_size) {
                printf("  +0x%04X  call -> RVA 0x%08X [%s]\n",
                       (unsigned)a, offToRva(t), secName(t));
                calls++;
            }
            continue;
        }
        /* FF 15 rel32 : call [rip+IAT] */
        if (p[0] == 0xFF && p[1] == 0x15) {
            int rel = *(const int *)(p + 2);
            size_t t = fo + a + 6 + rel;

            printf("  +0x%04X  call [rip+IAT] RVA 0x%08X\n",
                   (unsigned)a, offToRva(t));
            calls++;
        }
    }
    printf("\n%llu string references, %llu calls\n", (unsigned long long)str,
           (unsigned long long)calls);
    return 0;
}
