/*
 * java -- the launcher, in C, because the AmigaDOS script ate arguments.
 *
 * `java` used to be a DOS script:
 *
 *     .KEY args/F
 *     SetEnv LD_LIBRARY_PATH "PROGDIR:Sobjs"
 *     JAVA:jamvm-openjdk {args}
 *
 * ReadArgs parses that template, and ReadArgs treats '=' as an argument
 * separator.  So `java -Dfoo=bar -cp x.jar Main` reached the VM as `-Dfoo`,
 * `bar`, `-cp`, ... -- the value became a token of its own, and since the first
 * non-option token is the main class, the VM went looking for a class called
 * `bar`:
 *
 *     java -Djava.security.debug=provider -cp x.jar Main
 *         -> ClassNotFoundException: provider
 *
 * That is not a debug-flag inconvenience.  Every tuneable in a JVM is a system
 * property -- jdk.tls.client.cipherSuites, jdk.tls.client.protocols,
 * sun.nio.fs.chdirAllowed (which BUILDING.md tells people to pass, and which
 * had therefore never once arrived) -- so on this port none of them could be
 * set at all.  The workaround was to quote every one of them.
 *
 * A C program has no such template.  Its argv comes from clib4's startup code,
 * which splits the DOS command line on whitespace and quotes; '=' is an
 * ordinary character there, and survives.
 *
 *
 * WHY THIS SPAWNS A CHILD RATHER THAN BEING THE VM
 *
 * The obvious simplification -- make `java` a copy of `jamvm-openjdk` -- does
 * not work, and the reason is the one thing the old script was genuinely for.
 * Every shipped .so carries `-Wl,-rpath=JAVA:Sobjs`, an ABSOLUTE path, so a
 * runtime installed anywhere other than JAVA: cannot find its own clib4 sobjs.
 * LD_LIBRARY_PATH covers that, and the ELF loader reads it when the process
 * starts -- before main() -- so a program cannot set it for itself.  It has to
 * be set by whoever starts the VM.  Hence: set it here, then spawn.
 *
 *
 * WHY spawnv() AND NOT system()
 *
 * system() takes a STRING.  Handing it one means concatenating argv back into a
 * command line and quoting each element for the DOS shell by hand -- rebuilding
 * exactly the layer whose mis-parsing this program exists to fix, only with our
 * own bugs in it.  The first argument containing a space would break it.
 *
 * spawnv() takes argv as an ARRAY.  clib4 does the quoting itself, once, in
 * build_arg_string(), which already handles spaces, tabs, newlines and embedded
 * quotes with AmigaDOS '*' escapes.  Nothing here builds a command line, so
 * nothing here can mis-split one.
 *
 * GPLv2 (java-os4 project).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Same three -D's as src/version/verstag.c, so `Version JAVA:java` and
   `Version JAVA:jamvm-openjdk` cannot disagree about what is installed. */
#ifndef JAVAOS4_VER
#define JAVAOS4_VER "0.0"
#endif
#ifndef JAVAOS4_JAVAVER
#define JAVAOS4_JAVAVER "1.8.0"
#endif
#ifndef JAVAOS4_DATE
#define JAVAOS4_DATE "01.01.2026"
#endif

const char __attribute__((used)) java_launcher_verstag[] =
    "$VER: java " JAVAOS4_VER " (" JAVAOS4_DATE ") OpenJDK " JAVAOS4_JAVAVER;

/* The VM binary, next to us.  PROGDIR: is resolved by DOS against the running
   program's own directory, so this follows the installation wherever it is put
   -- and resolves to the same drawer for the child, which lives beside us. */
#define VM_PROGRAM "PROGDIR:jamvm-openjdk"
#define VM_SOBJS   "PROGDIR:Sobjs"

/*
 * Point the ELF loader at the bundled sobjs.
 *
 * setenv() here is deliberate, in place of the script's `SetEnv`.  AmigaDOS
 * SetEnv writes to ENV:, which is GLOBAL and persistent: the old launcher left
 * LD_LIBRARY_PATH set system-wide after java exited, for every other program on
 * the machine to trip over.  clib4's setenv() touches only this process's own
 * environ table, and DOS copies it to the child (NP_CopyVars), which is exactly
 * the scope wanted -- the child and nothing else.
 *
 * An existing LD_LIBRARY_PATH is kept, ours first: someone who set one meant it,
 * and silently discarding it would be its own bug report.
 */
static void set_library_path(void) {
    const char *existing = getenv("LD_LIBRARY_PATH");

    if(existing == NULL || existing[0] == '\0') {
        setenv("LD_LIBRARY_PATH", VM_SOBJS, 1);
        return;
    }

    /* Already ours at the front?  Leave it alone rather than growing the
       variable on every nested launch. */
    if(strncmp(existing, VM_SOBJS, sizeof(VM_SOBJS) - 1) == 0) {
        return;
    }

    {
        size_t len = sizeof(VM_SOBJS) + strlen(existing) + 1;  /* +1 for ';' */
        char *combined = malloc(len);

        if(combined == NULL) {
            /* Out of memory before the VM has even started: ours alone still
               gives a working runtime, which is better than failing here. */
            setenv("LD_LIBRARY_PATH", VM_SOBJS, 1);
            return;
        }

        /* ';' -- path.separator on AmigaOS, where ':' is the volume separator. */
        snprintf(combined, len, "%s;%s", VM_SOBJS, existing);
        setenv("LD_LIBRARY_PATH", combined, 1);
        free(combined);
    }
}

int main(int argc, char *argv[]) {
    const char **child;
    int status;
    int i;

    /*
     * child[0] is the program name and is NOT passed on: clib4's
     * build_arg_string() skips element 0 by convention (it builds the argument
     * string only, and spawnv prepends `file` itself).  Passing our own argv[0]
     * through would put the launcher's name in front of the VM's arguments,
     * where it would be read as the main class.
     */
    child = malloc((argc + 1) * sizeof(*child));
    if(child == NULL) {
        fprintf(stderr, "java: out of memory building the command line\n");
        return 1;
    }

    child[0] = VM_PROGRAM;
    for(i = 1; i < argc; i++) {
        child[i] = argv[i];
    }
    child[argc] = NULL;

    set_library_path();

    /*
     * P_WAIT, so this process stays alive for as long as the VM does.  That
     * costs one small process, and buys two things worth more: the shell does
     * not return to the prompt while Java is still running, and the exit code
     * below is the VM's own -- scripts testing $RC get the real answer.
     */
    status = spawnv(P_WAIT, VM_PROGRAM, child);

    if(status == -1) {
        /* The common cause by far is a broken installation -- `java` copied
           somewhere without the rest of the runtime beside it -- so say which
           file was not found rather than printing strerror alone. */
        fprintf(stderr, "java: cannot start %s: %s\n", VM_PROGRAM, strerror(errno));
        fprintf(stderr, "java: the runtime must sit in the same drawer as this program\n");
        free(child);
        return 127;   /* what a shell reports for "command not found" */
    }

    free(child);
    return status;
}
