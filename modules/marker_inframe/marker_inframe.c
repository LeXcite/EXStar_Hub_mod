/*
 * marker_inframe.c - module: change the vendor "marker in the current
 * scan frame" feedback from a red flash to a size increase.
 *
 * Protocol (full research: docs/research/marker-pipeline.md; live
 * uniform dump: probe=1):
 * every marker recognized in the current scanner frame is moved into
 * the model group "CURRENT_MARKER"; the marker shader program (one
 * shared program, identified by `frontInnerColor` at location 1) uses:
 *
 *   u1 frontInnerColor    vec4  (1,0,0,1) red disc (white = not in frame)
 *   u0 frontExternalColor vec4  (1,0,0,.5) red ring (white = not in frame)
 *   u8 radius             float outer marker radius in mm (5.0 etc.)
 *
 * OSG uploads only uniforms whose value CHANGED - per-marker color is
 * re-uploaded on every change, but `radius` is uploaded rarely (shared
 * value). Scaling it at upload time would therefore leak into every
 * subsequent marker draw. And radius alone is only the fragment-shader
 * disc mask: the quad geometry is placed/sized through
 * osg_ModelView(Matrix|ProjectionMatrix). The full enlarge is scoped
 * to ONE draw call: on glDrawArrays/glDrawElements of the marker
 * program, if the last color upload said "in frame", `radius` and the
 * two matrices are read, the radius and the 3x3 parts of the matrices
 * are multiplied by the scale (right-multiply keeps the translation
 * column, so the marker stays in place), and everything is restored
 * immediately after the real draw. No other draw call ever sees
 * modified values.
 *
 * In-frame recognition reads the disc upload (frontInnerColor): a value
 * is "in frame" when it is vendor red (see isRed) or when it exactly
 * matches one of the tagged colors marker_quality exports (see below).
 *
 * The red color itself is suppressed at upload time (RGB -> configured
 * "normal" color, alpha untouched); that direction needs no restore
 * because the module never writes red.  Nothing else is recolored: an
 * enlarged marker keeps its current disc color, so marker_quality's
 * quality colors survive the enlargement.
 *
 * Cooperation with marker_quality (both modules recolor the same osg
 * uniform `frontInnerColor`, so raw vendor red alone would never reach
 * this hook for markers quality already paints): quality reads what
 * the vendor just wrote; for a red (in-frame) marker it paints the
 * quality color with the green byte +1/255 - invisible to the eye -
 * and exports the exact float4 tuples via
 * `ex_mod_inframe_snapshot(const float **vals)` from marker_quality.dll.
 * Same process, same GL thread, the color callback runs before the
 * frame's draws, so an exact float match against that snapshot is a
 * race-free "this marker is in frame" signal.  Without marker_quality
 * (or with share_inframe = 0) detection degrades to vendor red only.
 *
 * Configuration (modules\marker_inframe.ini):
 *   scale         = F       in-frame size multiplier, 1.0..4.0 (def 1.6)
 *   color_inner   = R G B   replaces red disc RGB     (def 1 1 1)
 *   color_ring    = R G B   replaces red ring RGB     (def 1 1 1)
 *   probe         = 0|1     log marker events to mod.log (def 0)
 * Enable/disable: [marker_inframe] enabled = 0|1 in modules.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "exstar_sdk.h"

#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ACTIVE_UNIFORMS 0x8B86

typedef void (APIENTRY *PFN_glUniform4fv)(int location, int count, const float *value);
typedef void (APIENTRY *PFN_glUniform4f)(int location, float x, float y, float z, float w);
typedef float*(APIENTRY *PFN_glGetUniformfv)(unsigned program, int location, float *params);
typedef void (APIENTRY *PFN_glUniform1f)(int location, float x);
typedef void (APIENTRY *PFN_glGetIntegerv)(unsigned pname, int *params);
typedef int  (APIENTRY *PFN_glGetUniformLocation)(unsigned program, const char *name);
typedef void (APIENTRY *PFN_glGetProgramiv)(unsigned program, unsigned pname, int *params);
typedef void (APIENTRY *PFN_glGetActiveUniform)(unsigned program, unsigned index,
                                                int bufSize, int *length,
                                                int *size, unsigned *type, char *name);
typedef void (APIENTRY *PFN_glDrawArrays)(unsigned mode, int first, int count);
typedef void (APIENTRY *PFN_glDrawElements)(unsigned mode, int count,
                                            unsigned type, const void *indices);
typedef void (APIENTRY *PFN_glUniformMatrix4fv)(int location, int count,
                                                int transpose, const float *value);
typedef void (APIENTRY *PFN_glGetAttachedShaders)(unsigned program, int maxCount,
                                                  int *count, unsigned *shaders);
typedef void (APIENTRY *PFN_glGetShaderiv)(unsigned shader, unsigned pname, int *params);
typedef void (APIENTRY *PFN_glGetShaderSource)(unsigned shader, int bufSize,
                                               int *length, char *string);

#define GL_SHADER_SOURCE_LENGTH 0x8B88

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "marker_inframe",
    "Marker In-Frame",
    "1.1.0",
    "in-frame marker grows instead of flashing red; its color is kept"
};

static const ExHost *g_host;
static float g_scale = 1.6f;
static float g_colorInner[3] = { 1.0f, 1.0f, 1.0f };   /* = not-in-frame */
static float g_colorRing[3]  = { 1.0f, 1.0f, 1.0f };
static int   g_probe;

