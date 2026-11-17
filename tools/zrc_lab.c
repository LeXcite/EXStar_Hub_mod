/*
 * zrc_lab.c - offline lab for the compressed-QML layout inside the
 * vendor UI DLLs (dev tool; companion of modules/popup_dim).
 *
 * Scans the raw data of every readable section of a PE *file* for qrc
 * stream records ([u32 BE rawSize][zlib stream]), inflates each and
 * reports:
 *   - every occurrence of the search words with byte context;
 *   - for each `RecursiveBlur` occurrence, exactly what popup_dim's
 *     dimBlurBlocks() sees: is there a '{' within 64 bytes, where does
 *     the brace block end, does it contain `backgroundImg`, and would
 *     dimProp() change radius/loops/opacity (dumb text diff).
 *
 * Usage: zrc_lab <dll path> [word ...]   (default word: backgroundImg)
 * Uses the app's zlib1.dll next to the target, else one on PATH.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _ZS {
    unsigned char *next_in;
    unsigned       avail_in;
    unsigned long  total_in;
    unsigned char *next_out;
    unsigned       avail_out;
    unsigned long  total_out;
    char          *msg;
    void          *state;
    void          *zalloc;
    void          *zfree;
    void          *opaque;
    int            data_type;
    unsigned long  adler;
    unsigned long  reserve;
} ZS;

typedef int (*f_zinit)(ZS *, int, const char *, int);
typedef int (*f_zrun)(ZS *, int);
typedef int (*f_zend)(ZS *);

static f_zinit p_inflateInit2_;
static f_zrun  p_inflate;
static f_zend  p_inflateEnd;

static int loadZlib(const char *hint)
{
    HMODULE z = NULL;

    if (hint)
        z = LoadLibraryA(hint);
    if (!z)
        z = LoadLibraryA("zlib1.dll");
    if (!z)
        return 0;
    p_inflateInit2_ = (f_zinit)(void *)GetProcAddress(z, "inflateInit2_");
    p_inflate       = (f_zrun) (void *)GetProcAddress(z, "inflate");
    p_inflateEnd    = (f_zend) (void *)GetProcAddress(z, "inflateEnd");
    return p_inflateInit2_ && p_inflate && p_inflateEnd;
}

static void *memmem(const void *hay, size_t hl, const void *nd, size_t nl)
{
    size_t i;

    if (!nl || hl < nl)
        return NULL;
    for (i = 0; i + nl <= hl; i++)
        if (memcmp((const char *)hay + i, nd, nl) == 0)
            return (void *)((const char *)hay + i);
    return NULL;
}

static unsigned char *readFile(const char *p, size_t *out)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    unsigned char *buf;
    DWORD got;

    if (h == INVALID_HANDLE_VALUE) {
        printf("open failed (%lu)\n", GetLastError());
        return NULL;
    }
    GetFileSizeEx(h, &sz);
    buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf || !ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL)
        || got != (DWORD)sz.QuadPart) {
        printf("read failed\n");
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *out = (size_t)sz.QuadPart;
    return buf;
}

static void dumpCtx(const unsigned char *t, size_t n, size_t at,
                    const char *word, size_t wl)
{
    size_t a = at > 160 ? at - 160 : 0;
    size_t b = at + wl + 260;
    size_t i;

    if (b > n)
        b = n;
    printf("  ctx @%zu: \"", at);
    for (i = a; i < b; i++)
        putchar(t[i] == '\r' ? '~' :
                (t[i] == '\n' ? '/' :
                 (t[i] >= 9 && t[i] < 127) ? t[i] : '.'));
    printf("\"\n");
}

/* show how dimProp-style edits would land on one blur block: print
 * every `word:` value inside the block and the candidate new value
 * under the production rules (opacity->g_pct, radius/loops->soft) */
static void dimView(const unsigned char *t, size_t bs, size_t be)
{
    static const char *props[] = { "opacity", "radius", "loops", "visible" };
    size_t i, j;

    for (i = bs; i < be; i++) {
        int w;

        for (w = 0; w < 4; w++) {
            size_t wl = strlen(props[w]);

            if (i + wl + 2 > be)
                continue;
            if (memcmp(t + i, props[w], wl))
                continue;
            for (j = i + wl; j < be && (t[j] == ' ' || t[j] == '\t'); j++)
                ;
            if (j >= be || t[j] != ':')
                continue;
            for (i = j + 1; i < be && (t[i] == ' ' || t[i] == '\t'); i++)
                ;
            printf("    %s -> \"%.10s\"\n", props[w], (const char *)t + i);
        }
    }
}

