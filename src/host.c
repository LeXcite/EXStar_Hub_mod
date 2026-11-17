/*
 * host.c - ExStarRec.dll, the module host of the EXStar Hub mod framework.
 *
 * Loaded by the winmm proxy (winmm.dll in the application directory).
 * Responsibilities:
 *   - locate and load module DLLs from `<host dir>\modules\*.dll`;
 *   - validate the module ABI and the per-module `enabled` flag from
 *     `<app dir>\modules.ini` (default: enabled);
 *   - provide the ExHost service table (logging, per-module config,
 *     GL entry-point resolution, detour installation);
 *   - call ex_init() of every enabled module on one worker thread that
 *     owns a private (hidden-window) OpenGL context.
 *
 * Configuration layout:
 *   <app dir>\modules.ini            [host] log = 0|1
 *                                    [host] sibling_all = 0|1 (run the host
 *                                            in sibling exes of the app dir,
 *                                            e.g. scanservice.exe)
 *                                    [host] sibling_modules = a,b (whitelist
 *                                            loaded there)
 *                                    [host] dev_all/dev_modules  (legacy dev
 *                                            aliases, still honored; dev_all=1
 *                                            wins and prefers dev_modules)
 *                                    [<module id>] enabled = 0|1
 *   <app dir>\modules\<id>.ini       free-form `key = value` per module
 *
 * Safety rules: DllMain always returns TRUE; risky host work runs on a
 * worker thread; module failures never propagate to the application.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "MinHook.h"
#include "exstar_sdk.h"

#define HOST_VERSION "1.2.0"
#define HOST_EXE     "EXStar Hub.exe"
#define MODULES_DIR  "modules"
#define MODULES_INI  "modules.ini"
#define LOG_DIR_NAME "ExStarHubMods"
#define LOG_FILE     "mod.log"

#define MAX_MODULES 32

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static HINSTANCE          g_hInst;
static int                g_active;              /* host owns the process */
static int                g_logging;
static int                g_sibling_mode;        /* whitelist gate in siblings */
static char               g_sibling_modules[256] = "marker_probe";
static char               g_logPath[MAX_PATH];
static CRITICAL_SECTION   g_logCs;
static char               g_appDir[MAX_PATH];    /* application directory */
static HGLRC              g_glCtx;               /* worker-thread context */

typedef struct {
    ExHost                host;        /* service table for this module  */
    char                  iniPath[MAX_PATH];
    HMODULE               dll;
    const ExModuleInfo   *info;
    void                (*shutdown)(void);
    int                   started;
} Module;

static Module g_modules[MAX_MODULES];
static int    g_moduleCount;

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void hostLog(const char *fmt, ...)
{
    va_list ap;
    FILE *f;

    if (!g_logging || !g_logPath[0])
        return;
    EnterCriticalSection(&g_logCs);
    f = fopen(g_logPath, "a");
    if (f) {
        fprintf(f, "[%lu] ", (unsigned long) GetTickCount64());
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

/* ------------------------------------------------------------------ */
/* Minimal INI helpers (line based, `key = value`, `[section]`)        */
/* ------------------------------------------------------------------ */

static void trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'
                       || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
}

/*
 * section == NULL matches keys outside any section; otherwise keys only
 * inside the given section. Returns 1 and copies the value when found.
 */
static int iniGet(const char *path, const char *section,
                  const char *key, char *out, unsigned cap)
{
    FILE *f = fopen(path, "rb");
    char line[512];
    int inSection = (section == NULL);

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = NULL, *semi = NULL, *sep, *k, *v, *q;
        int quoted = 0;

        /* `#`/`;` only start a comment OUTSIDE "..." so that values like
         * `color = "#8ecbff"` survive; the earliest marker still cuts. */
        for (q = line; *q; q++) {
            if (*q == '"')
                quoted = !quoted;
            else if (!quoted && *q == '#' && !hash)
                hash = q;
            else if (!quoted && *q == ';' && !semi)
                semi = q;
        }
        if (hash && (!semi || hash < semi))
            *hash = '\0';
        if (semi)
            *semi = '\0';
        trim(line);
        if (!line[0])
            continue;
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close) {
                *close = '\0';
                inSection = (section != NULL)
                            && _stricmp(line + 1, section) == 0;
            }
            continue;
        }
        if (!inSection)
            continue;
        sep = strchr(line, '=');
        if (!sep)
            continue;
        *sep = '\0';
        k = line;
        v = sep + 1;
        trim(k);
        trim(v);
        if (_stricmp(k, key) == 0) {
            size_t vl = strlen(v);

            /* a fully quoted value keeps everything inside verbatim */
            if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') {
                v[vl - 1] = '\0';
                memmove(v, v + 1, vl - 1);   /* shifts the NUL too */
            }
            strncpy(out, v, cap - 1);
            out[cap - 1] = '\0';
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ExHost services                                                     */
/* ------------------------------------------------------------------ */

