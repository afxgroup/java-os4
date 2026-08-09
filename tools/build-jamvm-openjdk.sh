#!/bin/sh
# clib4 build + link of JamVM with the OPENJDK 8 classlib (Phase 2).
#
# Variant of build-jamvm-clib4.sh that builds JamVM against src/classlib/openjdk
# instead of src/classlib/gnuclasspath, so it runs the OpenJDK 8 class library
# (Temurin 8 rt.jar) rather than GNU Classpath.  Differences vs the clib4 script:
#   - INC/classlib loop point at classlib/openjdk (16 .c files).
#   - Defines: -DOPENJDK_VERSION=8 -DJSR292 -DJSR308 -DJSR335 -DJSR901
#     (configure.ac sets these for --with-java-runtime-library=openjdk8).
#   - DROP -DSHARED_CHAR_BUFFERS: it's a gnuclasspath/openjdk6-only thing; OpenJDK 8
#     java.lang.String has only value[] (no offset/count), so createString must NOT
#     use the shared-buffer layout.
#   - Output is build/jamvm-openjdk (keeps the working gnuclasspath build/jamvm).
# Everything else (clib4 overlay, -use-dynld, rpath=JAVA:Sobjs, libs) is identical.
#
#   docker run --rm -v "<proj>:/work" -w /work \
#       javaos4-build:latest sh /work/tools/build-jamvm-openjdk.sh
# (clib4 is the in-repo clib4/ submodule, built first by tools/build-clib4.sh.)
set -e
. "$(dirname "$0")/build-env.sh"

