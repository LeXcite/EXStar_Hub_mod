/* qmeta_lab.c - what the Qt this product ships calls its value types.
 *
 * selective_mesh reads QVariants by their numeric tag, and a wrong tag means a
 * wrong reading of a real command, so the numbers are not recalled from
 * documentation: QMetaType::typeName() is asked, in this very build, for
 * every id a command has ever carried.  A log line saying `nodes=list?` can
 * then be translated without guessing.
 *
 *   cl /nologo /O2 /MT /utf-8 qmeta_lab.c /link /DEFAULTLIB:user32
 *   qmeta_lab.exe "C:\Program Files\Shining3d\EXStar Hub\Qt5Core.dll"
 */

#include <windows.h>
#include <stdio.h>

typedef int (__cdecl *FnType)(const char *);
typedef const char *(*FnName)(int);
typedef int (__cdecl *FnCtor)(void *, int, const void *);

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\Qt5Core.dll";
    HMODULE q;
    FnType typeOf;
    FnName typeName;
    int i;

    q = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!q) { printf("cannot load %s (err %u)\n", path, (unsigned)GetLastError()); return 1; }

    /* the decorated names are x64: YAPBDH / YAHPBD, not the 32-bit YA */
    typeName = (FnName)(void *)GetProcAddress(q, "?typeName@QMetaType@@YAPEBDH@Z");
    if (!typeName)
        typeName = (FnName)(void *)GetProcAddress(q, "?typeName@QMetaType@@YAPBDH@Z");
    typeOf = (FnType)(void *)GetProcAddress(q, "?type@QMetaType@@YAHPBD@Z");
    if (!typeOf)
        typeOf = (FnType)(void *)GetProcAddress(q, "?type@QMetaType@@YAHH@Z");
    if (!typeName) { printf("QMetaType::typeName not found\n"); return 1; }

    printf("%s\n\n  id  name", path);
    for (i = 0; i <= 40; i++) {
        const char *n = typeName(i);
        printf("\n  %2d  %s", i, n ? n : "(none)");
    }
    printf("\n\n  by name:\n");
    if (typeOf) {
        static const char *want[] = { "bool", "int", "double", "char",
            "QString", "QStringList", "QVariantList", "QVariantMap",
            "QMap<QString,QVariant>", "QUrl", "QDate", "QDateTime", NULL };
        int k;

        for (k = 0; want[k]; k++)
            printf("   %-24s = %d\n", want[k], typeOf(want[k]));
    } else {
        printf("   (QMetaType::type(const char*) not exported)\n");
    }
    return 0;
}
