#!/bin/sh
# Phase 5: assemble the Java-OS4 release -- an Installation Utility package.
#
# Runs in the javaos4-build container (has /opt/jdk8 and lha); the runtime fonts
# come from the repo (src/fontconfig/fonts), not from the host.
# Needs only the repo mounted at /work, with build/ already populated by the
# build scripts (it gathers their outputs -- it does not compile anything).
#
# Produces the distribution drawer + its icon and a .lha of both:
#   build/release/Java-OS4/            installer drawer (double-click to install)
#     install.py                       AmigaOS 4.1 Installation Utility script
#     JavaOS4InstallerLocale.py        locale strings
#     install.py.info                  project icon -> Sys:Utilities/Installation Utility
#     content/Java/                    the runtime payload copied to the chosen drawer
#   build/release/Java-OS4.info        distribution-drawer icon (beside the drawer)
#   build/JavaOS4-<ver>.lha            the release archive
#
# The runtime layout is FLAT (the boot classpath is colon-free + CWD-relative
# because JamVM's C boot-path parser splits on ':'; see docs), so the runtime
# lives in one drawer and the `java` launcher CDs into JAVA: (the assign the
# installer adds to S:User-Startup).
set -e

. "$(dirname "$0")/build-env.sh"

VER=$(cat "$PROJECT_ROOT/VERSION" 2>/dev/null || echo "0.0.0")
PVER=$(echo "$VER" | cut -d. -f1,2)          # 0.5.0 -> 0.5 (clean major.minor)
DATE=$(date +%d.%m.%Y)
# Official Java version the runtime is built on (the class-library JDK).
JVER=$(sed -n 's/^JAVA_VERSION="\(.*\)"$/\1/p' "$BOOT_JDK/release" 2>/dev/null)
[ -n "$JVER" ] || JVER="1.8.0"
JDK8=$BOOT_JDK
B=$BUILD_ROOT
N="$B/openjdk-natives"
SRC=$PROJECT_ROOT/src/installer
OUT="$B/release"
R="$OUT/Java-OS4"          # distribution drawer
RT="$R/content/Java"       # runtime payload (installed into the chosen drawer)
SOBJ="$RT/Sobjs"           # runtime shared C/support objects

echo "=== Java-OS4 $VER -- assembling $R ==="
rm -rf "$OUT"
mkdir -p "$RT/lib/fonts" "$SOBJ"

# --- runtime: VM + launcher -----------------------------------------------
cp "$B/jamvm-openjdk" "$RT/jamvm-openjdk"
cp "$B/libjvm.so"     "$RT/"

# `java` launcher.  It does NOT change directory: the VM finds its own runtime
# (boot jars, java.home, native libs) relative to PROGDIR: -- jamvm-openjdk's own
# directory -- so the caller's shell cwd is left untouched and the user's
# relative -cp resolves against THEIR directory.  LD_LIBRARY_PATH="PROGDIR:Sobjs"
# points the ELF loader at the bundled clib4/support sobjs in Sobjs/ regardless of cwd.
{
    echo ".KEY args/F"
    echo ".BRA {"
    echo ".KET }"
    echo ";\$VER: Java-OS4 $PVER ($DATE) OpenJDK $JVER"
    echo 'SetEnv LD_LIBRARY_PATH "PROGDIR:Sobjs"'
    echo 'SetEnv JAVA_HOME "JAVA:"'
    echo "JAVA:jamvm-openjdk {args}"
} > "$RT/java"

# --- runtime: OpenJDK + AWT natives ---------------------------------------
for so in libjava libverify libzip libnio libnet \
          libawt libfontmanager libamigaawt liblcms; do
    cp "$N/$so.so" "$RT/"
done

# --- runtime: CRT / support shared objects --------------------------------
# clib4's .so front-ends come straight from the in-repo clib4/ submodule build,
# in lockstep with clib4.library below.  libz (zlib) and libgcc (gcc runtime)
# are third-party / toolchain sobjs, NOT clib4 -- they stay in build/sobjs.
if [ -n "${CLIB4_BUILD_ROOT:-}" ] && [ -f "$CLIB4_BUILD_ROOT/build/clib4.library" ]; then
    CLIB4_SO_DIR=$CLIB4_BUILD_ROOT/build/lib
    CLIB4_LIBRARY_FILE=$CLIB4_BUILD_ROOT/build/clib4.library
