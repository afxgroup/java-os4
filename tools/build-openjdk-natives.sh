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
EXCL="check_code.c check_format.c jspawnhelper.c java_props_macosx.c"
mkdir -p "$OUT/libjava"
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
echo "  libjava compile: $ok OK, $fail FAILED"

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
mkdir -p "$OUT/libzip"
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
        sun/nio/fs/DefaultFileSystemProvider.java java/io/UnixFileSystem.java 2>/dev/null \
    && "$BOOT_JDK/bin/jar" cf "$OUT/niopatch.zip" \
        sun/nio/fs/DefaultFileSystemProvider.class java/io/UnixFileSystem.class) \
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
mkdir -p "$OUT/libnio"
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
# IOUtil.c calls initInetAddressIDs() (defined in libnet's net_util.c on Unix).
# Our libnet.so is just a stub, so provide the symbol in libnio.so together with
# the InetAddress* init helpers it needs.
cat > "$OUT/libnio/inetaddress_compat.c" <<'IEOF'
#include "jni.h"
#include "jni_util.h"

static int ia_initialized = 0;
JNIEXPORT void JNICALL
Java_java_net_InetAddress_init(JNIEnv *env, jclass cls) {
    jclass c, iac;
    (void)cls;
    if (ia_initialized) return;
    c = (*env)->FindClass(env, "java/net/InetAddress");
    CHECK_NULL(c);
    iac = (*env)->FindClass(env, "java/net/InetAddress$InetAddressHolder");
    CHECK_NULL(iac);
    (void)(*env)->GetFieldID(env, c, "holder", "Ljava/net/InetAddress$InetAddressHolder;");
    (void)(*env)->GetStaticFieldID(env, c, "preferIPv6Address", "Z");
    (void)(*env)->GetFieldID(env, iac, "address", "I");
    (void)(*env)->GetFieldID(env, iac, "family", "I");
    (void)(*env)->GetFieldID(env, iac, "hostName", "Ljava/lang/String;");
    (void)(*env)->GetFieldID(env, iac, "originalHostName", "Ljava/lang/String;");
    ia_initialized = 1;
}

static int ia4_initialized = 0;
JNIEXPORT void JNICALL
Java_java_net_Inet4Address_init(JNIEnv *env, jclass cls) {
    jclass c;
    (void)cls;
    if (ia4_initialized) return;
    c = (*env)->FindClass(env, "java/net/Inet4Address");
    CHECK_NULL(c);
    (void)(*env)->GetMethodID(env, c, "<init>", "()V");
    ia4_initialized = 1;
}

static int ia6_initialized = 0;
JNIEXPORT void JNICALL
Java_java_net_Inet6Address_init(JNIEnv *env, jclass cls) {
    jclass c, h;
    (void)cls;
    if (ia6_initialized) return;
    c = (*env)->FindClass(env, "java/net/Inet6Address");
    CHECK_NULL(c);
    h = (*env)->FindClass(env, "java/net/Inet6Address$Inet6AddressHolder");
    CHECK_NULL(h);
    (void)(*env)->GetFieldID(env, c, "holder6", "Ljava/net/Inet6Address$Inet6AddressHolder;");
    (void)(*env)->GetFieldID(env, h, "ipaddress", "[B");
    (void)(*env)->GetFieldID(env, h, "scope_id", "I");
    (void)(*env)->GetFieldID(env, c, "cached_scope_id", "I");
    (void)(*env)->GetFieldID(env, h, "scope_id_set", "Z");
    (void)(*env)->GetFieldID(env, h, "scope_ifname", "Ljava/net/NetworkInterface;");
    (void)(*env)->GetMethodID(env, c, "<init>", "()V");
    ia6_initialized = 1;
}

JNIEXPORT void JNICALL
initInetAddressIDs(JNIEnv *env) {
    static int initialized = 0;
    if (!initialized) {
        Java_java_net_InetAddress_init(env, 0);
        Java_java_net_Inet4Address_init(env, 0);
        Java_java_net_Inet6Address_init(env, 0);
        initialized = 1;
    }
}
IEOF
if $CC -D_GNU_SOURCE $NIOINC -c "$OUT/libnio/inetaddress_compat.c" -o "$OUT/libnio/inetaddress_compat.o" 2>"$OUT/e"; then
    echo "  inetaddress compat OK"
else
    echo "  inetaddress compat FAIL"; head -10 "$OUT/e"
fi

if ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
       -o "$OUT/libnio.so" "$OUT"/libnio/*.o 2>"$OUT/e"; then
    echo "  libnio.so OK ($(wc -c < "$OUT/libnio.so") bytes)"
else
    echo "  libnio.so LINK FAIL"; head -10 "$OUT/e"
fi

# stub libnet.so: sun.nio.ch.IOUtil.load() does loadLibrary("net") before "nio";
# an empty lib satisfies it (real java.net natives are a future work item --
# they'd surface as UnsatisfiedLinkError on first socket use).
# We must also provide InetAddressImplFactory.isIPv6Supported() here: it is
# resolved from libnet during java.net.InetAddress clinit, before libnio is
# loaded, and a missing symbol breaks IOUtil/FileChannelImpl clinit.
cat > "$OUT/libnio/net_stub.c" <<'NEOF'
#include "jni.h"
static int amiga_libnet_stub;

JNIEXPORT jboolean JNICALL
Java_java_net_InetAddressImplFactory_isIPv6Supported(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return JNI_FALSE;
}

/* java.net.NetworkInterface -- "this machine has no enumerable interfaces".
 *
 * These are NOT optional for a headless-looking app: NetworkInterface.<clinit>
 * calls init(), so with the natives missing the class initialiser throws
 * UnsatisfiedLinkError.  That is an Error, not an Exception, so it sails
 * straight through sun.security.provider.SeedGenerator's catch(Exception) in
 * addNetworkAdapterInfo() -- which means SecureRandom's seeder cannot
 * initialise, and everything downstream of it dies.  Files.createTempFile()
 * draws from SecureRandom, so merely reading an icon through ImageIO was enough
 * to kill Invoicex with "UnsatisfiedLinkError: init".
 *
 * getAll() returns an EMPTY array rather than NULL: getNetworkInterfaces() is
 * specified to return null when there are none, and callers that skip the null
 * check would then NPE.  An empty enumeration just yields nothing.  The lookups
 * return null, which is their documented "not found" answer.  Enumerating real
 * interfaces would mean a bsdsocket.library implementation -- the rest of
 * libnet is a stub too, so that is a separate job. */
JNIEXPORT void JNICALL
Java_java_net_NetworkInterface_init(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
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
NEOF
$CC -D_GNU_SOURCE $EXP -c "$OUT/libnio/net_stub.c" -o "$OUT/libnio/net_stub.o" 2>/dev/null
ppc-amigaos-gcc -mcrt=clib4 -fPIC -shared -Wl,-rpath=SYS:Test \
    -o "$OUT/libnet.so" "$OUT/libnio/net_stub.o" 2>/dev/null \
    && echo "  libnet.so (stub) OK ($(wc -c < "$OUT/libnet.so") bytes)"

echo "=== built ==="; ls -l "$OUT"/*.a "$OUT"/*.so "$OUT"/niopatch.zip 2>/dev/null