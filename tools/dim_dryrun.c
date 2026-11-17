/* dim_dryrun.c - offline proof for popup_dim's zrc walkers.
 *
 * Builds popup_dim.c directly into a console tool (no host, no injection)
 * and replays dimBlurBlocks + dimGaussBlocks + the re-deflate sizing over
 * every qrc zlib record of a DLL FILE, printing the exact byte edits the
 * module would make in the mapped image.  Nothing is written anywhere.
 *
 * build:  cl /W3 /I include tools\dim_dryrun.c /Febuild\dim_dryrun.exe
 * usage:  dim_dryrun.exe <target.dll> <path\to\app\zlib1.dll>
 */
#define _CRT_SECURE_NO_WARNINGS
#include "modules/popup_dim/popup_dim.c"

static void dumpDiff(const unsigned char *a, const unsigned char *b,
                     size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t s, e;
        if (a[i] == b[i]) { i++; continue; }
        s = i;
        while (i < n && a[i] != b[i])
            i++;
        e = i;
        printf("    [%zu..%zu)\n", s, e);
        {
            size_t cs = s > 70 ? s - 70 : 0;
            size_t ce = e + 70 < n ? e + 70 : n;
            size_t k;
            printf("      old: ");
            for (k = cs; k < ce; k++)
                putchar(a[k] == '\r' || a[k] == '\n' ? '~' : a[k]);
            printf("\n      new: ");
            for (k = cs; k < ce; k++)
                putchar(b[k] == '\r' || b[k] == '\n' ? '~' : b[k]);
            printf("\n");
        }
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *file;
    long fsz;
    size_t i;
    int totalStreams = 0, totalBlocks = 0, tooBig = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <target.dll> <zlib1.dll>\n", argv[0]);
        return 2;
    }
    if (!LoadLibraryA(argv[2])) {
        fprintf(stderr, "cannot load %s\n", argv[2]);
        return 2;
    }
    loadZlib();
    if (!g_zlib) {
        fprintf(stderr, "zlib entry points missing\n");
        return 2;
    }
    /* production defaults: dim_alpha = 0, soft_blur = 1 */
    g_pct = 0;
    strcpy_s(g_opStr, sizeof g_opStr, "0.0");
    g_softBlur = 1;
    g_verbose  = 1;

    f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = (unsigned char *)malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, f) != (size_t)fsz) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);

    for (i = 0; i + 8 < (size_t)fsz; i++) {
        unsigned cmf = file[i + 4], flg = file[i + 5];
        unsigned raw;
        unsigned char *buf, *copy, *out;
        PD_ZS s;
        int r;
        size_t lc, dim;

        if (cmf != 0x78 || (((cmf << 8) | flg) % 31) != 0)
            continue;
        raw = ((unsigned)file[i] << 24) | ((unsigned)file[i + 1] << 16)
            | ((unsigned)file[i + 2] << 8) | (unsigned)file[i + 3];
        if (raw < 64 || raw > 0x400000)
            continue;
        buf = (unsigned char *)malloc(raw + 16);
        if (!buf)
            return 3;
        memset(&s, 0, sizeof s);
        s.next_in   = file + i + 4;
        s.avail_in  = (unsigned)((size_t)fsz - i - 4);
        s.next_out  = buf;
        s.avail_out = raw + 16;
        if (p_inflateInit2_(&s, 15, PD_ZLIB_VER, (int)sizeof s) != 0) {
            free(buf);
            continue;
        }
        r  = p_inflate(&s, 4);
        lc = (size_t)s.total_in;
        p_inflateEnd(&s);
        if (r != 1 || s.total_out != raw ||
            (findBytes(buf, raw, (const unsigned char *)"backgroundImg", 13) < 0 &&
             findBytes(buf, raw, (const unsigned char *)"GaussianBlur {", 14) < 0)) {
            free(buf);
            i += 3;
            continue;
        }

        copy = (unsigned char *)malloc(raw);
        if (!copy) { free(buf); return 3; }
        memcpy(copy, buf, raw);
        dim = (size_t)(dimBlurBlocks(buf, raw) + dimGaussBlocks(buf, raw));
        if (dim == 0) {
            free(copy);
            free(buf);
            i += lc >= 4 ? lc - 4 : 0;
            continue;
        }

        /* re-deflate into the original slot like applyZrc does */
        out = (unsigned char *)malloc(lc ? lc : 64);
        {
            unsigned long nc = 0;
            int lvl;
            for (lvl = 9; out && nc == 0 && lvl >= 6; lvl -= 3) {
                PD_ZS d;
                memset(&d, 0, sizeof d);
                d.next_in   = buf;
                d.avail_in  = (unsigned)raw;
                d.next_out  = out;
                d.avail_out = (unsigned)lc;
                if (p_deflateInit2_(&d, lvl, 8, 15, 8, 0, PD_ZLIB_VER,
                                    (int)sizeof d) == 0) {
                    if (p_deflate(&d, 4) == 1
                        && d.total_out <= (unsigned long)lc)
                        nc = d.total_out;
                    else if (lvl == 6) {
                        printf("  stream @0x%llX blocks=%zu RECOMPRESS-TOO-BIG "
                               "%lu>%lu (module would skip)\n",
                               (unsigned long long)i, dim,
                               (unsigned long)d.total_out, (unsigned long)lc);
                        tooBig++;
                    }
                    p_deflateEnd(&d);
                }
            }
            if (nc) {
                printf("  stream @0x%llX raw=%u csize=%zu blocks=%zu "
                       "redefl lvl->%lu bytes  fits\n",
                       (unsigned long long)i, raw, lc, dim,
                       (unsigned long)nc);
                totalStreams++;
                totalBlocks += (int)dim;
                dumpDiff(copy, buf, raw);
            }
        }
        free(out);
        free(copy);
        free(buf);
        i += lc >= 4 ? lc - 4 : 0;
    }
    printf("== %s: zlib streams=%d blocks=%d too_big=%d ==\n",
           argv[1], totalStreams, totalBlocks, tooBig);
    {   /* raw-QML pass equivalent: the walker straight on the file */
        unsigned char *orig = (unsigned char *)malloc((size_t)fsz);
        size_t dim;
        if (orig) {
            memcpy(orig, file, (size_t)fsz);
            dim = (size_t)dimGaussBlocks(file, (size_t)fsz);
            printf("== raw pass: blocks=%zu ==\n", dim);
            if (dim)
                dumpDiff(orig, file, (size_t)fsz);
            free(orig);
        }
    }
    free(file);
    return 0;
}
