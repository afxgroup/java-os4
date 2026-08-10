#!/bin/sh
# Cross-compile OpenJDK 8 core native libraries for AmigaOS 4 / clib4 (Phase 2,
# step 2).  Same model as the GNU Classpath natives: ppc-amigaos-gcc, -fPIC, and
# the -use-dynld shared-clib4 scheme (rpath=SYS:Test; ship clib4 sobjs with the app).
#
# OpenJDK has no src/amigaos/native, so the OS-specific (md) code is based on
# src/solaris (unix) and adapted per-file as clib4 gaps surface -- like the
# Classpath VMFile work, at larger scale.
#
# Starts with the PORTABLE deps that libjava links against (validate the pipeline):
#   libfdlibm.a  (pure C math, src/share/native/java/lang/fdlibm)
#   libverify.so (bytecode verifier, src/share/native/common/check_{code,format}.c)
# Then libjava / libzip / ... are added incrementally.
#
#   docker run --rm -v "<proj>:/work" -v "<clib4>:/clib4" -w /work \
#       javaos4-build:latest sh /work/tools/build-openjdk-natives.sh
set -e

. "$(dirname "$0")/build-env.sh"

# Apply docs/openjdk8-amiga.patch HERE, at the head of the pipeline, on a tree
# that is still pristine.  It used to be applied only by build-awt-natives.sh,
# which runs second -- by then this script's own seds have adapted 19 of the 20
# files the patch describes, so the tree matched neither the pristine state
# (forward apply fails) nor the patched one (reverse apply fails, because rect.h
# is adapted by the awt script alone).  The result was a "cannot apply OpenJDK
# patch cleanly" warning on every clean build, even though the end state was
# correct.  Applied first, the patch goes on in full and every sed below becomes
# the no-op its guard intends.
apply_openjdk_patch

SDKCLIB4=$SDK_CLIB4
if [ -n "${CLIB4_BUILD_ROOT:-}" ] && [ -d "$CLIB4_BUILD_ROOT/build/lib" ]; then
    cp -f "$CLIB4_BUILD_ROOT"/build/lib/*.a "$CLIB4_BUILD_ROOT"/build/lib/*.o "$SDKCLIB4/lib/" 2>/dev/null || true
    cp -rf "$CLIB4_BUILD_ROOT"/library/include/* "$SDKCLIB4/include/" 2>/dev/null || true
fi

J=${OPENJDK8_SRC:-$BUILD_ROOT/openjdk8/jdk-3334efeacd83}
OUT=$BUILD_ROOT/openjdk-natives
mkdir -p "$OUT"
# -fcommon: OpenJDK (like the 2016 JamVM tree) defines globals in headers without
# extern (e.g. parentPathv); gcc 10+ defaults to -fno-common -> multiple-definition.
CC="ppc-amigaos-gcc -mcrt=clib4 -fPIC -O2 -w -fcommon -gstabs"

# Compatibility shims for unix headers clib4 lacks (OpenJDK's md code targets unix;
# amigaos/clib4 differs).  Kept here (not by editing OpenJDK source) so the drop
# stays pristine.  Add more as gaps surface.
COMPAT="$OUT/compat"
mkdir -p "$COMPAT/sys"
echo '#include <signal.h>' > "$COMPAT/sys/signal.h"   # clib4 has <signal.h>, no <sys/signal.h>
# Single-source-of-truth Amiga charset-name normaliser (the "Amiga-1251" fix):
# force-included via jdkdefs.h below and called from java_props_md.c ParseLocale().
# Tested on the host by tools/test-amiga-charset.c against the same header.
cp "$PROJECT_ROOT/src/openjdk/amiga_charset.h" "$COMPAT/amiga_charset.h"
cp "$PROJECT_ROOT/src/openjdk/amiga_path.h" "$COMPAT/amiga_path.h"
# Self-test that normaliser on the HOST compiler before building libjava, so a
# broken charset mapping fails the build fast (10 languages + edge cases).
# Skipped (with a warning) if no host cc/gcc is present.
HOSTCC=$(command -v cc || command -v gcc || true)
if [ -n "$HOSTCC" ]; then
    if "$HOSTCC" "$PROJECT_ROOT/tools/test-amiga-charset.c" -o "$OUT/test-amiga-charset" 2>"$OUT/e"; then
        "$OUT/test-amiga-charset" || { echo "FATAL: amiga charset self-test FAILED"; exit 1; }
        echo "=== amiga charset self-test PASSED ==="
    else
        echo "WARN: host-compile of test-amiga-charset.c failed; skipping self-test"; head -3 "$OUT/e"
    fi
    # Same for the path conversion -- every open/stat/mkdir/rename/dlopen goes
    # through it, so a regression there breaks the runtime everywhere at once.
    if "$HOSTCC" "$PROJECT_ROOT/tools/test-amiga-path.c" -o "$OUT/test-amiga-path" 2>"$OUT/e"; then
        "$OUT/test-amiga-path" >/dev/null || { echo "FATAL: amiga path self-test FAILED"; "$OUT/test-amiga-path"; exit 1; }
        echo "=== amiga path self-test PASSED ==="
    else
        echo "WARN: host-compile of test-amiga-path.c failed; skipping self-test"; head -3 "$OUT/e"
    fi
fi
# String-literal defines OpenJDK's makefiles pass via -D (ARCHPROPNAME, version);
# put them in a -include header so the C-string quoting survives cleanly.
cat > "$COMPAT/jdkdefs.h" <<'EOF'
#ifndef JDKDEFS_H
#define JDKDEFS_H
#ifndef ARCHPROPNAME
#define ARCHPROPNAME "ppc"
#endif
#define JDK_MAJOR_VERSION "1"
#define JDK_MINOR_VERSION "8"
#define JDK_MICRO_VERSION "0"
#define JDK_BUILD_NUMBER "b03"
#define JDK_UPDATE_VERSION "77"
#ifndef RELEASE
#define RELEASE "amigaos4"   /* os.version string (normally uname -r) */
#endif
/* sysconf names clib4 doesn't define: map to unknown ids so sysconf() returns
   -1 and the JDK code takes its documented fallback (default buffer sizes,
   IOV_MAX=16). */
#include <unistd.h>
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX 9981
#endif
#ifndef _SC_GETGR_R_SIZE_MAX
#define _SC_GETGR_R_SIZE_MAX 9982
#endif
#ifndef _SC_IOV_MAX
#define _SC_IOV_MAX 9983
#endif
/* sun.nio.fs passes open() flags from Java (UnixConstants in Temurin's LINUX
   rt.jar = Linux octal values); clib4's O_* encoding is entirely different
   (O_CREAT 1<<3 vs 0100 etc) -- translate bit-by-bit.  ACCMODE bits match. */
#include <fcntl.h>
static int amiga_oflags(int lf) {
    int f = lf & 3;
    if (lf & 0100)     f |= O_CREAT;
    if (lf & 0200)     f |= O_EXCL;
    if (lf & 01000)    f |= O_TRUNC;
    if (lf & 02000)    f |= O_APPEND;
    if (lf & 04000)    f |= O_NONBLOCK;
    if (lf & 010000)   f |= O_DSYNC;
    if (lf & 04000000) f |= O_SYNC;
    if (lf & 0400000)  f |= O_NOFOLLOW;
    if (lf & 0200000)  f |= O_DIRECTORY;
    return f;
}
/* Java -> AmigaDOS path conversion.  Single source of truth in
   src/openjdk/amiga_path.h (copied into $COMPAT above); tested on the host by
   tools/test-amiga-path.c against that same header. */
#include "amiga_path.h"
/* POSIX rename() replaces an existing destination and works within any
   filesystem.  On AmigaOS neither holds:

     - AmigaDOS Rename() fails with ERROR_OBJECT_EXISTS (EEXIST) when the
       destination exists, and clib4 only papers over that when
       __unix_path_semantics is on -- which it is NOT here (clib4 turns it on
       only if PROGDIR:.unix exists, and we ship no such file).
     - some handlers do not implement Rename at all.  ENV: is one: deleting the
       destination first is not enough there, the rename itself is refused.

   So: try rename; on EEXIST drop the destination and retry; and if it still
   will not go, fall back to copy-then-delete-source, which is the only thing
   such a handler leaves us.  The copy path is for regular files only -- never
   silently "copy" a directory.

   If the copy succeeds but the source cannot be removed we still report
   success: the destination holds the right content, which is the whole point of
   the write-temp-then-rename pattern, and a stray temp file is harmless.

   Both arguments have already been through amiga_path(); nothing in here calls
   it again, so the ring buffers they point at stay valid throughout. */
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
static int amiga_copy_file(const char *from, const char *to) {
    char buff[8192];
    struct stat sb;
    int in, out;
    ssize_t got;

    if (stat(from, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    if ((in = open(from, O_RDONLY)) < 0)
        return -1;

    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, sb.st_mode & 0777)) < 0) {
        int saved = errno;
        close(in);
        errno = saved;
        return -1;
    }

    while ((got = read(in, buff, sizeof(buff))) > 0) {
        ssize_t done = 0;

        while (done < got) {
            ssize_t put = write(out, buff + done, got - done);

            if (put <= 0) {
                int saved = errno;
                close(in);
                close(out);
                remove(to);
                errno = saved;
                return -1;
            }
            done += put;
        }
    }

    if (got < 0) {
        int saved = errno;
        close(in);
        close(out);
        remove(to);
        errno = saved;
        return -1;
    }

    close(in);
    if (close(out) != 0) {
        int saved = errno;
        remove(to);
        errno = saved;
        return -1;
    }

    return 0;
}
static int amiga_rename(const char *from, const char *to) {
    if (rename(from, to) == 0)
        return 0;

    if (errno == EEXIST && remove(to) == 0 && rename(from, to) == 0)
        return 0;

    /* the handler will not rename at all (ENV: and friends) -- copy instead */
    if (amiga_copy_file(from, to) != 0)
        return -1;

    remove(from);
    return 0;
}
/* Amiga "canonicalize": no symlinks to resolve in practice; just normalise to the
   AmigaDOS form (amiga_path) and present it back to Java WITH a leading '/' so
   File.isAbsolute()/toURI() treat it as absolute (else getAbsoluteFile doubles it). */
static int amiga_canonicalize(char *path, char *out, int len) {
    const char *ap = amiga_path(path);
    int n = 0;
    if (len < 2) { if (len > 0) out[0] = 0; return 0; }
    out[n++] = '/';
    while (*ap != 0 && n < len - 1) {
        /* collapse "/./" and a leading "./" segment */
        if (ap[0] == '.' && ap[1] == '/' && (n == 1 || out[n-1] == '/')) { ap += 2; continue; }
        out[n++] = *ap++;
    }
    if (n > 1 && out[n-1] == '/') n--;   /* drop trailing '/' */
    out[n] = 0;
    return 0;
}
/* AmigaOS charset-name normaliser (the "Amiga-1251" fix); single source of
   truth in src/openjdk/amiga_charset.h, copied into $COMPAT above. */
