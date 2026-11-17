/*
 * replace_lab.c - offline dry-run for text-REPLACE modules over qrc
 * records inside a vendor DLL (dev tool; the dry-run harness of
 * modules/laser50 [caption] and modules/fps - any [u32+deflate] QML
 * edit can be modelled as a from->to byte replacement).
 *
 * Finds every qrc record [u32 BE rawSize][zlib stream] whose inflated
 * text contains `anchor`, replaces ALL occurrences of the bytes in
 * <from-file> with those in <to-file>, re-deflates at level 9 and
 * reports whether the new stream fits the record's original compressed
 * length (the next record's stream boundary must never move).
 *
 * build:  cl /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:build\replace_lab.exe tools\replace_lab.c
 * run:    build\replace_lab.exe "<dll>" "<anchor>" <from.bin> <to.bin> [patched-raw-out]
 * Uses the zlib1.dll next to the target, else one on PATH.
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
typedef int (*f_zdinit)(ZS *, int, int, int, int, int, const char *, int);
typedef int (*f_zrun)(ZS *, int);
typedef int (*f_zend)(ZS *);

static f_zinit    p_inflateInit2_;
static f_zdinit   p_deflateInit2_;
static f_zrun     p_inflate, p_deflate;
static f_zend     p_inflateEnd, p_deflateEnd;

static int loadZlib(const char *path)
{
    HMODULE z = LoadLibraryA(path && *path ? path : "zlib1.dll");

    if (!z)
        return 0;
    p_inflateInit2_  = (f_zinit)  (void *) GetProcAddress(z, "inflateInit2_");
    p_inflate        = (f_zrun)   (void *) GetProcAddress(z, "inflate");
    p_inflateEnd     = (f_zend)   (void *) GetProcAddress(z, "inflateEnd");
    p_deflateInit2_  = (f_zdinit) (void *) GetProcAddress(z, "deflateInit2_");
    p_deflate        = (f_zrun)   (void *) GetProcAddress(z, "deflate");
    p_deflateEnd     = (f_zend)   (void *) GetProcAddress(z, "deflateEnd");
    return p_inflateInit2_ && p_inflate && p_inflateEnd
        && p_deflateInit2_ && p_deflate && p_deflateEnd;
}

static unsigned char *readFile(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static int countStr(const unsigned char *b, size_t n,
                    const unsigned char *s, size_t sl)
{
    int c = 0;
    size_t i;

    for (i = 0; i + sl <= n; i++)
        if (b[i] == s[0] && !memcmp(b + i, s, sl))
            c++;
    return c;
}

static unsigned long defl9(const unsigned char *raw, size_t rn,
                           unsigned char *out, size_t cap)
{
    ZS d;
    unsigned long n = 0;

    memset(&d, 0, sizeof d);
    d.next_in   = (unsigned char *)raw;
    d.avail_in  = (unsigned)rn;
    d.next_out  = out;
    d.avail_out = (unsigned)cap;
    if (p_deflateInit2_(&d, 9, 8, 15, 8, 0, "1.2.11", (int)sizeof d) == 0) {
        if (p_deflate(&d, 4) == 1)
            n = d.total_out;
        p_deflateEnd(&d);
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *dll, *anchor;
    unsigned char *img, *from, *to, *probe;
    unsigned char *buf;
    size_t fsize, fromLen, toLen, i;
    int records = 0;

    if (argc < 5) {
        printf("usage: replace_lab <dll> <anchor> <from.bin> <to.bin> "
               "[raw-out]\n");
        return 2;
    }
    dll    = argv[1];
    anchor = argv[2];
    from   = readFile(argv[3], &fromLen);
    to     = readFile(argv[4], &toLen);
    if (!from || !to || !fromLen) {
        printf("pattern files unreadable\n");
        return 2;
    }
    probe = (unsigned char *)malloc(1 << 20);
    {   /* zlib hint: same folder as the target */
        char zn[MAX_PATH * 2];
        size_t dl = strlen(dll);
        while (dl && dll[dl - 1] != '\\')
            dl--;
        _snprintf(zn, sizeof zn, "%.*szlib1.dll", (int)dl, dll);
        if (!loadZlib(zn) && !loadZlib("zlib1.dll")) {
            printf("zlib not available\n");
            return 2;
        }
    }
    img = readFile(dll, &fsize);
    if (!img) {
        printf("read failed\n");
        return 2;
    }
    printf("target %s: anchor \"%s\", from %u B, to %u B\n",
           dll, anchor, (unsigned)fromLen, (unsigned)toLen);

    for (i = 4; i + 6 < fsize; i++) {
        unsigned cmf = img[i], flg = img[i + 1], raw;
        ZS s;
        int r, anchorHits, patHits;
        size_t lc;
        unsigned char *patched, *outb;
        size_t newRaw, w, src;
        unsigned long n9, nf;

        if (cmf != 0x78 || (((cmf << 8) | flg) % 31) != 0)
            continue;
        raw = ((unsigned)img[i - 4] << 24) | ((unsigned)img[i - 3] << 16)
            | ((unsigned)img[i - 2] << 8) | (unsigned)img[i - 1];
        if (raw < 64 || raw > 0x800000)
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
        lc = (size_t)s.total_in;
        p_inflateEnd(&s);
        if (r != 1 || s.total_out != raw) {
            free(buf);
            continue;
        }
        anchorHits = countStr(buf, raw, (const unsigned char *)anchor,
                              strlen(anchor));
        patHits = countStr(buf, raw, from, fromLen);
        if (!anchorHits && !patHits) {
            free(buf);
            continue;
        }
        records++;
        printf("== record @file 0x%zX raw=%u lc=%u anchor=%d pattern=%d ==\n",
               i, raw, (unsigned)lc, anchorHits, patHits);
        if (!anchorHits || !patHits) {
            printf("   (anchor or pattern absent - a patching module "
                   "skipping on \"anchor&&count==expected\" would pass)\n");
            free(buf);
            continue;
        }
        newRaw = raw + (size_t)patHits * (toLen - fromLen);
        patched = (unsigned char *)malloc(newRaw);
        outb = (unsigned char *)malloc(lc + 0x40000);
        for (w = 0, src = 0; src < raw; ) {
            if (src + fromLen <= raw
                && !memcmp(buf + src, from, fromLen)) {
                memcpy(patched + w, to, toLen);
                w += toLen;
                src += fromLen;
            } else {
                patched[w++] = buf[src++];
            }
        }
        n9 = defl9(buf, raw, outb, lc + 0x40000);
        nf = defl9(patched, newRaw, outb, lc + 0x40000);
        printf("   newRaw=%u  deflate9: stock=%lu (slack %ld) -> patched=%lu %s\n",
               (unsigned)newRaw, n9, (long)lc - (long)n9, nf,
               nf <= (unsigned long)lc ? "FITS" : "DOES NOT FIT");
        if (argc > 5) {
            FILE *o = fopen(argv[5], "wb");
            if (o) {
                fwrite(patched, 1, newRaw, o);
                fclose(o);
                printf("   patched raw -> %s\n", argv[5]);
            }
        }
        free(outb);
        free(patched);
        free(buf);
    }
    if (!records)
        printf("anchor/pattern found in no record\n");
    return 0;
}
