/*
 * amiga_net.c -- the AmigaOS 4 half of libnet.
 *
 * OpenJDK's java.net natives compile against clib4 essentially unchanged, but
 * they are only half the library.  The other half is per-platform and lives in
 * linux_close.c / bsd_close.c, neither of which we can use:
 *
 *   - The NET_* wrappers.  On Linux these are not thin wrappers at all: every
 *     blocking socket call is bracketed by a per-fd table of blocked threads, so
 *     that closing a socket can dispatch a real-time signal (__SIGRTMAX-2) to
 *     each thread parked in it and make the call return EBADF.  That is how
 *     Java's "close() unblocks a concurrent read()" behaviour is produced.
 *     AmigaOS has no equivalent to deliver, so the wrappers here are the plain
 *     syscalls with the EINTR retry.  The cost is stated in NET_SocketClose().
 *
 *   - getErrorString(), which on Linux/BSD is defined inside NetworkInterface.c
 *     and referenced from PlainDatagramSocketImpl.c.  We do not build
 *     NetworkInterface.c (see the stubs at the bottom), so it is provided here.
 *
 *   - The java.net.NetworkInterface natives, kept as the "no enumerable
 *     interfaces" stubs the port has always used.
 *
 * IPv6: the natives are built with -DDONT_ENABLE_IPV6, which makes
 * net_util_md.c's IPv6_supported() return JNI_FALSE.  net_util.c's JNI_OnLoad
 * then computes IPv6_available = IPv6_supported() & !preferIPv4Stack == 0, so
 * ipv6_available() is false everywhere and InetAddressImplFactory hands Java
 * Inet4AddressImpl.  clib4 does expose AF_INET6, so probing for it (which is
 * exactly what the non-DONT_ENABLE_IPV6 branch does) would answer "yes" and
 * send the runtime down an IPv6 path the AmigaOS stack cannot carry.  The
 * switch is what keeps that from happening -- do not rely on the
 * java.net.preferIPv4Stack property for it, since that is the caller's to set.
 * Inet6AddressImpl.c is not built either: it needs <netinet/icmp6.h>, and with
 * ipv6_available() false its natives are unreachable.
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "jni.h"
#include "jni_util.h"
#include "net_util.h"

/*
 * Every blocking entry point retries on EINTR and otherwise just forwards.
 * Mirrors linux_close.c's BLOCKING_IO_RETURN_INT minus the fd bookkeeping.
 */
#define AMIGA_NET_RETRY(FUNC) ({        \
    int _ret;                           \
    do {                                \
        _ret = (FUNC);                  \
    } while (_ret == -1 && errno == EINTR); \
    _ret;                               \
})

int NET_Read(int s, void *buf, size_t len) {
    return AMIGA_NET_RETRY(recv(s, buf, len, 0));
}

int NET_NonBlockingRead(int s, void *buf, size_t len) {
    return AMIGA_NET_RETRY(recv(s, buf, len, MSG_DONTWAIT));
}

int NET_RecvFrom(int s, void *buf, int len, unsigned int flags,
                 struct sockaddr *from, int *fromlen) {
    socklen_t socklen = (socklen_t)*fromlen;
    int ret = AMIGA_NET_RETRY(recvfrom(s, buf, len, flags, from, &socklen));
    *fromlen = (int)socklen;
    return ret;
}

int NET_Send(int s, void *msg, int len, unsigned int flags) {
    return AMIGA_NET_RETRY(send(s, msg, len, flags));
}

int NET_SendTo(int s, const void *msg, int len, unsigned int flags,
               const struct sockaddr *to, int tolen) {
    return AMIGA_NET_RETRY(sendto(s, msg, len, flags, to, tolen));
}

int NET_Accept(int s, struct sockaddr *addr, int *addrlen) {
    socklen_t socklen = (socklen_t)*addrlen;
    int ret = AMIGA_NET_RETRY(accept(s, addr, &socklen));
    *addrlen = (int)socklen;
    return ret;
}

int NET_Connect(int s, struct sockaddr *addr, int addrlen) {
    return AMIGA_NET_RETRY(connect(s, addr, addrlen));
}

int NET_Poll(struct pollfd *ufds, unsigned int nfds, int timeout) {
    return AMIGA_NET_RETRY(poll(ufds, nfds, timeout));
}

/*
 * clib4 keeps sockets in the same descriptor table as files (socket() calls
 * __initialize_fd with FDF_IS_SOCKET), so close()/dup2() are the right calls
 * and no CloseSocket() special case is needed.
 *
 * What is NOT reproduced: a thread already blocked in recv()/accept() on this
 * descriptor is not woken.  It stays parked until its own timeout or until the
 * peer acts.  Single-threaded use and the timeout-driven paths (NET_Timeout0
 * below, which is what SO_TIMEOUT goes through) are unaffected; asynchronous
 * Socket.close() from a second thread is the case that degrades.
 */
int NET_SocketClose(int fd) {
    return close(fd);
}

int NET_Dup2(int fd, int fd2) {
    return dup2(fd, fd2);
}

/*
 * poll() the socket for readability, re-arming across EINTR with the remaining
 * time.  Returns >0 ready, 0 on timeout, -1 on error.  This is the path
 * SO_TIMEOUT rides on, so the deadline arithmetic has to survive a signal
 * rather than restarting the full timeout.
 */
