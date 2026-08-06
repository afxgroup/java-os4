/*
 * Host unit test for the AmigaOS classpath rewriter.
 *
 * Includes src/jamvm/amiga_classpath.h DIRECTLY -- the same header the VM builds
 * (vendor/jamvm/src/os/amiga/os.c includes it), so this tests the real shipped
 * logic and there is no second copy to drift.  The two things it cannot have on
 * the host are stubbed: the AmigaDOS device list (amigaIsDosName) and the
 * directory scan (cpScanJars/cpFreeScan, via AMIGA_CP_SCAN_STUB).
 *
 *   make -f Makefile.local test-classpath
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRUE  1
#define FALSE 0

#define CP_MALLOC(n)      malloc(n)
#define CP_REALLOC(p, n)  realloc(p, n)
#define CP_FREE(p)        free(p)
#define CP_TRACE(...)     do { } while(0)

/* --- stubs for the AmigaOS-only environment ------------------------------- */

/* the only mounted volumes/assigns in this test machine */
static int amigaIsDosName(const char *name, int len) {
    static const char *names[] = {"Work", "SYS", "JAVA", "RAM", NULL};
    int i;

    for(i = 0; names[i] != NULL; i++)
        if((int)strlen(names[i]) == len &&
           strncasecmp(names[i], name, len) == 0)
            return TRUE;

    return FALSE;
}

/* the drawers that contain jars */
static int cpScanJars(const char *dir, char ***names) {
    static const char *lib[]  = {"a.jar", "b.jar"};
    static const char *top[]  = {"top.jar"};
    static const char *root[] = {"root.jar"};
    const char **src;
    int n, i;

    if(strcmp(dir, "Work:Invoicex/lib") == 0)   { src = lib;  n = 2; }
    else if(strcmp(dir, "Work:Invoicex") == 0)  { src = top;  n = 1; }
    else if(strcmp(dir, "Work:") == 0)          { src = root; n = 1; }
    else return -1;

    *names = CP_MALLOC(sizeof(char*) * n);
    for(i = 0; i < n; i++)
        (*names)[i] = strcpy(CP_MALLOC(strlen(src[i]) + 1), src[i]);

    return n;
}

static void cpFreeScan(char **names, int n) {
    int i;

    for(i = 0; i < n; i++)
        CP_FREE(names[i]);

    CP_FREE(names);
}

/* os.c's path normaliser, which the header calls -- copied here rather than
   pulled in with the rest of os.c, which needs the AmigaOS SDK.  Keep in step
   with amigaPath() in vendor/jamvm/src/os/amiga/os.c. */
static const char *amigaPath(const char *path) {
    static char bufs[4][1024];
    static int next = 0;
    char *out;
    int i = 0;

    if(path == NULL)
        return NULL;

    if(path[0] == '.' && path[1] == '/')
        path += 2;
    else if(path[0] == '/') {
        const char *c = path + 1;

        while(*c != '\0' && *c != '/') {
            if(*c == ':') { path++; break; }
            c++;
        }
    }

    out = bufs[next];
    next = (next + 1) % 4;

    while(*path != '\0' && i < 1023) {
        if(path[0] == ':' && path[1] == '/' && path[2] != '/') {
            out[i++] = ':';
            path += 2;
        } else
            out[i++] = *path++;
    }
    out[i] = '\0';

    return out;
}

static char *amiga_launch_cwd = "Work:Invoicex";

#define AMIGA_CP_SCAN_STUB 1
#include "../src/jamvm/amiga_classpath.h"

/* --- the tests ------------------------------------------------------------ */

static int fails = 0;

/* Split `in` with amigaIsPathListSep exactly as parseBootClassPath() and
   amigaBootClassPathProperty() do, and render the entries '|'-separated.  This
   guards the boot classpath, which is now java.home-absolute and ';'-joined --
   get the splitting wrong and the VM finds no rt.jar and does not start. */
static void ckSplit(const char *in, const char *want) {
    char got[1024];
    int n = strlen(in);
    int start = 0, seen_vol = FALSE, len = 0, i;

    for(i = 0; i <= n; i++) {
        if(i != n && !amigaIsPathListSep(in, start, i, &seen_vol))
            continue;

        if(i != start) {
            if(len != 0)
                got[len++] = '|';
            memcpy(got + len, &in[start], i - start);
            len += i - start;
        }

        start = i + 1;
        seen_vol = FALSE;
    }
    got[len] = '\0';

    printf("  %-44s -> %s\n", in, got);
    if(strcmp(got, want) != 0) {
        printf("      FAIL: expected %s\n", want);
        fails++;
    }
}

