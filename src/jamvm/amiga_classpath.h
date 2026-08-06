/*
 * amiga_classpath.h -- AmigaOS classpath rewriting for JamVM.
 *
 * SINGLE SOURCE OF TRUTH for the "java -cp a.jar:lib/*" fix.
 *   - vendor/jamvm/src/os/amiga/os.c includes this (the jamvm build scripts add
 *     -I src/jamvm), and class.c calls amigaClassPath()/amigaIsPathListSep().
 *   - tools/test-amiga-classpath.c includes it directly and tests it on the
 *     host, with the DOS lookup and the directory scan stubbed out.
 * Keep the logic here only; never duplicate it.
 *
 * It lives in this repo rather than in the vendored JamVM tree on purpose:
 * vendor/jamvm is gitignored and reaches other clones only through
 * docs/jamvm-amiga-openjdk.patch, so a NEW file there would be missing wherever
 * that patch does not apply -- while the build scripts that reference it are
 * tracked and always present.  A header pulled in by an already-patched os.c
 * cannot get out of step that way.
 *
 * THE BUG -------------------------------------------------------------------
 * On AmigaOS ':' is the VOLUME separator ("Work:lib/x.jar"), so it cannot also
 * be Java's path-list separator; path.separator is ';' here for that reason.
 * But everyone still types the Unix form, and
 *     java -cp Invoicex.jar:lib/*:plugins/* it.tnx.invoicex.Invoicex
 * was therefore read as ONE entry -- whose leading "Invoicex.jar:" AmigaDOS
 * treats as a volume, popping "Please insert volume Invoicex.jar:".  The
 * wildcards were a second, silent failure: AmigaOS shells do not glob and JamVM
 * had no wildcard support, so every jar in lib/ was simply absent.
 *
 * THE FIX -------------------------------------------------------------------
 * Accept BOTH separators and decide per ':' whether it is a volume separator
 * (amigaIsPathListSep -- it asks AmigaDOS whether the name is a real volume,
 * assign or device, which never pops a requester), then expand wildcards the
 * way the Sun launcher does elsewhere.  Entries come out ';'-separated in the
 * Unix-absolute spelling ("/Work:Invoicex/x.jar") that java.io.File and
 * sun.nio.fs need in order to treat a path as absolute; amigaPath() strips the
 * leading '/' again at the native chokepoints.
 *
 * The includer must provide: TRUE/FALSE, <string.h>, amigaPath(),
 * amigaIsDosName(), amiga_launch_cwd, and the CP_* macros below.  Define
 * AMIGA_CP_SCAN_STUB to supply your own cpScanJars()/cpFreeScan() instead of
 * the scandir-based pair here (the host test does).
 */

#ifndef AMIGA_CLASSPATH_H
#define AMIGA_CLASSPATH_H

#include <string.h>
#include <stdlib.h>

#ifndef CP_MALLOC
#define CP_MALLOC(n)      sysMalloc(n)
#define CP_REALLOC(p, n)  sysRealloc(p, n)
#define CP_FREE(p)        sysFree(p)
#endif

#ifndef CP_TRACE
#define CP_TRACE(...)     do { } while(0)
#endif

/* On AmigaOS ':' is BOTH the volume separator ("Work:lib/x.jar") and the
   separator everyone actually types in a path list ("x.jar:lib/y.jar").  We
   accept both and resolve each ':' on its own merits.  It is a VOLUME separator
   only when it can be one: it sits in the first component of the entry (a volume
   name cannot contain '/'), the entry has no volume yet, and AmigaDOS actually
   knows the name -- amigaIsDosName() asks the DOS list, which is exact and,
   unlike stat()ing the name, never pops a requester.  Every other ':' ends the
   entry, including one with nothing before it: "a.jar::b.jar" is an empty entry,
   as it is everywhere else in Java (the AmigaDOS ":file" spelling for the
   current volume's root is given up in exchange -- write it "Volume:file").

     "Invoicex.jar:lib"      ->  "Invoicex.jar" + "lib"   (there is no such volume)
     "Work:lib:Work:x.jar"   ->  "Work:lib"     + "Work:x.jar"
     "/Work:lib"             ->  one entry (Java's Unix-absolute spelling)

   `start`/`i` are offsets into `list`; *seen_vol is carried across the entry and
   reset by the caller after each separator. */