#include "amiga_charset.h"
#endif
EOF

# --- source adaptations for amigaos/clib4 (idempotent; keep additive) ---------
# jdk_util_md.h: add an amigaos branch to its OS #if ladder (ISNANF/ISNAND),
# else it #errors "missing platform-specific definition" -> blocks ~7 libjava .c.
MDU="$J/src/solaris/native/common/jdk_util_md.h"
if [ -f "$MDU" ] && ! grep -q __amigaos4__ "$MDU"; then
    sed -i 's|#elif defined(_AIX)|#elif defined(__amigaos4__)\n#include <math.h>\n#define ISNANF(f) isnan(f)\n#define ISNAND(d) isnan(d)\n#elif defined(_AIX)|' "$MDU"
    echo "=== adapted jdk_util_md.h (amigaos ISNANF/ISNAND) ==="
fi

# LFS: clib4 lacks struct stat64/dirent64/flock64 (it has 64-bit base APIs), so
# join OpenJDK's _ALLBSD_SOURCE path that maps the *64 names to base.  Extend the
# guards in the affected md files with __amigaos4__.
for f in "$J/src/solaris/native/java/io/io_util_md.h" \
         "$J/src/solaris/native/java/io/UnixFileSystem_md.c" \
         "$J/src/solaris/native/java/lang/childproc.c" \
         "$J/src/solaris/native/java/util/TimeZone_md.c" \
         "$J/src/solaris/native/java/util/FileSystemPreferences.c"; do
    [ -f "$f" ] || continue
    if ! grep -q __amigaos4__ "$f"; then
        sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g; s@defined(_ALLBSD_SOURCE)@(defined(_ALLBSD_SOURCE) || defined(__amigaos4__))@g' "$f"
        echo "=== adapted $(basename "$f") (amigaos LFS->base) ==="
    fi
done

# io_util_md.c: FIONREAD lives in clib4's <sys/filio.h>, included only under the
# __solaris__ guard; extend it (and the _ALLBSD ioctl guard) for amigaos.
IOMD="$J/src/solaris/native/java/io/io_util_md.c"
if [ -f "$IOMD" ] && ! grep -q __amigaos4__ "$IOMD"; then
    sed -i 's@#ifdef __solaris__@#if defined(__solaris__) || defined(__amigaos4__)@g; s@defined(_ALLBSD_SOURCE)@(defined(_ALLBSD_SOURCE) || defined(__amigaos4__))@g' "$IOMD"
    echo "=== adapted io_util_md.c (amigaos FIONREAD via sys/filio.h) ==="
fi

# UnixFileSystem_md.c statMode(): AmigaDOS relative paths carry NO "./" current-dir
# prefix; clib4 passes "./X" straight to Lock() which fails.  Strip a leading "./" so
# File.exists()/loadLibrary() resolve CWD-relative files (e.g. ./libzip.so during
# System.initializeSystemClass loadLibrary("zip")).  Idempotent.
#
# Each edit below is guarded on ITS OWN marker, not on a shared "has this file
# been touched at all?" test.  A single file-wide guard silently skips any edit
# added later: the tree already contained amiga_path from an earlier run, so the
# opendir/remove/mkdir/rename/utimes/statvfs64 group never fired, File.list()
# kept popping the volume requester, and docs/openjdk8-amiga.patch stopped
# matching the tree in either direction ("cannot apply OpenJDK patch cleanly").
UFS="$J/src/solaris/native/java/io/UnixFileSystem_md.c"
if [ -f "$UFS" ]; then
    ufs_adapted=0
    # statMode(): normalise ("./", "/Volume:") -> AmigaDOS form before stat64.
    # NOT idempotent -- the anchor it matches survives the insertion.
    if ! grep -q 'path = amiga_path(path);' "$UFS"; then
        perl -0pi -e 's/(statMode\(const char \*path, int \*mode\)\s*\{\n\s*struct stat64 sb;\n)/$1#ifdef __amigaos4__\n    path = amiga_path(path);\n#endif\n/' "$UFS"
        ufs_adapted=1
    fi
    # canonicalize0(): return a leading-"/" absolute path so File.toURI()/isAbsolute()
    # don't double the Amiga "Volume:" path (the -classpath/URLClassPath bug).
    # NOT idempotent -- the pattern is a substring of its own replacement.
    if ! grep -q 'amiga_canonicalize' "$UFS"; then
        sed -i 's@canonicalize((char \*)path,@amiga_canonicalize((char *)path,@' "$UFS"
        ufs_adapted=1
    fi
    # The remaining edits ARE idempotent (no pattern matches its replacement),
    # so they can just run: a re-run over an adapted file is a no-op.
    #
    # other stat/access/chmod sites (getLastModified/getLength/checkAccess/setPermission)
    sed -i 's@stat64(path, &sb)@stat64(amiga_path(path), \&sb)@g; s@access(path, mode)@access(amiga_path(path), mode)@g; s@chmod(path, mode)@chmod(amiga_path(path), mode)@g' "$UFS"
    # ...and every REMAINING path-taking call in this file.  Any one that is left
    # raw hands AmigaDOS the Unix-absolute Java form and pops a "Please insert
    # volume /Work:" requester -- File.list()/opendir() was the one that fired on
    # a plain `java -jar App.jar` (the ext-dirs scan and the app's own File.list).
    # rename() needs two live translations at once: amiga_path() rings its buffers.
    sed -i 's@remove(path)@remove(amiga_path(path))@g;
            s@opendir(path)@opendir(amiga_path(path))@g;
            s@mkdir(path, 0777)@mkdir(amiga_path(path), 0777)@g;
            s@rename(fromPath, toPath)@amiga_rename(amiga_path(fromPath), amiga_path(toPath))@g;
            s@utimes(path, tv)@utimes(amiga_path(path), tv)@g;
            s@statvfs64(path, &fsstat)@statvfs64(amiga_path(path), \&fsstat)@g' "$UFS"
    if [ "$ufs_adapted" = 1 ]; then
        echo "=== adapted UnixFileSystem_md.c (amiga_path/amiga_canonicalize) ==="
    fi
fi

# FileSystemPreferences.c: java.util.prefs writes under user.home -- same story,
# the Java side hands down the Unix-absolute "/Volume:..." form.
FSP="$J/src/solaris/native/java/util/FileSystemPreferences.c"
if [ -f "$FSP" ] && ! grep -q amiga_path "$FSP"; then
    sed -i 's@chmod(fname, permission)@chmod(amiga_path(fname), permission)@g;
            s@open(fname, O_RDONLY, 0)@open(amiga_path(fname), O_RDONLY, 0)@g;
            s@open(fname, O_WRONLY|O_CREAT, permission)@open(amiga_path(fname), O_WRONLY|O_CREAT, permission)@g' "$FSP"
    echo "=== adapted FileSystemPreferences.c (amiga_path) ==="
fi

# UnixFileSystem_md.c getLastModifiedTime(): clib4 struct stat uses st_mtime
# (no st_mtim/st_mtimespec). Use seconds precision on amiga.
if [ -f "$UFS" ] && ! grep -q "amiga st_mtime" "$UFS"; then
    perl -0pi -e 's@#ifndef MACOSX\n\s*rv\s*=\s*\(jlong\)sb\.st_mtim\.tv_sec \* 1000;\n\s*rv \+= \(jlong\)sb\.st_mtim\.tv_nsec / 1000000;\n\s*#else\n\s*rv\s*=\s*\(jlong\)sb\.st_mtimespec\.tv_sec \* 1000;\n\s*rv \+= \(jlong\)sb\.st_mtimespec\.tv_nsec / 1000000;\n\s*#endif@#if defined(__amigaos4__)\n            /* amiga st_mtime */\n            rv = (jlong)sb.st_mtime * 1000;\n#elif !defined(MACOSX)\n            rv  = (jlong)sb.st_mtim.tv_sec * 1000;\n            rv += (jlong)sb.st_mtim.tv_nsec / 1000000;\n#else\n            rv  = (jlong)sb.st_mtimespec.tv_sec * 1000;\n            rv += (jlong)sb.st_mtimespec.tv_nsec / 1000000;\n#endif@s' "$UFS"
    echo "=== adapted UnixFileSystem_md.c (amiga st_mtime) ==="
fi

# UnixFileSystem_md.c setLastModifiedTime(): clib4 struct stat uses st_atime
# (no st_atim/st_atimespec). Preserve access time with seconds precision.
if [ -f "$UFS" ] && ! grep -q "amiga st_atime" "$UFS"; then
    perl -0pi -e 's@#ifndef MACOSX\n\s*tv\[0\]\.tv_sec = sb\.st_atim\.tv_sec;\n\s*tv\[0\]\.tv_usec = sb\.st_atim\.tv_nsec / 1000;\n\s*#else\n\s*tv\[0\]\.tv_sec = sb\.st_atimespec\.tv_sec;\n\s*tv\[0\]\.tv_usec = sb\.st_atimespec\.tv_nsec / 1000;\n\s*#endif@#if defined(__amigaos4__)\n            /* amiga st_atime */\n            tv[0].tv_sec = sb.st_atime;\n            tv[0].tv_usec = 0;\n#elif !defined(MACOSX)\n            tv[0].tv_sec = sb.st_atim.tv_sec;\n            tv[0].tv_usec = sb.st_atim.tv_nsec / 1000;\n#else\n            tv[0].tv_sec = sb.st_atimespec.tv_sec;\n            tv[0].tv_usec = sb.st_atimespec.tv_nsec / 1000;\n#endif@s' "$UFS"
    echo "=== adapted UnixFileSystem_md.c (amiga st_atime) ==="
fi

# zip_util.c ZFILE_Open + io_util_md.c handleOpen: normalise Amiga "/Volume:"/"./" paths
# before the actual open() (so URLClassPath can open jars from the canonicalised
# leading-"/" path, and FileInputStream/Output work on them too).
ZU="$J/src/share/native/java/util/zip/zip_util.c"
if [ -f "$ZU" ] && ! grep -q amiga_path "$ZU"; then
    perl -0pi -e 's/(ZFILE_Open\(const char \*fname, int flags\) \{\n)/$1#ifdef __amigaos4__\n    fname = amiga_path(fname);\n#endif\n/' "$ZU"
    echo "=== adapted zip_util.c ZFILE_Open (amiga_path) ==="