static PFN_glUniform4fv         real_glUniform4fv;
static PFN_glUniform4f          real_glUniform4f;      /* optional    */
static PFN_glDrawArrays         real_glDrawArrays;
static PFN_glDrawElements       real_glDrawElements;
static PFN_glGetUniformfv       p_glGetUniformfv;
static PFN_glUniform1f          p_glUniform1f;
static PFN_glUniformMatrix4fv   p_glUniformMatrix4fv;
static PFN_glGetIntegerv        p_glGetIntegerv;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glGetProgramiv       p_glGetProgramiv;      /* probe only  */
static PFN_glGetActiveUniform   p_glGetActiveUniform;  /* probe only  */
static PFN_glGetAttachedShaders p_glGetAttachedShaders;/* probe only  */
static PFN_glGetShaderiv        p_glGetShaderiv;       /* probe only  */
static PFN_glGetShaderSource    p_glGetShaderSource;   /* probe only  */

static volatile LONG g_statCalls;   /* glUniform4fv calls        */
static volatile LONG g_statDisc;    /* red disc values replaced  */
static volatile LONG g_statRing;    /* red ring values replaced  */
static volatile LONG g_statDraws;   /* marker draws seen         */
static volatile LONG g_statGrow;    /* draws with scaled radius  */
static volatile LONG g_statTag;     /* in-frame hits via quality */

static float g_buf4[4 * 64];        /* GL-thread scratch only    */

/* Positive findings only: the application runs thousands of shader
 * programs and almost any of them uploads vec4 values at locations
 * 0/1, so negative results must NEVER occupy these slots (a 16-entry
 * cache filled with negatives once starved the marker lookup - the
 * vendor red then passed through untouched). Negatives are memoized
 * in a self-overwriting direct-mapped table instead. */
#define PROG_MAX 8
typedef struct {
    unsigned      id;
    int           radiusLoc;   /* location of "radius", -1 = absent    */
    int           ringLoc;     /* frontExternalColor (the rim)         */
    int           mvLoc;       /* osg_ModelViewMatrix, -1 = absent     */
    int           mvpLoc;      /* osg_ModelViewProjectionMatrix        */
    volatile LONG redMask;     /* bit j: inner color element j was red */
    int           redCount;    /* element count of that color upload   */
    int           dumped;      /* probe: uniform list logged once      */
} Prog;
static Prog     g_pos[PROG_MAX];
static int      g_posN;
static unsigned g_negSeen[256];

/* ------------------------------------------------------------------ */

/* Vendor "marker in current frame" color. The tolerances cover the
 * opaque red disc (1,0,0,1) and the half-transparent red ring
 * (1,0,0,0.5); white markers (1,1,1,*) never match. */
static int isRed(const float *v)
{
    return v[0] >= 0.55f && v[1] <= 0.38f && v[2] <= 0.38f;
}