/* replicate dimBlurBlocks()'s view of one RecursiveBlur occurrence */
static void probeBlurBlock(const unsigned char *t, size_t n, size_t b)
{
    static const char kBg[] = "backgroundImg";
    size_t e;
    int depth = 0;

    for (e = b + 13; e < n && t[e] != '{'; e++)
        if (e - b > 64)
            break;
    if (e >= n || e - b > 64) {
        printf("  BLUR @%zu: no '{' within 64B -> production skips\n", b);
        dumpCtx(t, n, b, "RecursiveBlur", 13);
        return;
    }
    for (; e < n && e - b < 4096; e++) {
        if (t[e] == '{')
            depth++;
        else if (t[e] == '}' && --depth == 0)
            break;
    }
    if (e >= n || e - b >= 4096) {
        printf("  BLUR @%zu: block unterminated -> production skips\n", b);
        dumpCtx(t, n, b, "RecursiveBlur", 13);
        return;
    }
    printf("  BLUR @%zu block=%zuB hasBg=%d\n", b, e - b,
           memmem(t + b, e - b, kBg, sizeof kBg - 1) ? 1 : 0);
    dumpCtx(t, n, b, "RecursiveBlur", 13);
    if (memmem(t + b, e - b, kBg, sizeof kBg - 1))
        dimView(t, b, e);
}

static void analyze(const unsigned char *t, size_t n,
                    const char **words, int nw, const char *tag)
{
    int w, found = 0;

    for (w = 0; w < nw; w++) {
        const unsigned char *p = t;
        size_t wl = strlen(words[w]);

        while ((p = (const unsigned char *)memmem(p, n - (size_t)(p - t),
                     (const unsigned char *)words[w], wl)) != NULL) {
            found = 1;
            printf("HIT %s \"%s\" @%zu\n", tag, words[w], (size_t)(p - t));
            dumpCtx(t, n, (size_t)(p - t), words[w], wl);
            p += wl;
        }
    }
    {
        const unsigned char *p = t;
        size_t rb = 13;

        while ((p = (const unsigned char *)memmem(p, n - (size_t)(p - t),
                     (const unsigned char *)"RecursiveBlur", rb)) != NULL) {
            found = 1;
            probeBlurBlock(t, n, (size_t)(p - t));
            p += rb;
        }
    }
    if (!found)
        printf("   (no word hits, no blur blocks)\n");
}

