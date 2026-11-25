/* scmd_lab.c - the full vocabulary of the scene's named commands.
 *
 * merge_select hooks commands by name and filters them by the model names
 * inside, so guessing which commands exist is no longer good enough: the press
 * of a button turned out to re-show models through a command the module was
 * not watching.  This tool reads the scene plugin's bytes (nothing is
 * executed) and prints every command its own registration code installs.
 *
 * A registration, as documented in
 * docs\research\merge-common-mesh.md section 8, materialises the name and the
 * handler next to each other:
 *
 *   lea rdx,[rip+commandName]     48 8D 15 rel32
 *   ...  QString construction
 *   lea rcx,[rip+handler]         48 8D 0D rel32
 *   mov [rsp+disp8],rcx           48 89 4C 24 disp8
 *
 * A name is accepted only at the START of a string (the byte before it is
 * NUL), which is what removed the mid-string noise from an earlier version of
 * this tool.
 *
 *   cl /nologo /O2 /MT /utf-8 scmd_lab.c
 *   scmd_lab.exe "<scenecontrol.dll>"
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

static int execSec(size_t off)
{
    int i;

    for (i = 0; i < g_nSec; i++)
        if (off >= g_sec[i].PointerToRawData
            && off < g_sec[i].PointerToRawData + g_sec[i].SizeOfRawData)
            return (g_sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    return 0;
}

/* an identifier-like name of 3..48 chars starting exactly at `p` */
static int nameAt(const unsigned char *p, char *out, int cap)
{
    int n = 0;

    if (!inImg(p))
        return 0;
    while (n < cap - 1 && p[n]) {
        char c = (char)p[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return 0;
        out[n++] = c;
    }
    out[n] = 0;
    return n >= 3;
}

struct CmdEnt { char nm[64]; unsigned long hRva; };

static int cmpCmd(const void *a, const void *b)
{
    return strcmp(((const struct CmdEnt *)a)->nm,
                  ((const struct CmdEnt *)b)->nm);
}

#define MAXC 512
static struct CmdEnt g_c[MAXC];
static int g_nc;

static void add(const char *nm, unsigned long hRva)
{
    int i;

    for (i = 0; i < g_nc; i++)
        if (g_c[i].hRva == hRva && strcmp(g_c[i].nm, nm) == 0)
            return;
    if (g_nc >= MAXC)
        return;
    strcpy_s(g_c[g_nc].nm, sizeof g_c[g_nc].nm, nm);
    g_c[g_nc].hRva = hRva;
    g_nc++;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\sn3d.digit.scenecontrol.dll";
    HANDLE hFile, hMap;
    IMAGE_DOS_HEADER *dh;
    IMAGE_NT_HEADERS *nt;
    int s, i;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("cannot open %s (err %u)\n", path, (unsigned)GetLastError());
        return 1;
    }
    hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    g_img = (unsigned char *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!g_img) { printf("view failed\n"); return 1; }
    g_size = GetFileSize(hFile, NULL);
    dh = (IMAGE_DOS_HEADER *)g_img;
    nt = (IMAGE_NT_HEADERS *)(g_img + dh->e_lfanew);
    g_sec = IMAGE_FIRST_SECTION(nt);
    g_nSec = nt->FileHeader.NumberOfSections;
    printf("%s: %llu bytes, %d sections\n\n", path,
           (unsigned long long)g_size, g_nSec);

    for (s = 0; s < g_nSec; s++) {
        unsigned char *code, *end;
        size_t a;

        if (!(g_sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        code = g_img + g_sec[s].PointerToRawData;
        end  = code + g_sec[s].Misc.VirtualSize;
        if (end > g_img + g_size)
            end = g_img + g_size;

        for (a = 0; a + 8 < (size_t)(end - code); a++) {
            const unsigned char *p = code + a;
            const unsigned char *tgt;
            size_t toff;
            char nm[64];
            int b;

            if (!(p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15))
                continue;
            tgt = p + 7 + *(const int *)(p + 3);
            if (!inImg(tgt))
                continue;
            toff = (size_t)(tgt - g_img);
            if (toff == 0 || tgt[-1] != 0)
                continue;                 /* not the start of a string */
            if (!nameAt(tgt, nm, (int)sizeof nm))
                continue;

            for (b = 7; b < 128 && a + (size_t)b + 11 < (size_t)(end - code); b++) {
                const unsigned char *q = p + b;
                const unsigned char *h;

                if (!(q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D))
                    continue;
                h = q + 7 + *(const int *)(q + 3);
                if (!inImg(h))
                    continue;
                if (!(q[7] == 0x48 && q[8] == 0x89 && q[9] == 0x4C
                      && q[10] == 0x24))
                    continue;             /* not mov [rsp+disp8],rcx */
                if (!execSec((size_t)(h - g_img)))
                    continue;             /* not a handler */
                add(nm, offToRva((size_t)(h - g_img)));
                break;
            }
        }
    }

    qsort(g_c, (size_t)g_nc, sizeof g_c[0], cmpCmd);
    for (i = 0; i < g_nc; i++)
        printf("  %-34s handler RVA 0x%08X\n", g_c[i].nm, g_c[i].hRva);
    printf("\n%d registrations\n", g_nc);
    return 0;
}