/* ------------------------------------------------------------------ */
/* marker_quality cooperation (contract in the file header)            */

typedef int (*mq_snap_fn)(const float **vals);
static mq_snap_fn g_mqSnap;                 /* NULL = quality not (yet) there */
static ULONGLONG  g_mqTryTick;

static int mqTags(const float **tag)        /* -> tuple count, 0 = none */
{
    ULONGLONG now = GetTickCount64();

    if (!g_mqSnap) {
        if (now - g_mqTryTick < 2000)
            return 0;
        g_mqTryTick = now;
        {
            HMODULE h = GetModuleHandleA("marker_quality.dll");
            if (h)
                g_mqSnap = (mq_snap_fn) (void *)
                    GetProcAddress(h, "ex_mod_inframe_snapshot");
        }
        if (!g_mqSnap)
            return 0;
    }
    {
        int n = g_mqSnap(tag);
        return (n > 0 && n <= 8) ? n : 0;
    }
}

/* exact float equality: the uploaded array carries the very floats
 * marker_quality wrote, so no tolerance is needed or wanted */
static int tagHit(const float *v, const float *tag, int n)
{
    int i;

    for (i = 0; i < n; i++, tag += 4)
        if (v[0] == tag[0] && v[1] == tag[1] &&
            v[2] == tag[2] && v[3] == tag[3])
            return 1;
    return 0;
}

static void probeSeq(const char *what, int n, float v0)
{
    static volatile LONG budget = 1200;

    if (g_probe && InterlockedDecrement(&budget) > 0)
        g_host->log("marker_inframe: probe %s n=%d v0=%.3f t=%lu",
                    what, n, v0, (unsigned long) GetTickCount64());
}

static void probeShaderSources(unsigned program)
{
    static char src[16384];
    unsigned shaders[8];
    int count = 0, s;

    if (!p_glGetAttachedShaders || !p_glGetShaderiv || !p_glGetShaderSource)
        return;
    p_glGetAttachedShaders(program, 8, &count, shaders);
    for (s = 0; s < count && s < 8; s++) {
        int len = 0, srcLen = 0, lines = 0;
        char *ln;

        p_glGetShaderiv(shaders[s], GL_SHADER_SOURCE_LENGTH, &srcLen);
        if (srcLen <= 1) {
            g_host->log("marker_inframe: probe shader %u: source not retained",
                        shaders[s]);
            continue;
        }
        if (srcLen > (int) sizeof src)
            srcLen = (int) sizeof src - 1;
        p_glGetShaderSource(shaders[s], srcLen, &len, src);
        if (len <= 0)
            continue;
        src[len] = 0;
        if (!strstr(src, "frontInnerColor") && !strstr(src, "radius")) {
            g_host->log("marker_inframe: probe shader %u: %d chars, unrelated",
                        shaders[s], len);
            continue;
        }
        g_host->log("marker_inframe: probe shader %u source, %d chars:",
                    shaders[s], len);
        for (ln = src; *ln && lines < 240; lines++) {
            char line[512];
            char *nl = strchr(ln, '\n');
            int cut = nl ? (int) (nl - ln) : (int) strlen(ln);

            if (cut > 500)
                cut = 500;
            memcpy(line, ln, cut);
            line[cut] = 0;
            g_host->log("marker_inframe: src| %s", line);
            if (!nl)
                break;
            ln = nl + 1;
        }
    }
}

static void probeDump(Prog *p, unsigned program)
{
    int n = 0, i;

    if (!g_probe || p->dumped)
        return;
    p->dumped = 1;
    probeShaderSources(program);
    if (p_glGetProgramiv && p_glGetActiveUniform) {
        p_glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
        g_host->log("marker_inframe: probe program %u: %d active uniforms", program, n);
        for (i = 0; i < n && i < 32; i++) {
            char name[64];
            int len = 0, size = 0, loc = -1;
            unsigned type = 0;

            name[0] = 0;
            p_glGetActiveUniform(program, (unsigned) i, 64, &len, &size, &type, name);
            if (p_glGetUniformLocation)
                loc = p_glGetUniformLocation(program, name);
            g_host->log("marker_inframe: probe   u%-2d %-32s size=%d type=0x%x loc=%d",
                        i, name, size, type, loc);
        }
    }
}

