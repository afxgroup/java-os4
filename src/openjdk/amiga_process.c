/*
 * amiga_process.c -- java.lang.UNIXProcess natives on top of clib4's spawnvpe.
 *
 * Runtime.exec()/ProcessBuilder died before reaching any native at all:
 * UNIXProcess$Platform.get() switches on os.name and knows only Linux, OS X,
 * SunOS and AIX, so it threw "java.lang.Error: AmigaOS is not a supported OS
 * platform."  src/niopatch/java/lang/UNIXProcess.java adds the AMIGAOS arm;
 * this file is what that arm calls.
 *
 * None of OpenJDK's three launch mechanisms can be used as they stand.  FORK
 * and VFORK need fork(), which AmigaOS does not have -- there is no address
 * space to duplicate.  POSIX_SPAWN needs the jspawnhelper binary, which we do
 * not build.  clib4 offers spawnvpe() instead: one call that both creates the
 * process and executes the program, launched asynchronously (SYS_Asynch,
 * NP_Child) and returning the child's pid.  That maps onto forkAndExec's
 * contract cleanly because the fork/exec split was never observable from Java.
 *
 * Two divergences worth knowing about:
 *
 *   - Environment.  spawnvpe takes a DELTA environment (variables to set for
 *     the child, restored afterwards), not a replacement.  Java hands us a
 *     complete block, so a child sees our environment with the requested
 *     variables applied over it.  ProcessBuilder's usual "inherit and modify"
 *     is therefore exact; a caller that clears the environment outright still
 *     inherits ours.
 *
 *   - Exit status.  clib4 records the child's raw AmigaDOS return code, not a
 *     POSIX wait status, so there is no WIFEXITED/WEXITSTATUS to unpack -- the
 *     value goes back to Java as-is.  A C program's exit(n) arrives as n.
 *
 * GPLv2 (java-os4 project).
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jni.h"
#include "jni_util.h"

/* The port's single source of truth for Java path -> AmigaDOS path. */
#include "amiga_path.h"

/*
 * Returned by waitForProcessExit0 when the child is still running.  No AmigaDOS
 * return code can collide with it: RC is a small non-negative number (OK 0,
 * WARN 5, ERROR 10, FAIL 20) and exit(n) arrives as n.
 */
#define NOT_EXITED ((jint)0x80000000)

/* ---- helpers -------------------------------------------------------- */

/*
 * The byte[] arguments are NUL-terminated C strings, not Java strings, so they
 * are copied out rather than decoded.  Returns NULL for a NULL array, which is
 * meaningful for `dir` (no chdir) and `envBlock` (inherit).
 */
static char *bytesToCString(JNIEnv *env, jbyteArray arr) {
    jsize len;
    char *out;

    if (arr == NULL) {
        return NULL;
    }

    len = (*env)->GetArrayLength(env, arr);
    if ((out = malloc((size_t)len + 1)) == NULL) {
        return NULL;
    }

    (*env)->GetByteArrayRegion(env, arr, 0, len, (jbyte *)out);
    out[len] = '\0';
    return out;
}

/*
 * argBlock and envBlock are single byte arrays holding `count` NUL-separated
 * strings back to back.  Point into the copied block rather than copying again;
 * the caller frees the block once, after the spawn.
 */
static void initVectorFromBlock(const char **vec, char *block, int count) {
    char *p = block;
    int i;

    for (i = 0; i < count; i++) {
        vec[i] = p;
        p += strlen(p) + 1;
    }
}