int amigaIsPathListSep(const char *list, int start, int i, int *seen_vol) {
    int ns, k;

    if(list[i] == ';')
        return TRUE;

    if(list[i] != ':')
        return FALSE;

    if(*seen_vol)                     /* at most one volume ':' per entry */
        return TRUE;

    ns = start;
    if(list[ns] == '/')               /* Java's Unix-absolute "/Volume:..." */
        ns++;

    for(k = ns; k < i; k++)           /* a volume name contains no '/' */
        if(list[k] == '/')
            return TRUE;

    if(i != ns && amigaIsDosName(&list[ns], i - ns)) {
        *seen_vol = TRUE;
        return FALSE;
    }

    return TRUE;
}

/* A growable string, used to assemble the rewritten classpath. */
typedef struct {
    char *buff;
    int len;
    int cap;
} CpBuff;

static void cpAppend(CpBuff *b, const char *str, int len) {
    if(b->len + len + 1 > b->cap) {
        do {
            b->cap = b->cap != 0 ? b->cap * 2 : 256;
        } while(b->len + len + 1 > b->cap);

        b->buff = CP_REALLOC(b->buff, b->cap);
    }

    memcpy(b->buff + b->len, str, len);
    b->len += len;
    b->buff[b->len] = '\0';
}

/* Append one finished entry, in the Unix-absolute form Java wants: `dos` is an
   AmigaDOS path ("Work:Invoicex/lib/x.jar") and we prefix the '/' that makes
   java.io.File/sun.nio.fs treat it as absolute.  amigaPath() strips it back off
   at the native chokepoints.  Only a "Volume:" path gets the marker -- on an
   entry with no volume the '/' would be a real AmigaDOS component (the parent
   directory), so such an entry is left relative for java.io to resolve against
   user.dir. */
static void cpEmit(CpBuff *out, const char *dos) {
    int k;

    if(out->len != 0)
        cpAppend(out, ";", 1);

    for(k = 0; dos[k] != '\0' && dos[k] != '/'; k++)
        if(dos[k] == ':') {
            cpAppend(out, "/", 1);
            break;
        }

    cpAppend(out, dos, strlen(dos));
}

/* Turn one raw entry into an absolute AmigaDOS path.  An entry that already
   names a volume/assign is taken as-is (after amigaPath() has undone Java's
   "/Volume:" spelling); a relative one is anchored at the directory `java` was
   launched from.  That anchor matters because the VM chdir's into its own
   install drawer at startup, so a plain relative entry would otherwise resolve
   against the runtime's drawer instead of the user's.  Returns a CP_MALLOC'd
   string. */
static char *cpAbsolute(const char *entry, int len) {
    char *raw = CP_MALLOC(len + 1);
    const char *dos;
    char *abs;
    int dlen, k, hasvol = FALSE;

    memcpy(raw, entry, len);
    raw[len] = '\0';

    dos = amigaPath(raw);
    dlen = strlen(dos);

    for(k = 0; k < dlen && dos[k] != '/'; k++)
        if(dos[k] == ':') {
            hasvol = TRUE;
            break;
        }

    if(hasvol || amiga_launch_cwd == NULL) {
        abs = strcpy(CP_MALLOC(dlen + 1), dos);
    } else {
        int llen = strlen(amiga_launch_cwd);

        abs = CP_MALLOC(llen + dlen + 2);
        memcpy(abs, amiga_launch_cwd, llen);
        /* "Work:" IS the directory -- a '/' after it means its PARENT */
        if(llen != 0 && amiga_launch_cwd[llen - 1] != ':')
            abs[llen++] = '/';
        strcpy(abs + llen, dos);
    }

    CP_FREE(raw);
    return abs;
}

#ifndef AMIGA_CP_SCAN_STUB
#include <dirent.h>
#include <strings.h>                  /* strcasecmp */