fi
if [ -f "$IOMD" ] && ! grep -q amiga_path "$IOMD"; then
    sed -i 's@RESTARTABLE(open64(path,@RESTARTABLE(open64(amiga_path(path),@' "$IOMD"
    echo "=== adapted io_util_md.c handleOpen (amiga_path) ==="
fi

# Temurin 8 (late 8u) renamed FileInputStream.available -> available0 (JDK-8080679);
# the 8u77 native drop still names it Java_..._available -> UnsatisfiedLinkError at
# runtime (rt.jar calls available0).  Rename to match the runtime class library.
FIS="$J/src/share/native/java/io/FileInputStream.c"
if [ -f "$FIS" ] && ! grep -q "FileInputStream_available0" "$FIS"; then
    sed -i 's/Java_java_io_FileInputStream_available(/Java_java_io_FileInputStream_available0(/' "$FIS"
    echo "=== adapted FileInputStream.c available->available0 (Temurin skew) ==="
fi

# sun.misc.VM.latestUserDefinedLoader -> latestUserDefinedLoader0 (Temurin late-8u
# renamed it; 8u77 VM.c has the un-suffixed name -> UnsatisfiedLinkError from
# ObjectInputStream during deserialization).  JVM_LatestUserDefinedLoader exists.
VMC="$J/src/share/native/sun/misc/VM.c"
if [ -f "$VMC" ] && ! grep -q "VM_latestUserDefinedLoader0" "$VMC"; then
    sed -i 's/Java_sun_misc_VM_latestUserDefinedLoader(/Java_sun_misc_VM_latestUserDefinedLoader0(/' "$VMC"
    echo "=== adapted VM.c latestUserDefinedLoader->...0 (Temurin skew) ==="
fi

# TimeZone_md.c: treat amigaos like __linux__ for the tz-file detection blocks (they
# fopen /etc/localtime etc. -> NULL on Amiga -> graceful fallback) and like MACOSX for
# getGMTOffsetID (uses struct tm.tm_gmtoff, which clib4 HAS; the plain branch uses the
# SysV `timezone` global, which clib4 LACKS).  3 targeted guards.  Idempotent.
TZMD="$J/src/solaris/native/java/util/TimeZone_md.c"
if [ -f "$TZMD" ] && ! grep -q "defined(MACOSX) || defined(__amigaos4__)" "$TZMD"; then
    sed -i \
      -e 's@#if defined(__linux__) || defined(MACOSX) || defined(__solaris__)@#if defined(__linux__) || defined(MACOSX) || defined(__solaris__) || defined(__amigaos4__)@' \
      -e 's@#if defined(__linux__) || defined(MACOSX)$@#if defined(__linux__) || defined(MACOSX) || defined(__amigaos4__)@' \
      -e 's@^#if defined(MACOSX)$@#if defined(MACOSX) || defined(__amigaos4__)@' \
      "$TZMD"
    echo "=== adapted TimeZone_md.c (amigaos -> linux tz-files + tm_gmtoff GMT offset) ==="
fi

# java_props_md.c ParseLocale(): clib4's nl_langinfo(CODESET) returns the AmigaOS
# diskfont MIME charset family "Amiga-NNNN" (ObtainCharsetInfo DFCS_MIMENAME) verbatim
# -- e.g. "Amiga-1251" on a Cyrillic locale.  The JDK has no charset provider named
# "Amiga-1251", so file.encoding/sun.jnu.encoding="Amiga-1251" forces the lazy
# ExtendedProviderHolder.<clinit> re-entrantly during initProperties (before
# sun.misc.VM.booted()) -> the cached class-init failure surfaces as
# java.lang.NoClassDefFoundError "unsupported charset extension: Amiga-1251".
# Normalise the name right after *std_encoding is set, via amiga_normalize_encoding()
# (src/openjdk/amiga_charset.h, force-included through jdkdefs.h above; the mapping
# table and its rationale live there and are host-tested by tools/test-amiga-charset.c).
# Pure no-op for any non-"Amiga" value.  Idempotent (guard: the inserted call).
JPM="$J/src/solaris/native/java/lang/java_props_md.c"
if [ -f "$JPM" ] && ! grep -q 'amiga_normalize_encoding' "$JPM"; then
    perl -0pi -e 's{(\? p : "ISO8859-1";\n)}{$1#ifdef __amigaos4__\n        /* AmigaOS diskfont reports a vendor charset name (e.g. "Amiga-1251")\n         * the JDK cannot resolve -> NoClassDefFoundError at bootstrap; map it\n         * to a standard-provider charset name (see src/openjdk/amiga_charset.h). */\n        *std_encoding = (char *) amiga_normalize_encoding(*std_encoding);\n#endif\n}gs' "$JPM"
    echo "=== adapted java_props_md.c (Amiga charset-name normalise via amiga_normalize_encoding) ==="
fi

# OpenJDK export headers (jni.h/jvm.h) + per-OS jni_md.h/jvm_md.h (solaris=unix) +
# the shared native common headers (jni_util.h, jlong.h, ...) + the compat shims.
EXP="-I $COMPAT -I $J/src/share/javavm/export -I $J/src/solaris/javavm/export -I $J/src/share/native/common"

echo "=== libfdlibm.a (pure C math) ==="
FD=$J/src/share/native/java/lang/fdlibm
fdobjs=""
for c in "$FD"/src/*.c; do
    o="$OUT/fd_$(basename "$c" .c).o"
    if $CC -I "$FD/include" -c "$c" -o "$o" 2>"$OUT/e"; then
        fdobjs="$fdobjs $o"
    else
        echo "  FDLIBM FAIL $(basename "$c")"; head -4 "$OUT/e"
    fi
done
ppc-amigaos-ar rcs "$OUT/libfdlibm.a" $fdobjs
echo "  libfdlibm.a OK ($(ppc-amigaos-ar t "$OUT/libfdlibm.a" | wc -l) objects)"

echo "=== libverify.so (bytecode verifier) ==="
C=$J/src/share/native/common
ok=1
for f in check_code check_format; do
    if ! $CC $EXP -c "$C/$f.c" -o "$OUT/$f.o" 2>"$OUT/e"; then
        echo "  VERIFY FAIL $f.c"; head -8 "$OUT/e"; ok=0
    fi
done
if [ $ok = 1 ]; then
    ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
        -o "$OUT/libverify.so" "$OUT/check_code.o" "$OUT/check_format.o" 2>"$OUT/e" \
        && echo "  libverify.so OK ($(wc -c < "$OUT/libverify.so") bytes)" \
        || { echo "  libverify.so LINK FAIL"; head -8 "$OUT/e"; }
fi

echo "=== javah: generate JNI headers (from Temurin rt.jar) ==="
HDR="$OUT/headers"; mkdir -p "$HDR"
RTJAR="$BOOT_JDK/jre/lib/rt.jar"
"$BOOT_JDK/bin/javah" -d "$HDR" -classpath "$RTJAR" \
  java.io.Console java.io.FileDescriptor java.io.FileInputStream java.io.FileOutputStream \
  java.io.FileSystem java.io.ObjectInputStream java.io.ObjectOutputStream \
  java.io.ObjectStreamClass java.io.RandomAccessFile java.io.UnixFileSystem \
  java.lang.Class java.lang.ClassLoader 'java.lang.ClassLoader$NativeLibrary' \
  java.lang.Compiler java.lang.Double java.lang.Float java.lang.Object java.lang.Package \
  java.lang.reflect.Array java.lang.reflect.Executable java.lang.reflect.Field \
  java.lang.reflect.Proxy java.lang.Runtime java.lang.SecurityManager java.lang.Shutdown \
  java.lang.StrictMath java.lang.String java.lang.System java.lang.Thread java.lang.Throwable \
  java.security.AccessController java.util.concurrent.atomic.AtomicLong java.util.jar.JarFile \
  java.util.TimeZone java.util.zip.Adler32 java.util.zip.CRC32 java.util.zip.Deflater \
  java.util.zip.Inflater java.util.zip.ZipFile \
  sun.misc.GC sun.misc.MessageUtils sun.misc.NativeSignalHandler sun.misc.Signal \
  sun.misc.URLClassPath sun.misc.Version sun.misc.VM sun.misc.VMSupport \
  sun.reflect.ConstantPool sun.reflect.Reflection \
  sun.reflect.NativeConstructorAccessorImpl sun.reflect.NativeMethodAccessorImpl 2>&1 | tail -3
echo "  generated $(ls "$HDR" 2>/dev/null | wc -l) headers"

echo "=== libjava: compile pass (collect adaptation list) ==="
LJDIRS="solaris/native/java/lang share/native/java/lang share/native/java/lang/reflect \
 share/native/java/io solaris/native/java/io share/native/java/nio share/native/java/security \
 share/native/common share/native/sun/misc share/native/sun/reflect share/native/java/util \
 share/native/java/util/concurrent/atomic solaris/native/common solaris/native/java/util"
LJINC="-I $HDR -I $J/src/share/native/java/lang/fdlibm/include $EXP -include $COMPAT/jdkdefs.h"
for d in $LJDIRS; do LJINC="$LJINC -I $J/src/$d"; done
# TimeZone_md.c IS built now (the amigaos guards below treat it like __linux__ for
# tz-file detection -- /etc/localtime etc. just don't exist on Amiga so it falls back
# -- and like MACOSX for getGMTOffsetID, which uses struct tm.tm_gmtoff that clib4 has,
# instead of the SysV `timezone`/`altzone` globals it lacks).
# handleAvailable(): stop available() lying about pipes.
#
# clib4 pipes are DOS handles on PIPE:, which fstat reports as S_IFIFO, so the
# function tries ioctl(FIONREAD) -- clib4 implements no such request -- and then
# falls through to lseek arithmetic that is meaningless on a pipe.  It answered
# with a positive count that never decreased, which is what grew UNIXProcess's
# reaper buffer until the heap ran out.  0 is a legal answer: available()
# promises only that this many bytes read without blocking, never that no more
# exist.  Placed after the ioctl attempt, so a clib4 that grows FIONREAD wins.
IOUTIL="$J/src/solaris/native/java/io/io_util_md.c"
if [ -f "$IOUTIL" ] && ! grep -q "AMIGA_NO_FIONREAD" "$IOUTIL"; then
    awk '
        /RESTARTABLE\(ioctl\(fd, FIONREAD, &n\), result\);/ { hit = 1 }
        { print }
        hit && /^            \}$/ && !done {
            print "#ifdef __amigaos4__ /* AMIGA_NO_FIONREAD */"
            print "            /* clib4 has no FIONREAD, and the lseek fallback below is"
            print "               meaningless on a PIPE: handle -- it answered with a count"
            print "               that never decreased.  Say 0 rather than something untrue. */"
            print "            *pbytes = 0;"
            print "            return 1;"
            print "#endif"
            done = 1; hit = 0
        }
    ' "$IOUTIL" > "$IOUTIL.tmp" && mv "$IOUTIL.tmp" "$IOUTIL"
    if grep -q "AMIGA_NO_FIONREAD" "$IOUTIL"; then
        echo "=== adapted io_util_md.c (amigaos: available()=0 on pipes, no FIONREAD) ==="
    else
        echo "  WARN: handleAvailable not adapted; available() may still lie about pipes"
    fi
