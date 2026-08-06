/*
 * Host unit test for the Java -> AmigaDOS path conversion.
 *
 * Compiles src/openjdk/amiga_path.h DIRECTLY -- the same header the natives
 * force-include via jdkdefs.h and that JamVM's os.c exports as amigaPath() --
 * so this tests the real shipped logic, with no second copy to drift.
 *
 * This is the single most load-bearing function in the port: every open, stat,
 * mkdir, rename and dlopen goes through it.  Get it wrong and the failure shows
 * up on AmigaOS as an "insert volume" requester or a bogus "no such file".
 *
 *   make -f Makefile.local test-path
 */
#include <stdio.h>
#include <string.h>

#include "../src/openjdk/amiga_path.h"

static int fails = 0;

static void ck(const char *in, const char *want) {
    const char *got = amiga_path(in);

    printf("  %-34s -> %-30s", in, got);
    if (strcmp(got, want) != 0) {
        printf(" FAIL (expected %s)", want);
        fails++;
    }
    printf("\n");
}

int main(void) {
    printf("=== Java path form -> AmigaDOS path form ===\n");

    printf("-- Java's Unix-absolute spelling of a volume path\n");
    ck("/Work:Invoicex/x.jar", "Work:Invoicex/x.jar");
    ck("/Work:", "Work:");
    ck("/RAM:t/f", "RAM:t/f");

    printf("-- already AmigaDOS, or no volume at all: untouched\n");
    ck("Work:Invoicex/x.jar", "Work:Invoicex/x.jar");
    ck("lib/x.jar", "lib/x.jar");
    ck("", "");
    /* no volume in the first component, so the '/' is AmigaDOS's parent */
    ck("/usr/share/x", "/usr/share/x");

    printf("-- '/' straight after a volume is the volume's PARENT, so drop it\n");
    ck("JAVA:/lib/fonts", "JAVA:lib/fonts");
    ck("/Work:/Invoicex", "Work:Invoicex");
    /* "://" is deliberate (parent of the volume root) -- leave it alone */
    ck("JAVA://x", "JAVA://x");

    printf("-- \".\" is not a directory entry on AmigaOS: drop the component\n");
    ck("./foo", "foo");
    ck("a/./b", "a/b");
    ck("a/.", "a");
    ck(".", "");
    ck("/Work:dir/.", "Work:dir");
    ck("/Work:dir/./file", "Work:dir/file");
    ck("./a/./b/./c", "a/b/c");
    ck("Work:./x", "Work:x");
    /* a name that merely starts with a dot is NOT the "." component */
    ck(".java/.userPrefs", ".java/.userPrefs");
    ck("ENV:.java/.userPrefs/it/prefs.xml", "ENV:.java/.userPrefs/it/prefs.xml");
    ck("a/.hidden/b", "a/.hidden/b");
    ck("..foo", "..foo");

    printf("-- \"..\" IS AmigaDOS's '/' parent: drop the name, keep the slashes\n");
    ck("../b", "/b");
    ck("a/../b", "a//b");
    ck("a/..", "a/");
    ck("..", "");

    printf("-- a trailing '/' means the parent on AmigaDOS, so it is dropped\n");
    ck("Work:dir/", "Work:dir");
    ck("/Work:dir/", "Work:dir");
    ck("a/b/", "a/b");
    ck("Work:/", "Work:");
    ck("/", "/");                 /* the parent itself: leave it */
    ck("a//", "a//");             /* explicit parent-of-parent: leave it */

    printf("-- the shapes that actually broke things\n");
    /* new File(".").getAbsolutePath() + "/" -- the "wd:/Work:Invoicex/./" case.
       The trailing separator goes too: "Work:Invoicex/" is the PARENT on Amiga. */
    ck("/Work:Invoicex/./", "Work:Invoicex");
    /* sun.nio.fs joining java.io.tmpdir */
    ck("/T:/tmp1234.tmp", "T:tmp1234.tmp");
    /* boot library path + a native */
    ck("/Work:Java/libzip.so", "Work:Java/libzip.so");

    printf("\n%s (%d failure%s)\n", fails == 0 ? "ALL PASS" : "FAILED",
           fails, fails == 1 ? "" : "s");
    return fails != 0;
}
