/*
 * amiga_path.h -- Java path form -> AmigaDOS path form.
 *
 * SINGLE SOURCE OF TRUTH for the conversion every file operation in this port
 * goes through.
 *   - tools/build-openjdk-natives.sh copies this into the natives' compat dir
 *     and force-includes it via jdkdefs.h, so libjava/libzip/libnio all use it
 *     (as amiga_path).
 *   - vendor/jamvm/src/os/amiga/os.c includes it and exports it as amigaPath()
 *     for JVM_Open, the native library loader and the VM's own file access.
 *   - tools/test-amiga-path.c compiles it directly and tests it on the host.
 * It used to be two hand-kept copies.  Keep the logic here only.
 *
 * THE MODEL ------------------------------------------------------------------
 * java.io hands paths around in UNIX-ABSOLUTE form ("/Volume:dir/file"): both
 * java.io.File and sun.nio.fs treat a path as absolute only if it starts with
 * '/', and that leading slash is what stops getAbsolutePath()/toURI() from
 * re-resolving an Amiga "Volume:dir" against user.dir.  AmigaDOS wants the raw
 * "Volume:dir/file" -- hand it "/Work:foo" and DOS looks for a volume literally
 * called "/Work" ("Please insert volume /Work:").
 *
 * AmigaDOS also differs from Unix in ways Java knows nothing about:
 *   - '/' is the PARENT directory, not the root.  "/x" is x in the parent.
 *   - "." and ".." are not directory entries.  They do not exist at all, so a
 *     path containing them simply fails -- java.io.File.normalize() does NOT
 *     remove them (only getCanonicalPath does), so "new File(\".\")
 *     .getAbsolutePath()" yields "/Work:dir/." and every use of it failed.
 *
 * THE CONVERSION -------------------------------------------------------------
 *   "/Work:foo/bar"  ->  "Work:foo/bar"   leading '/' introducing a volume
 *   "./foo"          ->  "foo"            a "." component is dropped...
 *   "a/./b"          ->  "a/b"            ...wherever it appears...
 *   "a/."            ->  "a"              ...including at the end
 *   "."              ->  ""               empty == the current directory to DOS
 *   "../b"           ->  "/b"             ".." IS the AmigaDOS '/' parent
 *   "a/../b"         ->  "a//b"           a, up, b -- the AmigaDOS spelling
 *   "a/"             ->  "a"              trailing '/' is the PARENT on Amiga
 *   "JAVA:/lib/x"    ->  "JAVA:lib/x"     '/' after ':' is the volume's PARENT
 *   "/usr/share/x"   ->  unchanged        no volume: a real relative DOS path
 *   "Work:foo"       ->  unchanged        already AmigaDOS
 *
 * Everything else is copied through untouched.  The result lives in one of a
 * small ring of per-thread buffers, so a caller can hold several translations
 * live at once -- rename(amiga_path(a), amiga_path(b)) must not have the second
 * call clobber the first.
 */

#ifndef AMIGA_PATH_H
#define AMIGA_PATH_H

#include <string.h>

#define AMIGA_PATH_BUFFERS 4
#define AMIGA_PATH_MAX     1024

static const char *amiga_path(const char *p) {
    static __thread char bufs[AMIGA_PATH_BUFFERS][AMIGA_PATH_MAX];
    static __thread int next = 0;
    const char *src = p;
    const char *end;
    char *buf;
    int at_component = 1;
    int i = 0;

    if (p == NULL)
        return NULL;

    /* A leading '/' is Java's absolute marker only when a volume follows it. */
    if (src[0] == '/') {
        const char *c = src + 1;

        while (*c != '\0' && *c != '/') {
            if (*c == ':') {
                src++;
                break;
            }
            c++;
        }
    }

    end = src + strlen(src);

    /* Unix's trailing '/' means "the same directory"; AmigaDOS's means the
       PARENT, so Java's "a/" would address the wrong place.  Drop it HERE,
       before "." and ".." are translated -- do it on the way out instead and
       "a/../" (which is the current directory) loses the slash that ".." left
       behind and becomes "a", the directory we were supposed to climb out of.
       "a//" is an explicit parent-of-parent and "Vol:/" is handled below. */
    if (end - src > 1 && end[-1] == '/' && end[-2] != '/' && end[-2] != ':')
        end--;

    buf = bufs[next];
    next = (next + 1) % AMIGA_PATH_BUFFERS;

    while (src < end && i < AMIGA_PATH_MAX - 1) {
        int rem = (int)(end - src);

        if (at_component && src[0] == '.' && (rem == 1 || src[1] == '/')) {
            /* a "." component: drop it and the separator that follows; if it
               ended the path, drop the separator that preceded it instead */
            src += (rem > 1) ? 2 : 1;
            if (src >= end && i > 0 && buf[i - 1] == '/')
                i--;
            continue;
        }

        if (at_component && rem >= 2 && src[0] == '.' && src[1] == '.' &&
            (rem == 2 || src[2] == '/')) {
            /* ".." IS AmigaDOS's '/': drop the name, keep BOTH separators, so
               "a/../b" becomes "a//b" -- a, up one, then b */
            src += 2;
            continue;
        }

        if (src[0] == ':' && rem > 1 && src[1] == '/' &&
            !(rem > 2 && src[2] == '/')) {
            /* "Volume:/x": the '/' right after ':' is the volume's parent */
            buf[i++] = ':';
            src += 2;
            at_component = 1;
            continue;
        }

        at_component = (*src == '/' || *src == ':');
        buf[i++] = *src++;
    }

    buf[i] = '\0';
    return buf;
}

#endif /* AMIGA_PATH_H */