fi

# UNIXProcess_md.c and its childproc.c helper are REPLACED, not built: their
# forkAndExec goes through fork/vfork (no address space to duplicate on
# AmigaOS) or posix_spawn via a jspawnhelper binary we do not ship.
# src/openjdk/amiga_process.c provides the same natives on top of clib4's
# spawnvpe instead, and defines the same symbols -- building both would collide.
EXCL="check_code.c check_format.c jspawnhelper.c java_props_macosx.c \
      UNIXProcess_md.c childproc.c"
# Wipe, do not just create: every .o in here is linked, so a source dropped
# from the build would otherwise stay in the library for ever.  That is how
# the excluded UNIXProcess_md.o kept colliding with amiga_process.o, and the
# same staleness once shipped a libsunec.so built before -static-libstdc++.
rm -rf "$OUT/libjava"; mkdir -p "$OUT/libjava"
ok=0; fail=0
for d in $LJDIRS; do
    for c in "$J/src/$d"/*.c; do
        [ -f "$c" ] || continue
        b=$(basename "$c")
        case " $EXCL " in *" $b "*) continue ;; esac
        # TimeZone_md.c needs struct tm.tm_gmtoff, which clib4 gates behind
        # _GNU_SOURCE/_BSD_SOURCE/_XOPEN_SOURCE (raw member is __tm_gmtoff).
        extra=""; [ "$b" = "TimeZone_md.c" ] && extra="-D_GNU_SOURCE"
        if $CC $extra $LJINC -c "$c" -o "$OUT/libjava/$(basename "$c" .c).o" 2>"$OUT/e"; then
            ok=$((ok+1))
        else
            fail=$((fail+1)); echo "  FAIL $d/$b"
            grep -m1 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
        fi
    done
done
# java.lang.UNIXProcess natives via spawnvpe, in place of the excluded
# UNIXProcess_md.c.  Paired with src/niopatch/java/lang/UNIXProcess.java, which
# adds the AMIGAOS arm to Platform.get() -- without it Runtime.exec throws
# "AmigaOS is not a supported OS platform" before reaching any native at all.
for extra in amiga_process amiga_crypto; do
    if $CC $LJINC -c "$PROJECT_ROOT/src/openjdk/$extra.c" \
           -o "$OUT/libjava/$extra.o" 2>"$OUT/e"; then
        ok=$((ok+1)); echo "  $extra.c OK"
    else
        fail=$((fail+1)); echo "  $extra.c FAIL"
        grep -m3 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done

echo "  libjava compile: $ok OK, $fail FAILED"
TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + fail))

# java.lang.Shutdown.beforeHalt(): some 8u drops already provide it; only synthesize
# the no-op compat stub when the native source does not.
if ! grep -q 'Java_java_lang_Shutdown_beforeHalt' "$J/src/share/native/java/lang/Shutdown.c"; then
cat > "$OUT/libjava/shutdown_beforehalt_compat.c" <<'SEOF'
#include "jni.h"
JNIEXPORT void JNICALL
Java_java_lang_Shutdown_beforeHalt(JNIEnv *env, jclass cls) { }
SEOF
$CC $LJINC -c "$OUT/libjava/shutdown_beforehalt_compat.c" -o "$OUT/libjava/shutdown_beforehalt_compat.o" 2>"$OUT/e" \
    && echo "  Shutdown.beforeHalt compat OK" || { echo "  beforeHalt compat FAIL"; head -4 "$OUT/e"; }
else
    rm -f "$OUT/libjava/shutdown_beforehalt_compat.c" "$OUT/libjava/shutdown_beforehalt_compat.o"
fi

echo "=== link libjava.so ==="
# Recipe (CoreLibraries.gmk): libjava links -lverify + static libfdlibm.  The VM
# (JVM_*) symbols come from jamvm at runtime (-use-dynld), so leave them undefined
# here (shared objects allow it).  rpath=SYS:Test for the clib4 sobjs.
if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
       -o "$OUT/libjava.so" "$OUT"/libjava/*.o "$OUT/libfdlibm.a" \
       -L"$OUT" -lverify 2>"$OUT/e"; then
    echo "  libjava.so OK ($(wc -c < "$OUT/libjava.so") bytes)"
else
    echo "  libjava.so LINK FAIL"; head -20 "$OUT/e"
fi

echo "=== libzip.so (java.util.zip + bundled zlib-1.2.13) ==="
# System.initializeSystemClass() calls loadLibrary("zip") -> libzip.so must exist
# on java.library.path (= sun.boot.library.path = SYS:Test).  zlib is compiled in
# (USE_EXTERNAL_LIBZ=false) -> self-contained (no dependency on clib4's libz version).
# Updated from OpenJDK 8u's in-tree zlib-1.2.8 (2013) to a TRACKED in-repo zlib-1.2.13
# (2022, tools/zlib-1.2.13/) -- security + inflate fixes; keeps OpenJDK's case-clash
# file names zadler32.c/zcrc32.c.  JNU_*/jio_* come from libjava at runtime (leave
# undefined, like libjava leaves JVM_* undefined).
ZIP=$J/src/share/native/java/util/zip
ZLIB=/work/tools/zlib-1.2.13
# Temurin 8 (a late 8u) dropped the addSlash arg from ZipFile.getEntry; the 8u77
# IcedTea native drop still has it -> signature clash with the javah header from
# Temurin's rt.jar.  Match the runtime class library: drop addSlash, pass JNI_FALSE
# to ZIP_GetEntry2 (later 8u handles the trailing-slash retry in Java). Idempotent.
ZF=$ZIP/ZipFile.c
if [ -f "$ZF" ] && grep -q "jbyteArray name, jboolean addSlash)" "$ZF"; then
    sed -i 's@jbyteArray name, jboolean addSlash)@jbyteArray name)@; s@ZIP_GetEntry2(zip, path, (jint)ulen, addSlash)@ZIP_GetEntry2(zip, path, (jint)ulen, JNI_FALSE)@' "$ZF"
    echo "=== adapted ZipFile.c getEntry (drop addSlash to match Temurin rt.jar) ==="
fi
ZINC="-I $HDR -I $ZIP -I $ZLIB $EXP -I $J/src/solaris/native/common \
 -I $J/src/share/native/java/io -I $J/src/solaris/native/java/io -include $COMPAT/jdkdefs.h"
rm -rf "$OUT/libzip"; mkdir -p "$OUT/libzip"
zok=0; zfail=0
for c in "$ZLIB"/*.c "$ZIP"/Adler32.c "$ZIP"/CRC32.c "$ZIP"/Deflater.c \
         "$ZIP"/Inflater.c "$ZIP"/zip_util.c "$ZIP"/ZipFile.c; do
    [ -f "$c" ] || continue
    if $CC $ZINC -c "$c" -o "$OUT/libzip/$(basename "$c" .c).o" 2>"$OUT/e"; then
        zok=$((zok+1))
    else
        zfail=$((zfail+1)); echo "  ZIP FAIL $(basename "$c")"
        grep -m1 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done
echo "  libzip compile: $zok OK, $zfail FAILED"
TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + zfail))

# ZipFile.getManifestNum: native added in later 8u (security: count META-INF/
# MANIFEST.MF entries); absent from the 8u77 source but Temurin's rt.jar calls it ->
# UnsatisfiedLinkError.  Supply it (0/1 via ZIP_GetEntry -- normal jars have exactly
# one canonical MANIFEST.MF), in a compat .c compiled into libzip.so.
cat > "$OUT/libzip/zip_manifest_compat.c" <<'ZEOF'
#include "jni.h"
#include "jlong.h"
#include "zip_util.h"
JNIEXPORT jint JNICALL
Java_java_util_zip_ZipFile_getManifestNum(JNIEnv *env, jclass cls, jlong zfile) {
    jzfile *zip = jlong_to_ptr(zfile);
    jzentry *ze = ZIP_GetEntry(zip, "META-INF/MANIFEST.MF", 0);
    jint n = (ze != NULL) ? 1 : 0;
    if (ze != NULL) ZIP_FreeEntry(zip, ze);
    return n;
}
ZEOF
if $CC $ZINC -c "$OUT/libzip/zip_manifest_compat.c" -o "$OUT/libzip/zip_manifest_compat.o" 2>"$OUT/e"; then
    echo "  getManifestNum compat OK"
else
    echo "  getManifestNum compat FAIL"; head -6 "$OUT/e"
fi

if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
       -o "$OUT/libzip.so" "$OUT"/libzip/*.o 2>"$OUT/e"; then
    echo "  libzip.so OK ($(wc -c < "$OUT/libzip.so") bytes)"
else
    echo "  libzip.so LINK FAIL"; head -20 "$OUT/e"
fi

echo "=== niopatch.zip (bootclasspath-prepend NIO.2 platform patch) ==="
# sun.nio.fs.DefaultFileSystemProvider in Temurin's rt.jar only recognises
# Solaris/Linux/Mac/AIX -> AssertionError "Platform not recognized" on AmigaOS,
# which kills URLClassLoader/-classpath.  Ship a patched class (always the generic
# unix/Linux provider) PREPENDED on -Xbootclasspath.  Run with
# -Dsun.nio.fs.chdirAllowed=true so provider construction needs no libnio natives.
# java.io.UnixFileSystem is also overridden: stock resolve() joins "JAVA:" +
# child as "JAVA:/child" (parent of the volume root on AmigaDOS), which breaks
# File.exists() in ClassLoader.loadLibrary -> UnsatisfiedLinkError for every
# System.loadLibrary call.  The patched class joins volume-root parents without
# the '/'.
NIOP="$PROJECT_ROOT/src/niopatch"
if [ -f "$NIOP/sun/nio/fs/DefaultFileSystemProvider.java" ]; then
    (cd "$NIOP" \
    && "$BOOT_JDK/bin/javac" -source 8 -target 8 \
        sun/nio/fs/DefaultFileSystemProvider.java java/io/UnixFileSystem.java \
        java/lang/UNIXProcess.java java/lang/AmigaDiag.java \
        com/sun/crypto/provider/GHASH.java \
        sun/security/provider/SHA2.java sun/security/provider/SHA5.java \
        sun/security/provider/SeedGenerator.java 2>/dev/null \
    && "$BOOT_JDK/bin/jar" cf "$OUT/niopatch.zip" \
        sun/nio/fs/DefaultFileSystemProvider.class java/io/UnixFileSystem.class \
        java/lang/UNIXProcess*.class java/lang/AmigaDiag.class \
        com/sun/crypto/provider/GHASH*.class \
        sun/security/provider/SHA2*.class sun/security/provider/SHA5*.class \
        sun/security/provider/SeedGenerator*.class) \
    && echo "  niopatch.zip OK ($(wc -c < "$OUT/niopatch.zip") bytes)" \
    || echo "  niopatch.zip FAIL"
fi

echo "=== libnio.so (sun.nio.fs + file-channel sun.nio.ch) ==="
# Real NIO.2 file ops (java.nio.file.Files.*) + FileChannel.  The fs natives are
# the generic-unix UnixNativeDispatcher (capability probes via dlsym(RTLD_DEFAULT)
# degrade gracefully on clib4) + the Linux dispatcher our (niopatch) provider
# class expects -- its mntent/xattr deps are shimmed (stubs: no mount table, no
# xattrs on AmigaDOS).  Socket/epoll/watch parts of sun.nio.ch are EXCLUDED;
# their natives surface as UnsatisfiedLinkError only if used.
NFS=$J/src/solaris/native/sun/nio/fs
NCH=$J/src/solaris/native/sun/nio/ch

# compat shims for LinuxNativeDispatcher
cat > "$COMPAT/mntent.h" <<'EOF'
/* clib4 has no mount-table API; stub it (FileStore iteration reports nothing). */
#ifndef AMIGA_MNTENT_SHIM_H
#define AMIGA_MNTENT_SHIM_H
#include <stdio.h>
struct mntent { char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts; int mnt_freq, mnt_passno; };
static FILE *setmntent(const char *fn, const char *type) { (void)fn; (void)type; return NULL; }
static struct mntent *getmntent_r(FILE *fp, struct mntent *m, char *buf, int len) { (void)fp; (void)m; (void)buf; (void)len; return NULL; }
static int endmntent(FILE *fp) { (void)fp; return 1; }
#endif
EOF
mkdir -p "$COMPAT/sys"
cat > "$COMPAT/sys/xattr.h" <<'EOF'
/* clib4/AmigaDOS: no extended attributes; stub to ENOTSUP. */
#ifndef AMIGA_XATTR_SHIM_H
#define AMIGA_XATTR_SHIM_H
#include <errno.h>
#include <sys/types.h>
static ssize_t fgetxattr(int fd, const char *n, void *v, size_t s) { (void)fd;(void)n;(void)v;(void)s; errno = ENOSYS; return -1; }
static int fsetxattr(int fd, const char *n, void *v, size_t s, int f) { (void)fd;(void)n;(void)v;(void)s;(void)f; errno = ENOSYS; return -1; }
static int fremovexattr(int fd, const char *n) { (void)fd;(void)n; errno = ENOSYS; return -1; }
static ssize_t flistxattr(int fd, char *l, size_t s) { (void)fd;(void)l;(void)s; errno = ENOSYS; return -1; }
#endif
EOF

# UnixNativeDispatcher.c: (a) join the _ALLBSD *64->base mapping; (b) normalise the
# Unix-absolute "/Volume:" paths (our Amiga path model) at the single arrival
# pattern `(const char*)jlong_to_ptr(xxxAddress)`.  Idempotent.
UND="$NFS/UnixNativeDispatcher.c"
if [ -f "$UND" ] && ! grep -q amiga_path "$UND"; then
    sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g' "$UND"
    perl -pi -e 's/\(const char\*\)jlong_to_ptr\((\w+)\)/amiga_path((const char*)jlong_to_ptr($1))/g' "$UND"
    echo "  adapted UnixNativeDispatcher.c (ALLBSD map + amiga_path)"
fi
UCF="$NFS/UnixCopyFile.c"
if [ -f "$UCF" ] && ! grep -q __amigaos4__ "$UCF"; then
    sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g' "$UCF"
    echo "  adapted UnixCopyFile.c"
fi
# FileDispatcherImpl.c: *64->base mapping + /dev/null -> NIL: (preClose dup target)
FDI="$NCH/FileDispatcherImpl.c"
if [ -f "$FDI" ] && ! grep -q __amigaos4__ "$FDI"; then
    sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g; s@defined(_ALLBSD_SOURCE)@(defined(_ALLBSD_SOURCE) || defined(__amigaos4__))@g; s@"/dev/null"@"NIL:"@' "$FDI"
    echo "  adapted FileDispatcherImpl.c"
fi
FCI="$NCH/FileChannelImpl.c"
if [ -f "$FCI" ] && ! grep -q __amigaos4__ "$FCI"; then
    sed -i 's@defined(_ALLBSD_SOURCE)@(defined(_ALLBSD_SOURCE) || defined(__amigaos4__))@g; s@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g' "$FCI"
    echo "  adapted FileChannelImpl.c"
fi
NT="$NCH/NativeThread.c"
if [ -f "$NT" ] && ! grep -q __amigaos4__ "$NT"; then
    # keep the no-op (non-signalling) variant on amiga: clib4 pthread_kill can't
    # interrupt; blocked-IO interruption degrades to close()-based wakeup.
    sed -i 's@#ifdef __linux__@#if defined(__linux__) \&\& !defined(__amigaos4__)@g' "$NT"
    sed -i 's@defined(_ALLBSD_SOURCE)@(defined(_ALLBSD_SOURCE) \&\& !defined(__amigaos4__))@g' "$NT"
    echo "  adapted NativeThread.c (no-op signalling)"
fi
# NativeThread.c falls into the #error branch on amiga -- give it a benign signal
# number (clib4 sigaction installs the null handler fine; pthread_kill is a
# set-bit no-op, so blocked-IO interruption simply isn't async -- acceptable).
if [ -f "$NT" ] && ! grep -q "INTERRUPT_SIGNAL SIGUSR2" "$NT"; then
    perl -0pi -e 's/#error "missing platform-specific definition here"/#include <pthread.h>\n  #include <signal.h>\n  #define INTERRUPT_SIGNAL SIGUSR2  \/* amiga: benign; not async *\//' "$NT"
    echo "  adapted NativeThread.c (amiga INTERRUPT_SIGNAL)"
fi
# UnixNativeDispatcher rename0: AmigaDOS Rename() will not replace an existing
# destination -- see amiga_rename() in jdkdefs.h.  Files.move(REPLACE_EXISTING)
# reaches AmigaDOS through here, so it needs the same treatment as java.io.
if [ -f "$UND" ] && ! grep -q amiga_rename "$UND"; then
    sed -i 's@if (rename(from, to) == -1)@if (amiga_rename(from, to) == -1)@' "$UND"
    echo "  adapted UnixNativeDispatcher.c (amiga_rename)"
fi

# UnixNativeDispatcher open0/openat0: translate the Java-side (Linux-valued)
# open flags to clib4's encoding.
if [ -f "$UND" ] && ! grep -q amiga_oflags "$UND"; then
    sed -i 's@(int)oflags@amiga_oflags((int)oflags)@g' "$UND"
    echo "  adapted UnixNativeDispatcher.c (amiga_oflags)"
fi
# UnixNativeDispatcher: clib4's struct stat has no st_atim timespec members --
# skip the nanosecond fields on amiga (whole-second timestamps).
if [ -f "$UND" ] && ! grep -q "200809L) || defined(__solaris__)) && !defined(__amigaos4__)" "$UND"; then
    sed -i 's@#if (_POSIX_C_SOURCE >= 200809L) || defined(__solaris__)@#if ((_POSIX_C_SOURCE >= 200809L) || defined(__solaris__)) \&\& !defined(__amigaos4__)@' "$UND"
    echo "  adapted UnixNativeDispatcher.c (no st_atim nsec)"
fi

# FileKey.c maps the *64 LFS names onto the base ones under _ALLBSD_SOURCE only,
# so on amiga it kept calling fstat64 -- which clib4 does not export (its base
# APIs are already 64-bit).  libnio.so is linked without --no-undefined, so this
# survived the build and only bit at runtime, as
# "unable to resolve symbol fstat64 in libnio.so" when sun.nio.ch first mapped a
# file.  Join the _ALLBSD_SOURCE branch, like the libjava md files above.
FKEY="$NCH/FileKey.c"
if [ -f "$FKEY" ] && ! grep -q __amigaos4__ "$FKEY"; then
    sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@g' "$FKEY"
    echo "  adapted FileKey.c (amigaos LFS->base)"
fi

echo "  javah (nio classes from Temurin rt.jar)"
"$BOOT_JDK/bin/javah" -d "$HDR" -classpath "$RTJAR" \
  sun.nio.fs.UnixNativeDispatcher sun.nio.fs.UnixCopyFile sun.nio.fs.LinuxNativeDispatcher \
  sun.nio.ch.FileChannelImpl sun.nio.ch.FileDispatcherImpl sun.nio.ch.FileKey \
  sun.nio.ch.IOUtil sun.nio.ch.NativeThread sun.nio.ch.IOStatus \
  java.lang.Integer java.lang.Long 2>&1 | tail -2

NIOINC="-I $HDR $EXP -I $J/src/share/native/common -I $J/src/solaris/native/common -I $NCH \
 -I $J/src/share/native/sun/nio/ch -I $J/src/share/native/java/io \
 -I $J/src/share/native/java/net -I $J/src/solaris/native/java/net \
 -I $J/src/solaris/native/java/io -include $COMPAT/jdkdefs.h"
rm -rf "$OUT/libnio"; mkdir -p "$OUT/libnio"
nok=0; nfail=0
for c in "$NFS/UnixNativeDispatcher.c" "$NFS/UnixCopyFile.c" "$NFS/LinuxNativeDispatcher.c" \
         "$NCH/FileChannelImpl.c" "$NCH/FileDispatcherImpl.c" "$NCH/FileKey.c" \
         "$NCH/IOUtil.c" "$NCH/NativeThread.c"; do
    [ -f "$c" ] || continue
    if $CC -D_GNU_SOURCE $NIOINC -c "$c" -o "$OUT/libnio/$(basename "$c" .c).o" 2>"$OUT/e"; then
        nok=$((nok+1))
    else
        nfail=$((nfail+1)); echo "  NIO FAIL $(basename "$c")"
        grep -m2 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done
# POSIX calls sun.nio.fs needs that clib4 does not have.  These are NOT dead
# weight: the AmigaOS ELF loader resolves lazily, so a missing symbol is not a
# load failure -- it kills the process the first time that code path runs, which
# is exactly how "unable to resolve symbol fstat64 in libnio.so" showed up.
# futimes/getgr*_r are implemented on top of what clib4 does have; mknod has no
# equivalent and reports ENOSYS, which sun.nio.fs turns into an IOException.
cat > "$OUT/libnio/posix_compat.c" <<'PEOF'
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <grp.h>

/* clib4 has futimens(fd, timespec[2]) but not futimes(fd, timeval[2]) */
int futimes(int fd, const struct timeval tv[2]) {
    struct timespec ts[2];

    if (tv == NULL)
        return futimens(fd, NULL);

    ts[0].tv_sec  = tv[0].tv_sec;
    ts[0].tv_nsec = (long)tv[0].tv_usec * 1000;
    ts[1].tv_sec  = tv[1].tv_sec;
    ts[1].tv_nsec = (long)tv[1].tv_usec * 1000;

    return futimens(fd, ts);
}

/* clib4 has the non-reentrant getgrgid/getgrnam only.  AmigaOS is single-user
   and these return a static, immediately-copied record, so wrapping them is
   sound here; gr_mem is reported empty rather than deep-copied, which is all
   sun.nio.fs.UnixUserPrincipals asks for (it reads gr_name/gr_gid). */
static int amiga_copy_group(struct group *found, struct group *grp, char *buf,
                            size_t buflen, struct group **result) {
    size_t namelen;

    *result = NULL;

    if (found == NULL)
        return 0;                      /* not found: no error, NULL result */

    namelen = (found->gr_name != NULL) ? strlen(found->gr_name) + 1 : 1;
    if (buflen < namelen + sizeof(char *))
        return ERANGE;

    grp->gr_gid = found->gr_gid;
    grp->gr_name = buf;
    if (found->gr_name != NULL)
        memcpy(buf, found->gr_name, namelen);
    else
        buf[0] = '\0';

    /* an empty, NULL-terminated member list placed after the name */
    grp->gr_mem = (char **)(void *)(buf + namelen);
    grp->gr_mem[0] = NULL;

    *result = grp;
    return 0;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
    return amiga_copy_group(getgrgid(gid), grp, buf, buflen, result);
}

int getgrnam_r(const char *name, struct group *grp, char *buf, size_t buflen,
               struct group **result) {
    return amiga_copy_group(getgrnam(name), grp, buf, buflen, result);
}

/* AmigaOS has no FIFOs/device nodes to create */
int mknod(const char *path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    errno = ENOSYS;
    return -1;
}
PEOF
if $CC -D_GNU_SOURCE $NIOINC -c "$OUT/libnio/posix_compat.c" -o "$OUT/libnio/posix_compat.o" 2>"$OUT/e"; then
    echo "  libnio posix compat OK (futimes/getgr*_r/mknod)"
else
    echo "  libnio posix compat FAILED"; head -10 "$OUT/e"; exit 1
fi

# FileDispatcherImpl.seek0: some 8u drops already provide it; only synthesize the
# compat stub when the native source does not.
if ! grep -q 'Java_sun_nio_ch_FileDispatcherImpl_seek0' "$NCH/FileDispatcherImpl.c"; then
cat > "$OUT/libnio/seek0_compat.c" <<'SEOF'
#include "jni.h"
#include "jni_util.h"
#include "jlong.h"
#include "nio_util.h"
#include "sun_nio_ch_IOStatus.h"
#include <unistd.h>
JNIEXPORT jlong JNICALL
Java_sun_nio_ch_FileDispatcherImpl_seek0(JNIEnv *env, jclass clazz,
                                         jobject fdo, jlong offset)
{
    jint fd = fdval(env, fdo);
    off_t result = (offset < 0) ? lseek(fd, 0, SEEK_CUR)
                                : lseek(fd, (off_t)offset, SEEK_SET);
    if (result >= 0)
        return (jlong)result;
    JNU_ThrowIOExceptionWithLastError(env, "lseek failed");
    return sun_nio_ch_IOStatus_THROWN;
}
SEOF
if $CC -D_GNU_SOURCE $NIOINC -c "$OUT/libnio/seek0_compat.c" -o "$OUT/libnio/seek0_compat.o" 2>"$OUT/e"; then
    nok=$((nok+1)); echo "  seek0 compat OK"
else
    echo "  seek0 compat FAIL"; head -4 "$OUT/e"
fi
else
    rm -f "$OUT/libnio/seek0_compat.c" "$OUT/libnio/seek0_compat.o"
fi

echo "  libnio compile: $nok OK, $nfail FAILED"
TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + nfail))
# initInetAddressIDs() and the java.net InetAddress natives used to be faked in
# here, because libnet.so was a stub.  libnet is real now and net_util.c owns
# them, so this must NOT define them too: libnio's copies only cached field IDs
# into libnio's own statics, and if those won the lookup the real net code would
# read addresses through IDs nobody ever filled in.  IOUtil.c's call resolves
# across shared objects at load time, the same way libnio already picks up JNU_*
# from libjava.so.
#
# The link below globs *.o, so deleting the generator is not enough on a tree
# that has built before -- the stale object would keep being linked in and keep
# winning.  Same for the old net_stub.o, which libnet.so no longer uses.
rm -f "$OUT/libnio/inetaddress_compat.c" "$OUT/libnio/inetaddress_compat.o" \
      "$OUT/libnio/net_stub.c" "$OUT/libnio/net_stub.o"