/* Which entry point actually draws the markers (one short line each) */
static void traceDraw(const char *how, unsigned program)
{
    static volatile LONG budget = 6;
    static volatile LONG draws;

    if (InterlockedIncrement(&draws) <= 3 || InterlockedDecrement(&budget) > 0)
        g_host->log("marker_inframe: marker draws via %s (program %u)",
                    how, program);
}

/* The marker program is identified by `frontInnerColor` living at fixed
 * location 1. GL thread only. */
static Prog *markerProgram(void)
{
    unsigned program = 0;
    int i;

    p_glGetIntegerv(GL_CURRENT_PROGRAM, (int *) &program);
    if (!program)
        return NULL;
    for (i = 0; i < g_posN; i++) {
        if (g_pos[i].id == program)
            return &g_pos[i];
    }
    if (g_negSeen[program & 255] == program)
        return NULL;
    if (p_glGetUniformLocation(program, "frontInnerColor") != 1) {
        g_negSeen[program & 255] = program;
        return NULL;
    }
    if (g_posN >= PROG_MAX)
        return NULL;
    {
        Prog *p = &g_pos[g_posN++];

        memset(p, 0, sizeof *p);
        p->id = program;
        p->radiusLoc = p_glGetUniformLocation(program, "radius");
        p->ringLoc  = p_glGetUniformLocation(program, "frontExternalColor");
        p->mvLoc  = p_glGetUniformLocation(program, "osg_ModelViewMatrix");
        p->mvpLoc = p_glGetUniformLocation(program, "osg_ModelViewProjectionMatrix");
        if (p->radiusLoc < 0 && p_glGetProgramiv && p_glGetActiveUniform) {
            /* not under the exact name: find the size uniform by
             * substring so a renamed/vendor-variant shader still works */
            int n = 0;

            p_glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
            for (i = 0; i < n && i < 32; i++) {
                char name[64];
                int len = 0, size = 0, loc = -1;
                unsigned type = 0;

                name[0] = 0;
                p_glGetActiveUniform(program, (unsigned) i, 64, &len,
                                     &size, &type, name);
                if (len <= 0)
                    continue;
                for (len = 0; name[len]; len++)
                    name[len] = (char) tolower((unsigned char) name[len]);
                if (!strstr(name, "radius"))
                    continue;
                loc = p_glGetUniformLocation(program, name);
                if (loc >= 0) {
                    p->radiusLoc = loc;
                    g_host->log("marker_inframe: size uniform auto-detected"
                                " '%s' type=0x%x loc=%d", name, type, loc);
                    break;
                }
            }
        }
        g_host->log("marker_inframe: marker program %u: radius=%d mv=%d mvp=%d",
                    program, p->radiusLoc, p->mvLoc, p->mvpLoc);
        probeDump(p, program);
        return p;
    }
}

/* Does the next drawn element belong to an in-frame marker? A
 * single-value color upload broadcasts; array uploads address by
 * element index. */
static int inFrameAt(const Prog *p, int j)
{
    if (p->redCount == 1)
        return (p->redMask & 1) != 0;
    if (j >= p->redCount)
        return 0;
    return ((LONG) p->redMask >> j & 1) != 0;
}

/* ------------------------------------------------------------------ */
/* color suppression (upload-time; module never writes red, so no
 * restore is needed - the vendor's next color upload overwrites again) */