int main(int argc, char **argv)
{
    unsigned char *img;
    size_t fsize, i, k;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    static const char *defWords[] = { "backgroundImg" };
    const char **words = defWords;
    int nw = 1, streams = 0, relevant = 0;
    char zn[MAX_PATH * 2];

    if (argc < 2) {
        printf("usage: zrc_lab <dll> [word ...]\n");
        return 2;
    }
    if (argc > 2) {
        words = (const char **)(argv + 2);
        nw = argc - 2;
    }
    {   /* zlib hint: same folder as the target */
        char *slash = strrchr(argv[1], '\\');

        snprintf(zn, sizeof zn, "%.*s\\zlib1.dll",
                 slash ? (int)(slash - argv[1]) : 0, argv[1]);
    }
    if (!loadZlib(zn) && !loadZlib(NULL)) {
        printf("zlib not available\n");
        return 2;
    }

    img = readFile(argv[1], &fsize);
    if (!img)
        return 2;
    dos = (PIMAGE_DOS_HEADER)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        printf("not a PE\n");
        return 2;
    }
    nt = (PIMAGE_NT_HEADERS)(img + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        printf("bad PE\n");
        return 2;
    }

    if (argc == 5 && strcmp(argv[2], "dump") == 0) {
        /* dump <hexoff> <outfile>: inflate the record whose zlib stream
         * starts at this FILE offset and write the raw bytes out */
        size_t want = (size_t)strtoull(argv[3], NULL, 16);
        size_t i;
        int done = 0;

        for (i = 4; i + 2 < fsize && !done; i++) {
            unsigned cmf = img[i], flg = img[i + 1], raw;
            unsigned char *buf;
            ZS s;
            int r;
            FILE *o;

            if (cmf != 0x78 || (((cmf << 8) | flg) % 31) != 0)
                continue;
            raw = ((unsigned)img[i - 4] << 24) | ((unsigned)img[i - 3] << 16)
                | ((unsigned)img[i - 2] << 8) | (unsigned)img[i - 1];
            if (raw < 64 || raw > 0x400000)
                continue;
            buf = (unsigned char *)malloc(raw + 16);
            if (!buf)
                continue;
            memset(&s, 0, sizeof s);
            s.next_in   = img + i;
            s.avail_in  = (unsigned)(fsize - i);
            s.next_out  = buf;
            s.avail_out = raw + 16;
            if (p_inflateInit2_(&s, 15, "1.2.11", (int)sizeof s) != 0) {
                free(buf);
                continue;
            }
            r = p_inflate(&s, 4);
            p_inflateEnd(&s);
            if (r != 1 || s.total_out != raw) {
                free(buf);
                continue;
            }
            if (i == want) {
                o = fopen(argv[4], "wb");
                if (o) {
                    fwrite(buf, 1, raw, o);
                    fclose(o);
                    printf("dumped %u bytes to %s\n", raw, argv[4]);
                }
                done = 1;
            }
            free(buf);
        }
        if (!done)
            printf("no record with stream at file offset 0x%llX\n",
                   (unsigned long long)want);
        return done ? 0 : 1;
    }

    for (k = 0, sec = IMAGE_FIRST_SECTION(nt);
         k < nt->FileHeader.NumberOfSections; k++, sec++) {
        unsigned char *p;
        size_t len;

        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
            || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
            continue;
        if (!sec->PointerToRawData
            || (size_t)sec->PointerToRawData + sec->SizeOfRawData > fsize)
            continue;
        p = img + sec->PointerToRawData;
        len = sec->SizeOfRawData;

        for (i = 4; i + 6 < len; i++) {
            unsigned cmf = p[i], flg = p[i + 1], raw;
            unsigned char *buf;
            ZS s;
            int r, w, has;

            if (cmf != 0x78 || (((cmf << 8) | flg) % 31) != 0)
                continue;
            raw = ((unsigned)p[i - 4] << 24) | ((unsigned)p[i - 3] << 16)
                | ((unsigned)p[i - 2] << 8) | (unsigned)p[i - 1];
            if (raw < 64 || raw > 0x400000)
                continue;
            buf = (unsigned char *)malloc(raw + 16);
            if (!buf)
                continue;
            memset(&s, 0, sizeof s);
            s.next_in   = p + i;
            s.avail_in  = (unsigned)(len - i);
            s.next_out  = buf;
            s.avail_out = raw + 16;
            if (p_inflateInit2_(&s, 15, "1.2.11", (int)sizeof s) != 0) {
                free(buf);
                continue;
            }
            r = p_inflate(&s, 4);   /* Z_FINISH */
            p_inflateEnd(&s);
            if (r != 1 || s.total_out != raw) {
                free(buf);
                continue;
            }
            streams++;
            has = memmem(buf, raw, "RecursiveBlur", 13) ? 1 : 0;
            if (!has)
                for (w = 0; w < nw; w++)
                    if (memmem(buf, raw, words[w], strlen(words[w])))
                        has = 1;
            if (!has) {
                free(buf);
                continue;
            }
            relevant++;
            printf("== stream @file 0x%llX (raw=%u) ==\n",
                   (unsigned long long)((sec->PointerToRawData) + i), raw);
            analyze(buf, raw, words, nw, "zlib");
            free(buf);
        }

        {   /* plain-text word hits straight in the section */
            int w;

            for (w = 0; w < nw; w++) {
                const unsigned char *q = p;
                size_t wl = strlen(words[w]);

                while ((q = (const unsigned char *)memmem(q,
                           len - (size_t)(q - p),
                           (const unsigned char *)words[w], wl)) != NULL) {
                    printf("== PLAIN \"%s\" sec %.8s off 0x%X ==\n",
                           words[w], (const char *)sec->Name,
                           (unsigned)(q - p));
                    dumpCtx(p, len, (size_t)(q - p), words[w], wl);
                    q += wl;
                }
            }
        }
    }
    printf("streams=%d relevant=%d\n", streams, relevant);
    return 0;
}
