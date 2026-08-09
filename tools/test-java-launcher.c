/*
 * Host unit test for src/launcher/java.c.
 *
 * The launcher cannot be run here -- it is a PowerPC binary -- but the thing
 * that can be silently and expensively wrong is testable anywhere: the COMMAND
 * LINE it hands to system().
 *
 * That line is built by us now rather than by clib4, because system() creates a
 * process that is not a DOS child of ours and does not signal our task when it
 * dies -- which spawnv's child does, and which crashed an auto-updater that
 * outlived the application that started it.  Taking on the string means taking
 * on the quoting, so the quoting is what this tests: AmigaDOS rules, where the
 * escape is '*' and not '\', and where getting it wrong turns one argument
 * into two or swallows the next.
 *
 *     cc -o test-java-launcher tools/test-java-launcher.c && ./test-java-launcher
 *
 * GPLv2 (java-os4 project).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char recorded[4096];
static int  recorded_calls;

/* Stands in for clib4's system(): records the command instead of running it. */
static int system_stub(const char *command) {
    snprintf(recorded, sizeof(recorded), "%s", command);
    recorded_calls++;
    return 0;
}
#define system system_stub

/* java.c's main becomes launcher_main so this file can keep its own. */
#define main launcher_main
#include "../src/launcher/java.c"
#undef main

static int failures;

static void check(int cond, const char *what) {
    printf("  %-58s %s\n", what, cond ? "ok" : "FAILED");
    if(!cond) {
        failures++;
    }
}

static void run(char *const *argv, int argc) {
    unsetenv("LD_LIBRARY_PATH");
    recorded[0] = '\0';
    launcher_main(argc, (char **)argv);
}