static void APIENTRY detour_glUniform4fv(int location, int count, const float *value)
{
    const float *out = value;

    InterlockedIncrement(&g_statCalls);

    /* The marker signal is only ever a vec4 upload at location 0/1. */
    if (value != NULL && (location == 0 || location == 1)
        && count >= 1 && count <= 64) {
        Prog *p = NULL;
        int i, rmask = 0, imask = 0;
        const float *tag = NULL;
        int nTag = 0;

        __try {
            p = markerProgram();
            if (p) {
                if (location == 1)
                    nTag = mqTags(&tag);   /* quality's in-frame tags   */
                for (i = 0; i < count && i < 32; i++) {
                    if (isRed(value + i * 4)) {
                        rmask |= 1 << i;
                        imask |= 1 << i;
                    } else if (nTag && tagHit(value + i * 4, tag, nTag)) {
                        imask |= 1 << i;   /* colored by marker_quality */
                        InterlockedIncrement(&g_statTag);
                    }
                }
                if (location == 1) {         /* inner disc upload also
                                              * refreshes the in-frame mask */
                    p->redMask = imask;
                    p->redCount = count;
                }
                if (rmask) {                 /* only true vendor red is
                                              * replaced; tagged quality
                                              * colors pass through kept */
                    const float *to = location == 1 ? g_colorInner : g_colorRing;

                    if (g_probe)
                        probeSeq(location == 1 ? "RED inner" : "RED ring",
                                 count, value[3]);
                    for (i = 0; i < count * 4; i += 4) {
                        if (rmask & (1 << (i / 4))) {
                            g_buf4[i + 0] = to[0];
                            g_buf4[i + 1] = to[1];
                            g_buf4[i + 2] = to[2];
                        } else {
                            g_buf4[i + 0] = value[i + 0];
                            g_buf4[i + 1] = value[i + 1];
                            g_buf4[i + 2] = value[i + 2];
                        }
                        g_buf4[i + 3] = value[i + 3];   /* keep alpha */
                    }
                    out = g_buf4;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = value;
            p = NULL;
            rmask = 0;
        }
        if (p && rmask)
            InterlockedIncrement(location == 1 ? &g_statDisc : &g_statRing);
    }

    real_glUniform4fv(location, count, out);

    {
        static ULONGLONG lastLogged;
        ULONGLONG now = GetTickCount64();
        if (now - lastLogged >= 60000) {
            lastLogged = now;
            g_host->log("marker_inframe: calls=%ld disc=%ld ring=%ld "
                        "draws=%ld grown=%ld tagged=%ld",
                        (long) g_statCalls, (long) g_statDisc, (long) g_statRing,
                        (long) g_statDraws, (long) g_statGrow, (long) g_statTag);
        }
    }
}

/* scalar variant of the color upload - the vendor path is 4fv, but if
 * the signal ever arrives here it is suppressed with the same rules */
static void APIENTRY detour_glUniform4f(int location, float x, float y,
                                        float z, float w)
{
    float nx = x, ny = y, nz = z;

    if (location == 0 || location == 1) {
        Prog *p = NULL;
        float v[4];
        int red, tagged = 0;

        v[0] = x; v[1] = y; v[2] = z; v[3] = w;
        red = isRed(v);
        __try {
            p = markerProgram();
            if (p && location == 1) {   /* keep the in-frame mask fresh */
                const float *tag = NULL;
                int nTag = mqTags(&tag);
                if (!red && nTag && tagHit(v, tag, nTag))
                    tagged = 1;
                p->redMask = (red || tagged) ? 1 : 0;
                p->redCount = 1;
            }
            if (p && red) {             /* only vendor red is replaced */
                const float *to = location == 1 ? g_colorInner : g_colorRing;

                nx = to[0];
                ny = to[1];
                nz = to[2];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            nx = x; ny = y; nz = z;
            p = NULL;
            red = 0;
        }
        if (p && red)
            InterlockedIncrement(location == 1 ? &g_statDisc : &g_statRing);
    }

    real_glUniform4f(location, nx, ny, nz, w);
}

/* ------------------------------------------------------------------ */
/* size scoping: enlarge the marker for exactly one draw call.
 *
 * Live findings: `radius` (float) only drives the disc/ring mask in the
 * fragment shader - the quad geometry itself is positioned/sized via
 * osg_ModelView(Matrix|ProjectionMatrix) (per-marker OSG transform).
 * A full enlarge therefore scopes THREE uniforms to the draw:
 *   - radius        x scale   (mask follows the geometry)
 *   - ModelView     3x3 x scale (right-multiply: geometry grows about
 *   - MVP           3x3 x scale  its own local origin; the translation
 *                                column is untouched -> marker stays)
 * All originals are restored immediately after the real draw. */

typedef struct {
    float radius;                                  /* -1 = untouched */
    int   nMat;
    struct { int loc; float m[16]; } mat[2];
} Scope;

static float ex_fabs(float v)
{
    return v < 0.0f ? -v : v;
}

static void scale3x3(float *m, float f)   /* column-major mat4 */
{
    static const int idx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
    int i;

    for (i = 0; i < 9; i++)
        m[idx[i]] *= f;
}

static void growBegin(Prog *p, Scope *sc)
{
    int locs[2];
    int k;

    memset(sc, 0, sizeof *sc);
    sc->radius = -1.0f;
    if (!inFrameAt(p, 0))
        return;

    /* enlargement is the ONLY in-frame cue; scale 1.0 therefore keeps
     * the vendor look entirely (size alone marks the state) */
    if (g_scale != 1.0f
        && p->radiusLoc >= 0 && p_glGetUniformfv && p_glUniform1f) {
        float r = 0.0f;

        p_glGetUniformfv(p->id, p->radiusLoc, &r);
        if (r > 0.0f) {
            sc->radius = r;
            p_glUniform1f(p->radiusLoc, r * g_scale);
            if (g_probe)
                probeSeq("grow radius", 1, r);
        }
    }

    if (g_scale != 1.0f && p_glGetUniformfv && p_glUniformMatrix4fv) {
        locs[0] = p->mvLoc;
        locs[1] = p->mvpLoc;
        for (k = 0; k < 2 && sc->nMat < 2; k++) {
            float m[16];
            float sum = 0.0f;
            int j;

            if (locs[k] < 0)
                continue;
            p_glGetUniformfv(p->id, locs[k], m);
            for (j = 0; j < 16; j++)
                sum += ex_fabs(m[j]);
            if (sum < 1e-6f)
                continue;              /* never set - leave untouched */
            sc->mat[sc->nMat].loc = locs[k];
            memcpy(sc->mat[sc->nMat].m, m, sizeof m);
            scale3x3(m, g_scale);
            p_glUniformMatrix4fv(locs[k], 1, 0, m);
            sc->nMat++;
            if (g_probe)
                probeSeq("grow matrix", locs[k], m[0]);
        }
    }
}

static void growEnd(Prog *p, const Scope *sc)
{
    int i;

    (void) p;
    if (sc->radius > 0.0f)
        p_glUniform1f(p->radiusLoc, sc->radius);
    for (i = 0; i < sc->nMat; i++)
        p_glUniformMatrix4fv(sc->mat[i].loc, 1, 0, sc->mat[i].m);
}

/* shared body: 1 = this draw belongs to the marker program */
static int drawGate(Prog **pp)
{
    InterlockedIncrement(&g_statDraws);
    *pp = NULL;
    __try {
        *pp = markerProgram();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *pp = NULL;
    }
    return *pp != NULL;
}

static void APIENTRY detour_glDrawArrays(unsigned mode, int first, int count)
{
    Prog *p;
    Scope sc;
    int armed = 0;

    if (drawGate(&p)) {
        traceDraw("glDrawArrays", p->id);
        __try {
            growBegin(p, &sc);
            armed = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&sc, 0, sizeof sc);
            sc.radius = -1.0f;
            armed = 0;
        }
        real_glDrawArrays(mode, first, count);
        if (armed) {
            __try {
                growEnd(p, &sc);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            if (sc.radius > 0.0f || sc.nMat)
                InterlockedIncrement(&g_statGrow);
        }
        return;
    }
    real_glDrawArrays(mode, first, count);
}

static void APIENTRY detour_glDrawElements(unsigned mode, int count,
                                           unsigned type, const void *indices)
{
    Prog *p;
    Scope sc;
    int armed = 0;

    if (drawGate(&p)) {
        traceDraw("glDrawElements", p->id);
        __try {
            growBegin(p, &sc);
            armed = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&sc, 0, sizeof sc);
            sc.radius = -1.0f;
            armed = 0;
        }
        real_glDrawElements(mode, count, type, indices);
        if (armed) {
            __try {
                growEnd(p, &sc);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            if (sc.radius > 0.0f || sc.nMat)
                InterlockedIncrement(&g_statGrow);
        }
        return;
    }
    real_glDrawElements(mode, count, type, indices);
}

/* ------------------------------------------------------------------ */

const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

int ex_init(const ExHost *host)
{
    char buf[64];
    void *t;

    g_host = host;

    if (host->cfg_string(host, "scale", buf, sizeof buf)) {
        float f;
        if (sscanf(buf, "%f", &f) == 1 && f >= 1.0f && f <= 4.0f)
            g_scale = f;
    }
    if (host->cfg_string(host, "color_inner", buf, sizeof buf)) {
        float r, g, b;
        if (sscanf(buf, "%f %f %f", &r, &g, &b) == 3) {
            g_colorInner[0] = clamp01(r);
            g_colorInner[1] = clamp01(g);
            g_colorInner[2] = clamp01(b);
        }
    }
    if (host->cfg_string(host, "color_ring", buf, sizeof buf)) {
        float r, g, b;
        if (sscanf(buf, "%f %f %f", &r, &g, &b) == 3) {
            g_colorRing[0] = clamp01(r);
            g_colorRing[1] = clamp01(g);
            g_colorRing[2] = clamp01(b);
        }
    }
    g_probe = host->cfg_int(host, "probe", 0);

    t = host->gl_thunk("glUniform4fv");        /* waits for host GL */
    if (!t)
        return 1;
    p_glGetIntegerv = (PFN_glGetIntegerv) host->gl_thunk("glGetIntegerv");
    p_glGetUniformLocation =
        (PFN_glGetUniformLocation) host->gl_thunk("glGetUniformLocation");
    p_glGetUniformfv = (PFN_glGetUniformfv) host->gl_thunk("glGetUniformfv");
    p_glUniform1f = (PFN_glUniform1f) host->gl_thunk("glUniform1f");
    p_glUniformMatrix4fv =
        (PFN_glUniformMatrix4fv) host->gl_thunk("glUniformMatrix4fv");
    if (!p_glGetIntegerv || !p_glGetUniformLocation
        || !p_glGetUniformfv || !p_glUniform1f || !p_glUniformMatrix4fv)
        return 2;

    if (!host->hook(t, (void *) detour_glUniform4fv,
                    (void **) &real_glUniform4fv))
        return 3;

    /* scalar color variant: best effort */
    t = host->gl_thunk("glUniform4f");
    if (t)
        host->hook(t, (void *) detour_glUniform4f, (void **) &real_glUniform4f);

    /* draw scoping for the radius - both classic entry points are
     * mandatory (whichever one the vendor uses must be covered, the
     * other is harmless) */
    t = host->gl_thunk("glDrawArrays");
    if (!t || !host->hook(t, (void *) detour_glDrawArrays,
                          (void **) &real_glDrawArrays))
        return 4;
    t = host->gl_thunk("glDrawElements");
    if (!t)
        t = (void *) GetProcAddress(GetModuleHandleA("opengl32.dll"),
                                    "glDrawElements");
    if (!t || !host->hook(t, (void *) detour_glDrawElements,
                          (void **) &real_glDrawElements))
        return 5;

    /* probe-only helpers: absence only disables the diagnostic dump */
    p_glGetProgramiv = (PFN_glGetProgramiv) host->gl_thunk("glGetProgramiv");
    p_glGetActiveUniform =
        (PFN_glGetActiveUniform) host->gl_thunk("glGetActiveUniform");
    p_glGetAttachedShaders =
        (PFN_glGetAttachedShaders) host->gl_thunk("glGetAttachedShaders");
    p_glGetShaderiv = (PFN_glGetShaderiv) host->gl_thunk("glGetShaderiv");
    p_glGetShaderSource =
        (PFN_glGetShaderSource) host->gl_thunk("glGetShaderSource");

    host->log("marker_inframe: armed, scale=%.2f disc=%.2f/%.2f/%.2f "
              "ring=%.2f/%.2f/%.2f probe=%d",
              g_scale,
              g_colorInner[0], g_colorInner[1], g_colorInner[2],
              g_colorRing[0], g_colorRing[1], g_colorRing[2],
              g_probe);
    return 0;
}

void ex_shutdown(void)
{
    /* Detours stay installed: the host is unloaded only when the
     * process is going away. Nothing to undo safely at this point. */
}