# --- overlay local clib4 over the SDK clib4 --------------------------------
SDKCLIB4=$SDK_CLIB4
if [ -n "${CLIB4_BUILD_ROOT:-}" ] && [ -d "$CLIB4_BUILD_ROOT/build/lib" ]; then
    cp -f "$CLIB4_BUILD_ROOT"/build/lib/*.a "$SDKCLIB4/lib/" 2>/dev/null || true
    cp -f "$CLIB4_BUILD_ROOT"/build/lib/*.o "$SDKCLIB4/lib/" 2>/dev/null || true
    cp -rf "$CLIB4_BUILD_ROOT"/library/include/* "$SDKCLIB4/include/" 2>/dev/null || true
    echo "=== overlaid in-repo clib4 submodule (clib4/build/lib + library/include) ==="
fi

cd "$PROJECT_ROOT/vendor/jamvm/src"

# Replicate configure's src/arch.h link for direct script-driven builds.
if [ ! -f arch.h ]; then
    cp -f arch/powerpc.h arch.h
fi

# Disable legacy JAmiga shim headers that shadow clib4/POSIX headers.
for h in pthread signal sched stdlib time; do
    if [ -f "os/amiga/$h.h" ] && [ ! -f "os/amiga/$h.h.jamiga-shim-bak" ]; then
        mv "os/amiga/$h.h" "os/amiga/$h.h.jamiga-shim-bak"
    fi
done

# Install the openjdk classlib headers as src/classlib*.h -- this is what
# configure would symlink for the chosen classlib.  `-I .` resolves
# #include "classlib.h" to src/ FIRST, so these MUST match the classlib we build;
# otherwise the (gnuclasspath) versions left in src/ shadow the openjdk ones
# (e.g. missing the JSR292 MethodHandle/invoke/invokeExact symbols).
echo "=== installing openjdk classlib headers into src/ ==="
for h in classlib.h classlib-defs.h classlib-excep.h classlib-symbol.h; do
    cp -f "classlib/openjdk/$h" "$h"
done

# AmigaOS java.home resolution (JAVA_HOME env -> PROGDIR: -> libjvm.so path ->
# JAVA:) now lives in classlib/openjdk/properties.c itself (via the docs patch);
# no build-time source injection needed here anymore.

INC="-I . -I $PROJECT_ROOT/src/jamvm -I $PROJECT_ROOT/src/openjdk -I os/amiga -I os/amiga/powerpc -I interp -I interp/engine -I classlib/openjdk"
# VERSION_* are JamVM's version (2.0.1), normally from config.h; the openjdk
# classlib's jvm.c (JVM_GetVersionInfo) needs them (gnuclasspath has no jvm.c).
# -fPIC: the VM objects go into the shared libjvm.so (see linking section).
# Interpreter engine level (see src/config.h): 0 switch, 1 threaded, 2 direct,
# 3 inline-threaded (the code-copying engine).  Upstream defaults to 3 on
# powerpc; we default to 2 because 3 miscompiles here -- docs/ppc32-performance.md.
JAMVM_ENGINE=${JAMVM_ENGINE:-2}
CFLAGS="-mcrt=clib4 -O2 -W -Wall -fPIC -D__USE_INLINE__ -DUSE_ZIP \
 -DJAMVM_ENGINE_LEVEL=$JAMVM_ENGINE \
 -DOPENJDK_VERSION=8 -DJSR292 -DJSR308 -DJSR335 -DJSR901 \
 -DVERSION_MAJOR=2 -DVERSION_MINOR=0 -DVERSION_MICRO=1 \
 -fcommon -fgnu89-inline -gstabs $INC"
OUT=/tmp/build-openjdk
rm -rf "$OUT"; mkdir -p "$OUT"
OBJS=""        # libcore objects -> libjvm.so
JAMO=""        # jam.c (main) -> the launcher, links against libjvm.so
compile() { ppc-amigaos-gcc $CFLAGS -c "$1" -o "$OUT/$2.o"; OBJS="$OBJS $OUT/$2.o"; }

echo "=== engine level $JAMVM_ENGINE (0 switch, 1 threaded, 2 direct, 3 inline) ==="
echo "=== core (all src/*.c except dll_ffi.c; jam.c -> launcher) ==="
for f in *.c; do
    case "$f" in
        dll_ffi.c) continue ;;
        jam.c) ppc-amigaos-gcc $CFLAGS -c jam.c -o "$OUT/jam.o"; JAMO="$OUT/jam.o"; continue ;;
    esac
    compile "$f" "$(basename "$f" .c)"
done

# -fno-reorder-blocks is REQUIRED by the inlining (code-copying) engine, and is
# exactly what upstream's configure adds for it: the engine copies each handler's
# machine code out of interp.c into a per-block buffer, so gcc must not move the
# handler bodies apart or splice shared tails between them.  Applies to the
# interpreter translation units only.
INTERP_CFLAGS="-fno-reorder-blocks"
# Only meaningful at level 3, where handler machine code is copied out of
# interp.c.  JamVM decides what may be copied by compiling the interpreter twice
# and memcmp'ing each handler between the two builds (relocatability.c) -- but a
# PC-relative branch to a target OUTSIDE the handler has the SAME encoding in
# both copies, so it passes that test and then points somewhere else once the
# block is moved.  Modern gcc manufactures exactly those by merging the common
# tails of different handlers, which upstream's lone -fno-reorder-blocks does not
# prevent.  This is a hypothesis for why level 3 miscompiles here, not a proven
# fix: it is untested on hardware, which is why 2 remains the default.
[ "$JAMVM_ENGINE" -ge 3 ] 2>/dev/null && \
    INTERP_CFLAGS="$INTERP_CFLAGS -fno-crossjumping -fno-tree-tail-merge"
icompile() { ppc-amigaos-gcc $CFLAGS $INTERP_CFLAGS -c "$1" -o "$OUT/$2.o"; OBJS="$OBJS $OUT/$2.o"; }

echo "=== interpreter ==="
compile interp/direct.c                i_direct
compile interp/inlining.c              i_inlining
icompile interp/engine/interp.c         e_interp
icompile interp/engine/interp2.c        e_interp2
icompile interp/engine/relocatability.c e_reloc

echo "=== classlib (openjdk) ==="
for f in thread class natives excep reflect dll jni jvm properties management \
         access frame shutdown alloc perf mh; do
    compile "classlib/openjdk/$f.c" "cl_$f"
done

echo "=== os/amiga (clib4: os.c + arch only) ==="
compile os/amiga/os.c             os_os
compile os/amiga/real_amiga_exit.c os_realexit
compile os/amiga/powerpc/dll_md.c ppc_dll_md
compile os/amiga/powerpc/init.c   ppc_init
ppc-amigaos-gcc $CFLAGS -c os/amiga/powerpc/callNative.S -o "$OUT/ppc_callNative.o"
OBJS="$OBJS $OUT/ppc_callNative.o"

# OpenJDK's Shutdown.beforeHalt() calls into the VM hook JVM_BeforeHalt().
# JamVM does not export that hook, so provide a harmless no-op compatibility
# symbol in libjvm.so.
cat > "$OUT/jvm_beforehalt_compat.c" <<'EOF'
void JVM_BeforeHalt(void) { }
EOF
ppc-amigaos-gcc $CFLAGS -c "$OUT/jvm_beforehalt_compat.c" -o "$OUT/jvm_beforehalt_compat.o"
OBJS="$OBJS $OUT/jvm_beforehalt_compat.o"

DEST=$BUILD_ROOT
mkdir -p "$DEST"

# libjvm.so = the VM as a SHARED library (upstream's libjvm.la = libcore).  A .so
# exports its defined symbols, so OpenJDK's libjava.so can resolve JVM_*/JNI_*
# against it at dlopen (an executable can't export them on AmigaOS -- see memory
# phase2-openjdk8-scoping).  rpath=JAVA:Sobjs for the clib4 sobjs.
echo "=== linking libjvm.so (shared VM) ==="
# Plain -shared (NOT -use-dynld, which forces --no-undefined): leave libc/pthread/
# zlib/JNI undefined -> resolved at runtime against the clib4 sobjs that the
# -use-dynld launcher loads (global symbol scope), same as the classpath natives.
ppc-amigaos-gcc -mcrt=clib4 -shared -fPIC -Wl,-rpath=JAVA:Sobjs \
    -o "$DEST/libjvm.so" $OBJS
