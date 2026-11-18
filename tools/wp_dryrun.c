/*
 * wp_dryrun.c - offline harness for the wallpaper module's JPEG slot
 * search (mirrors modules\wallpaper\wallpaper.c logic 1:1):
 * finds the largest JPEG slot >= min_slot in a PE file, prints offset
 * and size, verifies it against the expected known-good values.
 *
 * build:
 *   cl /W3 /Fe:build\wp_dryrun.exe tools\wp_dryrun.c
 * run:
 *   build\wp_dryrun.exe "<app dir>\AppUi.dll" [min_slot]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long findBytes(const unsigned char *p, size_t len,
                      const unsigned char *needle, size_t n)
{
    size_t i;

    if (len < n)
        return -1;
    for (i = 0; i + n <= len; i++)
        if (p[i] == needle[0] && memcmp(p + i, needle, n) == 0)
            return (long)i;
    return -1;
}

static DWORD jpegSlotSize(const unsigned char *p, size_t cap)
{
    long o = findBytes(p + 3, cap - 3, (const unsigned char *)"\xFF\xD9", 2);

    if (o < 0)
        return 0;
    return (DWORD)(3 + o + 2);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    unsigned minSlot = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0)
                                : 300000u;
    FILE *f;
    unsigned char *b;
    size_t size, off = 0;
    size_t bestOff = 0;
    DWORD  bestSize = 0;
    int    nCand = 0;

    if (!path) {
        printf("usage: wp_dryrun <dll> [min_slot]\n");
        return 2;
    }
    f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc(size);
    if (!b || fread(b, 1, size, f) != size) {
        printf("read failed\n");
        return 2;
    }
    fclose(f);

    while (off + 4 <= size) {
        long o = findBytes(b + off, size - off,
                           (const unsigned char *)"\xFF\xD8\xFF", 3);
        DWORD slot;
        size_t soi;

        if (o < 0)
            break;
        soi  = off + (size_t)o;
        slot = jpegSlotSize(b + soi, size - soi);
        if (slot >= minSlot) {
            nCand++;
            printf("candidate SOI @0x%zX  slot=%u\n", soi, slot);
            if (slot > bestSize) {
                bestOff  = soi;
                bestSize = slot;
            }
        }
        off = soi + 3;
    }
    if (!bestSize) {
        printf("NO slot >= %u found\n", minSlot);
        return 1;
    }
    printf("best: SOI @0x%zX  size %u  candidates %d\n", bestOff,
           bestSize, nCand);
    /* known-good on EXStar Hub 2.x (file stamp 3.1.x) AppUi.dll (2026-11-14) */
    if (bestOff == 0x643F95 && bestSize == 837517)
        printf("MATCH known-good (0x643F95 / 837517)\n");
    free(b);
    return 0;
}
