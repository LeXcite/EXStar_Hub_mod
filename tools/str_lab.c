/*
 * str_lab.c - offline lab for the trick marker_quality uses: OSG's
 * StateSet::getUniform() takes a `const std::string&`, and this project's
 * modules are C, so a std::string has to be described by hand.
 *
 * MSVC x64 (release, so no iterator-debug header - the vendor's
 * osg158-osg.dll is release too) lays a std::string out as
 *
 *     @0   union { char buffer[16]; char *pointer; }   16 bytes
 *     @16  size_t      size
 *     @24  size_t      capacity
 *
 * and picks buffer or pointer by "capacity <= 15".  So:
 *
 *   - a name up to 15 characters is described inline: buffer + size + 15
 *     ("frontInnerColor" is exactly 15),
 *   - a longer one needs the POINTER at @0 plus 8 bytes of PADDING, then
 *     size @16 and a capacity above 15 ("frontExternalColor" is 18).
 *
 * The lab proves both, and reproduces the miss that happens when the
 * padding is forgotten: a 24-byte struct { pointer, size, capacity } puts
 * size at @8 (inside the union) and leaves @24 zero, which makes the STL
 * read the name out of the pointer bytes - the lookup then silently misses
 * for every long name.
 *
 * Build (release is required, see above):
 *
 *   cl /nologo /W3 /MD /O2 /TP /Festr_lab.exe tools\str_lab.c
 *
 * Run from the repository root; exit code 0 = every expectation matched.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>

struct sso_str { char text[16]; size_t size, capacity; };     /* 32 bytes */
struct ref_str { const void *ptr; size_t pad; size_t size, capacity; };
struct old_str { const void *ptr; size_t size, capacity; };   /* 24 bytes */

typedef std::map<std::string, int> Map;

static int failures;

static void check(const char *what, Map &m, const void *fake, int wantHit)
{
    Map::const_iterator it =
        m.find(*reinterpret_cast<const std::string *>(fake));
    int hit = (it != m.end());
    if (hit != wantHit)
        failures++;
    printf("  %-46s -> %-4s (%s)\n", what, hit ? "HIT" : "MISS",
           hit == wantHit ? "as expected" : "UNEXPECTED");
}

/* ------------------------------------------------------------------ map walk
 * The same raw-offset walk modules/marker_quality uses in ss_lookup(), run
 * against a real std::map<std::string, pair<void*, unsigned>> - the shape of
 * osg::StateSet::UniformListMap (name -> { ref_ptr<Uniform>, index }).  It
 * proves the header/node offsets and the name decoding before a single frame
 * of the vendor application is involved.
 */
typedef std::map<std::string, std::pair<void *, unsigned> > UMap;

/* the same walk marker_quality's ss_lookup() does, against the same layout:
 * map+0 = pointer to the header node, map+8 = _Mysize; the header node holds
 * leftmost/_Left, root/_Parent, rightmost/_Right and _Isnil = 1, and every
 * unused child pointer of a real node holds the header, so the header doubles
 * as the null sentinel. */
static void *map_walk(UMap *m, const char *want, char *list, size_t cap,
                      int *nList)
{
    unsigned char *map = (unsigned char *) m;
    void *found = NULL, *nil, *cur, *stack[48];
    int sp = 0, n = 0, used = 0;
    size_t mysize = 0;

    if (list && cap)
        list[0] = 0;
    memcpy(&nil, map, 8);                            /* _Myhead            */
    memcpy(&mysize, map + 8, 8);                     /* _Mysize            */
    if (!nil || mysize == 0 || mysize > 4096)
        return NULL;
    memcpy(&cur, (unsigned char *) nil + 8, 8);       /* header->_Parent    */
    if (cur == nil)
        return NULL;

    for (;;) {
        while (cur && cur != nil && sp < (int) (sizeof stack / sizeof *stack)) {
            stack[sp++] = cur;
            memcpy(&cur, (unsigned char *) cur, 8);  /* left child         */
        }
        if (sp == 0)
            break;
        cur = stack[--sp];
        {
            unsigned char *node = (unsigned char *) cur;
            size_t sz = 0, cp = 0;
            const void *text = NULL;
            char inlineBuf[16];
            char name[40];
            void *u = NULL;

            if (node[25])                            /* the header itself  */
                goto next;
            memcpy(&sz, node + 32 + 16, 8);          /* std::string size    */
            memcpy(&cp, node + 32 + 24, 8);          /* std::string capacity*/
            if (sz > 0 && sz < sizeof name) {
                if (cp <= 15) {
                    memcpy(inlineBuf, node + 32, 16);
                    text = inlineBuf;
                } else {
                    memcpy(&text, node + 32, 8);
                }
                if (text) {
                    memcpy(name, text, sz);
                    name[sz] = 0;
                    memcpy(&u, node + 32 + 32, 8);   /* the pair's pointer */
                    n++;
                    if (want && strcmp(name, want) == 0 && !found)
                        found = u;
                    if (list && n <= 8 &&
                        used + (int) strlen(name) + 2 < (int) cap) {
                        size_t l = strlen(name);
                        memcpy(list + used, name, l);
                        used += (int) l;
                        list[used++] = ' ';
                        list[used] = 0;
                    }
                }
            }
        next:;
        }
        memcpy(&cur, (unsigned char *) cur + 16, 8);  /* right child        */
        if (n > 256)
            break;
    }
    if (nList)
        *nList = n;
    return found;
}