else
    CLIB4_SO_DIR=$SDK_CLIB4/lib
    CLIB4_LIBRARY_FILE=$SDK_CLIB4/clib4.library
fi

if [ -f "$B/sobjs/libz.so.1" ]; then
    LIBZ_SO_FILE=$B/sobjs/libz.so.1
else
    LIBZ_SO_FILE=$(find "$SDK_LOCAL_CLIB4_LIB" -maxdepth 1 -type f \( -name 'libz.so.1' -o -name 'libz.so.1.*' \) | sort | head -n 1)
fi
if [ -f "$B/sobjs/libgcc.so" ]; then
    LIBGCC_SO_FILE=$B/sobjs/libgcc.so
else
    LIBGCC_SO_FILE=$(ppc-amigaos-gcc -mcrt=clib4 -print-file-name=libgcc.so 2>/dev/null)
fi

cp "$CLIB4_SO_DIR/libc.so" "$CLIB4_SO_DIR/libpthread.so" \
    "$CLIB4_SO_DIR/libm.so" "$CLIB4_SO_DIR/librt.so" "$SOBJ/"
[ -n "$LIBZ_SO_FILE" ] && [ -f "$LIBZ_SO_FILE" ] || { echo "Missing libz.so.1 for packaging"; exit 1; }
[ -n "$LIBGCC_SO_FILE" ] && [ -f "$LIBGCC_SO_FILE" ] || { echo "Missing libgcc.so for packaging"; exit 1; }
cp "$LIBZ_SO_FILE" "$SOBJ/libz.so.1"
cp "$LIBGCC_SO_FILE" "$SOBJ/"

# --- runtime: clib4.library (the C runtime the VM + .so stubs call into) ---
# The bundled .so stubs (libc.so, ...) are clib4.library front-ends; the real
# C runtime lives in clib4.library, which must be present in LIBS: at runtime.
# Ship it so the installer can put it there on a machine that has no clib4 --
# the .so stubs alone are not enough (this was the missing-requirement that
# blocked installs in 0.5.0).  Built from the clib4/ submodule (AmigaLabs/clib4
# `development`, incl. the AltiVec vec_strcpy page-overread fix #438), in
# lockstep with the .so front-ends above.  clib4 2.1+.
cp "$CLIB4_LIBRARY_FILE" "$RT/"

# --- runtime: class library + toolkit -------------------------------------
# Keep bootstrap jars in JAVA: root for Amiga runtime lookup compatibility.
cp "$JDK8/jre/lib/rt.jar" "$JDK8/jre/lib/charsets.jar" "$JDK8/jre/lib/jce.jar" \
    "$JDK8/jre/lib/jsse.jar" "$JDK8/jre/lib/resources.jar" \
    "$RT/"
[ -f "$JDK8/jre/lib/sunrsasign.jar" ] && cp "$JDK8/jre/lib/sunrsasign.jar" "$RT/" || true
cp "$N/niopatch.zip"     "$RT/"
cp "$B/amigatoolkit.zip" "$RT/"

# --- runtime: lib/ext (the extension class loader's jars) ------------------
# We shipped no lib/ext at all, and jce.jar is only the javax.crypto API: every
# actual cipher lives in com.sun.crypto.provider, which ships ONLY in
# lib/ext/sunjce_provider.jar.  java.security still lists SunJCE as
# security.provider.5, so the provider was silently skipped and any
# Cipher.getInstance("DES") died with "Cannot find any provider supporting DES".
# classlibDefaultExtDirs() already resolves java.ext.dirs to <java.home>/lib/ext
# (see the JamVM patch), so dropping the jars in is enough.
#
# Deliberately NOT shipped: sunec.jar and sunpkcs11.jar need native libraries
# (libsunec.so / libj2pkcs11.so) we do not build, so shipping them would trade a
# missing provider for a failing one; nashorn.jar (1.9M) and cldrdata.jar (3.7M)
# are big and unused by default on 8 (java.locale.providers is JRE,SPI, which
# reads localedata.jar); jaccess.jar is the Windows accessibility bridge.
# meta-index is skipped on purpose too -- it is a load-time optimisation that
# describes a lib/ext we do not reproduce.
mkdir -p "$RT/lib/ext"
for j in sunjce_provider.jar localedata.jar zipfs.jar dnsns.jar; do
    cp "$JDK8/jre/lib/ext/$j" "$RT/lib/ext/" 2>/dev/null || \
        echo "  WARN: missing $JDK8/jre/lib/ext/$j"
