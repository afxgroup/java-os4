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
#include <fcntl.h>
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
#include "../common/amiga_cmdline.h"

#ifdef __amigaos4__
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/exec.h>
#include <interfaces/dos.h>
#include <interfaces/exec.h>

/*
 * Our own dos.library interface, opened on first use.
 *
 * NOT the global IDOS: a shared object here gets only what the loader can
 * resolve, and while libc.so exports IExec, nothing exports IDOS -- the
 * packaging symbol sweep says so plainly ("libjava.so references symbols
 * nothing provides: Close IoErr Lock Open SystemTags UnLock" when these were
 * called directly).  libamigaawt.c opens intuition.library the same way and for
 * the same reason.
 */
extern struct ExecIFace *IExec;
static struct DOSIFace *dosIFace;

static struct DOSIFace *dosInterface(void)
{
    if (dosIFace == NULL)
    {
        struct Library *base = IExec->OpenLibrary("dos.library", 50);

        if (base != NULL)
        {
            dosIFace = (struct DOSIFace *)
                           IExec->GetInterface(base, "main", 1, NULL);
        }
    }
    return dosIFace;
}
#endif

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
static char *bytesToCString(JNIEnv *env, jbyteArray arr)
{
    jsize len;
    char *out;

    if (arr == NULL)
    {
        return NULL;
    }

    len = (*env)->GetArrayLength(env, arr);
    if ((out = malloc((size_t)len + 1)) == NULL)
    {
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
static void initVectorFromBlock(const char **vec, char *block, int count)
{
    char *p = block;
    int i;

    for (i = 0; i < count; i++)
    {
        vec[i] = p;
        p += strlen(p) + 1;
    }
}

static void closeSafe(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}

/*
 * Keep this descriptor out of the child.
 *
 * clib4's build_fd_inherit_spec() walks every descriptor from 3 up and hands
 * the child a duplicate of each one that is not marked close-on-exec.  On Linux
 * the same inheritance exists, but fork() gives OpenJDK a window in which the
 * child closes everything it does not need before exec.  spawnvpe has no such
 * window -- it creates and execs in one call -- so the flag is the only place
 * left to say "not this one".
 *
 * Without it the child inherits copies of the parent's three pipe ends.  They
 * keep the pipes alive after both sides have closed, which is three PIPE:
 * handles per exec that never go away, and they also hold the write ends open,
 * so the parent's reader never sees EOF.
 */
static void dontInherit(int fd)
{
    if (fd >= 0)
    {
        int flags = fcntl(fd, F_GETFD, 0);

        if (flags >= 0)
        {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

/* ---- diagnostics ----------------------------------------------------- */

/*
 * java.lang.AmigaDiag.openFdCount() -- how many descriptors this process holds.
 *
 * PIPE: has no directory to list, so a leak of pipe handles cannot be counted
 * from the shell.  This counts what CAN be counted, and it happens to be the
 * discriminating half: the ends the parent keeps are in this table, the ends
 * handed to a child are not, because spawnvpe duplicates those and the child
 * owns them from then on.  A delta of six per exec here means we are not
 * closing what forkAndExec returned; a delta of zero means the leak is on the
 * child's side and belongs to clib4.
 *
 * fcntl(F_GETFD) rather than anything clib4-internal, so this keeps working if
 * the descriptor table is ever reorganised.
 */
JNIEXPORT jint JNICALL
Java_java_lang_AmigaDiag_openFdCount(JNIEnv *env, jclass clazz)
{
    int i, count = 0;

    (void)env;
    (void)clazz;

    /* Well past anything this runtime opens; the scan is cheap and only runs
       when a test asks for it. */
    for (i = 0; i < 1024; i++)
    {
        if (fcntl(i, F_GETFD) >= 0)
        {
            count++;
        }
    }
    return (jint)count;
}

/*
 * Detached spawning: the child keeps NOTHING of ours.
 *
 * clib4's spawnvpe ties the child to us in five ways -- NP_Child, a death
 * signal to our Task, spawnData.parentTask, the inherited descriptor list, and
 * the stdio pipes themselves.  Every one of them is a handle owned by a process
 * that is about to exit, which is the whole point of an auto-updater: the
 * application starts it precisely so it can replace the application.  Removing
 * them one at a time chased the crash around; the answer is not to hand over
 * anything at all.
 *
 * So this does what clib4's own system() does -- SystemTags with SYS_UserShell
 * and nothing else -- plus SYS_Asynch, which is the one thing system() does not
 * do and that this needs: system() blocks until the command finishes, and the
 * updater is waiting for US to exit, so a synchronous call is a deadlock rather
 * than a wait.  No NP_Child, no notify tag, no inherit spec, no pipes: DOS gets
 * a command line and the child gets NIL: for its stdio.
 *
 * THE COST, and it is not hidden.  There is no pid to wait on and no pipes to
 * read: Process.waitFor() returns immediately and getInputStream() is at EOF.
 * That is the trade -- a process that survives its parent, or a status and
 * output from one that must not.  SetEnv AMIGA_PROCESS_DETACHED 0 restores the
 * tethered spawn, with pipes and a real exit status, for a caller whose parent
 * outlives the child.
 */
static int detachedSpawn(void)
{
    static int cached = -1;

    if (cached < 0)
    {
        /* Read once: this is per-VM policy, not per-call. */
        const char *v = getenv("AMIGA_PROCESS_DETACHED");

        cached = (v != NULL && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'))
                     ? 0
                     : 1;
    }
    return cached;
}

/*
 * Launch it and forget it.  Returns a pid on success, -1 with errno set.
 *
 * The pid comes from IoErr() immediately after SystemTags, which is where DOS
 * leaves it for an asynchronous launch -- and "immediately" is literal: any
 * other DOS call in between overwrites it.
 */
static int spawnDetached(const char *program, const char *const *argv,
                         int argc, const char *cwd)
{
    struct DOSIFace *dos = dosInterface();
    char *command;
    LONG rc;
    int pid;

    if (dos == NULL)
    {
        errno = ENOSYS;
        return -1;
    }

    command = amiga_build_command(program, argv, argc);
    if (command == NULL)
    {
        errno = ENOMEM;
        return -1;
    }

    BPTR in = dos->DupFileHandle(dos->Input());
    BPTR out = dos->DupFileHandle(dos->Output());
    BPTR err = dos->DupFileHandle(dos->ErrorOutput());

    IExec->DebugPrintF("[AMIGA_PROCESS] spawnDetached: %s\n", command);
    rc = dos->SystemTags(command,
                         SYS_Input,     0,
                         SYS_Output,    0,
                         SYS_Error,     0,
                         SYS_Input,     in,
                         SYS_Output,    out,
                         SYS_Error,     err,
                         NP_CloseError, TRUE,                         
                         SYS_Asynch,    TRUE,
                         NP_CopyVars,   TRUE,
                         NP_StackSize,  8191875,
                         TAG_DONE);
    pid = (int)dos->IoErr(); /* before any other DOS call */
    IExec->DebugPrintF("[AMIGA_PROCESS] spawnDetached: rc=%ld, pid=%d\n", rc, pid);
    free(command);

    if (rc != 0)
    {
        if (in)
            dos->Close(in);
        if (out)
            dos->Close(out);
        if (err)
            dos->Close(err);
        errno = EINVAL;
        return -1;
    }

    return pid > 0 ? pid : 1; /* a pid Java can hold; never 0 */
}

/* ---- natives -------------------------------------------------------- */

/*
 * Nothing to do.  The stock init() caches the parent's PATH for its own exec
 * search and installs a SIGCHLD handler; we need neither -- spawnvpe does the
 * PATH search itself (that is its "p"), and clib4 tracks spawned children in
 * its own table, which is what waitpid consults.
 */
JNIEXPORT void JNICALL
Java_java_lang_UNIXProcess_init(JNIEnv *env, jclass clazz)
{
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
                                       jboolean redirectErrorStream)
{
    char *pprog = NULL, *pargBlock = NULL, *penvBlock = NULL, *pdir = NULL;
    const char **argv = NULL;
    const char **envv = NULL;
    jint *fds = NULL;
    /* [0] = read end, [1] = write end, as pipe(2) fills them. */
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};
    int childIn = -1, childOut = -1, childErr = -1;
    jint resultPid = -1;

    (void)process;
    (void)mode;       /* no launch mechanisms to choose between here */
    (void)helperpath; /* jspawnhelper is not used */

    if ((pprog = bytesToCString(env, prog)) == NULL)
    {
        goto Catch;
    }
    pargBlock = bytesToCString(env, argBlock);
    penvBlock = bytesToCString(env, envBlock);
    pdir = bytesToCString(env, dir);

    /* argv[0] is the program name and is NOT part of the command line --
       spawnvpe skips it exactly as execv would.  Then argc arguments, then
       the NULL terminator. */
    if ((argv = malloc(sizeof(char *) * ((size_t)argc + 2))) == NULL)
    {
        goto Catch;
    }
    argv[0] = pprog;
    if (pargBlock != NULL)
    {
        initVectorFromBlock(argv + 1, pargBlock, argc);
    }
    argv[argc + 1] = NULL;

    if (penvBlock != NULL)
    {
        if ((envv = malloc(sizeof(char *) * ((size_t)envc + 1))) == NULL)
        {
            goto Catch;
        }
        initVectorFromBlock(envv, penvBlock, envc);
        envv[envc] = NULL;
    }

    if ((fds = (*env)->GetIntArrayElements(env, std_fds, NULL)) == NULL)
    {
        goto Catch;
    }

    /*
     * -1 means "the caller wants a pipe"; anything else is an already-open
     * descriptor to hand straight to the child (ProcessBuilder redirecting to
     * a file).  Each pipe gives the child one end and keeps the other.
     */
    if (detachedSpawn())
    {
        /*
         * DETACHED: no pipes, and nothing else of ours either.
         *
         * A pipe is a reference to this process.  Handing the child one end of
         * it means that when we exit -- which is the whole point of a detached
         * spawn -- the handle dies under it, and the child faults the next time
         * it writes a line.  That is the crash that survived removing NP_Child,
         * the death signal and parentTask: those were the references we knew
         * about, and the pipes were the ones still there.
         *
         * -1 for all three tells spawnvpe to open NIL:, so the child's stdio
         * belongs to the child.  Java's Process therefore reports EOF on
         * getInputStream() immediately: with nobody left to read a pipe, that
         * is the truthful answer rather than a hang.  Set
         * AMIGA_PROCESS_DETACHED=0 for a child whose output you need to read
         * and whose parent will outlive it.
         */
        childIn = childOut = childErr = -1;
    }
    else if (fds[0] == -1)
    {
        if (pipe(in) < 0)
        {
            goto CatchIO;
        }
        childIn = in[0];    /* child reads its stdin */
        dontInherit(in[1]); /* ours; not the child's */
    }
    else
    {
        childIn = fds[0];
    }

    if (!detachedSpawn() && fds[1] == -1)
    {
        if (pipe(out) < 0)
        {
            goto CatchIO;
        }
        childOut = out[1];   /* child writes its stdout */
        dontInherit(out[0]); /* ours; not the child's */
    }
    else if (!detachedSpawn())
    {
        childOut = fds[1];
    }

    if (detachedSpawn())
    {
        /* already -1: NIL: on all three */
    }
    else if (redirectErrorStream)
    {
        childErr = childOut;
    }
    else if (fds[2] == -1)
    {
        if (pipe(err) < 0)
        {
            goto CatchIO;
        }
        childErr = err[1];
        dontInherit(err[0]); /* ours; not the child's */
    }
    else
    {
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
    if (detachedSpawn())
    {
        /* argv[0] is the program name by execv convention; the command line
           takes the program separately, so skip it here or it appears twice. */
        resultPid = (jint)spawnDetached(amiga_path(pprog),
                                        (const char *const *)(argv + 1), argc,
                                        pdir != NULL ? amiga_path(pdir) : NULL);
    }
    else
    {
        resultPid = (jint)spawnvpe(amiga_path(pprog), argv, (char **)envv,
                                   pdir != NULL ? amiga_path(pdir) : NULL,
                                   childIn, childOut, childErr);
    }

    if (resultPid < 0)
    {
        goto CatchIO;
    }

    /*
     * The child holds its own ends now.  Keeping ours open would mean the
     * parent never sees EOF on the child's output, so a read() on stdout would
     * block for good after the child died.
     */
    if (in[0] != -1)
        closeSafe(in[0]);
    if (out[1] != -1)
        closeSafe(out[1]);
    if (err[1] != -1)
        closeSafe(err[1]);

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
    closeSafe(in[0]);
    closeSafe(in[1]);
    closeSafe(out[0]);
    closeSafe(out[1]);
    closeSafe(err[0]);
    closeSafe(err[1]);
    resultPid = -1;

Catch:
    if (fds != NULL)
    {
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
 * Reads everything the pipe has RIGHT NOW and stops -- never blocks, never
 * grows without bound.
 *
 * UNIXProcess drains the child's output when it exits, so that a caller who
 * has not read yet still gets it after the fd is closed.  Neither obvious way
 * of doing that works here:
 *
 *   - The stock loop trusts available() to bound it, and available() cannot
 *     answer for a clib4 pipe: fstat says S_IFIFO, clib4 implements no
 *     FIONREAD, and the lseek fallback is meaningless on a pipe.  It returned
 *     a count that never decreased and the reaper ate the heap.
 *   - Reading to EOF instead removes the OOM but blocks whenever the write end
 *     outlives the child, and a reaper stuck in a native read never reaches an
 *     interpreter safepoint, never runs in.close(), and holds its pipe handles
 *     open for the life of the process.  DOS then still sees the process, and
 *     the jars cannot be replaced.
 *
 * So: switch the fd to non-blocking, read until it says EAGAIN, put the flag
 * back.  clib4 supports O_NONBLOCK on pipes (fdhookentry.c tests
 * FDF_NON_BLOCKING alongside FDF_PIPE).  Bounded by DRAIN_MAX as well, so even
 * a pipe that somehow always has more cannot exhaust the heap.
 *
 * Returns NULL when nothing was waiting, which the caller reads as "no
 * stragglers"; an empty array would instead mean a stream that is open and
 * eternally empty.
 */
#define DRAIN_MAX (256 * 1024)

JNIEXPORT jbyteArray JNICALL
Java_java_lang_UNIXProcess_drainPipe0(JNIEnv *env, jclass clazz, jint fd)
{
    char *buf = NULL;
    size_t cap = 8192, len = 0;
    int flags, restore = 0;
    jbyteArray result = NULL;

    (void)clazz;

    if (fd < 0)
    {
        return NULL;
    }

    if ((flags = fcntl(fd, F_GETFL, 0)) >= 0 &&
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0)
    {
        restore = 1;
    }
    /* If the flag would not take, read anyway: at worst we block once on a
       pipe that has data, which is what the caller wanted regardless. */

    if ((buf = malloc(cap)) == NULL)
    {
        goto done;
    }

    for (;;)
    {
        ssize_t n;

        if (len == cap)
        {
            char *bigger;
            if (cap >= DRAIN_MAX)
            {
                break; /* enough; the rest is the child's loss */
            }
            cap *= 2;
            if ((bigger = realloc(buf, cap)) == NULL)
            {
                break; /* keep what we have */
            }
            buf = bigger;
        }

        n = read(fd, buf + len, cap - len);
        if (n > 0)
        {
            len += (size_t)n;
            continue;
        }
        /* 0 is EOF, -1 with EAGAIN is "nothing more right now"; either way we
           are done.  Any other error is done too -- there is nobody to tell. */
        break;
    }

done:
    if (restore)
    {
        fcntl(fd, F_SETFL, flags);
    }

    if (len > 0)
    {
        if ((result = (*env)->NewByteArray(env, (jsize)len)) != NULL)
        {
            (*env)->SetByteArrayRegion(env, result, 0, (jsize)len, (jbyte *)buf);
        }
    }
    free(buf);
    return result;
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
                                               jint pid, jint timeoutMillis)
{
    int status = 0;
    int waited = 0;
    /* 20ms: short enough that Process.waitFor() returns promptly, long enough
       not to spin the child's parent while it works. */
    const int slice = 20;

    (void)env;
    (void)clazz;

    for (;;)
    {
        pid_t r = waitpid((pid_t)pid, &status, WNOHANG);

        if (r == (pid_t)pid)
        {
            return (jint)status;
        }
        if (r < 0)
        {
            /* ECHILD: already reaped, never ours -- or DETACHED, which is now
               the default and makes this the ordinary path rather than an edge
               case.  A detached child is not in clib4's child table, so there
               is nothing to wait on and no status to report; "exited, 0" is
               what Process.waitFor gets.  That is the acknowledged cost of a
               child that outlives us (see detachedSpawn), and it is why
               AMIGA_PROCESS_DETACHED=0 exists for callers that need the real
               status more than they need the child to survive. */
            return 0;
        }
        if (waited >= timeoutMillis)
        {
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
                                              jint pid)
{
    int status = 0;

    (void)env;
    (void)process;

    if (waitpid((pid_t)pid, &status, 0) < 0)
    {
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
                                          jint pid, jboolean force)
{
    (void)env;
    (void)clazz;
    (void)force;

    kill((pid_t)pid, SIGINT);
}
