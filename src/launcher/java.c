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

#ifdef __amigaos4__
#include <proto/dos.h>
#endif

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

/*
 * Where the runtime is, if we cannot work it out.
 *
 * "PROGDIR:jamvm-openjdk" cannot be used as the program to spawn, which is not
 * obvious and cost a release to find:
 *
 *     PROGDIR:jamvm-openjdk: Comando sconosciuto
 *
 * PROGDIR: is per-process, resolved from the running program's home directory.
 * The command name has to be resolved BEFORE the new program is loaded, in a
 * context where no such home directory exists yet, so DOS cannot expand it and
 * reports the whole string as an unknown command.  (It works fine inside
 * LD_LIBRARY_PATH, which the child's own loader expands once the child is
 * running -- that is why the old script got away with it.)
 *
 * So PROGDIR: is resolved HERE, where this program is running and it does mean
 * something, and spawnv is handed an absolute path.  JAVA: is the fallback, and
 * is what the installer uses.
 */
#define FALLBACK_HOME "JAVA:"
#define VM_BINARY     "jamvm-openjdk"
#define SOBJS_DIR     "Sobjs"

static char vm_program[512];
static char vm_sobjs[512];

/*
 * AddPart()'s rule, written out so the host test exercises the same code that
 * ships.  NameFromLock returns either a volume ("Work:") or a path within one
 * ("Work:Java"); a separator belongs between the two only in the second case,
 * since "Work:/jamvm-openjdk" means the parent of Work: on AmigaOS.
 */
static void join_path(char *dst, size_t size, const char *dir, const char *leaf) {
    size_t n = strlen(dir);
    int needs_slash = (n > 0 && dir[n - 1] != ':' && dir[n - 1] != '/');

    snprintf(dst, size, "%s%s%s", dir, needs_slash ? "/" : "", leaf);
}

/* Our own directory, absolute.  Zero if it cannot be determined -- on the host,
   always, which is how the test reaches the fallback. */
static int program_dir(char *buf, size_t size) {
#ifdef __amigaos4__
    BPTR lock = GetProgramDir();

    if(lock != ZERO && NameFromLock(lock, (STRPTR)buf, (LONG)size) != 0) {
        return 1;
    }
#else
    (void)buf;
    (void)size;
#endif
    return 0;
}

static void resolve_paths(void) {
    char dir[400];

    if(!program_dir(dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), "%s", FALLBACK_HOME);
    }

    join_path(vm_program, sizeof(vm_program), dir, VM_BINARY);
    join_path(vm_sobjs, sizeof(vm_sobjs), dir, SOBJS_DIR);
}

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
        setenv("LD_LIBRARY_PATH", vm_sobjs, 1);
        return;
    }

    /* Already ours at the front?  Leave it alone rather than growing the
       variable on every nested launch. */
    if(strncmp(existing, vm_sobjs, strlen(vm_sobjs)) == 0) {
        return;
    }

    {
        size_t len = strlen(vm_sobjs) + strlen(existing) + 2;  /* ';' and NUL */
        char *combined = malloc(len);

        if(combined == NULL) {
            /* Out of memory before the VM has even started: ours alone still
               gives a working runtime, which is better than failing here. */
            setenv("LD_LIBRARY_PATH", vm_sobjs, 1);
            return;
        }

        /* ';' -- path.separator on AmigaOS, where ':' is the volume separator. */
        snprintf(combined, len, "%s;%s", vm_sobjs, existing);
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
    resolve_paths();

    child = malloc((argc + 1) * sizeof(*child));
    if(child == NULL) {
        fprintf(stderr, "java: out of memory building the command line\n");
        return 1;
    }

    child[0] = vm_program;
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
    status = spawnv(P_WAIT, vm_program, child);

    if(status == -1) {
        /* The common cause by far is a broken installation -- `java` copied
           somewhere without the rest of the runtime beside it -- so say which
           file was not found rather than printing strerror alone.  Printing the
           RESOLVED path matters: the whole class of bug here is a path that
           looked right in the source and did not survive being resolved. */
        fprintf(stderr, "java: cannot start %s: %s\n", vm_program, strerror(errno));
        fprintf(stderr, "java: the runtime must sit in the same drawer as this program\n");
        free(child);
        return 127;   /* what a shell reports for "command not found" */
    }

    free(child);
    return status;
}