static void check_walk(void)
{
    int markerA = 1, markerB = 2, markerC = 3;
    char names[160];
    int n = 0, bad = 0;
    void *got;
    UMap m;

    m.insert(std::make_pair(std::string("frontInnerColor"),
                            std::make_pair((void *) &markerA, 0u)));
    m.insert(std::make_pair(std::string("frontExternalColor"),
                            std::make_pair((void *) &markerB, 0u)));
    m.insert(std::make_pair(std::string("radius"),
                            std::make_pair((void *) &markerC, 2u)));

    got = map_walk(&m, "frontExternalColor", names, sizeof names, &n);
    printf("\nmap walk over a 3-entry uniform map (%d names: %s):\n", n, names);
    printf("  frontExternalColor -> %s\n", got == &markerB ? "the right value"
                                                          : "WRONG VALUE");
    if (got != &markerB) bad++;
    if (n != 3) { printf("  name count != 3\n"); bad++; }
    if (map_walk(&m, "noSuchUniform", NULL, 0, NULL) != NULL) {
        printf("  unknown name resolved (should not)\n"); bad++;
    }
    got = map_walk(&m, "radius", NULL, 0, NULL);
    if (got != &markerC) { printf("  radius wrong\n"); bad++; }
    printf("  unknown name misses, radius resolves: %s\n",
           bad ? "FAILED" : "ok");
    failures += bad;
}

int main(void)
{
    static const char HEAP_NAME[] = "frontExternalColor";   /* 18 chars */
    static const char SSO_NAME[]  = "frontInnerColor";      /* 15 chars */
    struct sso_str inlineDisc;
    struct ref_str ring18  = { HEAP_NAME, 0, 18, 18 };
    struct ref_str ring31  = { HEAP_NAME, 0, 18, 31 };
    struct ref_str ringNoCap = { HEAP_NAME, 0, 18, 0 };
    struct ref_str discAsHeap = { SSO_NAME, 0, 15, 16 };
    struct old_str broken      = { HEAP_NAME, 18, 18 };
    unsigned char brokenBuf[32];
    Map m;
    std::string realHeap("frontExternalColor");
    std::string realSso("frontInnerColor");

    m.insert(std::make_pair(std::string(HEAP_NAME), 1));
    m.insert(std::make_pair(std::string(SSO_NAME),  2));

    printf("sizeof(std::string) = %zu (32 = MSVC x64 release)\n",
           sizeof(std::string));
    printf("real 18-char string: size=%zu capacity=%zu data=%s\n",
           realHeap.size(), realHeap.capacity(), realHeap.data());
    printf("real 15-char string: size=%zu capacity=%zu data=%s\n\n",
           realSso.size(), realSso.capacity(), realSso.data());

    memset(&inlineDisc, 0, sizeof inlineDisc);
    memcpy(inlineDisc.text, SSO_NAME, 16);
    inlineDisc.size = 15; inlineDisc.capacity = 15;

    /* what .rdata holds for a 24-byte initializer: the struct plus zeros */
    memset(brokenBuf, 0, sizeof brokenBuf);
    memcpy(brokenBuf, &broken, sizeof broken);

    printf("lookups in a map that holds both names:\n");
    check("inline {buffer,15,15}  - the disc name", m, &inlineDisc, 1);
    check("heap {ptr,pad,18,18}   - the ring name", m, &ring18, 1);
    check("heap {ptr,pad,18,31}   - a real capacity", m, &ring31, 1);
    check("heap {ptr,pad,15,16}   - short name as heap", m, &discAsHeap, 1);
    check("heap {ptr,pad,18,0}    - capacity read as 0", m, &ringNoCap, 0);
    check("24-byte {ptr,18,18} + zeros - the bug", m, brokenBuf, 0);

    check_walk();

    printf("\n%s (%d unexpected result%s)\n",
           failures ? "LAB FAILED" : "lab agrees with the model",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