echo "  libjvm.so OK ($(wc -c < "$DEST/libjvm.so") bytes)"

# AmigaOS $VER: cookie (read by the `Version` command), linked into the
# launcher.  Version proper = the Java-OS4 project version (clean major.minor);
# the OpenJDK class-library version follows as free text.
PVER=$(cut -d. -f1,2 "$PROJECT_ROOT/VERSION" 2>/dev/null)
[ -n "$PVER" ] || PVER="0.0"
JVER=$(sed -n 's/^JAVA_VERSION="\(.*\)"$/\1/p' "$BOOT_JDK/release" 2>/dev/null)
[ -n "$JVER" ] || JVER="1.8.0"
JDATE=$(date +%d.%m.%Y)
ppc-amigaos-gcc -mcrt=clib4 -O2 -fPIC \
    -DJAVAOS4_VER="\"$PVER\"" -DJAVAOS4_JAVAVER="\"$JVER\"" \
    -DJAVAOS4_DATE="\"$JDATE\"" \
    -c "$PROJECT_ROOT/src/version/verstag.c" -o "$OUT/verstag.o"

# jamvm launcher = jam.c (main) linked AGAINST libjvm.so -- one shared VM instance
# (so jam.c's main and any dlopen'd libjava see the same VM).
echo "=== linking jamvm-openjdk launcher (-> libjvm.so) ==="
ppc-amigaos-gcc -mcrt=clib4 -use-dynld -athread=native -Wl,-rpath=JAVA:Sobjs \
    -o "$DEST/jamvm-openjdk" "$JAMO" "$OUT/verstag.o" -L"$DEST" -ljvm \
    -lpthread -lm -lrt -lz -lauto
echo "LINK OK"

# The `java` launcher (src/launcher/java.c), which replaced the AmigaDOS script
# whose ReadArgs template ate '=' out of every -Dfoo=bar.
#
# STATIC, deliberately -- no -use-dynld here.  This program's whole job is to set
# LD_LIBRARY_PATH before the VM's loader runs, so it must be able to start when
# LD_LIBRARY_PATH is not yet set.  Linked against the clib4 sobjs it would need
# Sobjs/libc.so to be found in order to run the code that says where Sobjs is.
# The check below fails the build rather than shipping that circle.
# Host self-test first: it checks WHICH arguments reach the VM, which is the one
# thing here that can be wrong without looking wrong (a stray leading token is
# invisible whenever a real main class follows it).
if command -v cc >/dev/null 2>&1; then
    if cc -O1 -Wall -o "$OUT/test-java-launcher" \
          "$PROJECT_ROOT/tools/test-java-launcher.c" 2>"$OUT/e"; then
        "$OUT/test-java-launcher" >/dev/null || {
            echo "FATAL: java launcher self-test FAILED"
            "$OUT/test-java-launcher"
            exit 1
        }
        echo "=== java launcher self-test PASSED ==="
    else
        echo "WARN: host-compile of test-java-launcher.c failed; skipping self-test"
        head -3 "$OUT/e"
    fi
fi

echo "=== compiling java launcher (static) ==="
ppc-amigaos-gcc -mcrt=clib4 -O2 -Wall -Wextra \
    -DJAVAOS4_VER="\"$PVER\"" -DJAVAOS4_JAVAVER="\"$JVER\"" \
    -DJAVAOS4_DATE="\"$JDATE\"" \
    -o "$DEST/java" "$PROJECT_ROOT/src/launcher/java.c"

if ppc-amigaos-readelf -d "$DEST/java" 2>/dev/null | grep -qi "NEEDED"; then
    echo "ERROR: the java launcher has dynamic dependencies; it must be static." >&2
    ppc-amigaos-readelf -d "$DEST/java" | grep -i "NEEDED" >&2
    exit 1
fi
echo "  java OK ($(wc -c < "$DEST/java") bytes, no DT_NEEDED)"

ls -la "$DEST/libjvm.so" "$DEST/jamvm-openjdk" "$DEST/java"
echo "=== libjvm.so exports JVM_* ? ==="
ppc-amigaos-readelf --dyn-syms "$DEST/libjvm.so" 2>/dev/null | grep -cE "JVM_"