static void closeSafe(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

/* ---- natives -------------------------------------------------------- */

/*
 * Nothing to do.  The stock init() caches the parent's PATH for its own exec
 * search and installs a SIGCHLD handler; we need neither -- spawnvpe does the
 * PATH search itself (that is its "p"), and clib4 tracks spawned children in
 * its own table, which is what waitpid consults.
 */
JNIEXPORT void JNICALL
Java_java_lang_UNIXProcess_init(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
}

JNIEXPORT jint JNICALL
Java_java_lang_UNIXProcess_forkAndExec(JNIEnv *env, jobject process,
                                       jint mode, jbyteArray helperpath,
                                       jbyteArray prog,
                                       jbyteArray argBlock, jint argc,
                                       jbyteArray envBlock, jint envc,
                                       jbyteArray dir,
                                       jintArray std_fds,
                                       jboolean redirectErrorStream) {
    char *pprog = NULL, *pargBlock = NULL, *penvBlock = NULL, *pdir = NULL;
    const char **argv = NULL;
    const char **envv = NULL;
    jint *fds = NULL;
    /* [0] = read end, [1] = write end, as pipe(2) fills them. */
    int in[2]  = { -1, -1 };
    int out[2] = { -1, -1 };
    int err[2] = { -1, -1 };
    int childIn = -1, childOut = -1, childErr = -1;
    jint resultPid = -1;

    (void)process;
    (void)mode;        /* no launch mechanisms to choose between here */
    (void)helperpath;  /* jspawnhelper is not used */

    if ((pprog = bytesToCString(env, prog)) == NULL) {
        goto Catch;
    }
    pargBlock = bytesToCString(env, argBlock);
    penvBlock = bytesToCString(env, envBlock);
    pdir      = bytesToCString(env, dir);

    /* argv[0] is the program name and is NOT part of the command line --
       spawnvpe skips it exactly as execv would.  Then argc arguments, then
       the NULL terminator. */
    if ((argv = malloc(sizeof(char *) * ((size_t)argc + 2))) == NULL) {
        goto Catch;
    }
    argv[0] = pprog;
    if (pargBlock != NULL) {
        initVectorFromBlock(argv + 1, pargBlock, argc);
    }
    argv[argc + 1] = NULL;

    if (penvBlock != NULL) {
        if ((envv = malloc(sizeof(char *) * ((size_t)envc + 1))) == NULL) {
            goto Catch;
        }
        initVectorFromBlock(envv, penvBlock, envc);
        envv[envc] = NULL;
    }

    if ((fds = (*env)->GetIntArrayElements(env, std_fds, NULL)) == NULL) {
        goto Catch;
    }

    /*
     * -1 means "the caller wants a pipe"; anything else is an already-open
     * descriptor to hand straight to the child (ProcessBuilder redirecting to
     * a file).  Each pipe gives the child one end and keeps the other.
     */
    if (fds[0] == -1) {
        if (pipe(in) < 0) {
            goto CatchIO;
        }
        childIn = in[0];                        /* child reads its stdin */
    } else {
        childIn = fds[0];
    }

    if (fds[1] == -1) {
        if (pipe(out) < 0) {
            goto CatchIO;
        }
        childOut = out[1];                      /* child writes its stdout */
    } else {
        childOut = fds[1];
    }

    if (redirectErrorStream) {
        childErr = childOut;
    } else if (fds[2] == -1) {
        if (pipe(err) < 0) {
            goto CatchIO;
        }
        childErr = err[1];
    } else {
        childErr = fds[2];
    }

    /*
     * amiga_path() on the two arguments that ARE paths, and only those.
     *
     * clib4 will not do it: __translate_unix_to_amiga_path_name() accepts any
     * name containing a ':' "as is", so a Java-side absolute path -- which on
     * this port looks like "/Work:Tools/prog" or "/T:" -- reaches Lock()
     * verbatim, and AmigaDOS reads the leading '/' as the PARENT of the volume.
     * That is the requester for "/T:" instead of "T:".
     *
     * argv is deliberately left alone.  Which arguments are paths is the
     * child's business, not ours; execv does not translate them either.
     *
     * Two live translations at once is within the buffer ring (4), and holding
     * both is exactly what it exists for.
     */
    resultPid = (jint)spawnvpe(amiga_path(pprog), argv, (char **)envv,
                               pdir != NULL ? amiga_path(pdir) : NULL,
                               childIn, childOut, childErr);

    if (resultPid < 0) {
        goto CatchIO;
    }

    /*
     * The child holds its own ends now.  Keeping ours open would mean the
     * parent never sees EOF on the child's output, so a read() on stdout would
     * block for good after the child died.
     */
    if (in[0]  != -1) closeSafe(in[0]);
    if (out[1] != -1) closeSafe(out[1]);
    if (err[1] != -1) closeSafe(err[1]);

    /* Hand back the parent's ends: write to the child's stdin, read its
       stdout and stderr.  -1 where no pipe was made. */
    fds[0] = in[1];
    fds[1] = out[0];
    fds[2] = err[0];

    (*env)->ReleaseIntArrayElements(env, std_fds, fds, 0);
    fds = NULL;
    goto Finally;

CatchIO:
    JNU_ThrowIOExceptionWithLastError(env, "Could not start process");
    /* Only on failure: on success these belong to the child or to Java. */
    closeSafe(in[0]);  closeSafe(in[1]);
    closeSafe(out[0]); closeSafe(out[1]);
    closeSafe(err[0]); closeSafe(err[1]);
    resultPid = -1;

Catch:
    if (fds != NULL) {
        (*env)->ReleaseIntArrayElements(env, std_fds, fds, JNI_ABORT);
    }

Finally:
    free(pprog);
    free(pargBlock);
    free(penvBlock);
    free(pdir);
    free((void *)argv);
    free((void *)envv);
    return resultPid;
}

/*
 * Bounded wait: the exit code, or NOT_EXITED if the child is still running
 * when the timeout runs out.
 *
 * Bounded rather than blocking on purpose.  A thread parked inside a native
 * call never reaches an interpreter safepoint, and on AmigaOS a safepoint is
 * the only place a thread can be stopped (pthread_kill cannot async-interrupt
 * one).  A reaper blocked in waitpid() would therefore survive every attempt
 * to shut the VM down, and main would exit over the top of it.  Returning
 * regularly lets the caller loop in bytecode, where it stays stoppable.
 */
JNIEXPORT jint JNICALL
Java_java_lang_UNIXProcess_waitForProcessExit0(JNIEnv *env, jclass clazz,
                                               jint pid, jint timeoutMillis) {
    int status = 0;
    int waited = 0;
    /* 20ms: short enough that Process.waitFor() returns promptly, long enough
       not to spin the child's parent while it works. */
    const int slice = 20;

    (void)env;
    (void)clazz;

    for (;;) {
        pid_t r = waitpid((pid_t)pid, &status, WNOHANG);

        if (r == (pid_t)pid) {
            return (jint)status;
        }
        if (r < 0) {
            /* ECHILD: already reaped, or never ours.  Nothing better to say
               than "gone", and reporting failure would hang Process.waitFor. */
            return 0;
        }
        if (waited >= timeoutMillis) {
            return NOT_EXITED;
        }

        usleep((useconds_t)slice * 1000);
        waited += slice;
    }
}

/*
 * Kept for source fidelity with the stock class, which declares it; the
 * AMIGAOS arm calls waitForProcessExit0 instead, for the reason above.  If
 * something does reach here it gets the documented blocking behaviour.
 */
JNIEXPORT jint JNICALL
Java_java_lang_UNIXProcess_waitForProcessExit(JNIEnv *env, jobject process,
                                              jint pid) {
    int status = 0;

    (void)env;
    (void)process;

    if (waitpid((pid_t)pid, &status, 0) < 0) {
        return 0;
    }
    return (jint)status;
}

/*
 * There is no kill on AmigaOS -- a task cannot be torn down from outside, and
 * clib4's kill() reflects that: it sends SIGBREAKF_CTRL_C to the child's CLI
 * process, the same thing Ctrl-C does from a shell.  So this is a REQUEST.  A
 * child that honours break signals stops; one that ignores them keeps running,
 * and Process.destroyForcibly() is no more forcible than destroy() here.
 */
JNIEXPORT void JNICALL
Java_java_lang_UNIXProcess_destroyProcess(JNIEnv *env, jclass clazz,
                                          jint pid, jboolean force) {
    (void)env;
    (void)clazz;
    (void)force;

    kill((pid_t)pid, SIGINT);
}