static void ck(const char *in, const char *want) {
    char *got = amigaClassPath(in);

    printf("  %-44s -> %s\n", in, got);
    if(strcmp(got, want) != 0) {
        printf("      FAIL: expected %s\n", want);
        fails++;
    }

    free(got);
}

int main(void) {
    printf("=== AmigaOS classpath rewriter (launch dir = Work:Invoicex) ===\n");

    printf("-- the reported bug: ':' must separate, not name a volume\n");
    ck("Invoicex.jar:lib/*:lib_plugins/*:plugins/*",
       "/Work:Invoicex/Invoicex.jar;/Work:Invoicex/lib/a.jar;"
       "/Work:Invoicex/lib/b.jar");
    ck("Invoicex.jar:lib", "/Work:Invoicex/Invoicex.jar;/Work:Invoicex/lib");

    printf("-- ';' still works, and mixes with ':'\n");
    ck("a.jar;b.jar", "/Work:Invoicex/a.jar;/Work:Invoicex/b.jar");
    ck("a.jar;b.jar:c.jar",
       "/Work:Invoicex/a.jar;/Work:Invoicex/b.jar;/Work:Invoicex/c.jar");

    printf("-- a real volume/assign is NOT a separator\n");
    ck("Work:lib/x.jar", "/Work:lib/x.jar");
    ck("Work:lib/x.jar:SYS:y.jar", "/Work:lib/x.jar;/SYS:y.jar");
    ck("JAVA:rt.jar;Work:app.jar", "/JAVA:rt.jar;/Work:app.jar");

    printf("-- Java's Unix-absolute spelling survives a round trip\n");
    ck("/Work:lib/x.jar", "/Work:lib/x.jar");
    ck("/Work:lib/x.jar:/SYS:y.jar", "/Work:lib/x.jar;/SYS:y.jar");

    printf("-- wildcards (AmigaOS shells do not glob)\n");
    ck("*", "/Work:Invoicex/top.jar");
    ck("Work:*", "/Work:root.jar");
    ck("lib/*", "/Work:Invoicex/lib/a.jar;/Work:Invoicex/lib/b.jar");
    ck("nosuchdir/*", "");
    ck("foo*.jar", "/Work:Invoicex/foo*.jar");    /* not a wildcard entry */

    printf("-- relative entries anchor at the launch dir; empty entries vanish\n");
    ck("./a.jar", "/Work:Invoicex/a.jar");
    ck("a.jar::b.jar", "/Work:Invoicex/a.jar;/Work:Invoicex/b.jar");
    ck(":x.jar", "/Work:Invoicex/x.jar");
    ck("", "");

    printf("-- no launch dir known: an entry with no volume stays relative,\n"
           "   so java.io resolves it against user.dir instead of getting a\n"
           "   leading '/' AmigaDOS would read as the parent directory\n");
    amiga_launch_cwd = NULL;
    ck("lib/x.jar", "lib/x.jar");
    ck("Work:x.jar", "/Work:x.jar");

    printf("-- boot classpath: java.home-absolute, ';'-joined (parseBootClassPath)\n");
    ckSplit("Work:Java/niopatch.zip;Work:Java/rt.jar;Work:Java/classes",
            "Work:Java/niopatch.zip|Work:Java/rt.jar|Work:Java/classes");
    /* java.home at a volume root: no '/' after the ':' */
    ckSplit("Work:niopatch.zip;Work:rt.jar", "Work:niopatch.zip|Work:rt.jar");
    /* -Xbootclasspath/a: appends with ':' -- still has to split right */
    ckSplit("Work:Java/rt.jar:Work:extra.jar", "Work:Java/rt.jar|Work:extra.jar");
    /* and the Java-facing spelling round-trips too */
    ckSplit("/Work:Java/rt.jar;/Work:Java/classes",
            "/Work:Java/rt.jar|/Work:Java/classes");

    printf("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILED",
           fails, fails == 1 ? "" : "s");
    return fails != 0;
}
