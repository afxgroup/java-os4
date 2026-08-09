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
 * WHY system() AND NOT spawnv()
 *
 * This used to use spawnv(P_WAIT), on the reasoning that it takes argv as an
 * ARRAY and lets clib4 do the quoting, where system() takes a string and makes
 * the quoting ours to get wrong.  That reasoning was about the wrong risk.
 *
 * clib4's spawnv and spawnvpe both create the child with
 *
 *     NP_Child,                TRUE
 *     NP_NotifyOnDeathSigTask, me
 *
 * so the new process is a DOS child of this one and signals THIS TASK when it
 * dies.  That is fine while we are alive and is not fine afterwards.  An
 * auto-updater is the case that breaks it: the application spawns `java`, which
 * spawns the VM, and then the application exits -- so the VM is left signalling
 * a task that no longer exists, and DOS is left holding process structures for
 * a parent that has gone ("Parent #185 deletion resuming, all children have now
 * gone" appears in the crash dump).  The grandchild crashed after the shell had
 * returned to the prompt.
 *
 * system() creates a process with neither tag -- clib4 passes only
 * SYS_UserShell -- so it is independent of us, and still synchronous, which is
 * what a launcher wants: the shell does not return while Java runs, and the
 * exit code is real.
 *
 * The quoting that argument was originally about is real, and is done here
 * (quote_arg), to AmigaDOS's rules rather than a shell's, and tested on the
 * host.  It is a bounded problem with a known answer; a child tethered to a
 * dead parent is not.
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
 * something, and the command line carries an absolute path.  JAVA: is the
 * fallback, and is what the installer uses.
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

/*
 * Drop the last component: "Work:Java/bin" -> "Work:Java", "Work:Java" ->
 * "Work:".  Zero when there is nothing left to drop, so a caller cannot loop
 * forever on a volume root.
 */
static int parent_dir(char *dir) {
    char *slash = strrchr(dir, '/');

    if(slash != NULL) {
        *slash = '\0';
        return 1;
    }

    /* No '/', so we are at "Volume:something" -- the parent is the volume. */
    {
        char *colon = strchr(dir, ':');

        if(colon != NULL && colon[1] != '\0') {
            colon[1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int have_vm_in(const char *dir) {
    char candidate[512];

    join_path(candidate, sizeof(candidate), dir, VM_BINARY);
    return access(candidate, F_OK) == 0;
}

/*
 * Find the runtime, which is not always in the directory this program is in.
 *
 * A JRE keeps its launcher in bin/ -- $JAVA_HOME/bin/java -- and applications
 * build that path themselves rather than asking.  InvoiceX's auto-updater does
 * exactly this:
 *
 *     autoupdate eseguo [Work:Java/bin/java, -cp, ...]
 *
 * and it was right; we were the ones not laying out like a JRE.  So `java` now
 * ships in bin/ as well as at the top, and the same binary has to work from
 * both: from bin/ the VM is one level up.
 *
 * Checked by looking for the VM rather than by testing whether the directory is
 * called "bin", because the question that matters is where the runtime is, not
 * what the drawer is named.
 */
static void resolve_paths(void) {
    char dir[400];

    if(!program_dir(dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), "%s", FALLBACK_HOME);
    }

    if(!have_vm_in(dir)) {
        char up[400];

        snprintf(up, sizeof(up), "%s", dir);
        if(parent_dir(up) && have_vm_in(up)) {
            snprintf(dir, sizeof(dir), "%s", up);
        } else if(!have_vm_in(dir)) {
            /* Neither: fall back to the installed location, so the error names
               somewhere a user can check rather than wherever we happen to be. */
            snprintf(dir, sizeof(dir), "%s", FALLBACK_HOME);
        }
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

/*
 * Does this argument have to be quoted for the AmigaDOS shell?
 *
 * The same set clib4's own build_arg_string() uses: whitespace splits an
 * argument, and a quote would otherwise start or end one.  0xA0 is a
 * non-breaking space, which the shell also treats as whitespace and which
 * arrives from any application that took its arguments from a GUI.
 */
static int needs_quoting(const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    if(*p == '\0') {
        return 1;               /* an empty argument must survive as one */
    }
    for(; *p != '\0'; p++) {
        if(*p == ' ' || *p == '\t' || *p == '\n' || *p == 0xA0 || *p == '"') {
            return 1;
        }
    }
    return 0;
}

/*
 * Append one argument, quoted if it needs to be.  Returns the number of
 * characters it would have written, so the caller can size the buffer by
 * running it once with a NULL destination -- the same measure-then-write shape
 * snprintf has, and for the same reason: guessing a bound for something that
 * can double in length is how buffers get overrun.
 *
 * AmigaDOS escapes with '*', not '\': a literal quote is *" and a literal
 * asterisk is **.  A newline inside an argument becomes *N, since a raw one
 * would end the command.
 */
static size_t quote_arg(char *dst, const char *s) {
    size_t n = 0;

#define PUT(c) do { if(dst != NULL) dst[n] = (c); n++; } while(0)

    if(!needs_quoting(s)) {
        for(; *s != '\0'; s++) {
            PUT(*s);
        }
        return n;
    }

    PUT('"');
    for(; *s != '\0'; s++) {
        switch(*s) {
            case '"':
            case '*':
                PUT('*');
                PUT(*s);
                break;
            case '\n':
                PUT('*');
                PUT('N');
                break;
            default:
                PUT(*s);
                break;
        }
    }
    PUT('"');
    return n;

#undef PUT
}

/*
 * The whole command line: the VM, then our arguments, quoted and space
 * separated.  Measured first, then written, so the buffer is exactly right.
 * NULL if it cannot be allocated.
 */
static char *build_command(const char *program, int argc, char *argv[]) {
    size_t len = quote_arg(NULL, program);
    char *cmd;
    size_t n;
    int i;

    for(i = 1; i < argc; i++) {
        len += 1 + quote_arg(NULL, argv[i]);       /* separating space */
    }

    cmd = malloc(len + 1);
    if(cmd == NULL) {
        return NULL;
    }

    n = quote_arg(cmd, program);
    for(i = 1; i < argc; i++) {
        cmd[n++] = ' ';
        n += quote_arg(cmd + n, argv[i]);
    }
    cmd[n] = '\0';

    return cmd;
}

int main(int argc, char *argv[]) {
    char *command;
    int status;

    /*
     * The command line is built here rather than handed over as an array,
     * because system() takes a string -- see the note at the top for why that
     * trade is the right way round.  quote_arg() is what makes it safe.
     */
    resolve_paths();
    set_library_path();

    command = build_command(vm_program, argc, argv);
    if(command == NULL) {
        fprintf(stderr, "java: out of memory building the command line\n");
        return 1;
    }

    /*
     * Synchronous, and NOT a DOS child of this process: clib4's system() passes
     * only SYS_UserShell, so the VM does not signal this task when it dies and
     * DOS does not hold our process structure open waiting for it.  That is the
     * whole point of using system() here -- an auto-updater outlives the
     * application that started it, and a tethered process crashes when its
     * parent has gone.
     *
     * Still synchronous, so the shell does not return while Java runs and the
     * exit code below is the VM's own.
     */
    status = system(command);

    if(status == -1) {
        /* The common cause by far is a broken installation -- `java` copied
           somewhere without the rest of the runtime beside it -- so say which
           file was not found rather than printing strerror alone.  Printing the
           RESOLVED path matters: the whole class of bug here is a path that
           looked right in the source and did not survive being resolved. */
        fprintf(stderr, "java: cannot start %s: %s\n", vm_program, strerror(errno));
        fprintf(stderr, "java: the runtime must sit in the same drawer as this program\n");
        free(command);
        return 127;   /* what a shell reports for "command not found" */
    }

    free(command);
    return status;
}
