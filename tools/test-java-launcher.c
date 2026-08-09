/*
 * Host unit test for src/launcher/java.c.
 *
 * The launcher cannot be run here -- it is a PowerPC binary -- but the one thing
 * that can be silently and expensively wrong is testable anywhere: WHICH
 * arguments reach the VM.
 *
 * clib4's build_arg_string() skips argv[0] by convention, because spawnv puts
 * the program name in front itself.  Get that off by one and the launcher's own
 * name is passed to the VM as its first argument -- where the VM reads the first
 * non-option token as the main class, so `java -version` would try to load a
 * class called `java` and every error message would be about the wrong thing.
 * It would also look FINE in the common case, because a stray leading token is
 * invisible whenever a real main class follows it.
 *
 * So: compile java.c for the host with spawnv() replaced by a recorder, and
 * assert on exactly what it was handed.
 *
 *     cc -o test-java-launcher tools/test-java-launcher.c && ./test-java-launcher
 *
 * GPLv2 (java-os4 project).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stand in for clib4's unistd.h before java.c includes it. */
#define P_WAIT 1

#define MAX_RECORDED 32

static char recorded_file[512];
static char recorded[MAX_RECORDED][512];
static const char *recorded_argv[MAX_RECORDED];
static int recorded_argc;
static int spawn_result = 0;

/*
 * The recorder COPIES.  java.c frees its argument array before returning -- as
 * it should -- so holding the pointer and reading it afterwards is a use after
 * free, which is how the first version of this test crashed rather than failed.
 */
static int spawnv(int mode, const char *file, const char **argv) {
    int i;
    (void)mode;

    snprintf(recorded_file, sizeof(recorded_file), "%s", file);
    for(i = 0; argv[i] != NULL && i < MAX_RECORDED - 1; i++) {
        snprintf(recorded[i], sizeof(recorded[i]), "%s", argv[i]);
        recorded_argv[i] = recorded[i];
    }
    recorded_argv[i] = NULL;
    recorded_argc = i;
    return spawn_result;
}

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
    recorded_argc = 0;
    recorded_argv[0] = NULL;
    recorded_file[0] = '\0';
    launcher_main(argc, (char **)argv);
}

int main(void) {
    /*
     * Path resolution first, because a wrong path here does not produce a
     * subtle bug -- it produces "PROGDIR:jamvm-openjdk: Comando sconosciuto"
     * and a runtime that cannot start at all, which is what shipped once.
     *
     * PROGDIR: must never reach spawnv: the command name is resolved before the
     * new program is loaded, so there is no home directory for it to mean.
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
              "PROGDIR: never reaches spawnv (it cannot be resolved there)");
    }

    printf("\njava launcher -- argument passing\n\n");

    /*
     * The bug this program exists to prevent.  '=' must arrive as ONE token:
     * the AmigaDOS script turned `-Dfoo=bar` into `-Dfoo` and `bar`, and the VM
     * then treated `bar` as the class to run.
     */
    {
        char *argv[] = { "java", "-Djava.security.debug=provider",
                         "-cp", "x.jar", "Main", NULL };
        run(argv, 5);

        check(recorded_argc == 5, "-Dk=v: five elements reach spawnv");
        check(strcmp(recorded_argv[0], vm_program) == 0,
              "-Dk=v: element 0 is the VM, not our own argv[0]");
        check(strcmp(recorded_argv[1], "-Djava.security.debug=provider") == 0,
              "-Dk=v: the property arrives whole, '=' intact");
        check(strcmp(recorded_argv[4], "Main") == 0,
              "-Dk=v: the main class is still the last token");
        check(recorded_argv[5] == NULL,
              "-Dk=v: the array is NULL-terminated");
    }

    /*
     * The launcher's own name must NOT be forwarded.  If it were, element 1
     * would be "java" and the VM would run the wrong class.
     */
    {
        char *argv[] = { "java", "-version", NULL };
        run(argv, 2);

        check(recorded_argc == 2, "-version: exactly one argument forwarded");
        check(strcmp(recorded_argv[1], "-version") == 0,
              "-version: argv[0] of the launcher is dropped, not passed on");
    }

    /* No arguments at all: the VM must still be started, so it can print its
       own usage rather than the launcher inventing one. */
    {
        char *argv[] = { "java", NULL };
        run(argv, 1);

        check(recorded_argc == 1, "no args: the VM is still spawned");
        check(strcmp(recorded_file, vm_program) == 0,
              "no args: spawnv is told which program to run");
    }

    /*
     * An argument with a space is passed as one array element and is NOT
     * quoted here -- clib4's build_arg_string does that, and doing it twice
     * would embed literal quotes in the path.
     */
    {
        char *argv[] = { "java", "-cp", "Work:My Dir/app.jar", "Main", NULL };
        run(argv, 4);

        check(strcmp(recorded_argv[2], "Work:My Dir/app.jar") == 0,
              "spaces: passed through untouched, quoting left to clib4");
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