int NET_Timeout0(int s, long timeout, long currentTime) {
    long prevtime = currentTime, newtime;
    struct timeval t;

    for (;;) {
        struct pollfd pfd;
        int rv;

        pfd.fd = s;
        pfd.events = POLLIN | POLLERR;

        rv = poll(&pfd, 1, timeout);

        if (rv < 0 && errno == EINTR) {
            if (timeout > 0) {
                gettimeofday(&t, NULL);
                newtime = t.tv_sec * 1000 + t.tv_usec / 1000;
                timeout -= newtime - prevtime;
                if (timeout <= 0) {
                    return 0;
                }
                prevtime = newtime;
            }
        } else {
            return rv;
        }
    }
}

/*
 * getErrorString(), which PlainDatagramSocketImpl.c formats errno through, is
 * NOT defined here: despite the Linux/BSD copies inside NetworkInterface.c it
 * is really jni_util.h's, implemented in solaris/native/common/jni_util_md.c,
 * so libjava.so already exports it and libnet imports it like the JNU_* calls.
 */

/*
 * net_util.h declares these two as globals and NetworkInterface.c is what
 * defines them, so leaving that file out took them with it -- libnet.so shipped
 * with ni_addrsID / ni_indexID unresolved (package.sh's symbol check is what
 * caught it).
 *
 * They are NOT dead weight to satisfy the linker.  PlainDatagramSocketImpl.c
 * declares its own file-static shadows at most use sites, but not all: the
 * plain IPv4 multicast-join path reads ni_addrsID through the global (the local
 * static beside it lives inside a "#if defined(__linux__) && defined(AF_INET6)"
 * block we compile out), and the IPv6 mreq path reads ni_indexID the same way.
 * So MulticastSocket.joinGroup(group, netIf) really does dereference this, and
 * a NULL jfieldID there is a crash, not a no-op.
 *
 * Filled in below from the same class and signatures NetworkInterface.c uses.
 */
jfieldID ni_indexID;
jfieldID ni_addrsID;

/*
 * java.net.NetworkInterface -- "this machine has no enumerable interfaces".
 *
 * These are NOT optional even for a program that never touches
 * NetworkInterface: its <clinit> calls init(), so a missing native throws
 * UnsatisfiedLinkError.  That is an Error, not an Exception, so it sails
 * straight through sun.security.provider.SeedGenerator's catch(Exception) in
 * addNetworkAdapterInfo() -- SecureRandom's seeder then cannot initialise and
 * everything downstream of it dies.  Files.createTempFile() draws on
 * SecureRandom, so merely reading an icon through ImageIO was enough to kill
 * Invoicex with "UnsatisfiedLinkError: init".
 *
 * getAll() returns an EMPTY array rather than NULL: getNetworkInterfaces() is
 * specified to return null when there are none, and callers skipping the null
 * check would NPE.  An empty enumeration just yields nothing.  The lookups
 * return null, their documented "not found" answer.
 *
 * Enumerating for real means SIOCGIFCONF/SIOCGIFFLAGS against bsdsocket.library
 * (that is what the __linux__ / _ALLBSD_SOURCE halves of NetworkInterface.c do,
 * which is why we do not compile it) -- a separate job from making sockets work.
 */
/*
 * Enumeration is stubbed, but the field IDs above are still cached here -- this
 * is java.net.NetworkInterface's <clinit>, the same point upstream fills them
 * in, and every caller that reads them already holds a NetworkInterface object,
 * so the class is initialised by then.
 */
JNIEXPORT void JNICALL
Java_java_net_NetworkInterface_init(JNIEnv *env, jclass cls) {
    jclass ni_class;

    (void)cls;

    ni_class = (*env)->FindClass(env, "java/net/NetworkInterface");
    CHECK_NULL(ni_class);
    ni_class = (*env)->NewGlobalRef(env, ni_class);
    CHECK_NULL(ni_class);
    ni_indexID = (*env)->GetFieldID(env, ni_class, "index", "I");
    CHECK_NULL(ni_indexID);
    ni_addrsID = (*env)->GetFieldID(env, ni_class, "addrs",
                                    "[Ljava/net/InetAddress;");
    CHECK_NULL(ni_addrsID);
}

JNIEXPORT jobjectArray JNICALL
Java_java_net_NetworkInterface_getAll(JNIEnv *env, jclass cls) {
    /* cls IS java/net/NetworkInterface -- getAll() is a static native on it */
    return (*env)->NewObjectArray(env, 0, cls, NULL);
}

JNIEXPORT jobject JNICALL
Java_java_net_NetworkInterface_getByName0(JNIEnv *env, jclass cls, jstring name) {
    (void)env;
    (void)cls;
    (void)name;
    return NULL;
}

JNIEXPORT jobject JNICALL
Java_java_net_NetworkInterface_getByIndex0(JNIEnv *env, jclass cls, jint index) {
    (void)env;
    (void)cls;
    (void)index;
    return NULL;
}

JNIEXPORT jobject JNICALL
Java_java_net_NetworkInterface_getByInetAddress0(JNIEnv *env, jclass cls,
                                                 jobject addr) {
    (void)env;
    (void)cls;
    (void)addr;
    return NULL;
}