if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
       -o "$OUT/libnio.so" "$OUT"/libnio/*.o 2>"$OUT/e"; then
    echo "  libnio.so OK ($(wc -c < "$OUT/libnio.so") bytes)"
else
    echo "  libnio.so LINK FAIL"; head -10 "$OUT/e"
fi

# Socket defaults: TCP_NODELAY, and a receive window worth having.
#
# Roadshow's defaults are not Linux's, and on any link with latency the receive
# window is what caps a download -- no buffer size in the Java code can lift it.
# Nagle matters the other way round: with it on, a request written before the
# reply is read waits on the peer's delayed ACK, which is HTTP's whole shape.
#
# Raise, never lower: if the stack already offers more than we ask for, leave it
# alone.  Applied to stream sockets only -- a datagram socket has no Nagle and
# no window.  NetTest reports the values, so the effect is measurable rather
# than assumed.
PSI="$J/src/solaris/native/java/net/PlainSocketImpl.c"
if [ -f "$PSI" ] && ! grep -q "AMIGA_SOCK_DEFAULTS" "$PSI"; then
    python3 - "$PSI" <<'AMIGA_PSI_PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = """    }

    (*env)->SetIntField(env, fdObj, IO_fd_fdID, fd);
}"""
new = """    }

#ifdef __amigaos4__ /* AMIGA_SOCK_DEFAULTS */
    if (stream) {
        int on = 1;
        int want = 64 * 1024;
        int cur = 0;
        socklen_t len = sizeof(cur);

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof(on));

        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&cur, &len) == 0
                && cur < want) {
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&want, sizeof(want));
        }

        cur = 0;
        len = sizeof(cur);
        if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&cur, &len) == 0
                && cur < want) {
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&want, sizeof(want));
        }
    }