static int cfgString(const ExHost *self, const char *key, char *buf, unsigned cap)
{
    const Module *m = (const Module *) self->internal;
    if (!m)
        return 0;
    return iniGet(m->iniPath, NULL, key, buf, cap);
}

static int cfgInt(const ExHost *self, const char *key, int dflt)
{
    char buf[64];
    int v;
    if (!cfgString(self, key, buf, sizeof buf))
        return dflt;
    return (sscanf(buf, "%d", &v) == 1) ? v : dflt;
}

/*
 * Resolve an OpenGL entry point. wglGetProcAddress first (modern driver
 * thunks are shared process-wide, which is exactly what we want to hook),
 * opengl32.dll exports second (GL 1.x functions are not exported via
 * wgl). Waits for the application to bring up OpenGL (up to ~5 min).
 */
static void *glThunk(const char *name)
{
    int i;
    if (!g_glCtx)
        return NULL;
    for (i = 0; i < 600; i++) {
        PROC fp = wglGetProcAddress(name);
        if ((size_t) fp > 4)             /* 0..4 mean "not found" */
            return (void *) fp;
        fp = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
        if (fp)
            return (void *) fp;
        Sleep(500);
    }
    hostLog("host: gl_thunk(%s) timed out", name);
    return NULL;
}

static int installHook(void *target, void *detour, void **tramp)
{
    if (!target || !detour)
        return 0;
    if (MH_CreateHook(target, detour, tramp) != MH_OK)
        return 0;
    if (MH_EnableHook(target) != MH_OK)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Private GL context (hidden message window)                          */
/* ------------------------------------------------------------------ */

static int createGlBridge(void)
{
    WNDCLASSA wc;
    HWND window;
    HDC dc;
    PIXELFORMATDESCRIPTOR pfd;
    int pf;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.style         = CS_OWNDC;
    wc.lpszClassName = "ExStarHostGlBridge";
    RegisterClassA(&wc);

    window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, NULL, NULL);
    dc = window ? GetDC(window) : NULL;
    if (!dc) {
        hostLog("host: no device context");
        return 0;
    }
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize      = sizeof pfd;
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        hostLog("host: pixel format rejected");
        return 0;
    }
    g_glCtx = wglCreateContext(dc);
    if (!g_glCtx || !wglMakeCurrent(dc, g_glCtx)) {
        hostLog("host: cannot create GL context");
        g_glCtx = NULL;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Module discovery and startup                                        */
/* ------------------------------------------------------------------ */

static int isModuleEnabled(const char *id)
{
    char path[MAX_PATH], value[32];

    snprintf(path, sizeof path, "%s\\%s", g_appDir, MODULES_INI);
    if (!iniGet(path, id, "enabled", value, sizeof value))
        return 1;                       /* no entry => enabled by default */
    return atoi(value) != 0;
}

static void loadModule(const char *fullPath, const char *fileName)
{
    Module *m;
    ExModuleInfo *(*getInfo)(void);
    char id[64];
    char *dot;

    if (g_moduleCount >= MAX_MODULES) {
        hostLog("host: too many modules, skipping %s", fileName);
        return;
    }

    /* module id = file name without extension */
    strncpy(id, fileName, sizeof id - 1);
    id[sizeof id - 1] = '\0';
    dot = strrchr(id, '.');
    if (dot)
        *dot = '\0';

    /* dev mode: sibling processes load only the whitelisted modules */
    if (g_sibling_mode) {
        const char *p = g_sibling_modules;
        size_t len = strlen(id);
        int hit = 0;
        while (*p) {
            while (*p == ' ' || *p == ',' || *p == ';')
                p++;
            const char *e = p;
            while (*e && *e != ' ' && *e != ',' && *e != ';')
                e++;
            if ((size_t)(e - p) == len && _strnicmp(p, id, len) == 0)
                hit = 1;
            p = e;
        }
        if (!hit) {
            hostLog("host: skipping '%s' (not in sibling whitelist)", id);
            return;
        }
    }

    m = &g_modules[g_moduleCount];
    memset(m, 0, sizeof *m);
    m->dll = LoadLibraryExA(fullPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m->dll) {
        hostLog("host: cannot load %s (err=%lu)", fileName, GetLastError());
        return;
    }

    getInfo = (ExModuleInfo *(*)(void)) (void *)
        GetProcAddress(m->dll, "ex_get_info");
    m->info = getInfo ? getInfo() : NULL;
    if (!m->info || m->info->abi != EX_ABI_VERSION || !m->info->id) {
        hostLog("host: %s has invalid info or unsupported ABI, skipping",
                fileName);
        FreeLibrary(m->dll);
        m->dll = NULL;
        return;
    }
    m->shutdown = (void (*)(void)) (void *)
        GetProcAddress(m->dll, "ex_shutdown");

    if (!isModuleEnabled(m->info->id)) {
        hostLog("host: module '%s' is disabled (modules.ini)", m->info->id);
        FreeLibrary(m->dll);
        m->dll = NULL;
        return;
    }

    snprintf(m->iniPath, sizeof m->iniPath, "%s\\%s\\%s.ini",
             g_appDir, MODULES_DIR, m->info->id);

    /* service table for this module */
    m->host.abi      = EX_ABI_VERSION;
    m->host.size     = (uint32_t) sizeof(ExHost);
    m->host.internal = m;
    m->host.log      = hostLog;
    m->host.cfg_int  = cfgInt;
    m->host.cfg_string = cfgString;
    m->host.gl_thunk = glThunk;
    m->host.hook     = installHook;

    g_moduleCount++;                    /* publish before ex_init */

    {
        int (*init)(const ExHost *) = (int (*)(const ExHost *)) (void *)
            GetProcAddress(m->dll, "ex_init");
        if (!init) {
            hostLog("host: module '%s' has no ex_init, skipping", m->info->id);
            FreeLibrary(m->dll);
            m->dll = NULL;
            return;
        }
        hostLog("host: module '%s' v%s (%s)", m->info->id,
                m->info->version, m->info->description);
        if (init(&m->host) == 0) {
            m->started = 1;
            hostLog("host: module '%s' started", m->info->id);
        } else {
            hostLog("host: module '%s' declined to start", m->info->id);
        }
    }
}

static void scanModules(void)
{
    char pattern[MAX_PATH];
    char dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;

    /* modules live next to the host DLL, not next to the EXE */
    if (!GetModuleFileNameA(g_hInst, dir, MAX_PATH))
        return;
    {
        char *slash = strrchr(dir, '\\');
        if (!slash)
            return;
        *slash = '\0';
    }
    snprintf(pattern, sizeof pattern, "%s\\%s\\*.dll", dir, MODULES_DIR);
    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        hostLog("host: no modules directory or no modules found");
        return;
    }
    do {
        char full[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(full, sizeof full, "%s\\%s\\%s", dir, MODULES_DIR, fd.cFileName);
        loadModule(full, fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static DWORD WINAPI worker(LPVOID unused)
{
    int i, started = 0;

    (void) unused;
    if (MH_Initialize() != MH_OK) {
        hostLog("host: MH_Initialize failed");
        return 1;
    }
    createGlBridge();
    scanModules();
    for (i = 0; i < g_moduleCount; i++)
        started += g_modules[i].started;
    hostLog("host: startup complete, %d module(s) loaded, %d started",
            g_moduleCount, started);
    for (;;)
        Sleep(60000);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void) reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        char exe[MAX_PATH];
        char local[MAX_PATH];
        char iniPath[MAX_PATH];
        char value[32];
        char *base;
        DWORD n;

        g_hInst = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_logCs);

        n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return TRUE;
        base = strrchr(exe, '\\');
        if (!base)
            return TRUE;
        *base = '\0';
        strcpy_s(g_appDir, MAX_PATH, exe);

        /* logging must be resolvable before anything else runs */
        snprintf(iniPath, sizeof iniPath, "%s\\%s", g_appDir, MODULES_INI);
        g_logging = iniGet(iniPath, "host", "log", value, sizeof value)
                    ? (atoi(value) != 0) : 0;
        if (g_logging && GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) {
            strcat_s(local, MAX_PATH, "\\" LOG_DIR_NAME);
            CreateDirectoryA(local, NULL);
            snprintf(g_logPath, sizeof g_logPath, "%s\\" LOG_FILE, local);
        }

        *base = '\\';
        if (_stricmp(base + 1, HOST_EXE) != 0) {
            /* The proxy also lands in sibling exes.  Normally the host stays
             * idle there.  Two opt-in mechanisms activate it:
             *   [host] sibling_all / sibling_modules   production (written by
             *          the mod manager for modules that need e.g. the scan
             *          service process), and
             *   [host] dev_all / dev_modules           legacy developer pair.
             * dev_all=1 always wins and prefers the dev_modules whitelist. */
            char v[32];
            int devOn = iniGet(iniPath, "host", "dev_all", v, sizeof v)
                        && atoi(v) != 0;
            int sibOn = iniGet(iniPath, "host", "sibling_all", v, sizeof v)
                        && atoi(v) != 0;
            if (!devOn && !sibOn) {
                hostLog("host: loaded in foreign process \"%s\" pid=%lu (idle)",
                        base + 1, (unsigned long) GetCurrentProcessId());
                return TRUE;
            }
            if (!(devOn && iniGet(iniPath, "host", "dev_modules",
                                  g_sibling_modules, sizeof g_sibling_modules)) &&
                !iniGet(iniPath, "host", "sibling_modules",
                        g_sibling_modules, sizeof g_sibling_modules))
                strcpy_s(g_sibling_modules, sizeof g_sibling_modules, "marker_probe");
            g_sibling_mode = 1;
            g_active   = 1;
            hostLog("=== ExStarRec host " HOST_VERSION " %s mode in \"%s\" "
                    "pid=%lu (modules: %s) ===", devOn ? "dev" : "sibling",
                    base + 1, (unsigned long) GetCurrentProcessId(),
                    g_sibling_modules);
            if (!CreateThread(NULL, 0, worker, NULL, 0, NULL))
                hostLog("host: CreateThread failed");
            break;
        }

        hostLog("=== ExStarRec host " HOST_VERSION " started, pid=%lu ===",
                (unsigned long) GetCurrentProcessId());
        g_active = 1;
        if (!CreateThread(NULL, 0, worker, NULL, 0, NULL))
            hostLog("host: CreateThread failed");
        break;
    }
    case DLL_PROCESS_DETACH:
        if (g_active) {
            int i;
            for (i = 0; i < g_moduleCount; i++) {
                if (g_modules[i].started && g_modules[i].shutdown)
                    __try { g_modules[i].shutdown(); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { /* ignore */ }
            }
        }
        break;
    default:
        break;
    }
    return TRUE;                        /* never block the process */
}