done

# JamVM -jar dispatch path requires jamvm.java.lang.JarLauncher to be available
# on the boot class path.  In OpenJDK mode JamVM includes java.home/classes by
# default, so compile and ship the class there.
JARLAUNCHER_SRC="$PROJECT_ROOT/vendor/jamvm/src/classlib/gnuclasspath/lib/jamvm/java/lang/JarLauncher.java"
[ -f "$JARLAUNCHER_SRC" ] || { echo "Missing JarLauncher source: $JARLAUNCHER_SRC"; exit 1; }
mkdir -p "$RT/classes"
"$JDK8/bin/javac" -source 8 -target 8 -bootclasspath "$JDK8/jre/lib/rt.jar" \
    -d "$RT/classes" "$JARLAUNCHER_SRC"

# --- examples + test suite (runnable out of the box) ----------------------
# 0.5.0 shipped nothing to run but `java -version`; bundle a headless demo, a
# Swing demo, and the self-verifying VM test suite.
mkdir -p "$RT/examples"
cp "$B/examples/"HelloJava.jar "$B/examples/"SwingDemo.jar "$RT/examples/"
[ -f "$B/examples/"awttest.jar ] && cp "$B/examples/"awttest.jar "$RT/examples/"
cp "$B/testsuite.zip" "$RT/examples/"

# --- runtime: lib/ resources (read from java.home/lib) --------------------
# lib/fonts must hold the JRE's own Lucida set, under the JRE's file names:
# sun.font.FontUtilities derives isOpenJDK from whether LucidaSansRegular.ttf
# exists there, and when it is missing SunFontManager.getDefaultFontFile()
# stays null -- which is the file every logical font is composed from in
# FontConfiguration.get2DCompositeFontInfo().  The DejaVu fonts we used to pull
# off the build host therefore left the runtime with no usable font at all.
# The fonts are vendored in-repo (src/fontconfig/fonts), so this no longer
# depends on what the build host happens to have installed.
cp "$PROJECT_ROOT/src/fontconfig/fontconfig.properties" "$RT/lib/"
for f in LucidaSansRegular.ttf LucidaSansDemiBold.ttf LucidaSansOblique.ttf \
         LucidaSansDemiOblique.ttf LucidaTypewriterRegular.ttf \
         LucidaTypewriterBold.ttf; do
    cp "$PROJECT_ROOT/src/fontconfig/fonts/$f" "$RT/lib/fonts/"
done
for p in currency.data tzdb.dat calendars.properties content-types.properties \
         flavormap.properties hijrah-config-umalqura.properties \
         logging.properties net.properties psfontj2d.properties \
         sound.properties; do
    cp "$JDK8/jre/lib/$p" "$RT/lib/" 2>/dev/null || true
done

# --- runtime: lib/cmm (ICC colour profiles) --------------------------------
# java.awt.color.ICC_Profile loads these by name from <java.home>/lib/cmm.  Miss
# them and ColorSpace.getInstance(CS_GRAY) throws "Can't load standard profile:
# GRAY.pf", which surfaces as an ExceptionInInitializerError out of anything
# doing image work -- it was killing Invoicex's splash screen through imgscalr.
mkdir -p "$RT/lib/cmm"
cp "$JDK8/jre/lib/cmm/"*.pf "$RT/lib/cmm/"