int main(void) {
    /*
     * Path resolution first, because a wrong path here does not produce a
     * subtle bug -- it produces "PROGDIR:jamvm-openjdk: Comando sconosciuto"
     * and a runtime that cannot start at all, which is what shipped once.
     *
     * PROGDIR: must never reach the command line: the command name is resolved
     * before the new program is loaded, so there is no home directory for it to
     * mean.
     */
    printf("java launcher -- path resolution\n\n");
    {
        char buf[128];

        join_path(buf, sizeof(buf), "Work:Java", "jamvm-openjdk");
        check(strcmp(buf, "Work:Java/jamvm-openjdk") == 0,
              "join: a drawer gets a '/' before the leaf");

        join_path(buf, sizeof(buf), "Work:", "jamvm-openjdk");
        check(strcmp(buf, "Work:jamvm-openjdk") == 0,
              "join: a volume does NOT ('Work:/x' is the parent of Work:)");

        join_path(buf, sizeof(buf), "Work:Java/", "Sobjs");
        check(strcmp(buf, "Work:Java/Sobjs") == 0,
              "join: an existing trailing '/' is not doubled");

        /* Dropping a component, which is how bin/java finds the VM one level
           up.  Must terminate at a volume root rather than loop. */
        {
            char d[128];

            snprintf(d, sizeof(d), "Work:Java/bin");
            check(parent_dir(d) && strcmp(d, "Work:Java") == 0,
                  "parent: Work:Java/bin -> Work:Java");

            snprintf(d, sizeof(d), "Work:Java");
            check(parent_dir(d) && strcmp(d, "Work:") == 0,
                  "parent: Work:Java -> Work: (the volume)");

            snprintf(d, sizeof(d), "Work:");
            check(parent_dir(d) == 0,
                  "parent: Work: has none, and says so instead of looping");
        }

        /* The host cannot ask DOS, so this exercises the fallback. */
        resolve_paths();
        check(strcmp(vm_program, "JAVA:jamvm-openjdk") == 0,
              "fallback: JAVA:jamvm-openjdk when the program dir is unknown");
        check(strcmp(vm_sobjs, "JAVA:Sobjs") == 0,
              "fallback: JAVA:Sobjs likewise");
        check(strstr(vm_program, "PROGDIR:") == NULL,
              "PROGDIR: never reaches the command line");
    }

    printf("\njava launcher -- the command line\n\n");

    /*
     * The bug this program exists to prevent.  '=' must survive as part of one
     * argument: the AmigaDOS script turned `-Dfoo=bar` into `-Dfoo` and `bar`,
     * and the VM read the value as the class to run.
     */
    {
        char *argv[] = { "java", "-Djava.security.debug=provider",
                         "-cp", "x.jar", "Main", NULL };
        run(argv, 5);

        check(strstr(recorded, "-Djava.security.debug=provider") != NULL,
              "-Dk=v: the property arrives whole, '=' intact");
        check(strstr(recorded, "\"-Djava") == NULL,
              "-Dk=v: and is NOT quoted -- it needs none");
        check(strncmp(recorded, vm_program, strlen(vm_program)) == 0,
              "the VM is the first word of the command");
        check(strstr(recorded, "Main") != NULL,
              "the main class is still there");
    }

    /* The launcher's own argv[0] must not be forwarded: it would become the
       VM's first argument, where it reads as the main class. */
    {
        char *argv[] = { "java", "-version", NULL };
        char expect[600];

        run(argv, 2);
        snprintf(expect, sizeof(expect), "%s -version", vm_program);
        check(strcmp(recorded, expect) == 0,
              "-version: exactly '<vm> -version', our own name dropped");
    }

    {
        char *argv[] = { "java", NULL };
        run(argv, 1);
        check(strcmp(recorded, vm_program) == 0,
              "no args: the VM alone, and it IS still started");
    }

    printf("\nquoting (AmigaDOS rules)\n\n");

    /* A space is the whole reason quoting exists: unquoted, this classpath
       would become two arguments and the main class would be eaten. */
    {
        char *argv[] = { "java", "-cp", "Work:My Dir/app.jar", "Main", NULL };
        run(argv, 4);
        check(strstr(recorded, "\"Work:My Dir/app.jar\"") != NULL,
              "an argument containing a space is quoted");
        check(strstr(recorded, "Main") != NULL,
              "and the argument after it survives");
    }

    /* AmigaDOS escapes with '*', so a literal quote is *" -- a backslash here
       would be passed through as an ordinary character and the quote would
       close the argument early. */
    {
        char *argv[] = { "java", "-Dmsg=say \"hi\"", NULL };
        run(argv, 2);
        check(strstr(recorded, "*\"hi*\"") != NULL,
              "an embedded quote is escaped with '*', not backslash");
    }

    /* A literal asterisk must be doubled, or it would be read as an escape and
       eat the character after it. */
    {
        char *argv[] = { "java", "-cp", "lib/* extra", NULL };
        run(argv, 3);
        check(strstr(recorded, "lib/**") != NULL,
              "a literal '*' is doubled inside a quoted argument");
    }

    /* An empty argument has to survive as an argument. */
    {
        char *argv[] = { "java", "-cp", "", "Main", NULL };
        run(argv, 4);
        check(strstr(recorded, "\"\"") != NULL,
              "an empty argument is quoted so it is not lost");
    }

    /* Nothing that needs no quoting should get any: a quoted classpath is still
       correct, but noise in every command line makes real problems harder to
       see. */
    {
        char *argv[] = { "java", "-cp", "app.jar", "Main", NULL };
        run(argv, 4);
        check(strchr(recorded, '"') == NULL,
              "ordinary arguments are left alone");
    }

    printf("\nLD_LIBRARY_PATH\n\n");

    {
        char *argv[] = { "java", "-version", NULL };
        const char *v;

        unsetenv("LD_LIBRARY_PATH");
        launcher_main(2, argv);
        v = getenv("LD_LIBRARY_PATH");
        check(v != NULL && strcmp(v, vm_sobjs) == 0,
              "unset: set to the bundled sobjs");

        /* Someone else's setting is kept, ours first. */
        setenv("LD_LIBRARY_PATH", "SDK:local/lib", 1);
        launcher_main(2, argv);
        v = getenv("LD_LIBRARY_PATH");
        check(v != NULL && strncmp(v, vm_sobjs, strlen(vm_sobjs)) == 0,
              "already set: ours goes first");
        check(v != NULL && strstr(v, "SDK:local/lib") != NULL,
              "already set: theirs is kept, not discarded");
        check(v != NULL && strchr(v, ';') != NULL,
              "already set: joined with ';', not ':' (a volume separator)");

        /* Nested launch must not grow the variable without bound. */
        launcher_main(2, argv);
        launcher_main(2, argv);
        v = getenv("LD_LIBRARY_PATH");
        {
            int n = 0;
            const char *p = v;
            while((p = strstr(p, vm_sobjs)) != NULL) {
                n++;
                p++;
            }
            check(n == 1, "nested launch: the sobjs path is not appended twice");
        }
    }

    printf("\n%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
