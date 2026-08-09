/*
 * amiga_management.c -- the platform half of libmanagement.so.
 *
 * java.lang.management works through two layers: the shared natives in
 * share/native/sun/management, which call into the VM through the jmm interface
 * (JamVM implements it -- classlib/openjdk/management.c), and a per-platform
 * file that answers questions about the OPERATING SYSTEM.  OpenJDK's Unix one
 * is OperatingSystemImpl.c, which reads /proc, <sys/swap.h> and statvfs64.
 * AmigaOS has none of those, so this file stands in for it.
 *
 * Without it the whole library is absent and the first call fails at class
 * initialisation, taking out something the caller never asked for:
 *
 *     java.lang.UnsatisfiedLinkError: no management in java.library.path
 *         at sun.management.ManagementFactoryHelper.<clinit>
 *         at java.lang.management.ManagementFactory.getRuntimeMXBean
 *
 * -- an application asking for the RUNTIME bean gets nothing because the
 * OPERATING SYSTEM bean's natives are missing, since one library carries both.
 *
 *
 * WHY THE NAMES END IN 0
 *
 * These are declared by the rt.jar we ship, which is the boot JDK's (8u502),
 * and that is NEWER than the OpenJDK source drop in vendor/.  8u502 renamed
 * most of these to a trailing 0 and added ThreadImpl.getTotalThreadAllocatedMemory.
 * A native whose name does not match its declaration compiles perfectly and
 * simply never binds, so the build compares the symbols defined here against the
 * ones rt.jar declares, rather than trusting that the source drop agrees.
 *
 *
 * UNAVAILABLE IS -1, NOT 0
 *
 * java.lang.management's contract is that a quantity which cannot be measured
 * is -1; zero means measured-and-zero.  Reporting 0 for "committed virtual
 * memory" on a machine with no virtual memory would be a lie that monitoring
 * code cannot tell from the truth, so anything AmigaOS genuinely cannot answer
 * returns -1 and lets the caller decide.  Swap is the exception: AmigaOS 4 runs
 * without swap, so zero there IS the measurement.
 *
 * GPLv2 (java-os4 project).
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jni.h"
#include "jvm.h"

#include <proto/exec.h>

extern struct ExecIFace *IExec;

/* ---- memory ---------------------------------------------------------- */

/*
 * AvailMem(MEMF_TOTAL) is the machine's RAM, uncapped.
 *
 * JamVM's nativePhysicalMemory() caps the same figure at 1GB, because it sizes
 * a single MAP_ANON with it and a 2GB machine would ask for more than clib4 can
 * give in one go.  That cap is about allocation, not about the truth, and this
 * is a reporting interface: an application asking how much memory the machine
 * has should be told, not handed a number chosen to keep an mmap happy.
 */
JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getTotalPhysicalMemorySize0(JNIEnv *env, jobject o) {
    uint64 total = IExec->AvailMem(MEMF_TOTAL);

    return total == 0 ? -1LL : (jlong)total;
}

JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getFreePhysicalMemorySize0(JNIEnv *env, jobject o) {
    uint64 avail = IExec->AvailMem(MEMF_ANY);

    return avail == 0 ? -1LL : (jlong)avail;
}

/*
 * AmigaOS has no virtual memory: every allocation is resident, so there is no
 * "committed virtual" figure distinct from what the process physically holds,
 * and no per-process accounting to read it from either.  -1 rather than a
 * plausible-looking substitute.
 */
JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getCommittedVirtualMemorySize0(JNIEnv *env, jobject o) {
    return -1LL;
}

/* No swap.  Zero here is a measurement, not a gap. */
JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getTotalSwapSpaceSize0(JNIEnv *env, jobject o) {
    return 0LL;
}

JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getFreeSwapSpaceSize0(JNIEnv *env, jobject o) {
    return 0LL;
}

/* ---- file descriptors ------------------------------------------------ */

JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getMaxFileDescriptorCount0(JNIEnv *env, jobject o) {
    long max = sysconf(_SC_OPEN_MAX);

    return max < 0 ? -1LL : (jlong)max;
}

/*
 * Counted by probing, because clib4 has no /proc/self/fd to read.
 *
 * fcntl(F_GETFD) is the cheapest question that distinguishes an open descriptor
 * from a free slot without disturbing either.  Bounded by _SC_OPEN_MAX, and by
 * a hard ceiling as well: this is a monitoring call an application may make on
 * a timer, and a runaway limit would turn it into a stall.
 */
JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getOpenFileDescriptorCount0(JNIEnv *env, jobject o) {
    long max = sysconf(_SC_OPEN_MAX);
    jlong count = 0;
    int fd;

    if(max < 0 || max > 4096) {
        max = 4096;
    }

    for(fd = 0; fd < (int)max; fd++) {
        if(fcntl(fd, F_GETFD) != -1) {
            count++;
        }
    }
    return count;
}

/* ---- cpu -------------------------------------------------------------- */

/*
 * clock() measures this process, which is what getProcessCpuTime asks for, and
 * the contract wants nanoseconds.
 *
 * CLOCKS_PER_SEC is scaled BEFORE the division to keep the resolution: doing it
 * the other way on a tick-granular clock quantises every answer to whole
 * seconds, which reads as a process using no CPU at all.
 */
JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getProcessCpuTime0(JNIEnv *env, jobject o) {
    clock_t t = clock();

    if(t == (clock_t)-1) {
        return -1LL;
    }
    return (jlong)t * (1000000000LL / CLOCKS_PER_SEC);
}

/*
 * Load figures.  AmigaOS keeps no per-CPU idle accounting that these could be
 * derived from, and the contract has a defined value for exactly this case:
 * a negative result means "not available".  Guessing from a single clock()
 * sample would produce a number that moves, which is worse than none.
 */
JNIEXPORT jdouble JNICALL
Java_sun_management_OperatingSystemImpl_getProcessCpuLoad0(JNIEnv *env, jobject o) {
    return -1.0;
}

JNIEXPORT jdouble JNICALL
Java_sun_management_OperatingSystemImpl_getSystemCpuLoad0(JNIEnv *env, jobject o) {
    return -1.0;
}

JNIEXPORT jdouble JNICALL
Java_sun_management_OperatingSystemImpl_getSingleCpuLoad0(JNIEnv *env, jobject o, jint cpu) {
    return -1.0;
}

JNIEXPORT jlong JNICALL
Java_sun_management_OperatingSystemImpl_getHostTotalCpuTicks0(JNIEnv *env, jobject o) {
    return -1LL;
}

/*
 * These two exist for container-aware sizing on Linux (cgroup limits versus the
 * host).  There are no containers here, so both are the real processor count.
 */
JNIEXPORT jint JNICALL
Java_sun_management_OperatingSystemImpl_getHostConfiguredCpuCount0(JNIEnv *env, jobject o) {
    long n = sysconf(_SC_NPROCESSORS_CONF);

    return n < 1 ? 1 : (jint)n;
}

JNIEXPORT jint JNICALL
Java_sun_management_OperatingSystemImpl_getHostOnlineCpuCount0(JNIEnv *env, jobject o) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n < 1 ? 1 : (jint)n;
}

JNIEXPORT void JNICALL
Java_sun_management_OperatingSystemImpl_initialize0(JNIEnv *env, jclass cls) {
    /* The Unix version caches a page size and opens /proc handles.  Nothing to
       prepare here -- every reading above is taken fresh. */
}

/* ---- file permissions ------------------------------------------------- */

/*
 * Used by the management agent to check that its config file cannot be read by
 * other users before trusting a password in it.
 *
 * AmigaOS has no user ownership: the protection bits are per-file, not per-user,
 * so no file can be shown to be readable by its owner ALONE.  The honest answer
 * is JNI_FALSE, which makes the agent refuse a password file rather than accept
 * one it cannot vouch for.  Returning JNI_TRUE would silence the complaint by
 * asserting a guarantee this platform does not make.
 */
JNIEXPORT jboolean JNICALL
Java_sun_management_FileSystemImpl_isAccessUserOnly0(JNIEnv *env, jclass cls, jstring str) {
    return JNI_FALSE;
}

/* ---- threads ---------------------------------------------------------- */

/*
 * New in 8u502, and absent from the source drop in vendor/ -- the reason the
 * build compares symbols against rt.jar rather than assuming the two agree.
 *
 * It wants the total memory allocated by all threads since VM start.  JamVM's
 * jmm interface does not track per-thread allocation, so -1: unavailable.
 */
JNIEXPORT jlong JNICALL
Java_sun_management_ThreadImpl_getTotalThreadAllocatedMemory(JNIEnv *env, jclass cls) {
    return -1LL;
}