# --- runtime: lib/security ------------------------------------------------
# JCE refuses to start without these.  javax.crypto.JceSecurity's class
# initialiser calls setupJurisdictionPolicies(), which looks for the policy jars
# under <java.home>/lib/security (falling back to policy/unlimited when the
# crypto.policy property is unset, as it is in stock java.security); if they are
# missing it throws "Cannot locate policy or framework files!", which surfaces as
# an ExceptionInInitializerError from anything touching Cipher -- including
# SSLContext.getInstance().  java.security itself matters too: without it
# java.security.Security falls back to a hardcoded six-provider list and every
# other setting in that file is silently lost.  cacerts is what makes TLS trust
# work at all.
mkdir -p "$RT/lib/security"
for f in java.security java.policy blacklisted.certs cacerts; do
    cp "$JDK8/jre/lib/security/$f" "$RT/lib/security/"
done
cp -r "$JDK8/jre/lib/security/policy" "$RT/lib/security/"

# AmigaOS has no /dev/random or /dev/urandom; it has the RANDOM: DOS device.
# sun.security.provider.SeedGenerator opens securerandom.source eagerly and falls
# back to the (much slower) ThreadedSeedGenerator if that fails, so this is safe
# even on a system without RANDOM: -- it just gets used when present.  The URL
# must be "file:/RANDOM:", not "file:RANDOM:": SunEntries.getDeviceFile() treats
# a URI whose scheme-specific part does not start with '/' as opaque and
# resolves it against user.dir.  The leading '/' is the same Unix-absolute
# spelling used everywhere else here; amigaPath() strips it before the open.
sed -i 's|^securerandom.source=file:/dev/random$|securerandom.source=file:/RANDOM:|' \
    "$RT/lib/security/java.security"
grep -q '^securerandom.source=file:/RANDOM:$' "$RT/lib/security/java.security" \
    || { echo "Failed to point securerandom.source at RANDOM:"; exit 1; }

# Same assumption, second place: SecureRandom.getInstanceStrong() resolves
# securerandom.strongAlgorithms, and NativePRNGBlocking only registers when
# /dev/urandom exists -- so on AmigaOS every caller of getInstanceStrong() would
# get NoSuchAlgorithmException.  SHA1PRNG is always present, and now seeds from
# RANDOM: through securerandom.source above.
sed -i 's|^securerandom.strongAlgorithms=NativePRNGBlocking:SUN$|securerandom.strongAlgorithms=SHA1PRNG:SUN|' \
    "$RT/lib/security/java.security"
grep -q '^securerandom.strongAlgorithms=SHA1PRNG:SUN$' "$RT/lib/security/java.security" \
    || { echo "Failed to set securerandom.strongAlgorithms"; exit 1; }

# --- runtime: version + README --------------------------------------------
echo "$VER" > "$RT/VERSION"
cat > "$RT/README" <<README
Java-OS4 $VER -- runtime
========================

A Java 8 runtime for AmigaOS 4: JamVM 2.0 + the OpenJDK 8 class library, with a
Swing/AWT toolkit so Java GUIs run in Workbench windows.

Requirements
  clib4.library 2.1 or newer in LIBS: -- the C runtime the VM depends on.  The
  installer copies the bundled clib4.library there if it is missing.

The installer assigns JAVA: to this drawer (added to S:User-Startup so it
survives reboots) and copies the 'java' launcher to C: so it runs from any
Shell.  Try:

    java -version
    java -cp examples/HelloJava.jar HelloJava
    java -cp examples/SwingDemo.jar  SwingDemo
    java -cp examples/testsuite.zip  VmSuite

Swing/AWT applications need no extra flags -- the Amiga toolkit is the default.
App classpath (-cp) entries resolve relative to your CURRENT directory, like a
normal `java` command -- run from anywhere.  'javac' is not included -- compile
on a host JDK 8 with 'javac --release 8' and copy the jar over.  Bytecode newer
than Java 8 is rejected up front with UnsupportedClassVersionError.

Licensing: JamVM GPLv2; OpenJDK 8 GPLv2 + Classpath exception.
README