#endif

    (*env)->SetIntField(env, fdObj, IO_fd_fdID, fd);
}"""
if s.count(old) == 1:
    open(p, "w").write(s.replace(old, new, 1))
    print("=== adapted PlainSocketImpl.c (amigaos: TCP_NODELAY + 64K buffers) ===")
else:
    print("  WARN: socketCreate tail not uniquely found; socket defaults NOT applied")
AMIGA_PSI_PY
fi

echo "=== libnet.so (java.net) ==="
# The real java.net natives.  They were a stub until now, which is why the first
# socket use died with "UnsatisfiedLinkError: initProto" -- PlainSocketImpl's
# <clinit> calls it, so nothing network-facing could even initialise.
#
# IPv6 is compiled OUT with -DDONT_ENABLE_IPV6: it makes net_util_md.c's
# IPv6_supported() return JNI_FALSE, so ipv6_available() is false everywhere and
# java.net picks Inet4AddressImpl.  clib4 DOES expose AF_INET6, so the normal
# runtime probe would answer "yes" and hand the runtime to an IPv6 path the
# AmigaOS stack cannot carry.  Inet6AddressImpl.c is therefore not built either
# (it also wants <netinet/icmp6.h>, which clib4 has no equivalent of), and with
# ipv6_available() false its natives are unreachable.
#
# NetworkInterface.c is not built: its guts are inside __linux__ /
# _ALLBSD_SOURCE blocks (enumIPv4Interfaces/getFlags/getMacAddress/getMTU), so
# it would compile to a shell of unresolved calls.  src/openjdk/amiga_net.c
# keeps the long-standing "no enumerable interfaces" stubs instead, plus the
# NET_* blocking-IO wrappers and getErrorString.
NETSRC_SOL="PlainSocketImpl SocketInputStream SocketOutputStream Inet4AddressImpl \
InetAddressImplFactory PlainDatagramSocketImpl net_util_md"
NETSRC_SH="net_util InetAddress Inet4Address Inet6Address DatagramPacket"

# net_util_md.c wants SysV <values.h> purely for the integer limits.
cat > "$COMPAT/values.h" <<'VEOF'
/* SysV <values.h> shim -- net_util_md.c only wants the integer limits. */
#ifndef _AMIGA_VALUES_H
#define _AMIGA_VALUES_H
#include <limits.h>
#define MAXINT   INT_MAX
#define MAXSHORT SHRT_MAX
#define MAXLONG  LONG_MAX
#endif
VEOF

for c in java.net.PlainSocketImpl java.net.SocketOptions java.net.SocketInputStream \
         java.net.SocketOutputStream java.net.Inet4AddressImpl java.net.Inet6AddressImpl \
         java.net.InetAddressImplFactory java.net.NetworkInterface java.net.DatagramPacket \
         java.net.PlainDatagramSocketImpl java.net.InetAddress java.net.Inet4Address \
         java.net.Inet6Address; do
    "$BOOT_JDK/bin/javah" -d "$HDR" -classpath "$RTJAR" "$c" >/dev/null 2>&1 || true
done

NETINC="-I $HDR $EXP -I $J/src/share/native/common -I $J/src/solaris/native/common \
 -I $J/src/share/native/java/net -I $J/src/solaris/native/java/net \
 -I $J/src/solaris/native/java/io"
rm -rf "$OUT/libnet"; mkdir -p "$OUT/libnet"
netok=0; netfail=0
for f in $NETSRC_SOL; do
    if $CC -D_GNU_SOURCE -DDONT_ENABLE_IPV6 $NETINC \
           -c "$J/src/solaris/native/java/net/$f.c" -o "$OUT/libnet/$f.o" 2>"$OUT/e"; then
        netok=$((netok+1))
    else
        netfail=$((netfail+1)); echo "  NET FAIL $f.c"
        grep -m2 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done
for f in $NETSRC_SH; do
    if $CC -D_GNU_SOURCE -DDONT_ENABLE_IPV6 $NETINC \
           -c "$J/src/share/native/java/net/$f.c" -o "$OUT/libnet/sh_$f.o" 2>"$OUT/e"; then
        netok=$((netok+1))
    else
        netfail=$((netfail+1)); echo "  NET FAIL $f.c"
        grep -m2 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done
# sun.net.spi.DefaultProxySelector.
#
# Missing it was not a proxy problem: ProxySelector's own class initialiser does
# Class.forName("sun.net.spi.DefaultProxySelector"), so the UnsatisfiedLinkError
# from its <clinit> took out java.net.ProxySelector itself, and with it anything
# that opens a URL.
#
# Built from the unmodified Unix source.  Its gconf and gproxy paths are inside
# __linux__/_ALLBSD_SOURCE, so on AmigaOS init() dlopens two libraries that are
# not there, gets nothing, and returns JNI_FALSE.  That is the correct answer
# rather than a degraded one: it means "this platform has no system-wide proxy
# configuration", and DefaultProxySelector then reads the http.proxyHost /
# https.proxyHost properties like any other JVM.
"$BOOT_JDK/bin/javah" -d "$HDR" -classpath "$RTJAR" sun.net.spi.DefaultProxySelector \
    >/dev/null 2>&1 || true
if $CC -D_GNU_SOURCE -DDONT_ENABLE_IPV6 $NETINC \
       -c "$J/src/solaris/native/sun/net/spi/DefaultProxySelector.c" \
       -o "$OUT/libnet/DefaultProxySelector.o" 2>"$OUT/e"; then
    netok=$((netok+1)); echo "  DefaultProxySelector.c OK"
else
    netfail=$((netfail+1)); echo "  DefaultProxySelector.c FAIL"
    grep -m4 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
fi

if $CC -D_GNU_SOURCE -DDONT_ENABLE_IPV6 $NETINC \
       -c "$PROJECT_ROOT/src/openjdk/amiga_net.c" -o "$OUT/libnet/amiga_net.o" 2>"$OUT/e"; then
    netok=$((netok+1)); echo "  amiga_net.c OK"
else
    netfail=$((netfail+1)); echo "  amiga_net.c FAIL"
    grep -m4 -E "error:" "$OUT/e" | sed 's/^/        /'
fi
echo "  libnet compile: $netok OK, $netfail FAILED"
TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + netfail))

if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=JAVA:Sobjs \
       -o "$OUT/libnet.so" "$OUT"/libnet/*.o 2>"$OUT/e"; then
    echo "  libnet.so OK ($(wc -c < "$OUT/libnet.so") bytes)"
else
    echo "  libnet.so LINK FAIL"; head -10 "$OUT/e"
fi

# The two symbols whose absence used to be fatal, and the one libnio now imports
# from here instead of defining itself.  Cheap, and it catches a silent
# regression in the source list above before it reaches AmigaOS.
for sym in Java_java_net_PlainSocketImpl_initProto \
           Java_java_net_PlainSocketImpl_socketCreate initInetAddressIDs \
           Java_sun_net_spi_DefaultProxySelector_init; do
    if ppc-amigaos-nm -D --defined-only "$OUT/libnet.so" 2>/dev/null | grep -qw "$sym"; then
        echo "    $sym OK"
    else
        echo "    MISSING $sym -- java.net will not work"
    fi
done

# Every java.net/sun.net native rt.jar DECLARES, against what libnet DEFINES.
#
# The same check libmanagement gets, and for the same reason: a native that is
# never implemented costs nothing until something touches its class, and these
# classes call their natives from STATIC INITIALISERS -- so the failure is not
# "this feature is missing" but "this class cannot be loaded", surfacing far
# from the cause.  sun.net.spi.DefaultProxySelector was reported as
# "UnsatisfiedLinkError: init" from java.net.ProxySelector's <clinit>, which
# does Class.forName on it: one absent native took out ProxySelector itself.
#
# The expected-missing list is the point of the check -- everything else is a
# genuine gap.  IPv6 is compiled out (-DDONT_ENABLE_IPV6), so Inet6AddressImpl
# is never instantiated; NetworkInterface's per-interface queries cannot be
# reached because getAll() returns an empty array and every getBy*0 returns
# NULL, so no NetworkInterface object exists to call them on; SdpSupport is
# Solaris InfiniBand.
NET_NATIVE_CLASSES="java.net.InetAddress java.net.InetAddressImplFactory \
java.net.Inet4AddressImpl java.net.Inet4Address java.net.Inet6Address \
java.net.PlainSocketImpl java.net.DatagramPacket java.net.PlainDatagramSocketImpl \
java.net.SocketOutputStream java.net.SocketInputStream \
sun.net.PortConfig sun.net.ExtendedOptionsImpl \
sun.net.dns.ResolverConfigurationImpl sun.net.spi.DefaultProxySelector"

NETHDR="$OUT/nethdr"; rm -rf "$NETHDR"; mkdir -p "$NETHDR"
for c in $NET_NATIVE_CLASSES; do
    "$BOOT_JDK/bin/javah" -d "$NETHDR" -classpath "$RTJAR" "$c" >/dev/null 2>&1 || true
done
if [ -n "$(ls "$NETHDR" 2>/dev/null)" ]; then
    # LC_ALL=C on every stage, not just the first: it has to reach `sort`, or
    # its collation and comm's disagree and comm rejects the input.
    grep -h "^JNIEXPORT" -A1 "$NETHDR"/*.h 2>/dev/null \
        | grep -oE "Java_[A-Za-z0-9_]+" | LC_ALL=C sort -u > "$OUT/e.netdecl"
    ppc-amigaos-nm -D --defined-only "$OUT/libnet.so" 2>/dev/null \
        | awk '{print $3}' | LC_ALL=C sort -u > "$OUT/e.netdef"
    netmissing=$(LC_ALL=C comm -23 "$OUT/e.netdecl" "$OUT/e.netdef" || true)
    if [ -n "$netmissing" ]; then
        echo "  java.net natives declared by rt.jar but NOT implemented:"
        echo "$netmissing" | sed 's/^/    MISSING /'
    else
        echo "  all $(wc -l < "$OUT/e.netdecl") java.net natives rt.jar declares are implemented"
    fi
fi
# Anything platform-half that amiga_net.c forgot would show up here.  getErrorString
# is expected to be missing: it is jni_util.h's, exported by libjava.so, and gets
# resolved across shared objects at load time like the JNU_* calls.
echo "  undefined check:"; ppc-amigaos-nm -D -u "$OUT/libnet.so" 2>/dev/null \
    | awk '{print $2}' | grep -E "^(NET_|ni_|enum|getFlags|getMacAddress|getMTU)" \
    | sort -u | sed 's/^/    UNRESOLVED /'
ppc-amigaos-nm -D --defined-only "$OUT/libjava.so" 2>/dev/null | grep -qw getErrorString \
    || echo "    UNRESOLVED getErrorString (not exported by libjava.so either)"

echo "=== libmanagement.so (java.lang.management) ==="
# Was simply missing, so ManagementFactory.getRuntimeMXBean() -- a routine call,
# and one InvoiceX makes during startup -- died in ManagementFactoryHelper's
# class initialiser with "no management in java.library.path".
#
# JamVM does implement the VM side (classlib/openjdk/management.c exports a
# jmmInterface_1_ and JVM_GetManagement returns it for JMM_VERSION_1_0), so the
# shared natives work as they are.  What is missing on AmigaOS is the PLATFORM
# half: OperatingSystemImpl.c reads /proc, <sys/swap.h> and statvfs64, and
# FileSystemImpl.c wants statvfs64 as well.  src/openjdk/amiga_management.c
# stands in for both -- the same treatment amiga_net.c gives NetworkInterface.c.
MGMT_SH="ClassLoadingImpl DiagnosticCommandImpl Flag GarbageCollectorImpl \
GcInfoBuilder HotSpotDiagnostic HotspotThread MemoryImpl MemoryManagerImpl \
MemoryPoolImpl ThreadImpl VMManagementImpl management"

for c in ClassLoadingImpl DiagnosticCommandImpl Flag GarbageCollectorImpl \
         GcInfoBuilder HotSpotDiagnostic HotspotThread MemoryImpl \
         MemoryManagerImpl MemoryPoolImpl ThreadImpl VMManagementImpl \
         FileSystemImpl OperatingSystemImpl; do
    "$BOOT_JDK/bin/javah" -d "$HDR" -classpath "$RTJAR" "sun.management.$c" >/dev/null 2>&1 || true
done

MGMTINC="-I $HDR $EXP -I $J/src/share/native/common -I $J/src/solaris/native/common \
 -I $J/src/share/native/sun/management"
rm -rf "$OUT/libmanagement"; mkdir -p "$OUT/libmanagement"
mgmtok=0; mgmtfail=0
for f in $MGMT_SH; do
    if $CC $MGMTINC -c "$J/src/share/native/sun/management/$f.c" \
           -o "$OUT/libmanagement/$f.o" 2>"$OUT/e"; then
        mgmtok=$((mgmtok+1))
    else
        mgmtfail=$((mgmtfail+1)); echo "  MGMT FAIL $f.c"
        grep -m2 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
    fi
done
if $CC $MGMTINC -c "$PROJECT_ROOT/src/openjdk/amiga_management.c" \
       -o "$OUT/libmanagement/amiga_management.o" 2>"$OUT/e"; then
    mgmtok=$((mgmtok+1)); echo "  amiga_management.c OK"
else
    mgmtfail=$((mgmtfail+1)); echo "  amiga_management.c FAIL"
    grep -m4 -E "error:" "$OUT/e" | sed 's/^/        /'
fi
echo "  libmanagement compile: $mgmtok OK, $mgmtfail FAILED"
TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + mgmtfail))

if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=JAVA:Sobjs \
       -o "$OUT/libmanagement.so" "$OUT"/libmanagement/*.o 2>"$OUT/e"; then
    echo "  libmanagement.so OK ($(wc -c < "$OUT/libmanagement.so") bytes)"
else
    echo "  libmanagement.so LINK FAIL"; head -10 "$OUT/e"
fi

# Every native rt.jar DECLARES must be DEFINED here.
#
# This check is not decoration.  The rt.jar we ship is the boot JDK's (8u502),
# which is newer than the OpenJDK drop in vendor/: 8u502 renamed most of
# OperatingSystemImpl's natives to a trailing 0 and added
# ThreadImpl.getTotalThreadAllocatedMemory.  A native whose name does not match
# its declaration compiles cleanly and simply never binds, so building against
# the source drop and assuming the two agree would produce a library that loads
# and then throws UnsatisfiedLinkError on first use -- the exact failure this
# whole block exists to remove.
if [ -s "$OUT/libmanagement.so" ]; then
    # LC_ALL=C has to reach `sort`, not just the first command in the pipeline,
    # or its collation and comm's disagree and comm rejects the input.
    grep -h "^JNIEXPORT" -A1 "$HDR"/sun_management_*.h 2>/dev/null \
        | grep -oE "Java_sun_management_[A-Za-z0-9_]+" | LC_ALL=C sort -u > "$OUT/e.declared"
    ppc-amigaos-nm -D --defined-only "$OUT/libmanagement.so" 2>/dev/null \
        | grep -oE "Java_sun_management_[A-Za-z0-9_]+" | LC_ALL=C sort -u > "$OUT/e.defined"
    missing=$(LC_ALL=C comm -23 "$OUT/e.declared" "$OUT/e.defined" || true)
    if [ -n "$missing" ]; then
        echo "  MGMT natives declared by rt.jar but NOT implemented:"
        echo "$missing" | sed 's/^/    MISSING /'
    else
        echo "  all $(wc -l < "$OUT/e.declared") natives rt.jar declares are implemented"
    fi
fi

echo "=== libamigacrypto.so (AES counter mode, for the extension loader) ==="
# Its own library, not part of libjava.so, and the reason is class loaders
# rather than modularity.  ClassLoader.findNative looks only at the libraries
# owned by the calling class's own loader, with no fallback to the system list.
# GCTR comes from lib/ext/sunjce_provider.jar through the extension loader,
# whose library list is empty -- so a native in libjava.so, loaded by the
# bootstrap loader, is unreachable from it no matter how correct the code is.
# GCTR loads this one itself, which registers it under the right loader.
rm -rf "$OUT/libamigacrypto"; mkdir -p "$OUT/libamigacrypto"
if $CC $LJINC -c "$PROJECT_ROOT/src/openjdk/amiga_aes.c" \
       -o "$OUT/libamigacrypto/amiga_aes.o" 2>"$OUT/e"; then
    if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=JAVA:Sobjs \
           -o "$OUT/libamigacrypto.so" "$OUT"/libamigacrypto/*.o 2>"$OUT/e"; then
        echo "  libamigacrypto.so OK ($(wc -c < "$OUT/libamigacrypto.so") bytes)"
    else
        echo "  libamigacrypto.so LINK FAIL"; head -10 "$OUT/e"
    fi
else
    echo "  amiga_aes.c FAIL"
    grep -m3 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
fi

echo "=== libsunec.so (SunEC: elliptic curve crypto) ==="
# Without this the SunEC provider cannot register (its class initialiser does
# System.loadLibrary("sunec")), so java.security's security.provider.3 is
# skipped and the runtime has no EC at all.  Every modern TLS server then
# rejects us: TLS 1.3 negotiates over the EC named groups, and TLS 1.2 servers
# are typically ECDHE-only, so the ClientHello carries nothing they accept and
# the handshake dies with "Received fatal alert: handshake_failure".  Verified
# by removing SunEC from a host JDK 8 and reproducing the identical failure.
#
# The NSS-derived sources build against clib4 unchanged; only their platform
# conditionals need to learn about us.  Each guarded on its own marker: the
# sys/systm.h edit is not idempotent (its pattern is a prefix of its own
# replacement), so re-running unguarded would keep appending.
EC=$J/src/share/native/sun/security/ec
if [ -d "$EC" ]; then
    # ecc_impl.h has per-platform blocks for __linux__/_ALLBSD_SOURCE/AIX/_WIN32
    # and nothing else; join the BSD one for ulong_t/boolean_t/B_FALSE (it is
    # the variant that does NOT redefine uint8_t, which clib4 already has).
    if ! grep -q "__amigaos4__" "$EC/impl/ecc_impl.h"; then
        sed -i 's@#ifdef _ALLBSD_SOURCE@#if defined(_ALLBSD_SOURCE) || defined(__amigaos4__)@' \
            "$EC/impl/ecc_impl.h"
        echo "  adapted ecc_impl.h (amigaos joins the BSD typedefs)"
    fi
    # <sys/systm.h> is a Solaris kernel header; the guard excludes linux and bsd.
    for f in ecdecode.c oid.c secitem.c; do
        [ -f "$EC/impl/$f" ] || continue
        if ! grep -q "__amigaos4__" "$EC/impl/$f"; then
            sed -i 's@#if !defined(__linux__) \&\& !defined(_ALLBSD_SOURCE)@#if !defined(__linux__) \&\& !defined(_ALLBSD_SOURCE) \&\& !defined(__amigaos4__)@' \
                "$EC/impl/$f"
        fi
    done

    for c in sun.security.ec.ECKeyPairGenerator sun.security.ec.ECDSASignature \
             sun.security.ec.ECDHKeyAgreement; do
        "$BOOT_JDK/bin/javah" -d "$HDR" \
            -classpath "$RTJAR:$BOOT_JDK/jre/lib/ext/sunec.jar" "$c" >/dev/null 2>&1 || true
    done

    # -DMP_API_COMPATIBLE -DNSS_ECC_MORE_THAN_SUITE_B are what the OpenJDK
    # makefile passes (jdk/make/lib/SecurityLibraries.gmk).  No jdkdefs.h
    # force-include here: SunEC touches no files, so it needs no amiga_path.
    ECINC="-I $HDR $EXP -I $J/src/share/native/common -I $EC -I $EC/impl"
    ECDEFS="-DMP_API_COMPATIBLE -DNSS_ECC_MORE_THAN_SUITE_B"
    rm -rf "$OUT/libsunec"; mkdir -p "$OUT/libsunec"
    ecok=0; ecfail=0
    for f in "$EC"/impl/*.c; do
        if $CC $ECDEFS $ECINC -c "$f" -o "$OUT/libsunec/$(basename "${f%.c}").o" 2>"$OUT/e"; then
            ecok=$((ecok+1))
        else
            ecfail=$((ecfail+1)); echo "  EC FAIL $(basename "$f")"
            grep -m2 -E "error:|No such file" "$OUT/e" | sed 's/^/        /'
        fi
    done
    # ECC_JNI is the one C++ file in the whole runtime; LANG := C++ upstream too.
    # It needs exactly two things from the C++ runtime -- operator new[] and
    # operator delete[] -- which src/openjdk/amiga_cxx_alloc.cpp supplies, so we
    # link no libstdc++ at all.  See that file for why not.
    for cpp in "$EC/ECC_JNI.cpp" "$PROJECT_ROOT/src/openjdk/amiga_cxx_alloc.cpp"; do
        if ppc-amigaos-g++ -mcrt=clib4 -fPIC -O2 -w -fno-exceptions -fno-rtti \
               $ECDEFS $ECINC -c "$cpp" \
               -o "$OUT/libsunec/$(basename "${cpp%.cpp}").o" 2>"$OUT/e"; then
            ecok=$((ecok+1))
        else
            ecfail=$((ecfail+1)); echo "  EC FAIL $(basename "$cpp")"
            grep -m4 "error:" "$OUT/e" | sed 's/^/        /'
        fi
    done
    echo "  libsunec compile: $ecok OK, $ecfail FAILED"
    TOTAL_COMPILE_FAIL=$((${TOTAL_COMPILE_FAIL:-0} + ecfail))

    # ECC_JNI.cpp uses new[]/delete[], so the link pulls operator new[]
    # (_Znaj) and operator delete[] (_ZdaPv) from libstdc++.  Upstream just adds
    # -lstdc++; here that would mean shipping another sobj for two symbols, so
    # link them in statically instead and keep Sobjs/ as it is.
    #
    # -static-libstdc++ is what actually delivers that.  Without it g++ links
    # libstdc++.so dynamically, and since Sobjs/ ships only libc/libpthread/
    # libm/librt/libz/libgcc, loading libsunec.so then depended on a libstdc++.so
    # from wherever the machine happened to have one -- and on the AmigaOS ELF
    # loader failing with "Unresolved symbol: __gthread_mutex_destroy in
    # libstdc++.so".  Linked statically the gthread references are not merely
    # satisfied, they are absent: nothing pulls that threading layer in, and
    # libsunec.so needs only libm, libgcc and libc, all of which we ship.
    if ppc-amigaos-g++ -mcrt=clib4 -fPIC -shared -static-libstdc++ \
           -Wl,-rpath=JAVA:Sobjs \
           -o "$OUT/libsunec.so" "$OUT"/libsunec/*.o 2>"$OUT/e"; then
        echo "  libsunec.so OK ($(wc -c < "$OUT/libsunec.so") bytes)"
    else
        echo "  libsunec.so LINK FAIL"; head -10 "$OUT/e"
    fi
    ecn=$(ppc-amigaos-nm -D --defined-only "$OUT/libsunec.so" 2>/dev/null \
          | grep -c "Java_sun_security_ec")
    [ "$ecn" = 5 ] && echo "    5 SunEC natives OK" \
                   || echo "    expected 5 SunEC natives, found $ecn -- EC will not work"
else
    echo "  SKIP: $EC not present"
fi

echo "=== built ==="; ls -l "$OUT"/*.a "$OUT"/*.so "$OUT"/niopatch.zip 2>/dev/null

#
# A file that did not compile must not leave with a library still being shipped.
#
# It used to.  Each library counts its failures and carries on, so a broken
# source dropped one object and linked the rest -- the .so appeared, packaging
# accepted it, and the change under test was simply absent from the runtime.
# That is the worst shape a build failure can take: everything looks like it
# worked and the next round of testing is spent on the previous binary.  It cost
# one, on a single-token mistake (SYS_CopyVars for NP_CopyVars) sitting in a
# screenful of unused-parameter warnings.
#
# Reported at the END, after everything has been attempted, so one run shows
# every error rather than stopping at the first.
if [ "${TOTAL_COMPILE_FAIL:-0}" -gt 0 ]; then
    echo ""
    echo "########################################################################"
    echo "  BUILD FAILED: $TOTAL_COMPILE_FAIL source file(s) did not compile."
    echo "  The .so files above are INCOMPLETE -- do not package or install them."
    echo "  Search this output for 'FAIL' to find which."
    echo "########################################################################"
    exit 1
fi