/* .jar/.zip, case-insensitively -- the same set JamVM's own boot-path scanner
   accepts, which is a superset of the .jar/.JAR the Sun launcher globs. */
static int cpJarFilter(const struct dirent *entry) {
    int len = strlen(entry->d_name);

    return len >= 4 && (strcasecmp(entry->d_name + len - 4, ".zip") == 0 ||
                        strcasecmp(entry->d_name + len - 4, ".jar") == 0);
}

/* Sorted list of the jars in `dir`; -1 if it cannot be read.  Names are
   released by the caller via cpFreeScan(). */
static int cpScanJars(const char *dir, char ***names) {
    struct dirent **namelist;
    int n, i;

    if((n = scandir(dir, &namelist, &cpJarFilter, &alphasort)) < 0)
        return -1;

    *names = CP_MALLOC(sizeof(char*) * (n != 0 ? n : 1));

    for(i = 0; i < n; i++) {
        (*names)[i] = strcpy(CP_MALLOC(strlen(namelist[i]->d_name) + 1),
                             namelist[i]->d_name);
        free(namelist[i]);
    }

    free(namelist);
    return n;
}

static void cpFreeScan(char **names, int n) {
    int i;

    for(i = 0; i < n; i++)
        CP_FREE(names[i]);

    CP_FREE(names);
}
#endif

/* Expand a wildcard entry -- a lone "*", or one ending in a slash (or a volume
   ':') then "*" -- into the jars it matches: one level, no recursion, exactly as
   the Sun launcher's classpath wildcards work elsewhere (we sort, so runs are
   reproducible).  AmigaOS shells do not glob, so without this the entry reaches
   Java verbatim and every jar in the drawer is silently missing.  A wildcard
   that matches nothing expands to nothing.  Returns TRUE if `dos` was one. */
static int cpExpandWildcard(CpBuff *out, char *dos) {
    char **names;
    int len = strlen(dos);
    int dirlen, n, i;

    if(len == 0 || dos[len - 1] != '*')
        return FALSE;

    dirlen = len - 1;
    if(dirlen > 0 && dos[dirlen - 1] == '/')
        dirlen--;
    else if(dirlen > 0 && dos[dirlen - 1] != ':')
        return FALSE;                 /* "foo*" is a name, not a wildcard */

    if(dirlen == 0)
        return FALSE;

    dos[dirlen] = '\0';

    if((n = cpScanJars(dos, &names)) < 0) {
        CP_TRACE("[CP] wildcard '%s' -- cannot read directory\n", dos);
        return TRUE;
    }

    for(i = 0; i < n; i++) {
        CpBuff entry = {NULL, 0, 0};

        cpAppend(&entry, dos, dirlen);
        if(dos[dirlen - 1] != ':')
            cpAppend(&entry, "/", 1);
        cpAppend(&entry, names[i], strlen(names[i]));

        cpEmit(out, entry.buff);
        CP_FREE(entry.buff);
    }

    cpFreeScan(names, n);
    return TRUE;
}

/* Rewrite a path list into the form the Java side expects: ';'-separated (that
   is path.separator on AmigaOS), absolute, Unix-spelled, wildcards expanded. */
char *amigaClassPath(const char *cp) {
    CpBuff out = {NULL, 0, 0};
    int n = strlen(cp);
    int start = 0, seen_vol = FALSE, i;

    for(i = 0; i <= n; i++) {
        if(i != n && !amigaIsPathListSep(cp, start, i, &seen_vol))
            continue;

        if(i != start) {
            char *dos = cpAbsolute(&cp[start], i - start);

            if(!cpExpandWildcard(&out, dos))
                cpEmit(&out, dos);

            CP_FREE(dos);
        }

        start = i + 1;
        seen_vol = FALSE;
    }

    CP_TRACE("[CP] '%s' -> '%s'\n", cp, out.buff != NULL ? out.buff : "");

    return out.buff != NULL ? out.buff : strcpy(CP_MALLOC(1), "");
}

#endif /* AMIGA_CLASSPATH_H */