# --- unresolved-symbol check ----------------------------------------------
# The AmigaOS ELF loader binds lazily, so a symbol nothing provides does NOT
# fail the link or the load: it kills the running process the first time that
# code path is reached ("unable to resolve symbol fstat64 in libnio.so", which is
# how FileKey.c's missing LFS mapping surfaced -- long after the build was green).
# Check the whole shipped set against everything that will be loaded with it, and
# report anything new.  KNOWN_MISSING lists the gaps we accept, with a reason;
# anything outside it is a regression worth looking at before release.
KNOWN_MISSING="
BZ2_bzDecompress BZ2_bzDecompressEnd BZ2_bzDecompressInit BrotliDecoderDecompress
png_create_info_struct png_create_read_struct png_destroy_read_struct png_error
png_get_IHDR png_get_error_ptr png_get_io_ptr png_get_valid png_read_end
png_read_image png_read_info png_read_update_info png_set_expand_gray_1_2_4_to_8
png_set_filler png_set_gray_to_rgb png_set_interlace_handling png_set_longjmp_fn
png_set_packing png_set_palette_to_rgb png_set_read_fn
png_set_read_user_transform_fn png_set_strip_16 png_set_tRNS_to_alpha
fork vfork
"
# libfontmanager: freetype's optional PNG (colour-emoji bitmaps), bzip2 and WOFF2
#   paths.  Unreachable with the TrueType fonts we ship; fixing them means
#   bundling libpng/libbz2/brotli.
# fork/vfork: clib4 has neither, so java.lang.ProcessBuilder cannot work at all.
SYMS_PROVIDED=$B/.syms-provided
SYMS_NEEDED=$B/.syms-needed
: > "$SYMS_PROVIDED"
for so in "$SOBJ"/*.so* "$RT"/lib*.so "$RT"/libjvm.so; do
    [ -f "$so" ] || continue
    ppc-amigaos-nm -D --defined-only "$so" 2>/dev/null \
        | awk 'NF>1 && $2!="U" {print $NF}' >> "$SYMS_PROVIDED"
done
# resolved by the clib4 startup / SDK auto-open rather than by any .so
printf '%s\n' IExec IDOS IIntuition IGraphics IUtility __errno __ctype_ptr \
    __environ __stdin __stdout __stderr $KNOWN_MISSING >> "$SYMS_PROVIDED"
LC_ALL=C sort -u "$SYMS_PROVIDED" -o "$SYMS_PROVIDED"

unresolved=0
for so in "$RT"/lib*.so; do
    ppc-amigaos-nm -u "$so" 2>/dev/null | awk '{print $NF}' | LC_ALL=C sort -u > "$SYMS_NEEDED"
    miss=$(LC_ALL=C comm -23 "$SYMS_NEEDED" "$SYMS_PROVIDED")
    if [ -n "$miss" ]; then
        echo "WARNING: $(basename "$so") references symbols nothing provides:"
        echo "$miss" | sed 's/^/    /'
        unresolved=1
    fi
done
rm -f "$SYMS_PROVIDED" "$SYMS_NEEDED"
[ "$unresolved" = 0 ] && echo "=== symbol check: all shipped .so resolve (bar the known gaps) ==="

# --- installer (Installation Utility / Python 2.5) ------------------------
sed -e "s/@VERSION@/$VER/g" -e "s/@DATE@/$DATE/g" \
    "$SRC/install.py" > "$R/install.py"
cp "$SRC/JavaOS4InstallerLocale.py" "$R/"
cp "$SRC/install.py.info"           "$R/install.py.info"
cp "$SRC/drawer.info"               "$OUT/Java-OS4.info"
# drawer icon the installer copies to "<dest>.info" so the install drawer
# gets a Workbench icon.
cp "$SRC/drawer.info"               "$R/content/drawer.info"

# --- archive (the drawer + its icon) --------------------------------------
rm -f "$B/JavaOS4-$VER.lha"        # lha 'a' appends; start clean
( cd "$OUT" && lha -aq2 "$B/JavaOS4-$VER.lha" Java-OS4 Java-OS4.info >/dev/null )
echo "=== distribution tree ==="
( cd "$OUT" && find Java-OS4 Java-OS4.info -type f | sort | sed 's,^,  ,' )
echo "=== JavaOS4-$VER.lha ($(wc -c < "$B/JavaOS4-$VER.lha") bytes) ==="
lha -l "$B/JavaOS4-$VER.lha" | tail -6
